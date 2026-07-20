# `find_corresponding_draws` — Design Spec

**Date:** 2026-07-20
**Status:** Approved
**Context:** RenderDoc MCP server (`renderdoc-mcp/`) exposes replay introspection tools for cross-capture comparison between emulator and recompilation runs (see `docs/renderdoc-cross-capture-diagnostics.md`). `diff_shader_invocations` (priority #1), `trace_pixel_provenance` (priority #3), and `decode_post_vs_outputs` (priority #2) are implemented. This spec covers priority #4, "Content-aware cross-capture draw matching" — the doc's own ideal workflow names this step `find_corresponding_draw`, and no tool currently fills it.

---

## Background & Use Case

An agent comparing an emulator capture against a recompilation capture needs to find "the same draw" in both — e.g. a specific car's body-paint pass, or a specific UI element — before it can usefully call `diff_shader_invocations`, `trace_pixel_provenance`, or a future constant-buffer diff on a *matched pair*. Event IDs, resource IDs, and draw order are never comparable across captures (different APIs, different command buffer construction), so today an agent has to eyeball `list_draws_with_state` output from both captures and guess. The existing `diff_draw_sequences` tool aligns draws by *shape only* (topology, buffer/target counts) via `difflib.SequenceMatcher`, which assumes the two draw sequences are already in roughly the same relative order — it does not help when the agent has one specific draw of interest and needs to find its counterpart regardless of position.

`find_corresponding_draws` fills this gap directly: given one draw event in capture A, it ranks the most likely corresponding draw(s) in capture B by content-based fingerprint similarity, returning confidence scores with a per-signal breakdown so the agent (or a human) can judge the match rather than trust an opaque number.

**Cross-API reality check:** the roadmap doc's original wording ("fingerprint shader bytecode") doesn't hold up for this use case — emulator vs. recompilation captures are typically *different graphics APIs entirely* (e.g. Xbox 360 D3D9-style vs. modern D3D12/Vulkan), so raw shader bytecode never matches, byte-for-byte or otherwise, across compilers/ISAs. This spec deliberately de-emphasizes shader identity in favor of portable, API-agnostic signals (geometry shape, texture dimensions/content, constant values), with shader *reflection shape* (semantic names, not bytecode) contributing only a light weight.

---

## Scope

One new tool in `renderdoc-mcp/`:

| Feature | Kind | Files |
|---|---|---|
| `find_corresponding_draws` | New tool | `server.py`, new `draw_matching.py` |

**Scope decision:** single-reference-draw lookup (one draw in A → ranked candidates in B), not a full bipartite whole-sequence assignment. A full assignment problem (matching every draw in A to every draw in B optimally) is a materially different, more expensive algorithm (O(N×M) scoring, needs a real assignment solver to avoid double-booking candidates) and is a separately-justifiable future extension, not part of this spec.

No new Python dependencies. Reuses existing primitives:
- `serialize.build_draw_state_row` (structural prefilter — same shape key `diff_draw_sequences` already uses)
- `mesh_decode.decode_post_vs_outputs` (geometry signal — post-VS position bounding box)
- `analysis.analyze_texture_bytes` (texture content signal)
- `cbuffer` module (constant-buffer decode — constants signal)
- Shader reflection (`GetShaderReflection().outputSignature`/`.constantBlocks`) for the shader-shape signal

---

## `find_corresponding_draws`

### Purpose

Given a reference draw in one capture, rank the most likely corresponding draws in another capture by content-based similarity, in one call.

### Algorithm

**Phase 1 — cheap structural prefilter (no extra replay cost beyond what `build_draw_state_row` already does):**
Compute the reference draw's shape key — `(topology, has_index_buffer, len(vertex_buffers), len(color_targets), depth_target is not None)`, identical to `diff_draw_sequences`'s existing `shape_key` function. Filter capture B's candidate pool (from `event_ids_b` if given, else every Drawcall event up to `limit`) to only those sharing this exact shape key. If zero candidates survive, fall back to the full unfiltered candidate pool (up to `limit`) rather than returning an empty result — a shape mismatch is itself useful signal, not a reason to give up silently.

**Phase 2 — content-fingerprint scoring**, per surviving candidate, four signals each producing a 0.0–1.0 sub-score:

1. **Geometry (weight 0.4).** Vertex/index count closeness: `1 - min(1, abs(count_a - count_b) / max(count_a, count_b, 1))`. Post-VS position bounding-box shape: call `decode_post_vs_outputs(stage="vsout", preview_vertices=32)` on both draws, compute each draw's clip-space POSITION bounding-box extents per axis from the decoded sample (`dx, dy, dz = max - min` per axis, each floored at a small epsilon to avoid division by zero for degenerate/flat geometry), normalize each draw's 3-vector to sum to 1 (`ratio = (dx, dy, dz) / (dx + dy + dz)`) — this compares *proportions* (is it wide-and-flat vs. tall-and-thin), not absolute clip-space scale, which can differ slightly cross-API even for "the same" geometry. Score as `1 - (L1 distance between the two ratio vectors) / 2` (both vectors sum to 1, so their maximum possible L1 distance is 2, mapping this cleanly to a 0.0–1.0 similarity). Final geometry sub-score is the average of the count-closeness and bbox-ratio-similarity scores.
2. **Texture (weight 0.25).** Dimension-match half: 1.0 if both draws have a color target and its `(width, height)` match exactly (from `build_draw_state_row`'s existing target info, no extra replay cost), 0.5 if both have a color target but dimensions differ, 0.0 if exactly one draw has a color target at all (both-absent is treated as 1.0 — nothing to disagree on). Content-stat half: mean channel values via `analyze_texture_bytes` on the *primary* color target (first entry in `color_targets`) for both draws, scored as `1 - min(1, mean_abs_diff_across_channels)` (channel means are already in each format's normalized value range); if either draw has no color target, this half is skipped and the dimension-match half is used alone (not averaged with a fabricated 0). Deliberately does not attempt to pick/compare arbitrary bound input (SRV) textures — a draw may bind several, and choosing which one corresponds is itself an unsolved matching problem; scope stays to the render target being written, which is unambiguous.
3. **Constants (weight 0.2).** Decode both draws' bound constant buffers using the existing `cbuffer` module and compare typed scalar values (matched by constant *name*, present in both draws' CB layouts) with the same float tolerance convention `diff_shader_invocations` already uses (default `abs_tolerance=1e-6, rel_tolerance=1e-5`). Sub-score is the fraction of name-matched values that agree within tolerance; values present in only one draw's CB layout are not counted either way (CB layouts commonly differ slightly cross-API/cross-compiler even for equivalent shaders). If zero constants share a name across both draws, this signal is neutral (`0.5`) rather than 0 or 1 — no evidence either way shouldn't swing the score to an extreme.
4. **Shader shape (weight 0.15).** Jaccard similarity (`|intersection| / |union|`) of output-signature semantic names (`{semanticName}` sets) plus Jaccard similarity of constant-block names, averaged; if both sets being compared are empty for a given sub-signal, that sub-signal is defined as `1.0` (vacuously identical), not `0.0`. No bytecode or disassembly involved.

`confidence = 0.4*geometry + 0.25*texture + 0.2*constants + 0.15*shader_shape`. Candidates are sorted by `confidence` descending and truncated to `top_k`.

### Parameters

| Name | Type | Default | Description |
|---|---|---|---|
| `capture_a` | `str` | required | Reference capture |
| `event_id_a` | `int` | required | Reference draw event |
| `capture_b` | `str` | required | Capture to search for candidates |
| `event_ids_b` | `list[int] \| None` | `None` | Restrict the candidate pool; defaults to every Drawcall event up to `limit` |
| `limit` | `int` | `200` | Cap on candidate pool size before prefiltering, matching `diff_draw_sequences`'s existing convention |
| `top_k` | `int` | `5` | Number of ranked candidates to return |

### Response

```json
{
  "reference": {"event_id": 850, "name": "..."},
  "prefiltered_count": 340,
  "scored_count": 12,
  "shape_prefilter_applied": true,
  "candidates": [
    {
      "event_id": 812,
      "name": "...",
      "confidence": 0.87,
      "signals": {"geometry": 0.95, "texture": 0.80, "constants": 0.90, "shader_shape": 0.70}
    }
  ]
}
```

`shape_prefilter_applied: false` signals the phase-1 fallback occurred (no candidate shared the reference's exact shape key), so the caller knows every candidate in the pool was scored, not just shape-matching ones.

### Error Handling / Limitations

- If the reference event isn't a Drawcall, return an explicit error (`not_a_drawcall`) rather than scoring against it.
- Shader bytecode/disassembly is never compared (see Background) — a coincidentally-similar-looking draw with a completely different shader can still score high if its geometry/texture/constants happen to match; this is a heuristic ranking tool, not a guaranteed-correct match, and the per-signal breakdown exists precisely so a human/agent can judge which signals drove a given score.
- The texture signal only inspects the primary color target, never arbitrary bound input textures.
- Weights (0.4/0.25/0.2/0.15) are fixed, not caller-tunable, in this version.
- `decode_post_vs_outputs` is called twice per scored candidate (once for A, cached; once per B candidate) — each call does a real GPU readback, so `limit`/`top_k` bound the cost; no additional caching beyond memoizing the reference draw's own decode once per tool call.

### Testing

- Unit tests (`tests/test_draw_matching.py`) against the pure scoring functions (count-closeness, bbox-shape-closeness, Jaccard similarity, weighted combination) using synthetic inputs — no real replay session needed.
- Integration test under `util/test/rdtest` exercising a real capture with at least two structurally-similar-but-distinct draws, if a suitable existing test asset covers that; otherwise note the gap rather than fabricate one (per this project's established convention for this kind of test).

---

## Out of Scope (for this spec)

Full bipartite whole-sequence draw assignment (matching *every* draw in A to *every* draw in B optimally, avoiding double-booking) — a separate, larger feature if ever needed. Cross-capture constant-buffer diff (doc's #5) and texture threshold analysis (doc's #7) remain separate future specs.
