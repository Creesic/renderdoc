"""FastMCP server: RenderDoc replay tools on HTTP `/mcp`."""

from __future__ import annotations

import asyncio
import base64
from contextlib import asynccontextmanager
from typing import Any, Literal, cast

from mcp.server.fastmcp import FastMCP
from mcp.server.transport_security import TransportSecuritySettings

from renderdoc_mcp import responses as R
from renderdoc_mcp import rdutil
from renderdoc_mcp.analysis import (
    analyze_texture_bytes,
    diff_pipeline_snapshots,
    diff_texture_analysis,
    draw_visibility_analysis,
    find_texture_description,
    normalize_pixel_history,
)
from renderdoc_mcp.imaging import (
    detect_texture_anomalies,
    describe_texture,
    save_texture_as_png_bytes,
)
from renderdoc_mcp.mesh_decode import decode_mesh_inputs as decode_mesh_inputs_core
from renderdoc_mcp.serialize import (
    normalize_bound_resources,
    normalize_pipeline_state,
    serialize_shader_reflection_summary,
)
from renderdoc_mcp.session import CaptureSessionManager, filter_events


replay_lock = asyncio.Lock()
sessions = CaptureSessionManager()

_replay_init_lock = asyncio.Lock()
_replay_initialized = False


async def ensure_replay_initialized() -> None:
    """Load pymodules and InitialiseReplay on first tool use — keeps MCP handshake instant."""
    global _replay_initialized
    if _replay_initialized:
        return
    async with _replay_init_lock:
        if _replay_initialized:
            return

        def _init():
            rd = rdutil.get_renderdoc()
            rd.InitialiseReplay(rd.GlobalEnvironment(), [])

        await asyncio.to_thread(_init)
        _replay_initialized = True


def _shutdown_replay_sync() -> None:
    try:
        rdutil.get_renderdoc().ShutdownReplay()
    except Exception:
        pass


@asynccontextmanager
async def replay_execution():
    """Serialize replay-backed tools after lazy InitialiseReplay (matches Tracy-style immediate HTTP MCP startup)."""
    await ensure_replay_initialized()
    async with replay_lock:
        yield


def _stage_from_string(rd: Any, name: str) -> Any | None:
    key = name.strip().lower()
    mapping = {
        "vertex": "Vertex",
        "vs": "Vertex",
        "pixel": "Pixel",
        "fragment": "Fragment",
        "fs": "Fragment",
        "ps": "Pixel",
        "hull": "Hull",
        "domain": "Domain",
        "geometry": "Geometry",
        "compute": "Compute",
        "cs": "Compute",
        "mesh": "Mesh",
        "amplification": "Amplification",
        "task": "Task",
    }
    attr = mapping.get(key, key.title())
    if hasattr(rd.ShaderStage, attr):
        return getattr(rd.ShaderStage, attr)
    if hasattr(rd.ShaderStage, key.title()):
        return getattr(rd.ShaderStage, key.title())
    return None


def _resource_type_from_string(rd: Any, name: str | None) -> Any | None:
    if not name:
        return None
    key = name.strip().lower()
    for candidate in dir(rd.ResourceType):
        if candidate.lower() == key:
            return getattr(rd.ResourceType, candidate)
    return None


@asynccontextmanager
async def _lifespan(_: FastMCP):
    try:
        yield
    finally:
        sessions.close_all()
        async with _replay_init_lock:
            global _replay_initialized
            if _replay_initialized:
                await asyncio.to_thread(_shutdown_replay_sync)
                _replay_initialized = False


def build_mcp() -> FastMCP:
    mcp = FastMCP(
        "renderdoc-replay",
        instructions=(
            "Graphics debugging via RenderDoc replay: open captures, inspect events, pipeline state, "
            "resources, textures, buffers, pixel history, and diffs. Requires RenderDoc pymodules on PYTHONPATH."
        ),
        host="127.0.0.1",
        port=8765,
        streamable_http_path="/mcp",
        lifespan=_lifespan,
        transport_security=TransportSecuritySettings(enable_dns_rebinding_protection=False),
    )

    @mcp.tool()
    async def open_capture(path: str) -> dict[str, Any]:
        """Load a .rdc capture for replay. Returns capture_id for other tools."""

        async with replay_execution():

            def _go() -> dict[str, Any]:
                try:
                    sess = sessions.open_capture(path)
                except Exception as ex:
                    return R.err("open_failed", str(ex))
                rd = rdutil.get_renderdoc()
                return R.ok(
                    {
                        "capture_id": sess.capture_id,
                        "path": sess.path,
                        "driver": sess.driver_name or rdutil.replay_driver_hint(sess.controller),
                        "api": enum_api(rd, sess.controller),
                        "event_count": len(sess.events_ordered),
                    }
                )

            return await asyncio.to_thread(_go)

    @mcp.tool()
    async def close_capture(capture_id: str) -> dict[str, Any]:
        async with replay_execution():

            def _go() -> dict[str, Any]:
                sessions.close_capture(capture_id)
                return R.ok({"capture_id": capture_id, "closed": True})

            return await asyncio.to_thread(_go)

    @mcp.tool()
    async def list_events(
        capture_id: str,
        name_contains: str | None = None,
        name_regex: str | None = None,
        require_flags: list[str] | None = None,
        cursor: str | None = None,
        limit: int = 200,
    ) -> dict[str, Any]:
        async with replay_execution():

            def _go() -> dict[str, Any]:
                sess = sessions.get(capture_id)
                if sess is None:
                    return R.err("unknown_capture", capture_id)
                rows, next_cur = filter_events(
                    sess,
                    name_contains=name_contains,
                    name_regex=name_regex,
                    require_flags=require_flags,
                    cursor=cursor,
                    limit=min(limit, 1000),
                )
                return R.ok({"events": rows, "next_cursor": next_cur})

            return await asyncio.to_thread(_go)

    @mcp.tool()
    async def set_event(capture_id: str, event_id: int, force_complete_replay: bool = True) -> dict[str, Any]:
        async with replay_execution():

            def _go() -> dict[str, Any]:
                sess = sessions.get(capture_id)
                if sess is None:
                    return R.err("unknown_capture", capture_id)
                try:
                    sessions.set_frame_event(sess, int(event_id), force_complete_replay)
                except Exception as ex:
                    return R.err("set_event_failed", str(ex))
                ie = sess.events_by_id.get(int(event_id))
                summary = None
                if ie:
                    summary = {
                        "event_id": ie.event_id,
                        "name": ie.name,
                        "flags": ie.flags_names,
                        "marker_stack": ie.marker_stack,
                    }
                return R.ok({"capture_id": capture_id, "event_id": int(event_id), "action": summary})

            return await asyncio.to_thread(_go)

    @mcp.tool()
    async def get_pipeline_state(capture_id: str, event_id: int) -> dict[str, Any]:
        async with replay_execution():

            def _go() -> dict[str, Any]:
                sess = sessions.get(capture_id)
                if sess is None:
                    return R.err("unknown_capture", capture_id)
                try:
                    sessions.set_frame_event(sess, int(event_id), True)
                    snap = normalize_pipeline_state(sess.controller, sess.structured_file, int(event_id))
                except Exception as ex:
                    return R.err("pipeline_state_failed", str(ex))
                return R.ok(snap)

            return await asyncio.to_thread(_go)

    @mcp.tool()
    async def get_bound_resources(capture_id: str, event_id: int) -> dict[str, Any]:
        async with replay_execution():

            def _go() -> dict[str, Any]:
                sess = sessions.get(capture_id)
                if sess is None:
                    return R.err("unknown_capture", capture_id)
                try:
                    sessions.set_frame_event(sess, int(event_id), True)
                    data = normalize_bound_resources(sess.controller, sess.structured_file, int(event_id))
                except Exception as ex:
                    return R.err("bound_resources_failed", str(ex))
                return R.ok(data)

            return await asyncio.to_thread(_go)

    @mcp.tool()
    async def list_resources(capture_id: str, resource_type: str | None = None, limit: int = 500) -> dict[str, Any]:
        async with replay_execution():

            def _go() -> dict[str, Any]:
                rd = rdutil.get_renderdoc()
                sess = sessions.get(capture_id)
                if sess is None:
                    return R.err("unknown_capture", capture_id)
                rt = _resource_type_from_string(rd, resource_type) if resource_type else None
                out = []
                for res in sess.controller.GetResources():
                    if rt is not None and res.type != rt:
                        continue
                    out.append(
                        {
                            "resource_id": rdutil.rid_str(res.resourceId),
                            "name": res.name,
                            "type": rdutil.enum_name(res.type),
                        }
                    )
                    if len(out) >= min(limit, 5000):
                        break
                return R.ok({"resources": out, "truncated": len(out) >= min(limit, 5000)})

            return await asyncio.to_thread(_go)

    @mcp.tool()
    async def get_resource_usages(capture_id: str, resource_id: str) -> dict[str, Any]:
        async with replay_execution():

            def _go() -> dict[str, Any]:
                rd = rdutil.get_renderdoc()
                sess = sessions.get(capture_id)
                if sess is None:
                    return R.err("unknown_capture", capture_id)
                try:
                    rid = rdutil.parse_resource_id(resource_id)
                except ValueError as ex:
                    return R.err("bad_resource_id", str(ex))
                usages = sess.controller.GetUsage(rid)
                rows = [{"event_id": int(u.eventId), "usage": rdutil.enum_name(u.usage)} for u in usages]
                payload: dict[str, Any] = {
                    "resource_id": resource_id,
                    "usages": rows[:5000],
                    "truncated": len(rows) > 5000,
                }
                rn = rdutil.resource_name_for(sess.controller, rid)
                if rn:
                    payload["resource_name"] = rn
                return R.ok(payload)

            return await asyncio.to_thread(_go)

    @mcp.tool()
    async def analyze_texture(
        capture_id: str,
        resource_id: str,
        event_id: int,
        mip: int = 0,
        slice_index: int = 0,
        sample: int = 0,
    ) -> dict[str, Any]:
        async with replay_execution():

            def _go() -> dict[str, Any]:
                rd = rdutil.get_renderdoc()
                sess = sessions.get(capture_id)
                if sess is None:
                    return R.err("unknown_capture", capture_id)
                try:
                    rid = rdutil.parse_resource_id(resource_id)
                except ValueError as ex:
                    return R.err("bad_resource_id", str(ex))
                try:
                    sessions.set_frame_event(sess, int(event_id), True)
                    tex = find_texture_description(sess.controller, rid)
                    if tex is None:
                        return R.err("not_a_texture", resource_id)
                    sub = rd.Subresource(int(mip), int(slice_index), int(sample))
                    raw = sess.controller.GetTextureData(rid, sub)
                    stats = analyze_texture_bytes(tex, raw)
                    stats["resource_id"] = resource_id
                    rname = rdutil.resource_name_for(sess.controller, rid)
                    if rname:
                        stats["resource_name"] = rname
                    stats["capture_id"] = capture_id
                    stats["event_id"] = int(event_id)
                except Exception as ex:
                    return R.err("analyze_texture_failed", str(ex))
                return R.ok(stats)

            return await asyncio.to_thread(_go)

    @mcp.tool()
    async def save_texture(
        capture_id: str,
        resource_id: str,
        event_id: int,
        path: str,
        dest_type: str = "png",
    ) -> dict[str, Any]:
        async with replay_execution():

            def _go() -> dict[str, Any]:
                rd = rdutil.get_renderdoc()
                sess = sessions.get(capture_id)
                if sess is None:
                    return R.err("unknown_capture", capture_id)
                try:
                    rid = rdutil.parse_resource_id(resource_id)
                except ValueError as ex:
                    return R.err("bad_resource_id", str(ex))
                sessions.set_frame_event(sess, int(event_id), True)
                ts = rd.TextureSave()
                ts.resourceId = rid
                ts.mip = 0
                ts.slice.sliceIndex = 0
                ts.alpha = rd.AlphaMapping.BlendToCheckerboard
                dt = dest_type.lower()
                if dt == "jpg" or dt == "jpeg":
                    ts.destType = rd.FileType.JPG
                elif dt == "hdr":
                    ts.destType = rd.FileType.HDR
                elif dt == "dds":
                    ts.destType = rd.FileType.DDS
                elif dt == "bmp":
                    ts.destType = rd.FileType.BMP
                else:
                    ts.destType = rd.FileType.PNG
                    ts.alpha = rd.AlphaMapping.Preserve
                res = sess.controller.SaveTexture(ts, path)
                ok_res = True
                if isinstance(res, bool):
                    ok_res = res
                elif hasattr(res, "code"):
                    ok_res = res.code == rd.ResultCode.Succeeded
                if not ok_res:
                    return R.err("save_texture_failed", str(res))
                saved: dict[str, Any] = {"path": path, "resource_id": resource_id, "event_id": int(event_id)}
                svname = rdutil.resource_name_for(sess.controller, rid)
                if svname:
                    saved["resource_name"] = svname
                return R.ok(saved)

            return await asyncio.to_thread(_go)

    @mcp.tool()
    async def get_texture_image(
        capture_id: str,
        resource_id: str,
        event_id: int,
        mip: int = 0,
        slice_index: int = 0,
        include_image: bool = True,
        max_dimension: int = 256,
    ) -> dict[str, Any]:
        """Inspect a texture or render target at an event: stats, anomaly description, optional inline PNG.

        For text-only clients set include_image=False. The description field always summarises
        what the texture looks like in plain English.
        """
        async with replay_execution():

            def _go() -> dict[str, Any]:
                rd = rdutil.get_renderdoc()
                sess = sessions.get(capture_id)
                if sess is None:
                    return R.err("unknown_capture", capture_id)
                try:
                    rid = rdutil.parse_resource_id(resource_id)
                except ValueError as ex:
                    return R.err("bad_resource_id", str(ex))
                try:
                    sessions.set_frame_event(sess, int(event_id), True)
                    tex = find_texture_description(sess.controller, rid)
                    if tex is None:
                        return R.err("not_a_texture", resource_id)
                    sub = rd.Subresource(int(mip), int(slice_index), 0)
                    raw = sess.controller.GetTextureData(rid, sub)
                    stats = analyze_texture_bytes(tex, raw)
                    anomalies = detect_texture_anomalies(stats)
                    description = describe_texture(stats, anomalies)
                    out: dict[str, Any] = {
                        "resource_id": resource_id,
                        "width": int(tex.width),
                        "height": int(tex.height),
                        "format": rdutil.enum_name(tex.format.type) if tex.format else "",
                        "event_id": int(event_id),
                        "stats": stats,
                        "anomalies": anomalies,
                        "description": description,
                    }
                    rname = rdutil.resource_name_for(sess.controller, rid)
                    if rname:
                        out["resource_name"] = rname
                    if include_image:
                        png_bytes = save_texture_as_png_bytes(
                            sess.controller, rd, rid, int(mip), int(slice_index), int(max_dimension)
                        )
                        if png_bytes:
                            out["image_base64"] = base64.b64encode(png_bytes).decode("ascii")
                            out["image_format"] = "png"
                        else:
                            out["image_unavailable"] = True
                except Exception as ex:
                    return R.err("get_texture_image_failed", str(ex))
                return R.ok(out)

            return await asyncio.to_thread(_go)

    @mcp.tool()
    async def read_buffer(
        capture_id: str,
        resource_id: str,
        offset: int,
        length: int,
        event_id: int,
        max_bytes: int = 65536,
    ) -> dict[str, Any]:
        async with replay_execution():

            def _go() -> dict[str, Any]:
                sess = sessions.get(capture_id)
                if sess is None:
                    return R.err("unknown_capture", capture_id)
                try:
                    rid = rdutil.parse_resource_id(resource_id)
                except ValueError as ex:
                    return R.err("bad_resource_id", str(ex))
                sessions.set_frame_event(sess, int(event_id), True)
                ln = min(int(length), int(max_bytes), 4 * 1024 * 1024)
                data = rdutil.controller_get_buffer_data(sess.controller, rid, int(offset), ln)
                preview = data[:512]
                buf_out: dict[str, Any] = {
                    "resource_id": resource_id,
                    "offset": int(offset),
                    "requested_length": ln,
                    "byte_length": len(data),
                    "hex_preview": preview.hex(),
                    "base64": base64.b64encode(data).decode("ascii"),
                }
                bfname = rdutil.resource_name_for(sess.controller, rid)
                if bfname:
                    buf_out["resource_name"] = bfname
                return R.ok(buf_out)

            return await asyncio.to_thread(_go)

    @mcp.tool()
    async def pixel_history(
        capture_id: str,
        resource_id: str,
        x: int,
        y: int,
        event_id: int | None = None,
        mip: int = 0,
        slice_index: int = 0,
        sample_index: int = 0,
        type_cast: str = "Typeless",
    ) -> dict[str, Any]:
        async with replay_execution():

            def _go() -> dict[str, Any]:
                rd = rdutil.get_renderdoc()
                sess = sessions.get(capture_id)
                if sess is None:
                    return R.err("unknown_capture", capture_id)
                try:
                    rid = rdutil.parse_resource_id(resource_id)
                except ValueError as ex:
                    return R.err("bad_resource_id", str(ex))
                if event_id is not None:
                    sessions.set_frame_event(sess, int(event_id), True)
                sub = rd.Subresource(int(mip), int(slice_index), int(sample_index))
                cast = rd.CompType.Typeless
                tc = type_cast.strip()
                if hasattr(rd.CompType, tc):
                    cast = getattr(rd.CompType, tc)
                hist = sess.controller.PixelHistory(rid, int(x), int(y), sub, cast)
                norm = normalize_pixel_history(hist)
                norm["capture_id"] = capture_id
                norm["resource_id"] = resource_id
                pxname = rdutil.resource_name_for(sess.controller, rid)
                if pxname:
                    norm["resource_name"] = pxname
                norm["coordinates"] = {"x": int(x), "y": int(y)}
                return R.ok(norm)

            return await asyncio.to_thread(_go)

    @mcp.tool()
    async def diff_pipeline_state(
        good_capture_id: str,
        good_event_id: int,
        bad_capture_id: str,
        bad_event_id: int,
    ) -> dict[str, Any]:
        async with replay_execution():

            def _go() -> dict[str, Any]:
                g = sessions.get(good_capture_id)
                b = sessions.get(bad_capture_id)
                if g is None or b is None:
                    return R.err("unknown_capture", "good or bad capture_id invalid")
                try:
                    sessions.set_frame_event(g, int(good_event_id), True)
                    sg = normalize_pipeline_state(g.controller, g.structured_file, int(good_event_id))
                    sessions.set_frame_event(b, int(bad_event_id), True)
                    sb = normalize_pipeline_state(b.controller, b.structured_file, int(bad_event_id))
                    diff = diff_pipeline_snapshots(sg, sb)
                    diff["good"] = {"capture_id": good_capture_id, "event_id": int(good_event_id)}
                    diff["bad"] = {"capture_id": bad_capture_id, "event_id": int(bad_event_id)}
                except Exception as ex:
                    return R.err("diff_failed", str(ex))
                return R.ok(diff)

            return await asyncio.to_thread(_go)

    @mcp.tool()
    async def diff_texture_stats(
        good_capture_id: str,
        good_event_id: int,
        good_resource_id: str,
        bad_capture_id: str,
        bad_event_id: int,
        bad_resource_id: str,
        mip: int = 0,
        slice_index: int = 0,
        sample: int = 0,
    ) -> dict[str, Any]:
        async with replay_execution():

            def _go() -> dict[str, Any]:
                rd = rdutil.get_renderdoc()
                g = sessions.get(good_capture_id)
                b = sessions.get(bad_capture_id)
                if g is None or b is None:
                    return R.err("unknown_capture", "good or bad capture_id invalid")
                try:
                    gr = rdutil.parse_resource_id(good_resource_id)
                    br = rdutil.parse_resource_id(bad_resource_id)
                except ValueError as ex:
                    return R.err("bad_resource_id", str(ex))

                def analyze(sess: Any, eid: int, rid: Any, rid_s: str) -> dict[str, Any]:
                    sessions.set_frame_event(sess, int(eid), True)
                    tex = find_texture_description(sess.controller, rid)
                    if tex is None:
                        return {"error": "not_a_texture", "resource_id": rid_s}
                    sub = rd.Subresource(int(mip), int(slice_index), int(sample))
                    raw = sess.controller.GetTextureData(rid, sub)
                    return analyze_texture_bytes(tex, raw)

                ga = analyze(g, good_event_id, gr, good_resource_id)
                ba = analyze(b, bad_event_id, br, bad_resource_id)
                return R.ok(diff_texture_analysis(ga, ba))

            return await asyncio.to_thread(_go)

    @mcp.tool()
    async def decode_mesh_inputs(capture_id: str, event_id: int, preview_vertices: int = 8) -> dict[str, Any]:
        async with replay_execution():

            def _go() -> dict[str, Any]:
                sess = sessions.get(capture_id)
                if sess is None:
                    return R.err("unknown_capture", capture_id)
                try:
                    sessions.set_frame_event(sess, int(event_id), True)
                    data = decode_mesh_inputs_core(sess.controller, sess.structured_file, int(event_id), preview_vertices)
                except Exception as ex:
                    return R.err("decode_mesh_failed", str(ex))
                if isinstance(data, dict):
                    if data.get("error"):
                        return R.err(str(data.get("error")), str(data.get("message", data)))
                    if data.get("ok") is False:
                        return R.err("decode_mesh_failed", str(data.get("error", "")))
                return R.ok(data)

            return await asyncio.to_thread(_go)

    @mcp.tool()
    async def get_shader(
        capture_id: str,
        event_id: int,
        stage: str,
        include_disassembly: bool = False,
        include_reflection: bool = False,
        max_disassembly_chars: int = 12000,
    ) -> dict[str, Any]:
        async with replay_execution():

            def _go() -> dict[str, Any]:
                rd = rdutil.get_renderdoc()
                sess = sessions.get(capture_id)
                if sess is None:
                    return R.err("unknown_capture", capture_id)
                st = _stage_from_string(rd, stage)
                if st is None:
                    return R.err("bad_stage", stage)
                sessions.set_frame_event(sess, int(event_id), True)
                pipe = sess.controller.GetPipelineState()
                refl = pipe.GetShaderReflection(st)
                if refl is None:
                    return R.ok({"bound": False, "stage": stage})
                out: dict[str, Any] = {
                    "bound": True,
                    "stage": stage,
                }
                rdutil.enrich_resource_dict(sess.controller, out, refl.resourceId)
                if include_reflection:
                    out["reflection"] = serialize_shader_reflection_summary(refl, sess.controller)
                if include_disassembly:
                    pipe_obj = pipe.GetGraphicsPipelineObject()
                    if pipe_obj == rd.ResourceId.Null():
                        pipe_obj = pipe.GetComputePipelineObject()
                    targets = sess.controller.GetDisassemblyTargets(True)
                    target = targets[0] if targets else ""
                    text = sess.controller.DisassembleShader(pipe_obj, refl, target)
                    if len(text) > max_disassembly_chars:
                        out["disassembly"] = text[:max_disassembly_chars]
                        out["disassembly_truncated"] = True
                    else:
                        out["disassembly"] = text
                return R.ok(out)

            return await asyncio.to_thread(_go)

    @mcp.tool()
    async def get_shader_reflection(capture_id: str, event_id: int, stage: str) -> dict[str, Any]:
        async with replay_execution():

            def _go() -> dict[str, Any]:
                rd = rdutil.get_renderdoc()
                sess = sessions.get(capture_id)
                if sess is None:
                    return R.err("unknown_capture", capture_id)
                st = _stage_from_string(rd, stage)
                if st is None:
                    return R.err("bad_stage", stage)
                sessions.set_frame_event(sess, int(event_id), True)
                pipe = sess.controller.GetPipelineState()
                refl = pipe.GetShaderReflection(st)
                if refl is None:
                    return R.ok({"bound": False, "stage": stage})
                sref: dict[str, Any] = {"bound": True, "stage": stage}
                rdutil.enrich_resource_dict(sess.controller, sref, refl.resourceId)
                sref["reflection"] = serialize_shader_reflection_summary(refl, sess.controller)
                return R.ok(sref)

            return await asyncio.to_thread(_go)

    @mcp.tool()
    async def analyze_draw_visibility(capture_id: str, event_id: int) -> dict[str, Any]:
        async with replay_execution():

            def _go() -> dict[str, Any]:
                sess = sessions.get(capture_id)
                if sess is None:
                    return R.err("unknown_capture", capture_id)
                try:
                    sessions.set_frame_event(sess, int(event_id), True)
                    snap = normalize_pipeline_state(sess.controller, sess.structured_file, int(event_id))
                    vis = draw_visibility_analysis(sess.controller, sess.structured_file, snap, int(event_id))
                except Exception as ex:
                    return R.err("visibility_failed", str(ex))
                return R.ok(vis, evidence=vis.get("evidence"))

            return await asyncio.to_thread(_go)

    return mcp


def enum_api(rd: Any, controller: Any) -> str:
    try:
        if hasattr(controller, "GetAPIProperties"):
            props = controller.GetAPIProperties()
            pt = getattr(props, "pipelineType", None)
            if pt is not None:
                return str(pt)
    except Exception:
        pass
    return "Unknown"


def main(host: str = "127.0.0.1", port: int = 8765, transport: str = "streamable-http") -> None:
    app = build_mcp()
    if transport != "stdio":
        app.settings.host = host
        app.settings.port = port
    t = cast(Literal["stdio", "sse", "streamable-http"], transport)
    app.run(transport=t)
