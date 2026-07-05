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

**Post-ship bug fix**: exactly this untested gap turned up a real crash — `debug_pixel` on
`fm2pressstart8.rdc` (DXIL/SM6+ shaders) returned `{"code": "debug_pixel_failed", "message":
"'value'"}`, a `KeyError` in `shader_variable_to_value()`: composite `ShaderVariable`s (structs/
arrays, common in DXIL traces, often with empty names) return a dict with `"members"` but no
`"value"` key, and `summarize_debug_trace` did a blind `after_val["value"]`. Fixed by (1) always
including a `"value"` key (`None` for composite types) so blind access never crashes again, (2)
preserving the full composite structure in `final_registers`/`"after"` instead of collapsing it to
`None`, and (3) falling back to a positional key (`_unnamed_{step}_{change}`) for empty register
names so distinct anonymous DXIL registers don't silently overwrite each other. Reproduced and
verified fixed end-to-end against real synthetic `ShaderVariable`/`ShaderDebugState` objects
(constructed via the actual RenderDoc API, not mocks) matching the exact reported scenario.

**Post-ship bug fix (2)**: `debug_vertex` (and `debug_pixel`) reported "keeps timing out". Root
cause confirmed live: `run_debug_trace()`'s `while True: ContinueDebug()` loop has no bound, and
every tool call is serialized behind a single global lock + single-worker thread (required because
`ReplayController` is not thread-safe). A shader that runs a very large or genuinely infinite number
of steps therefore never returns, wedging every subsequent tool call — not just `debug_vertex` — for
the lifetime of the process. Watched this happen live in this session: the renderdoc-mcp server
process grew to 20+GB / 1000+s CPU accumulating `ShaderDebugState`s and then crashed outright,
taking the whole MCP server down (confirmed via `Get-CimInstance`/`Get-Process` and an `open_capture`
call that itself hung for 300s waiting on the wedged lock). The C++ UI (`ShaderViewer.cpp`) has the
same unbounded loop but lets the user cancel by closing the debug view — this server has no
equivalent, so the fix adds a hard cap instead: `run_debug_trace()` now stops after
`MAX_DEBUG_STEPS` (20000) states and returns `(states, truncated)`; `debug_pixel`/`debug_vertex`
surface `truncated: true` + a `truncated_reason` in the response instead of hanging. Verified the
cap actually stops a simulated infinite `ContinueDebug()` loop (500-step cap, exactly 500 calls
made, `truncated=True`) and that a normal short trace is unaffected (`truncated=False`, all steps
kept). Synced the fix into the already-vendored `x64/Development/mcp/renderdoc_mcp` and `mcp_site/
renderdoc_mcp` copies (build outputs, gitignored) so it takes effect on the next server restart
without waiting for a full rebuild — **the currently-running server process still has the old
unbounded code loaded in memory and needs a restart** (Settings → MCP toggle off/on, or relaunch
qrenderdoc) to pick up the fix.

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

**Post-ship usability fix**: real usage against `fm2pressstart8.rdc` confirmed the tool works, but
that capture has 4 descriptor stores, forcing the caller to pick one every single call via the
`ambiguous_descriptor_store` error. Now, when `descriptor_store` is omitted, multiple stores exist,
and `is_sampler=false` (the common case), the store with the most descriptors is auto-picked
instead of erroring — sampler heaps are capped small (2048 max on D3D12) while a bindless engine's
shader-visible CBV/SRV/UAV heap is typically far larger (65536 in the reported case), so store size
alone reliably distinguishes them. The response includes `descriptor_store_auto_selected`/
`_candidates` so a caller who actually wanted a different store (e.g. a non-shader-visible staging
heap) can tell auto-selection happened and pass `descriptor_store` explicitly.
`is_sampler=true` with multiple stores still requires an explicit choice (unchanged, not part of
the report).

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

**Status: implemented.**

- `list_draws_with_state(capture_id, event_ids=None, name_contains=None, limit=200)`: one row per
  draw via a new `build_draw_state_row()` (`serialize.py`) — deliberately narrower than
  `normalize_pipeline_state` (no viewports/blend/depth-state/rasterizer/anomaly fields), just VB/IB
  bindings, RT/DS, topology, and per-stage shader constant-block names. Reuses the existing
  `serialize_vertex_inputs`/`serialize_graphics_targets` rather than re-deriving them.

- `diff_draw_sequences(capture_a, capture_b, event_ids_a=None, event_ids_b=None, limit=200)`:
  builds the same compact rows for both captures, then aligns them with stdlib
  `difflib.SequenceMatcher` over a **shape key** per draw (topology, VB count, has-index-buffer,
  color-target count, has-depth-target) — not event_id or resource_id, which are never comparable
  across two different capture files. `equal`/`replace` opcodes become aligned pairs (diffed with
  the existing `deep_diff` from `analysis.py`); `delete`/`insert` become `only_in_a`/`only_in_b`.

  Found during testing (not in the original plan): `event_id`, `resource_id`, *and* `name` all have
  to be stripped before diffing a pair, not just resource_id. Draw call names are the raw API
  function name (e.g. `vkCmdDrawIndexed` vs `DrawIndexedInstanced`) so they differ by backend even
  for the semantically-identical draw — left in, every single pair would show a spurious "name
  changed" entry, drowning out real diffs. `resource_name` is deliberately kept (unlike
  `resource_id`) since it's often preserved across backends and a genuine mismatch there (bound to
  the wrong named texture) is exactly the kind of thing this tool should surface.

Validated with synthetic draw-row fixtures (pure Python, no capture needed): identical draws
(differing only in resource ids/names/event ids) correctly produce zero changes; a draw with a
genuinely different bound texture name surfaces exactly that one change; a structurally different
draw (extra vertex buffer) surfaces the count change; and a draw entirely missing from one side
correctly lands in `only_in_a` via the `delete` opcode rather than being force-paired with an
unrelated neighbor.

## 9. Reflection completeness

`get_shader_reflection` should include input/output signatures (semantic names — to distinguish
interpolated vertex colors from UVs), sampler bindings, and CB variable layouts (names/offsets/
sizes), not just block names.

**Status: implemented.** Extended `serialize_shader_reflection_summary` (`serialize.py`) with three
new sections, plus richer `constant_blocks` entries — all from fields already on `ShaderReflection`
that just weren't surfaced:

- `input_signature`/`output_signature`: semantic name/index, register index, system value, var
  type, component count per `SigParameter` (new `serialize_sig_parameter`) — directly answers "is
  this interpolant a vertex color or a UV" from the semantic name alone.
- `samplers`: name, bind point, bind set/space, array size per `ShaderSampler` (new
  `serialize_shader_sampler`).
- `constant_blocks[].variables`: name, byte offset, type name/base type/rows/columns/elements, and
  a computed `byte_size`, recursing into nested struct members (new `serialize_shader_constant`,
  capped at depth 8 / 128 members for safety). `byte_offset` is relative to the immediate parent
  (struct or CB root), matching the API's own documented semantics — not accumulated into an
  absolute CB-root offset.

`readonly_bindings`/`readwrite_bindings` were deliberately left as bare name lists (out of scope —
the backlog's "not just block names" phrasing was about constant blocks specifically, not
read-only/read-write resource bindings).

Validated by constructing real `ShaderReflection`/`ConstantBlock`/`ShaderConstant`/`ShaderSampler`/
`SigParameter` objects directly (all default-constructible from Python, confirmed) and running them
through the actual serializer — caught and fixed a real bug this way: struct-typed constants
initially got a bogus 4-byte `byte_size` from the generic scalar-width fallback instead of their
actual aggregate size. Fixed by computing struct size as the sum of already-serialized member
sizes (a reasonable approximation that ignores alignment padding between members — true
std140/std430-exact sizing would need per-API packing-rule knowledge, out of scope here), and using
the authoritative `arrayByteStride` field directly for arrays (exact, not an approximation) when
present.

## 10. Replay stability with multiple captures

Opening 2–3 captures serially triggers a DXGI device error (source text was garbled here — likely
device-removed/hung), apparently worse with the qrenderdoc GUI open concurrently. Wants
one-replay-at-a-time with transparent LRU close/reopen, and automatic device-lost recovery instead
of failing calls outright.

**Current state:** `CaptureSessionManager` (`session.py`) already supports multiple concurrent
sessions with no eviction policy — every `open_capture` call creates a new `ReplayController`
without closing any others.
**Status: investigated (code-level only, no live repro available), root cause NOT confirmed. Did
NOT build the LRU/recovery workaround** — per this item's own instruction, and because I couldn't
get a conclusive answer.

What I checked in RenderDoc's C++ source: `D3D12_CreateReplayDevice()`
(`renderdoc/driver/d3d12/d3d12_replay.cpp`) creates a fresh `IDXGIFactory1`/adapter/`ID3D12Device`
on every call — no cached/singleton device, no `RegisterReplayProvider`-level cap, and no
"only one replay device" comment or check anywhere in `renderdoc/core/` or the D3D12 driver.
Architecturally, nothing blocks multiple simultaneous replay devices in one process.

Leading hypothesis (not confirmed): plain GPU/VRAM resource accumulation, not a hard concurrency
limit. `CaptureSessionManager` (`session.py`) has no eviction policy — every `open_capture` adds a
session without closing any others — and worse, `CaptureSession.shutdown()` wrapped
`controller.Shutdown()` in a bare `except Exception: pass`, silently swallowing any failure. If
Shutdown ever failed for a since-closed capture (plausible if the device was already in a bad
DXGI-error state), the GPU resources it held would never be freed, and there'd be zero evidence of
it in the logs — exactly the kind of gap that would make "opens 2-3 captures, hits a DXGI error"
look mysterious rather than an accumulating-resource problem.

**What I did fix** (safe, no policy/behavior change, purely observability): `shutdown()` now
returns whether `Shutdown()` actually succeeded and logs a warning with the capture_id on failure,
instead of swallowing it; `close_capture` propagates this into the tool response (`closed: false` +
a warning instead of always claiming success); `open_capture` now logs a warning listing which
other sessions are still open whenever a new one is opened while others are live. None of this caps
or evicts anything — it just means the *next* time this happens, there will be actual log evidence
(how many sessions were open, whether any prior Shutdown had silently failed) instead of none.

**What would actually confirm root cause**: reproducing it with logging in place, ideally with
`--pyrenderdoc`/MCP server logs captured across the failure. I don't have a way to do that in this
environment (see tasks #1/#3's note on the target-control sandbox blocker — same constraint
applies here, and this one additionally needs a real multi-GPU-resource scenario, not just a single
capture). Recommend watching for it to recur now that the logging is in place, or deliberately
reproducing it (open 3+ real captures via the actual MCP client without closing them, ideally with
qrenderdoc also open) and sending the resulting log — that would settle it either way before any
LRU/recovery logic gets built.

## Small fixes

**Status: both implemented.**

- **ResourceFormat serialization bug**: confirmed and reproduced directly against a real
  `rd.ResourceFormat()` instance — `enum_name(fmt)` on the whole struct falls through to
  `str(fmt)` (a raw `"<Swig Object of type 'ResourceFormat *' at 0x...>"` repr) because
  `ResourceFormat` has no `.name` attribute, only `.type`/`.compCount`/`.compByteWidth` and a
  `Name()` *method* that `enum_name()` never calls. Fixed in `serialize_vertex_inputs` (the
  reported case) — **and** found the identical bug in `serialize_used_descriptor` via a codebase
  grep for the same buggy pattern (`enum_name(getattr(..., "format", ...))` without a `.type`
  sub-access), which feeds `get_bound_resources`/`get_pipeline_state`/`get_shader`'s readonly/
  readwrite/sampler binding lists — same fix applied there too, since it's the same class of bug
  the backlog is asking to fix, just present in a second spot. Both now emit `format` (enum name),
  `format_compcount`, `format_bytewidth` instead of the raw struct repr.

- **pixel_history interpolants**: added `include_interpolants` (default `false`) to `pixel_history`,
  reusing task #1's `shader_debug.shader_variable_to_value` — for each history entry, calls
  `DebugPixel` at that entry's own `event_id`/`primitive_id` and reads `trace.inputs` directly
  without walking the full instruction trace (genuinely cheap, as the backlog hoped), then
  immediately frees it. Off by default since it's still one extra `SetFrameEvent` + `DebugPixel`
  call per entry; entries where debugging doesn't apply (e.g. a clear, not a fragment shader
  invocation) get `interpolants_error` instead of failing the whole call.
