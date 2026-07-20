# `trace_pixel_provenance` — Design Spec

**Date:** 2026-07-20
**Status:** Approved
**Context:** RenderDoc MCP server (`renderdoc-mcp/`) exposes replay introspection tools for cross-capture comparison between emulator and recompilation runs (see `docs/renderdoc-cross-capture-diagnostics.md`). `diff_shader_invocations` (priority #1 on that doc's roadmap) is implemented. This spec covers priority #3, `trace_pixel_provenance`, recommended as the next addition because it directly answers "where did this pixel's value come from" — the question that recurs across pixel-correctness investigations once `diff_shader_invocations` has confirmed *a* shader invocation diverges but the agent still needs to find *which* upstream draw/copy fed it the wrong data.

---

## Background & Use Case

An agent comparing an emulator capture against a recompilation capture finds a pixel with the wrong final color. `pixel_history` shows what touched that pixel on its final render target, but real frames chain render targets together through blits, MSAA resolves, and copies (e.g. EDRAM resolve, post-process ping-pong buffers) before reaching the backbuffer. Today, walking that chain requires the agent to manually: call `pixel_history`, notice the last entry is a resolve/copy, call `get_resource_usages`/`trace_resource` to find the source resource, recompute the pixel coordinates, and call `pixel_history` again — repeating per hop, per capture.

`trace_pixel_provenance` collapses that manual loop into one call: given a final pixel and event, it walks backward through draws, copies, and resolves, reporting the shader output, blend result, and (optionally) sampled source textures at each hop, stopping when it reaches an unwritten/cleared resource or a hop it cannot safely resolve.

This is a **single-capture, single-point-in-time** tool, matching the existing `pixel_history` / `trace_resource` shape. Cross-capture comparison remains the agent's job — call it once per capture (same pattern as `debug_pixel` → `diff_shader_invocations`), then diff the two chains.

---

## Scope

One new tool in `renderdoc-mcp/`:

| Feature | Kind | Files |
|---|---|---|
| `trace_pixel_provenance` | New tool | `server.py`, `analysis.py` |

No new Python dependencies. Reuses existing primitives:
- `analysis.normalize_pixel_history` (already used by `pixel_history`)
- `shader_debug.summarize_debug_trace` (already used by `debug_pixel`/`debug_vertex`) for the optional sampled-resource detail on draw producers
- `session.find_action()` + `ActionDescription.copySource`/`copySourceSubresource` (public replay API) for jumping across Copy/Resolve actions
- `events_by_id[eid].flags_names` (precomputed at `open_capture`) to classify a producer event as Drawcall vs. Copy/Resolve/Clear without an extra replay call. Note: `renderdoc::ActionFlags` has no separate "blit" flag — blit-like copies (format-converting, scaling) are flagged `Copy`, same as same-format copies, so they're handled by the same "copy" branch below.

---

## `trace_pixel_provenance`

### Purpose

Given a final pixel, walk backward through its full provenance chain — across render targets connected by copies/resolves/blits — in one call, reporting what produced the value at each hop.

### Algorithm

Per hop, starting from `(resource_id, x, y, event_id)`:

1. Run `PixelHistory` at the current point (reusing `normalize_pixel_history`, same as the existing `pixel_history` tool).
2. Select the **last entry that was not culled/clipped/depth-or-stencil-failed/predicate-failed** as this hop's producer — the fragment that established the currently-visible value. Earlier entries are still returned in `entries` for context but are not recursed into (a hop only has one producer to chain backward from).
3. Classify the producer event via its precomputed `flags_names`:
   - **Drawcall** → attach `shader_output` and `post_mod`/`pre_mod` (already available from step 1, no extra replay cost). If `include_interpolants` or `include_resource_accesses` is set, run a debug trace (reusing the same machinery as `debug_pixel`) to attach pixel-shader inputs and/or the full resolved list of sampled SRVs/samplers with their resulting register values. The chain **stops here** — sampled source texels are reported as data, not recursed into as a further hop (a pixel shader may sample several textures per invocation; recursing into each would be combinatorial and is out of scope).
   - **Copy / Resolve** → look up the `ActionDescription` via `find_action()`, read `copySource` and `copySourceSubresource` (`.mip`/`.slice`/`.sample`), and continue to the next hop at `(copySource, x, y, mip=copySourceSubresource.mip, slice_index=copySourceSubresource.slice, sample_index=copySourceSubresource.sample, event_id=producer_event_id)`. Source and destination coordinates are assumed aligned (see Limitations).
   - **Clear / no entries** → stop; `stopped_reason` is `"cleared"` or `"initial_state"`.
4. Stop when: `max_depth` is reached, a hop can't be resolved (see Limitations), or there is no further history.

### Parameters

| Name | Type | Default | Description |
|---|---|---|---|
| `capture_id` | `str` | required | Session ID from `open_capture` |
| `resource_id` | `str` | required | Starting (final) resource |
| `x`, `y` | `int` | required | Pixel coordinates in the starting resource |
| `event_id` | `int` | required | Point in time to start the walk from |
| `mip`, `slice_index`, `sample_index` | `int` | `0` | Matches `pixel_history` |
| `type_cast` | `str` | `"Typeless"` | Matches `pixel_history` |
| `max_depth` | `int` | `4` (capped at 16) | Maximum number of hops, matching `trace_resource`'s convention |
| `include_interpolants` | `bool` | `False` | Attach pixel-shader inputs at each draw producer (opt-in: runs `DebugPixel`) |
| `include_resource_accesses` | `bool` | `False` | Attach resolved sampled-SRV/sampler values at each draw producer (opt-in: runs a full debug trace, the most expensive option) |

### Response

```json
{
  "chain": [
    {
      "capture_id": "...",
      "resource_id": "ResourceId::1234",
      "coordinates": {"x": 512, "y": 300},
      "event_id": 850,
      "entries": [ "...pixel_history entries, as today..." ],
      "producer": {
        "event_id": 848,
        "kind": "draw",
        "shader_output": {"...": "..."},
        "interpolants": [ "...only if requested..." ],
        "resource_accesses": [ "...only if requested..." ]
      }
    },
    {
      "capture_id": "...",
      "resource_id": "ResourceId::5678",
      "coordinates": {"x": 512, "y": 300},
      "event_id": 812,
      "entries": [ "..." ],
      "producer": {"event_id": 810, "kind": "resolve"}
    }
  ],
  "stopped_reason": "initial_state",
  "truncated": false
}
```

### Error Handling / Limitations

- The public replay API (`ActionDescription.copySource`/`copyDestination`) exposes only source/destination **resource + subresource**, not a sub-rectangle offset. Partial-rect copies or blits (source box not covering the full resource, or source/destination dimensions that don't match) cannot be safely remapped to the same `(x, y)`. The tool detects this by comparing resource dimensions before crossing the hop and stops with `stopped_reason: "partial_rect_copy_unsupported"` rather than silently returning a pixel from the wrong location — same honesty convention as `diff_shader_invocations`' truncation reporting and the README's existing "returns `available: false` instead of silently pretending defaults" pattern.
- MSAA resolve sources: `copySourceSubresource.sample` is used to pick which sample to continue the walk into for ordinary multi-sample copies. For a true `Resolve` action (which combines *all* samples into the destination pixel, not one specific sample), that field is not meaningful, so the walk falls back to `sample_index=0` and reports this as a simplification rather than fanning out into every sample.
- Unknown/unsupported producer kinds (e.g. compute writes via UAV, indirect args buffers feeding into vertex data — not framebuffer writes) stop the chain with `stopped_reason: "unsupported_producer_kind"` rather than guessing.

### Testing

- Unit tests (`tests/test_pixel_provenance.py`, following `test_shader_diff.py`'s style) against the pure chain-assembly/remap logic, using synthetic stand-ins for `PixelHistory` entries and `ActionDescription` (mock `copySource`/`copySourceSubresource`, `flags_names`) — no real replay session needed. Cover: multi-hop draw→resolve→draw chains, `max_depth` truncation, `partial_rect_copy_unsupported` detection, and the `include_interpolants`/`include_resource_accesses` opt-in paths.
- One integration test under `util/test/rdtest` exercising a real capture whose frame includes an MSAA resolve or render-target copy in the pixel's history, if a suitable existing test asset covers that; otherwise note the gap rather than fabricate a capture.

---

## Out of Scope (for this spec)

Everything else on `docs/renderdoc-cross-capture-diagnostics.md`'s roadmap: `decode_post_vs_outputs`, content-aware cross-capture draw matching, cross-capture constant-buffer diff, interpolant provenance as its own tool (partially covered here via `include_interpolants`), and texture threshold analysis. These remain candidates for future specs, prioritized separately.
