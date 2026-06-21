# MCP Agentic Features — Design Spec

**Date:** 2026-06-21  
**Status:** Approved  
**Context:** RenderDoc MCP server (`renderdoc-mcp/`) already exposes 20 tools for replay introspection. This spec adds five features to support open-ended, symptom-driven debugging of console emulation captures (primarily Xenia / Xbox 360), where no reference capture exists and the agent must reason from a human-described visual symptom alone.

---

## Background & Use Case

A developer running a game in Xenia describes a visual problem ("black screen with UI on top", "sun renders through all geometry"). An AI agent connected via MCP needs to:

1. Quickly orient itself to the capture's frame structure
2. Visually inspect render targets at key points
3. Read typed shader data to find wrong values
4. Surface validation errors without being asked
5. Flag suspicious pipeline state inline on tools it was already going to call

No reference capture from real hardware is available. The agent works from a single `.rdc` capture and the symptom description.

Clients include both vision-capable (Claude, Cursor) and text-only agents, so every tool must be useful in both modes.

---

## Scope

Five additions to `renderdoc-mcp/`:

| Feature | Kind | Files |
|---|---|---|
| `get_frame_overview` | New tool | `server.py`, `analysis.py` |
| `get_texture_image` | New tool | `server.py`, new `imaging.py` |
| `read_constant_buffer` | New tool | `server.py`, `serialize.py` |
| `get_debug_messages` | New tool | `server.py` |
| Anomaly hints | Enrichment of existing responses | `analysis.py`, `serialize.py` |

No new Python dependencies. No new locking or session architecture. All new tools follow the existing `async with replay_execution() → asyncio.to_thread(_go) → R.ok/R.err` pattern.

---

## 1. `get_frame_overview`

### Purpose

First call on any capture. Gives the agent a structural map of the frame — render passes, render targets, draw counts — without replaying to every event.

### Implementation

Uses only `GetRootActions()`, `GetTextures()`, and `GetUsage()`. No `SetFrameEvent` calls, so no expensive replay seeks. Fast even on large captures.

- Walk the action tree: marker groups that contain draw children become render passes
- Call `GetTextures()` to enumerate textures with format/dimensions (capped at 2000 textures to avoid excessive work on captures with many resources)
- Call `GetUsage()` on each texture to find `ColourTarget` / `DepthStencilTarget` usages → these are render targets, with write and clear event IDs attached
- Count draws, dispatches, and copies at the top level

### Parameters

| Name | Type | Default | Description |
|---|---|---|---|
| `capture_id` | `str` | required | Session ID from `open_capture` |

### Response

```json
{
  "api": "Vulkan",
  "total_events": 1243,
  "draw_count": 87,
  "dispatch_count": 4,
  "render_passes": [
    {
      "marker_name": "Shadow Pass",
      "start_event_id": 10,
      "end_event_id": 120,
      "draw_count": 12
    }
  ],
  "render_targets": [
    {
      "resource_id": "ResourceId::1234",
      "resource_name": "MainColorRT",
      "format": "B8G8R8A8_UNORM",
      "width": 1280,
      "height": 720,
      "first_write_event_id": 45,
      "write_event_count": 38,
      "clear_event_ids": [12]
    }
  ],
  "debug_message_count": 3
}
```

### Diagnostic value

- A render target with `write_event_count: 0` after a clear → likely "black screen" root cause
- `debug_message_count > 0` hints the agent to call `get_debug_messages`
- Render pass structure tells the agent which event ranges to focus on

---

## 2. `get_texture_image`

### Purpose

Inspect what a render target or texture actually looks like at a given event. Returns statistics (text-only clients) and an optional inline base64 PNG (vision clients), plus a programmatically generated description.

### Implementation

- Reuses `analyze_texture_bytes` from `analysis.py` for statistics
- For the image: calls `SaveTexture` to a temp `.png` path (already implemented), reads bytes back, base64-encodes, deletes the temp file
- Selects the highest mip level whose longest dimension is ≥ `max_dimension` (i.e. the smallest mip that still covers the requested size). For textures with no mips beyond mip 0 (common for render targets), saves at mip 0 full resolution — `max_dimension` is a hint, not a guarantee
- Anomaly detection runs on the stats; `description` is generated from detected anomalies

Image generation lives in a new `imaging.py` module so it can be tested and reused independently.

### Parameters

| Name | Type | Default | Description |
|---|---|---|---|
| `capture_id` | `str` | required | |
| `resource_id` | `str` | required | |
| `event_id` | `int` | required | Replay is seeked to this event before sampling |
| `mip` | `int` | `0` | |
| `slice_index` | `int` | `0` | |
| `include_image` | `bool` | `True` | Set `False` for text-only clients to skip encoding |
| `max_dimension` | `int` | `256` | Longest axis of the output thumbnail in pixels |

### Response

```json
{
  "resource_id": "ResourceId::1234",
  "resource_name": "MainColorRT",
  "width": 1280,
  "height": 720,
  "format": "B8G8R8A8_UNORM",
  "stats": {
    "channels": {
      "r": {"min": 0.0, "max": 0.02, "mean": 0.001},
      "g": {"min": 0.0, "max": 0.01, "mean": 0.001},
      "b": {"min": 0.0, "max": 0.01, "mean": 0.001}
    },
    "black_ratio": 0.98,
    "nan_count": 0
  },
  "anomalies": ["blank", "uniform"],
  "description": "Nearly entirely black (98% blank pixels, mean brightness ~0.001). Draw calls targeting this RT may not be executing, or the shader is outputting zero.",
  "image_base64": "iVBORw0KGgo...",
  "image_format": "png"
}
```

`image_base64` and `image_format` are omitted when `include_image=False` or when the format is unsupported for PNG export.

### Anomaly keys

| Key | Condition |
|---|---|
| `blank` | All-channel mean < 0.01 |
| `saturated` | All-channel mean > 0.99 |
| `nan_present` | `nan_count > 0` |
| `uniform` | Per-channel max − min < 0.01 across the whole texture |
| `alpha_zero` | Alpha channel present and mean alpha < 0.01 |
| `single_color` | Channel variance < 0.001 (very tight distribution) |

### `description` generation

Generated by `imaging.py` from the anomaly list and stats. Examples:

- `blank`: "Nearly entirely black (N% blank pixels, mean brightness ~X)."
- `nan_present`: "Contains NaN values — likely an uninitialized or incorrectly cleared float buffer."
- `uniform + !blank`: "Solid or near-solid color (very low pixel variance). Possible clear target or shader outputting a constant."
- No anomalies: "Appears to contain normal image content. Channel means: R=X G=Y B=Z."

---

## 3. `read_constant_buffer`

### Purpose

Decode a constant buffer slot to typed named variables using shader reflection. Critical for finding wrong transform matrices, bad light direction values, or data the emulator failed to upload correctly.

### Implementation

1. Get `ShaderReflection` for the stage via `pipe.GetShaderReflection(stage)`
2. Find the constant buffer at `slot` in `refl.constantBlocks`
3. Resolve the buffer's `ResourceId` and read raw bytes with `GetBufferData`
4. Walk `ShaderConstant` entries — each has `name`, `byteOffset`, and `type` (component count, component type, row/column counts)
5. Unpack bytes with `struct` according to type; build variable dicts
6. Detect per-variable anomalies (zero matrix, identity matrix, NaN/Inf)
7. Fall back to raw hex if reflection is missing or the buffer cannot be read

Handles: `float` / `float2` / `float3` / `float4`, `float3x3` / `float3x4` / `float4x4`, `int` / `uint` / `bool`. Nested structs and arrays are flattened with dot-notation names.

### Parameters

| Name | Type | Default | Description |
|---|---|---|---|
| `capture_id` | `str` | required | |
| `event_id` | `int` | required | |
| `stage` | `str` | required | Same stage names as `get_shader` (`"vertex"`, `"pixel"`, etc.) |
| `slot` | `int` | required | Constant buffer register / binding slot index |

### Response

```json
{
  "stage": "vertex",
  "slot": 0,
  "name": "PerFrameConstants",
  "variables": [
    {
      "name": "WorldViewProj",
      "type": "float4x4",
      "value": [[0,0,0,0],[0,0,0,0],[0,0,0,0],[0,0,0,0]],
      "anomaly": "zero_matrix"
    },
    {
      "name": "LightDir",
      "type": "float3",
      "value": [0.577, 0.577, 0.577]
    },
    {
      "name": "Time",
      "type": "float",
      "value": 1.234
    }
  ],
  "anomalies": ["zero_matrix:WorldViewProj"],
  "raw_bytes_hex": "00000000..."
}
```

A zero `WorldViewProj` immediately explains why geometry is not appearing — the emulator did not upload the transform. `raw_bytes_hex` is always included as a fallback for partial reflection.

### Variable anomaly keys

| Key | Condition |
|---|---|
| `zero_matrix` | All elements are 0.0 |
| `identity_matrix` | Diagonal is 1.0, off-diagonal is 0.0 (may be valid, flagged for awareness) |
| `nan_or_inf` | Any element is NaN or Inf |
| `zero_vector` | All components are 0.0 |

---

## 4. `get_debug_messages`

### Purpose

Surface GPU validation layer errors and warnings. Console emulators frequently trigger these — format mismatches, descriptor binding errors, invalid state combinations — and they often name the exact root cause.

### Implementation

Single call to `controller.GetDebugMessages()`, mapped to dicts and grouped by severity. No replay seek needed.

### Parameters

| Name | Type | Default | Description |
|---|---|---|---|
| `capture_id` | `str` | required | |
| `severity_filter` | `str \| None` | `None` | `"Error"`, `"Warning"`, or `"Info"` to restrict output |

### Response

```json
{
  "error_count": 1,
  "warning_count": 3,
  "info_count": 12,
  "messages": [
    {
      "event_id": 45,
      "severity": "Error",
      "category": "Execution",
      "message": "Descriptor set binding 0 was not updated before use."
    }
  ]
}
```

When `severity_filter` is set, `messages` contains only matching entries; the counts always reflect the full set.

---

## 5. Anomaly Hints on Existing Tools

### Purpose

Flag suspicious pipeline state inline on responses the agent was already going to request — no extra round-trips.

### Scope

`get_pipeline_state` and `analyze_draw_visibility` gain an `anomalies: list[str]` field. The field is always present (empty list if nothing is suspicious) so clients can rely on its existence.

### Implementation

A new `detect_pipeline_anomalies(snap: dict) -> list[str]` function in `analysis.py` inspects the normalized pipeline snapshot dict that `normalize_pipeline_state` already builds. No additional `SetFrameEvent` or replay API calls.

### Anomaly keys

| Key | What it flags | Emulation relevance |
|---|---|---|
| `depth_test_disabled` | Depth test off on a draw with geometry output | Geometry may overdraw everything regardless of Z |
| `additive_blend` | Src + Dst additive blend on a color output | Common cause of "sun / glow renders through all geometry" |
| `no_color_outputs` | No color RT bound | Draw produces nothing visible |
| `depth_write_disabled` | Depth test on but depth writes off | Depth buffer not updated; may cause sort artifacts |
| `zero_viewport` | Viewport width or height is 0 | All geometry clipped before rasterization |
| `scissor_clips_all` | Scissor rect excludes the entire render target area | Same effect as `zero_viewport` |

### Response change (example on `get_pipeline_state`)

```json
{
  "ok": true,
  "rts": [...],
  "depth": {...},
  "shaders": {...},
  "anomalies": ["depth_test_disabled", "additive_blend"]
}
```

Existing clients that do not read `anomalies` are unaffected.

---

## Error Handling

All new tools follow the existing convention: unexpected exceptions return `R.err("error_code", str(ex))`. Specific cases:

- `get_texture_image` with unsupported format for PNG: returns stats + anomalies, omits `image_base64`, adds `"image_unavailable": true`
- `read_constant_buffer` with no reflection: returns `"variables": []`, includes `raw_bytes_hex`, adds `"reflection_unavailable": true`
- `get_frame_overview`: non-fatal if `GetUsage` fails on a resource — that resource is skipped with a warning in `"warnings": [...]`

---

## Testing

- Smoke import: existing `scripts/smoke_import.py` — just verifies the module builds without a capture
- Manual: open a Xenia `.rdc` capture and call each new tool via the MCP client
- Unit-testable in isolation: `detect_pipeline_anomalies` (pure dict → list), `description` generation in `imaging.py`, `struct`-unpacking in `read_constant_buffer`'s CB decoder
