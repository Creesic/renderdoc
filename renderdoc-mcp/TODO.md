# RenderDoc MCP — Feature Backlog

Captured from an FM2/plume debugging session: concrete pain points hit while using the MCP tools
(`renderdoc_mcp/server.py`) against real captures. Priority order as given by the reporter. Each
item below adds a "Current state" / complexity note gathered from reading the existing code, to
help with re-prioritizing before implementation starts — none of this has been implemented yet.

Several items touch environment/version quirks already logged in `AGENTS.md` (DLL loading next to
the replay host, API shape varying by RenderDoc build) — check there before assuming an API is
available or a fix is purely a Python change.

## 1. Pixel/vertex shader debugging (highest value)

New tools `debug_pixel(capture_id, event_id, x, y, sample=0, view=0)` and `debug_vertex(capture_id,
event_id, vertex_id, instance=0)`, using `ReplayController.DebugPixel()` / `DebugVertex()` →
`ShaderDebugTrace`, walked with `ContinueDebug()`.

- Default response should be a *summary*, not the full trace: input registers (interpolants with
  values), every resource access (sample/ld instructions — descriptor index/resource id and the
  value returned), constant-buffer reads (which CB, offset, value), and the final output register
  value.
- `full_trace=true` dumps the complete instruction-by-instruction trace to a file and returns the
  path (full traces are too large for inline responses).
- Why: hours spent inferring shader behavior from output colors alone. One call answering "why is
  this pixel black — bad sample, bad constant, or no sample at all?" would settle it immediately.

**Status: implemented** in `renderdoc_mcp/shader_debug.py` + `debug_pixel`/`debug_vertex` tools in
`server.py`. Walks `ContinueDebug()` to completion, summarizes inputs/constant blocks/final output
registers, and flags `SampleLoadGather` steps with a best-effort DXBC-style regex match
(`t#`/`s#`/`u#`/`cb#[#]`) against the disassembly line to resolve descriptor/resource identity —
falls back to raw instruction text + register value changes when the regex finds nothing (SPIR-V,
GLSL, or DXIL disassembly with different conventions). `full_trace=true` dumps the complete
per-instruction trace to a JSON file.

Every non-obvious API assumption (enum `.name` access, `ShaderValue` tuple read semantics,
`DebugPixelInputs()` defaults, `ShaderStage.Pixel`/`Fragment` aliasing) was checked against the
actual compiled `pymodules` in `x64/Development`, not just the C++ header docs — see the git log
for the verification session. **Not yet verified against a live capture**: getting a real .rdc via
`ExecuteAndInject` + target control in this environment hit `ident: 0` (target control connection
failed) even though injection reported success; root cause wasn't chased down further. Recommend
sanity-checking `debug_pixel` against one of the real FM2/plume captures this backlog came from
before relying on it.

## 2. Shader disassembly that works for DXIL

`get_shader(include_disassembly=true)` returns `"; Invalid Shader Specified"` for DXIL shaders in
at least one tested title, while DXBC shaders from another title disassembled fine. Fix direction:
use `GetDisassemblyTargets()` + `DisassembleShader()` with an explicit target, ensure
`dxcompiler.dll`/`dxil.dll` are loadable in the replay process, and expose the available targets in
the response.

**Status: root cause found, corrected, and partially addressed.** The original theory above (wrong
target picked, or `dxcompiler.dll`/`dxil.dll` not loadable) was **wrong**. Traced
`"; Invalid Shader Specified"` to the actual RenderDoc source — it's returned identically by
`D3D11Replay`, `D3D12Replay`, `VulkanReplay`, and `GLReplay`'s `DisassembleShader()` *before* the
`target` parameter is even inspected, whenever `refl.resourceId` fails to resolve to a tracked
shader resource in the replay driver's resource manager (e.g.
`renderdoc/driver/d3d12/d3d12_replay.cpp:594-598`). Confirmed against the real crash log
(`x64/Development/renderdoc_mcp_crash.log`, xenia_edge D3D12 capture): 3 vertex-stage and 1
pixel-stage shader hit this exact failure, while a different pixel shader in the same capture
disassembled fine — target selection had nothing to do with it (qrenderdoc's own `ShaderViewer.cpp`
even passes an empty target string, which is treated identically to the first target in this
driver's dispatch).

This means the underlying gap is a RenderDoc-core (C++) resource-tracking issue for however Xenia
creates those specific shaders (likely dynamic/JIT PSO compilation) — **not fixable from
`renderdoc_mcp`**, and not diagnosable further without the actual `xenia-edge` capture (a different
repo). Filing/investigating it upstream is a separate, much bigger effort than this backlog item.

**What was actually implemented** (in `server.py`'s `get_shader` + `shader_debug.py`'s
`disassembly_failure_reason`): always expose `available_targets` in the response; try every target
in the list instead of blindly using `targets[0]` (still correct in general — some *other* failure
reasons genuinely are target-specific, e.g. a missing DXC toolchain for the DXC-DXIL target while
the native target still works); and turn any of the known sentinel failure strings (across all four
backends) into a structured `disassembly_available: false` + `disassembly_error: "..."` instead of
silently returning cryptic text in the `disassembly` field. The `"; Invalid Shader Specified"` case
specifically gets a message explaining it's a replay-side tracking gap, not a target/DLL problem.

## 3. Descriptor-heap / bindless resolution (D3D12)

New tool `get_descriptor(capture_id, event_id, heap_index)`: resolve which resource a
shader-visible descriptor heap slot points to, with view type/format. API: `GetDescriptors()` /
`GetDescriptorStores()` (newer RenderDoc versions), or walk `GetD3D12PipelineState()` root
elements.

- Why: bindless engines pull texture indices out of a constant buffer value (e.g. CB slot 0 =
  heap index 34); there's currently no way to answer "what texture is heap[34]?" through the MCP.
  This blocked texture-binding verification repeatedly.

**Status: implemented, and it's not D3D12-only after all.** `GetDescriptors()`/`GetDescriptorStores()`
(plus `GetSamplerDescriptors()`, `GetDescriptorAccess()`, `GetDescriptorLocations()`) are a generic
cross-API "descriptor store" abstraction, not a D3D12-specific extension — confirmed present with
the exact expected field/method names in the real built `pymodules`
(`DescriptorType`, `DescriptorRange`, `DescriptorStoreDescription`, `DescriptorAccess`,
`DescriptorLogicalLocation` and all five `ReplayController` methods). This unifies D3D12 heap
indices and other APIs' descriptor storage under one model, so the `GetD3D12PipelineState()` root
elements fallback wasn't needed.

New tool `get_descriptor(capture_id, event_id, heap_index, descriptor_store=None, is_sampler=False,
count=1)` in `server.py`, with `serialize_descriptor`/`serialize_sampler_descriptor` in
`serialize.py`. Converts `heap_index` to a byte offset via the store's `descriptorByteSize`/
`firstDescriptorOffset`, then calls `GetDescriptors()` (or `GetSamplerDescriptors()` for
`is_sampler=True`). If `descriptor_store` is omitted: auto-picks when there's exactly one store in
the capture, otherwise returns the available stores (id, descriptor count/size) so the caller can
choose — this was necessary since a capture can have multiple stores (e.g. separate
CBV_SRV_UAV and Sampler heaps in D3D12) and nothing in `DescriptorStoreDescription` says which is
which.

**Not yet tested against a live capture** — hit the same target-control triggering blocker as
task #1 (see its note) when trying to get a real D3D12 capture with populated descriptor heaps to
test against. Only the API surface itself was validated directly against the built `pymodules`.
Recommend testing `get_descriptor` against a real bindless-heap capture before relying on it,
particularly the heap-index-to-byte-offset math and the sampler-vs-resource store disambiguation.

## 4. Full/raw constant buffer reads (quick win)

`read_constant_buffer` (`server.py` ~line 720) already fetches the *entire* CB into `raw` (capped
at 65536 bytes) and decodes all `variables` from it — but the `raw_bytes_hex` preview field is
hardcoded to `raw[:256].hex()` (512 hex chars) with no way to page further. Needed bytes 752–767 of
a 768-byte CB and had to reconstruct it by searching the backing buffer manually.

**Status: implemented.** Added `raw_offset: int = 0` and `raw_length: int = 256` params to
`read_constant_buffer` (`server.py`). `variables` was already decoded from the full CB and is
unaffected; only the `raw_bytes_hex` preview window changed, now sliced as
`raw[raw_offset:raw_offset+raw_length]` (length capped at 8192). Response also now reports
`raw_offset` and `raw_byte_length` (total decoded size) so callers know what range is pageable.
Verified the exact reported scenario (bytes 752-767 of a 768-byte CB) against a synthetic buffer —
`raw_offset=752, raw_length=16` returns exactly that tail slice.

## 5. Server-side buffer search

New tool `find_in_buffer(capture_id, resource_id, pattern_hex, event_id, start=0, end=buffer_size)`
→ list of match offsets. An 869KB buffer had to be downloaded in 64KB base64 chunks (each exceeding
the token cap, detouring through files) just to run a byte-pattern search the server could do in
milliseconds.

**Status: implemented.** `find_in_buffer(capture_id, event_id, resource_id, pattern_hex, start=0,
end=None, max_matches=1000)` in `server.py`, backed by `rdutil.find_pattern_offsets` — a
chunked scan (1MB fetches via the existing `controller_get_buffer_data`) with `len(pattern) - 1`
byte overlap between chunks so matches straddling a chunk boundary aren't missed or double-counted.
`end` defaults to the buffer's full length via `GetBuffers()` if not given.

The overlap/dedup bookkeeping is the part most likely to have off-by-one bugs, so it was unit
tested standalone (no capture needed, pure byte-buffer logic) against: patterns planted exactly on
chunk boundaries, chunk sizes smaller than the pattern itself (an edge case that silently broke the
first draft — fixed by flooring the effective chunk size well above the pattern length), all-overlapping
matches (e.g. searching `"AAA"` in `"AAAA..."`), no-match and pattern-longer-than-buffer cases, the
`max_matches` truncation path, and a stress test replaying the actual reported scenario (869KB
buffer, deliberately placed cross-boundary matches) across five different chunk sizes — all matched
a reference brute-force search exactly. Not yet tested against a live capture (same target-control
blocker as tasks #1/#3), but this one leans only on `GetBuffers()`/`GetBufferData()`, both already
proven via the existing `read_buffer` tool, so that gap is lower-risk here than for #1/#3.

## 6. Binary dumps straight to disk

Add an `out_file` param to `read_buffer` and `decode_mesh_inputs` that writes raw bytes (or CSV/NPY
for decoded meshes) directly to a path and returns metadata only. Base64-through-JSON at ~64KB/call
is the single biggest friction point across the whole session.

Also: raise `decode_mesh_inputs`'s `preview_vertices` cap — defaults to 8 (`server.py` ~line 627)
with no documented ceiling; needed 117+ vertices to plot glyph meshes.

**Status: implemented.**

- `read_buffer`: added `out_file`. When set, writes the raw bytes to disk and omits `base64` from
  the response (keeps the existing 512-byte `hex_preview` either way as a quick sanity check).
  `max_bytes` may go up to 256MB when `out_file` is set, vs. the normal 4MB inline cap — the smaller
  cap only existed to bound the base64/JSON payload, which no longer applies once writing to disk.

- `decode_mesh_inputs` (`mesh_decode.py`): added `out_file`, writing a CSV with one numeric column
  per vector component (e.g. `POSITION[0]`, `POSITION[1]`, `POSITION[2]`, or a bare `BONEID` column
  for scalar attributes) rather than a single opaque cell per attribute — chosen over NPY since it
  needs no new dependency (this project has no numpy dependency currently) and is directly
  plottable/importable. Also fixed the actual bug behind "lift the cap beyond 8": the number of
  indices fetched was hardcoded to `min(numIndices, 256)`, completely decoupled from
  `preview_vertices` — so asking for e.g. 300 previews silently capped at 256 with no indication
  why. Index fetch count now scales with `preview_vertices` directly (ceiling raised to 8192 for
  safety, since each vertex costs one `GetBufferData` call per attribute). The default
  (`preview_vertices=8`, no `out_file`) still returns byte-identical output to before (verified: the
  `repr()`-string inline format for `vertex_previews` is unchanged; only the CSV/out_file path is
  new), so no existing caller is affected. When `out_file` is set, the response keeps a small
  inline sample (first 8 rows) plus `vertex_count` instead of inlining everything.

Both changes were unit tested standalone (no capture needed): CSV column expansion for
multi/single-component attributes and `None` (fetch-error) values produce correctly empty cells
without misaligning columns, and the preview-formatting logic correctly distinguishes a genuine
decode error from a legitimate `None` value.

## 7. Structured event parameters

New tool `get_event_details(capture_id, event_id)` returning the API call's actual arguments from
`GetStructuredFile()` — critically for copies: `CopyTextureRegion` src/dst resources, subresources,
and regions. Had to infer copy sources/destinations from `get_resource_usages` cross-referencing,
which produced several wrong theories.

**Status: implemented.** `get_event_details(capture_id, event_id)` in `server.py`, backed by a new
`structured.py` module (`serialize_sdobject`/`serialize_chunk`) plus a `chunk_index_by_event` map
built once at capture-open time in `session.py`.

Key finding worth recording: `SDObject`'s Python-exposed API is much narrower than the C++ header
suggests. Almost every `IsXxx()`/`AsXxx()` convenience method in `structured_data.h` (`IsString`,
`IsArray`, `IsEnum`, `AsInt64`, `AsUInt64`, `AsDouble`, `SetCustomString`, etc.) is inside an
`#if !defined(SWIG)` guard and is **not** available from Python — confirmed directly against the
built `pymodules`, not assumed from the header. Only `AsBool`, `AsFloat`, `AsInt`, `AsResourceId`,
`AsString` exist in Python. So object *kind* has to come from `type.basetype` (an `SDBasic` enum
comparison) rather than any `IsXxx()` check, and there's no way to construct a custom-string object
from Python either (`SetCustomString` isn't exposed) — only read one via `obj.data.string` if a
chunk already has one set (from capture time).

Mapping `event_id -> chunk`: `ActionDescription.events` (a list of `APIEvent`, each with
`eventId`/`chunkIndex`) covers not just the action's own event but every state-setting call leading
up to it — so `get_event_details` works for *any* event, not just draws/copies. Built once during
the existing action-tree walk in `session.py`'s `open_capture` (reusing the same walk that already
builds `events_by_id`) rather than re-walking per call.

Validated unusually thoroughly for this codebase: built a real synthetic `SDChunk` via RenderDoc's
own `rd.SDChunk`/`rd.makeSD*` factory functions (a fake `CopyTextureRegion` chunk with resource IDs,
a nested struct, an array, an enum, a bool, a string, a signed int, and a float) and ran it through
`serialize_chunk` directly — every field type round-tripped correctly. This is real `SDObject`
traversal via the actual RenderDoc API, not a mock, so confidence here is higher than the
API-surface-only checks done for tasks #1/#3. The one part still untested against a live capture is
the `chunk_index_by_event` population itself (needs a real `ActionDescription.events` list).

## 8. Bulk per-draw state table

New tool `list_draws_with_state(capture_id)` → one row per draw with resource ids, reflection CB
block names, VB bindings (resource/offset/stride/size), IB binding, RT/DS, topology. Dozens of
`get_pipeline_state` calls (~10KB of noise each) were issued by hand to build this table — across
two captures, to diff backends. Wants `diff_draw_sequences(capture_a, capture_b)` on top.

**Complexity:** medium. Mostly composition of the existing `normalize_pipeline_state`
(`serialize.py`) across all draw events; `diff_draw_sequences` needs a sequence-alignment strategy
since draws won't necessarily line up 1:1 between two captures from different backends.

## 9. Reflection completeness

`get_shader_reflection` should include input/output signatures (semantic names — to distinguish
interpolated vertex colors from UVs), sampler bindings, and CB variable layouts (names/offsets/
sizes), not just block names.

**Complexity:** low-medium — likely extends `serialize_shader_reflection_summary`
(`serialize.py`) with fields already present on `ShaderReflection` but not yet surfaced.

## 10. Replay stability with multiple captures

Opening 2–3 captures serially triggers a DXGI device error (source text was garbled here — likely
device-removed/hung), apparently worse with the qrenderdoc GUI open concurrently. Wants
one-replay-at-a-time with transparent LRU close/reopen, and automatic device-lost recovery instead
of failing calls outright.

**Current state:** `CaptureSessionManager` (`session.py`) already supports multiple concurrent
sessions with no eviction policy — every `open_capture` call creates a new `ReplayController`
without closing any others.
**Complexity:** high, and the design shouldn't start until the root cause is confirmed — is this a
real GPU/device concurrent-context limit, or something fixable in session lifecycle? Don't build
the LRU/recovery workaround before that's known.

## Small fixes

- `get_pipeline_state`: vertex attribute format fields serialize as `"<Swig Object at 0x...>"`
  instead of a proper `ResourceFormat` breakdown (type/compCount/byteWidth). Not yet located
  precisely in `serialize.py` — needs a look before fixing.
- `pixel_history`: already good; add the fragment's interpolant values if cheap — natural
  follow-on once #1's `ShaderDebugTrace` plumbing exists, since interpolants come from the same
  machinery.
