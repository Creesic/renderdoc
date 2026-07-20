# `decode_post_vs_outputs` — Design Spec

**Date:** 2026-07-20
**Status:** Approved
**Context:** RenderDoc MCP server (`renderdoc-mcp/`) exposes replay introspection tools for cross-capture comparison between emulator and recompilation runs (see `docs/renderdoc-cross-capture-diagnostics.md`). `diff_shader_invocations` (priority #1) and `trace_pixel_provenance` (priority #3) are implemented. This spec covers priority #2, `decode_post_vs_outputs`.

---

## Background & Use Case

`decode_mesh_inputs` (existing tool) decodes the vertex *inputs* a draw reads from vertex buffers — the pre-transform attributes. It cannot answer "what did the vertex shader actually output for COLOR1 at vertex N" — that value only exists after the shader runs, in a driver-managed transform-feedback/stream-out buffer that RenderDoc's replay API calls "post-VS data." Mesh Viewer's GUI decodes this today by walking the shader's output-signature reflection to compute each semantic's byte offset inside that buffer. No MCP tool currently exposes it, so an agent verifying "does packed 0x4D000424 become the expected GPU-side COLOR1" has to reason about the raw output buffer bytes manually.

`decode_post_vs_outputs` exposes this same decode as a typed-semantics tool: given a draw event, return POSITION, COLOR0, COLOR1, texture coordinates, etc. for selected vertices (or all vertices, capped), decoded the same way Mesh Viewer's "VS Out" / "GS Out" tabs decode them.

This is a **single-capture, single-event** tool, matching `decode_mesh_inputs`'s existing shape.

---

## Scope

One new tool in `renderdoc-mcp/`:

| Feature | Kind | Files |
|---|---|---|
| `decode_post_vs_outputs` | New tool | `server.py`, new `mesh_decode.py` additions |

No new Python dependencies. Reuses:
- `session.find_action()` / `session.expand_action_flags()` (Drawcall validation, same as `decode_mesh_inputs`)
- `rdutil.controller_get_buffer_data()` (buffer reads, same as `decode_mesh_inputs`)
- `ReplayController.GetPostVSData(instance, view, stage)` and `PipeState.HasAlignedPostVSData(stage)` (both real virtual-interface methods on the public replay API, not constexpr helpers — safe to call directly from Python bindings)

New, reimplemented in Python rather than assumed-available as a SWIG-wrapped helper (same caution `mesh_decode.py`'s `unpack_data` already takes with format conversion): the `VarType` → byte-size / `CompType` lookup tables that `renderdoc/api/replay/replay_enums.h`'s `VarTypeByteSize`/`VarTypeCompType` define as C++ `constexpr` free functions. These are small, static tables (documented below) — reimplementing them in Python avoids depending on whether SWIG happens to wrap a non-virtual free function, which the codebase has no existing evidence either way for.

---

## `decode_post_vs_outputs`

### Purpose

Decode a draw's post-vertex-shader (or post-geometry/tessellation) output stream into typed, named semantics for selected vertices, in one call — the output-side counterpart to `decode_mesh_inputs`.

### Algorithm

1. `find_action()` + flag check: reject non-Drawcall events the same way `decode_mesh_inputs` does.
2. Resolve which shader's reflection describes the requested `stage`:
   - `stage="vsout"` → `pipe.GetShaderReflection(ShaderStage.Vertex)`.
   - `stage="gsout"` → if `pipe.GetShader(ShaderStage.Geometry) != Null`, use the Geometry shader's reflection; else if `pipe.GetShader(ShaderStage.Domain) != Null` (tessellation active), use the Domain shader's reflection; else return an error (`no_geometry_or_tessellation_stage`) rather than silently falling back to VS data — mirrors the real constraint (`BufferViewer.cpp:5683`: "if geometry/tessellation is enabled, only the GS out stage is rasterized output").
3. Call `controller.GetPostVSData(instance, view, MeshDataStage.VSOut|GSOut)`. If the returned `MeshFormat.vertexResourceId` is `Null`, return an explicit "no post-VS data available for this draw" response rather than proceeding with empty data.
4. Build the column layout from `reflection.outputSignature`, replicating `BufferViewer.cpp:1665-1759`'s exact algorithm:
   - Skip parameters where `stream != 0` (multi-stream GS output — out of scope, see Limitations) and where `systemValue == OutputIndices`.
   - Compute each parameter's `elem_byte_width` = 8 if `VarTypeByteSize(varType) > 4` else 4, and `comp_type` = `VarTypeCompType(varType)`, using a Python-native lookup table matching `replay_enums.h:304-410` exactly (not a call into an assumed SWIG binding).
   - Move the POSITION-tagged parameter (`systemValue == Position`) to the front of the list, keeping the rest in original order.
   - Walk in that order accumulating `byte_offset`: on Vulkan VSOut specifically (`pipe.HasAlignedPostVSData(stage)`), align 2-component fields to `2 * elem_byte_width` and 3-/4-component fields to `4 * elem_byte_width` before assigning the offset; otherwise pack tight. No separate per-primitive-rate offset track (see Limitations).
5. Fetch `preview_vertices` vertices' worth of raw bytes via the `MeshFormat`'s own index buffer (if present) / `vertexByteOffset`+`vertexByteStride`, same indexing shape as `decode_mesh_inputs`'s `fetch_indices`.
6. Decode each semantic's bytes per its `comp_type`/`comp_count`/`elem_byte_width` via `struct.unpack` — no UNorm/SNorm/BGRA handling needed here (post-VS output registers are always plain typed float/int/uint values, unlike vertex-input formats).
7. For the POSITION semantic specifically, additionally compute the perspective-divided NDC value (`x/w, y/w, z/w`) alongside the raw clip-space value, and include `unproject`, `flip_y`, `near_plane`, `far_plane` from the `MeshFormat` as top-level response metadata (mirrors `renderdoc/data/hlsl/mesh.hlsl:388-444`'s own unprojection, which is exactly a perspective divide plus an optional Y sign flip — not a camera/view-matrix reconstruction). Mapping further into screen-pixel coordinates stays out of scope for this tool (that's `pixel_history`/`trace_pixel_provenance`'s job).

### Parameters

| Name | Type | Default | Description |
|---|---|---|---|
| `capture_id` | `str` | required | Session ID from `open_capture` |
| `event_id` | `int` | required | Draw event to decode |
| `stage` | `str` | `"vsout"` | `"vsout"` or `"gsout"` |
| `instance` | `int` | `0` | Instance index to fetch post-VS data for |
| `view` | `int` | `0` | Multi-view/viewport index |
| `preview_vertices` | `int` | `8` | Vertices to decode (capped at `MAX_PREVIEW_VERTICES`, matching `decode_mesh_inputs`) |
| `out_file` | `str \| None` | `None` | Optional CSV export path, matching `decode_mesh_inputs`'s convention |

### Response

```json
{
  "event_id": 850,
  "stage": "vsout",
  "draw_name": "...",
  "semantics": [
    {"name": "POSITION", "semantic_name": "SV_Position", "semantic_index": 0,
     "system_value": "Position", "comp_type": "Float", "comp_count": 4, "byte_offset": 0},
    {"name": "COLOR1", "semantic_name": "COLOR", "semantic_index": 1,
     "system_value": "Undefined", "comp_type": "Float", "comp_count": 4, "byte_offset": 16}
  ],
  "vertex_count": 8,
  "vertex_previews": [
    {
      "vertex_index": 0,
      "index": 12,
      "values": {
        "POSITION": {"clip": [0.1, 0.2, 0.9, 1.0], "ndc": [0.1, 0.2, 0.9]},
        "COLOR1": [1.0, 0.0, 0.0, 1.0]
      }
    }
  ],
  "unproject": true,
  "flip_y": false,
  "near_plane": 0.1,
  "far_plane": 100.0
}
```

### Error Handling / Limitations

- Non-Drawcall events return the same kind of explicit `note` response `decode_mesh_inputs` already returns, not an exception.
- `stage="gsout"` with no Geometry or Domain (tessellation) shader bound returns an explicit `no_geometry_or_tessellation_stage` error — it does not silently substitute VS output.
- `MeshFormat.vertexResourceId == Null` (API/driver returned no post-VS data) returns an explicit "unavailable" response rather than proceeding to decode garbage.
- Multi-stream geometry shader output (`stream != 0`) is not decoded — only the primary stream (stream 0). A capture using GS stream-out to multiple streams would need a future extension.
- Mesh-shader/task-shader stages (`MeshOut`/`TaskOut`) are out of scope for this tool; only `VSOut`/`GSOut`.
- Per-primitive-rate output fields (a mesh-shader-specific concept) are not given their own offset track, since they cannot occur on a classic VSOut/GSOut pipeline this tool targets.
- POSITION's NDC value is a perspective divide only (`xyz/w`) plus the API's own `flip_y`/`near_plane`/`far_plane` passed through as metadata — not a screen-pixel reconstruction.

### Testing

- Unit tests (`tests/test_post_vs_outputs.py`) against the pure column-layout/offset-computation logic and the `VarType` lookup tables, using synthetic `SigParameter`-like stand-ins — no real replay session needed. Cover: POSITION reordering, Vulkan alignment vs. tight packing, GS/Domain reflection resolution rule, multi-stream skip, NDC perspective divide.
- Integration test under `util/test/rdtest` exercising a real capture with distinguishable per-vertex output semantics, if a suitable existing test asset covers that (learned from the `trace_pixel_provenance` plan: verify this before committing to a specific fixture, and log the gap rather than fabricate one if nothing suitable exists).

---

## Out of Scope (for this spec)

Everything else on `docs/renderdoc-cross-capture-diagnostics.md`'s roadmap: content-aware cross-capture draw matching, cross-capture constant-buffer diff, interpolant provenance as a standalone tool, and texture threshold analysis. `MeshOut`/`TaskOut` stages and multi-stream GS output remain candidates for a future extension of this same tool if ever needed.
