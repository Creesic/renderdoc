# RenderDoc MCP (replay-backed)

Expose RenderDoc **replay introspection** to MCP clients over **Streamable HTTP** at `http://<host>:<port>/mcp` — structured JSON tools for pipeline state, resources, textures, buffers, pixel history, mesh previews, shaders, and good-vs-bad capture diffs.

This package does **not** automate the RenderDoc GUI; it wraps the same **`renderdoc` Python module** shipped as **`pymodules`** next to a RenderDoc build.

## Using qrenderdoc (recommended)

1. Build or install RenderDoc so **`qrenderdoc`** sits next **`pymodules/`** (same layout as always).
2. Open **Settings → MCP**, enable **Enable MCP server**, pick a **Port** (default `8765`), and optionally set **Python executable** if `python3` / `python` / `py` is not on `PATH`.
3. Confirm status on the **main window status bar** (`MCP: Running`, etc.). The endpoint is shown in Settings as `http://127.0.0.1:<port>/mcp`.
4. Point your MCP client at that URL (e.g. Cursor). The GUI launches `python -m renderdoc_mcp` with `PYTHONPATH` including `pymodules`, **`mcp/`** (this package), and optional **`mcp_site/`** when shipped.

**CMake packagers:** `install()` copies `renderdoc_mcp` under `prefix/bin/mcp/`. Enable **`RENDERDOC_BUNDLE_MCP_PYTHON_SITE`** to run `pip install --target` during the build and install vendored wheels under **`prefix/bin/mcp_site/`** for fewer host Python dependencies.

## Prerequisites

1. A RenderDoc build with **`pymodules`** (e.g. `…/bin/x64/Development/pymodules` on Windows).
2. GPU/driver capable of replaying your captures locally.
3. **Python 3.10+** on `PATH` (or explicit path in Settings) with MCP dependencies installed, **unless** your distribution shipped **`mcp_site/`** via the CMake bundle option above.

## Install

From this directory:

```bash
pip install -e .
```

## Run

**Windows (PowerShell):**

```powershell
$env:PYTHONPATH="C:\path\to\RenderDoc\x64\Development\pymodules"
python -m renderdoc_mcp --host 127.0.0.1 --port 8765
```

**Linux:**

```bash
PYTHONPATH=/path/to/renderdoc/bin/Linux/Debug/pymodules python3 -m renderdoc_mcp --host 127.0.0.1 --port 8765
```

### StdIO (recommended for editors that spawn MCP locally)

Agents like **OpenCode** often use MCP over process **stdin/stdout** when configured as `"type": "local"`. Prefer this over TCP — it avoids Streamable HTTP / SSE negotiation quirks and flaky remote timeouts.

Use the same `PYTHONPATH` as above (ensure **`mcp`** appears **before** **`mcp_site`** so imports resolve to the shipped package beside the build):

```powershell
cd C:\path\to\RenderDoc\x64\Development
$env:PYTHONPATH="pymodules;mcp;mcp_site"
$env:RENDERDOC_MCP_APPDIR=(Get-Location).Path
.\python\python.exe -m renderdoc_mcp --transport stdio
```

OpenCode `config.json` example (adjust paths):

```jsonc
"mcp": {
  "renderdoc": {
    "type": "local",
    "command": [
      "C:\\path\\to\\RenderDoc\\x64\\Development\\python\\python.exe",
      "-m",
      "renderdoc_mcp",
      "--transport",
      "stdio"
    ],
    "environment": {
      "PYTHONPATH": "pymodules;mcp;mcp_site",
      "PYTHONUTF8": "1",
      "RENDERDOC_MCP_APPDIR": "C:\\path\\to\\RenderDoc\\x64\\Development"
    },
    "enabled": true,
    "timeout": 120000
  }
}
```

If OpenCode supports a **`cwd`** for the child process, set it to **`…\\Development\\python`** on Windows when using bundled `python.exe` so a stray **`_ctypes.pyd`** next to **`qrenderdoc.exe`** does not shadow the interpreter’s **`DLLs`**.

### Remote URL (Streamable HTTP)

Editors with **`type: "remote"`** must speak MCP **Streamable HTTP** (correct `Accept` headers, session IDs). A plain **`curl`** to **`GET /mcp`** may return **406** — wrong headers, not necessarily a dead listener.

The server finishes **`initialize` / `tools/list` immediately**: **`InitialiseReplay`** runs on the **first replay tool call**, not during HTTP startup (Tracy-style — listen first, heavy work later). That avoids remote clients timing out while the UI still shows “loading…”.

**OpenCode tips:** use **`"oauth": false`** for trivial localhost servers, **`"timeout": 120000`** if the editor defaults are short, and **`opencode mcp debug …`** when diagnosing transport issues. Prefer **local + stdio** if your editor supports it.

Then configure your MCP client (e.g. Cursor) with URL:

```text
http://127.0.0.1:8765/mcp
```

## Tools

| Tool | Purpose |
|------|---------|
| `open_capture` | Load `.rdc`, returns `capture_id` |
| `close_capture` | Release replay resources |
| `list_events` | Filter/search frame actions + marker stacks |
| `search_marker_paths` | Search nested marker paths and return matching events/actions |
| `set_event` | Replay to `event_id` |
| `get_pipeline_state` | Normalized RTs/depth, viewport/scissor, raster/blend/depth, shaders/bindings |
| `get_bound_resources` | Descriptor/bind summaries merged across stages |
| `get_descriptor` | Resolve descriptor-store/heap slots to bound resources or samplers |
| `get_frame_overview` | Fast render-pass / RT / depth-target summary without replaying to each event |
| `list_resources` | Resources optionally filtered by `resource_type` |
| `get_resource_usages` | Per-resource usage timeline (`event_id`, usage enum) |
| `trace_resource` | Walk producer/consumer usage chains for a texture/buffer around an event |
| `analyze_texture` | mip/slice stats: min/max/mean channels, black ratio, NaN count |
| `get_texture_image` | Downsample/export texture image previews plus safe stats when format is simple |
| `save_texture` | Export PNG/JPEG/HDR/DDS/BMP via `TextureSave` |
| `read_buffer` | `GetBufferData` as hex preview + base64 (size capped) |
| `diff_buffer_between_events` | Compare a buffer byte range at two events and return changed ranges |
| `find_in_buffer` | Server-side byte-pattern search in large buffers |
| `read_constant_buffer` | Decode constant buffers by reflection and page raw byte previews |
| `pixel_history` | Structured pixel modifications (+ optional `event_id`) |
| `trace_pixel_provenance` | Walk a final pixel's provenance backward across draws, copies, and resolves in one call |
| `diff_pipeline_state` | Deep diff of normalized pipeline snapshots |
| `list_draws_with_state` | Compact per-draw state table for scanning many draws |
| `diff_draw_sequences` | Align and diff compact draw-state rows between captures |
| `find_corresponding_draws` | Rank the most likely corresponding draw(s) in another capture by content-fingerprint similarity |
| `diff_texture_stats` | Compare `analyze_texture`-style stats good vs bad |
| `decode_mesh_inputs` | Vertex layout + indexbuffer summary + vertex previews |
| `decode_post_vs_outputs` | Decode a draw's post-VS/GS output into typed semantics (POSITION, COLORn, texcoords) for selected vertices |
| `get_shader` | Shader IDs + optional disassembly/reflection |
| `get_shader_reflection` | Constant buffers / binding names summary |
| `debug_pixel` / `debug_vertex` | Shader trace summaries with inputs, resource accesses, constants, and outputs |
| `diff_shader_invocations` | Compare two vertex/pixel traces and report the first divergent instruction/result |
| `analyze_draw_visibility` | `SamplesPassed` counter when available + viewport/scissor heuristics |

## Smoke check

With `PYTHONPATH` set so `import renderdoc` succeeds:

```bash
python -c "from renderdoc_mcp.server import build_mcp; build_mcp(); print('ok')"
```

Or:

```bash
python scripts/smoke_import.py
```

## Limitations

- **`SamplesPassed`** and **pixel history** may be unavailable or slow depending on API/GPU.
- Some RenderDoc Python builds expose raster/depth/stencil/blend state through different PipeState
  method names. The MCP tries the known generic and D3D12-style accessors; if none are available,
  those sections return `available: false` instead of silently pretending defaults.
- **`decode_mesh_inputs`** rejects instanced draws (same as upstream decode_mesh sample).
- **`diff_shader_invocations`** aligns matching disassembly text when available, otherwise it falls
  back to execution position. Cross-API shaders compiled to substantially different instruction
  streams may therefore need manual interpretation even though input/constant/output diffs remain useful.
- Replay APIs must run serialized; the server uses a lock around all tools.
- **`trace_pixel_provenance`** stops a Copy/Resolve hop with
  `stopped_reason: "partial_rect_copy_unsupported"` when source and destination dimensions differ
  at their respective mips, since renderdoc's `ActionDescription` exposes only a resource +
  subresource for a copy, never a sub-rectangle offset. This only catches partial-rect
  copies/blits that also change the overall resource size -- an offset partial-rect copy between
  two equally-sized resources (e.g. `CopySubresourceRegion`/`CopyTextureRegion` copying just the
  bottom half of a texture into an equally-sized one) is indistinguishable from a full-surface
  copy through this API, so the tool silently assumes `(x, y)` is aligned; this is a real,
  undetectable-from-here limitation rather than something the dimension check guards against. A
  true `Resolve` action's
  `copySourceSubresource.sample` isn't a single meaningful sample (all samples combine into the
  destination pixel), so the walk falls back to `sample_index=0` when continuing into a resolve
  source rather than fanning out into every sample.
- **`decode_post_vs_outputs`** only decodes the primary output stream (`stream=0`) of a Geometry
  shader's multi-stream output, and only supports `VSOut`/`GSOut` -- mesh-shader/task-shader
  stages (`MeshOut`/`TaskOut`) aren't exposed by this tool. POSITION's reported NDC value is a
  perspective divide only (`xyz/w`), not a full camera/view-matrix reconstruction into screen
  pixel coordinates -- use `pixel_history`/`trace_pixel_provenance` for that.
- **`find_corresponding_draws`** never compares shader bytecode or disassembly (meaningless across
  different graphics APIs) -- it ranks by post-VS position bounding-box shape, primary
  render-target dimensions/content, constant-buffer values, and shader reflection name sets only.
  It only inspects the primary color target (`color_targets[0]`), never arbitrary bound input
  textures, and signal weights are fixed, not caller-tunable. This is a heuristic ranking to
  narrow a search, not a guaranteed-correct match.

## License

MIT (consistent with RenderDoc).
