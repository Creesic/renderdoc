"""FastMCP server: RenderDoc replay tools on HTTP `/mcp`."""

from __future__ import annotations

import asyncio
import base64
import concurrent.futures
import difflib
from contextlib import asynccontextmanager
from typing import Any, Literal, cast

from mcp.server.fastmcp import FastMCP
from mcp.server.transport_security import TransportSecuritySettings

from renderdoc_mcp import responses as R
from renderdoc_mcp import rdutil
from renderdoc_mcp.analysis import (
    analyze_texture_bytes,
    build_frame_overview,
    classify_producer_action,
    copy_hop_is_safe,
    deep_diff,
    diff_pipeline_snapshots,
    diff_texture_analysis,
    draw_visibility_analysis,
    find_texture_description,
    mip_dims,
    normalize_pixel_history,
    select_provenance_producer,
)
from renderdoc_mcp.imaging import (
    detect_texture_anomalies,
    describe_texture,
    save_texture_as_png_bytes,
)
from renderdoc_mcp.mesh_decode import (
    decode_mesh_inputs as decode_mesh_inputs_core,
    decode_post_vs_outputs as decode_post_vs_outputs_core,
)
from renderdoc_mcp.draw_matching import draw_shape_key
from renderdoc_mcp.serialize import (
    build_draw_state_row,
    normalize_bound_resources,
    normalize_pipeline_state,
    serialize_descriptor,
    serialize_sampler_descriptor,
    serialize_shader_reflection_summary,
)
from renderdoc_mcp.cbuffer import decode_cb_bytes, detect_variable_anomalies
from renderdoc_mcp.session import CaptureSessionManager, filter_events, find_action
from renderdoc_mcp import shader_debug
from renderdoc_mcp.isolated_replay import (
    DEFAULT_DRAW_MATCHING_TIMEOUT_SECONDS,
    DEFAULT_SHADER_DEBUG_TIMEOUT_SECONDS,
    IsolatedReplayError,
    IsolatedReplayTimeout,
    clamp_shader_debug_timeout,
    run_draw_matching_worker,
    run_shader_debug_worker,
)
from renderdoc_mcp.structured import serialize_chunk

import logging
_log = logging.getLogger("renderdoc_mcp.server")

replay_lock = asyncio.Lock()
sessions = CaptureSessionManager()

_replay_init_lock = asyncio.Lock()
_replay_initialized = False

# Single-threaded executor: ReplayController captures m_ThreadID at construction and
# CHECK_REPLAY_THREAD() enforces that every method runs on that same thread.
# Using max_workers=1 guarantees InitialiseReplay and all replay-backed tool calls
# land on the same OS thread.
_replay_executor = concurrent.futures.ThreadPoolExecutor(
    max_workers=1, thread_name_prefix="renderdoc-replay"
)


_WRITE_USAGE_NAMES = {
    "ColorTarget",
    "ColourTarget",
    "DepthStencilTarget",
    "Clear",
    "Copy",
    "CopyDst",
    "Resolve",
    "ResolveDst",
    "GenMips",
    "StreamOut",
    "CPUWrite",
    "Discard",
    "VertexBufferWrite",
    "IndexBufferWrite",
    "Indirect",
}

_READ_USAGE_NAMES = {
    "All_Constants",
    "VS_Resource",
    "VS_Constants",
    "HS_Resource",
    "HS_Constants",
    "DS_Resource",
    "DS_Constants",
    "GS_Resource",
    "GS_Constants",
    "PS_Resource",
    "PS_Constants",
    "CS_Resource",
    "CS_Constants",
    "MS_Resource",
    "MS_Constants",
    "TS_Resource",
    "TS_Constants",
    "All_Resource",
    "InputTarget",
    "VertexBuffer",
    "IndexBuffer",
    "CopySrc",
    "ResolveSrc",
    "ConstantBuffer",
}


def _event_summary(sess: Any, event_id: int) -> dict[str, Any]:
    ie = sess.events_by_id.get(int(event_id))
    if ie is None:
        return {"event_id": int(event_id)}
    return {
        "event_id": ie.event_id,
        "name": ie.name,
        "flags": ie.flags_names,
        "marker_stack": ie.marker_stack,
        "draw": {
            "num_vertices": ie.num_vertices,
            "num_instances": ie.num_instances,
            "num_indices": ie.num_indices,
        },
    }


def _marker_path_for_event(ie: Any, include_event_name: bool = True) -> str:
    names = [str(m.get("name", "")) for m in ie.marker_stack if str(m.get("name", "")).strip()]
    if include_event_name:
        names.append(str(ie.name))
    return " / ".join(n for n in names if n)


def _usage_rows(controller: Any, rid: Any) -> list[dict[str, Any]]:
    return [
        {"event_id": int(u.eventId), "usage": rdutil.enum_name(u.usage)}
        for u in controller.GetUsage(rid)
    ]


def _is_write_usage(name: str) -> bool:
    return name in _WRITE_USAGE_NAMES or "Write" in name or "RWResource" in name or name.endswith("Dst")


def _is_read_usage(name: str) -> bool:
    return name in _READ_USAGE_NAMES or name.endswith("Resource") or name.endswith("Constants") or name.endswith("Src")


def _changed_ranges(a: bytes, b: bytes, base_offset: int, max_ranges: int) -> tuple[list[dict[str, int]], bool]:
    ranges: list[dict[str, int]] = []
    i = 0
    ln = min(len(a), len(b))
    while i < ln:
        if a[i] == b[i]:
            i += 1
            continue
        start = i
        while i < ln and a[i] != b[i]:
            i += 1
        if len(ranges) < max_ranges:
            ranges.append({"offset": base_offset + start, "length": i - start})
        else:
            return ranges, True
    if len(a) != len(b):
        if len(ranges) < max_ranges:
            ranges.append({"offset": base_offset + ln, "length": abs(len(a) - len(b))})
            return ranges, False
        return ranges, True
    return ranges, False


def _target_resource_ids(targets: dict[str, Any] | None) -> set[str]:
    ids: set[str] = set()
    if not targets:
        return ids
    for target in targets.get("color_targets") or []:
        rid = target.get("resource_id")
        if rid and rid != "Null":
            ids.add(str(rid))
    for key in ("depth_target", "stencil_target"):
        target = targets.get(key)
        if target:
            rid = target.get("resource_id")
            if rid and rid != "Null":
                ids.add(str(rid))
    return ids


def _capability_error(sess: Any, feature: str) -> dict[str, Any] | None:
    """Return an MCP error only when the replay driver explicitly reports a feature unavailable.

    Older/non-Metal drivers may not publish the capability array yet, so absence preserves their
    existing behavior. An explicit unavailable entry is authoritative and must stop the tool
    before it invokes an unsupported replay operation.
    """
    capability = sess.capability(feature) if hasattr(sess, "capability") else None
    if capability is None or bool(capability.get("available", False)):
        return None
    reason = str(capability.get("reason") or "This replay driver does not support the feature")
    return R.err(
        "unsupported_capability",
        reason,
        {
            "capture_id": getattr(sess, "capture_id", ""),
            "api": getattr(sess, "api_name", "Unknown"),
            "feature": feature,
            "available": False,
        },
    )


async def ensure_replay_initialized() -> None:
    """Load pymodules and InitialiseReplay on first tool use — keeps MCP handshake instant."""
    global _replay_initialized
    if _replay_initialized:
        return
    async with _replay_init_lock:
        if _replay_initialized:
            return

        def _init():
            _log.info("InitialiseReplay: importing renderdoc module")
            rd = rdutil.get_renderdoc()
            _log.info("InitialiseReplay: calling rd.InitialiseReplay")
            rd.InitialiseReplay(rd.GlobalEnvironment(), [])
            _log.info("InitialiseReplay: done")

        await asyncio.get_running_loop().run_in_executor(_replay_executor, _init)
        _replay_initialized = True


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


def _select_constant_block(cb_blocks: list, slot: int) -> tuple[Any | None, int]:
    """Match a register/bind slot to a reflection constant block.

    Returns (block, reflection_index). The index is the block's position in the
    constantBlocks array — the value PipeState.GetConstantBlock takes — which only
    coincides with the bind number when registers are dense from 0.
    """
    for i, block in enumerate(cb_blocks):
        if int(getattr(block, "fixedBindNumber", -1)) == int(slot):
            return block, i
    if 0 <= int(slot) < len(cb_blocks):
        return cb_blocks[int(slot)], int(slot)
    return None, -1


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
    # FastMCP runs this lifespan for Streamable HTTP session lifetime. Clients
    # frequently terminate sessions while the process should keep serving MCP, so
    # replay teardown must stay on explicit tool calls or process exit.
    yield


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
                _log.info("open_capture: path=%s", path)
                try:
                    sess = sessions.open_capture(path)
                except Exception as ex:
                    _log.exception("open_capture: sessions.open_capture raised")
                    return R.err("open_failed", str(ex))
                _log.info("open_capture: success capture_id=%s driver=%s", sess.capture_id, sess.driver_name)
                rd = rdutil.get_renderdoc()
                return R.ok(
                    {
                        "capture_id": sess.capture_id,
                        "path": sess.path,
                        "driver": sess.driver_name or rdutil.replay_driver_hint(sess.controller),
                        "api": sess.api_name or enum_api(rd, sess.controller),
                        "degraded": sess.degraded,
                        "capabilities": sess.capabilities,
                        "event_count": len(sess.events_ordered),
                    }
                )

            return await asyncio.get_running_loop().run_in_executor(_replay_executor, _go)

    @mcp.tool()
    async def get_capture_capabilities(capture_id: str) -> dict[str, Any]:
        """Return the replay driver's explicit per-feature availability and reasons.

        Call this before expensive or advanced inspection. Metal Apple GPU Trace sessions are
        intentionally read-only and report unsupported operations here instead of fabricating
        empty pipeline, texture, pixel-history, mesh, or shader-debug results.
        """
        async with replay_execution():

            def _go() -> dict[str, Any]:
                sess = sessions.get(capture_id)
                if sess is None:
                    return R.err("unknown_capture", capture_id)
                return R.ok(
                    {
                        "capture_id": capture_id,
                        "driver": sess.driver_name,
                        "api": sess.api_name,
                        "degraded": sess.degraded,
                        "capabilities": sess.capabilities,
                    }
                )

            return await asyncio.get_running_loop().run_in_executor(_replay_executor, _go)

    @mcp.tool()
    async def close_capture(capture_id: str) -> dict[str, Any]:
        """Close a capture session and release its replay device.

        `closed` reflects whether the underlying device actually reported a clean shutdown --
        previously this always claimed success even when it silently failed, which could mask a
        leaked GPU device/resources (see renderdoc-mcp/TODO.md #10 on multi-capture stability).
        """
        async with replay_execution():

            def _go() -> dict[str, Any]:
                shutdown_ok = sessions.close_capture(capture_id)
                if shutdown_ok is None:
                    return R.err("unknown_capture", capture_id)
                out: dict[str, Any] = {"capture_id": capture_id, "closed": bool(shutdown_ok)}
                if not shutdown_ok:
                    out["warning"] = "Device Shutdown() reported failure; its GPU resources may not have been released"
                return R.ok(out)

            return await asyncio.get_running_loop().run_in_executor(_replay_executor, _go)

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

            return await asyncio.get_running_loop().run_in_executor(_replay_executor, _go)

    @mcp.tool()
    async def search_marker_paths(
        capture_id: str,
        path_contains: str | None = None,
        name_contains: str | None = None,
        require_flags: list[str] | None = None,
        include_actions: bool = True,
        limit: int = 100,
    ) -> dict[str, Any]:
        """Search nested marker paths, not just flat event names.

        Returns each matching event with `marker_path` built from PushMarker ancestry plus the
        event name. This is intended for jumping straight to pass paths like "EDRAM / Resolve"
        in deeper captures where a flat name search is too noisy.
        """
        async with replay_execution():

            def _go() -> dict[str, Any]:
                sess = sessions.get(capture_id)
                if sess is None:
                    return R.err("unknown_capture", capture_id)

                path_q = (path_contains or "").lower()
                name_q = (name_contains or "").lower()
                wanted_flags = {f.lower() for f in (require_flags or [])}
                rows: list[dict[str, Any]] = []
                matched = 0

                for eid in sess.events_ordered:
                    ie = sess.events_by_id[eid]
                    if not include_actions and "Drawcall" in ie.flags_names:
                        continue
                    if wanted_flags and not wanted_flags.issubset({f.lower() for f in ie.flags_names}):
                        continue
                    path = _marker_path_for_event(ie)
                    if path_q and path_q not in path.lower():
                        continue
                    if name_q and name_q not in ie.name.lower():
                        continue
                    matched += 1
                    if len(rows) >= max(1, min(int(limit), 1000)):
                        continue
                    row = _event_summary(sess, eid)
                    row["marker_path"] = path
                    rows.append(row)

                return R.ok(
                    {
                        "capture_id": capture_id,
                        "matches": rows,
                        "count": len(rows),
                        "total_matches": matched,
                        "truncated": matched > len(rows),
                    }
                )

            return await asyncio.get_running_loop().run_in_executor(_replay_executor, _go)

    @mcp.tool()
    async def set_event(capture_id: str, event_id: int, force_complete_replay: bool = False) -> dict[str, Any]:
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

            return await asyncio.get_running_loop().run_in_executor(_replay_executor, _go)

    @mcp.tool()
    async def get_event_details(capture_id: str, event_id: int) -> dict[str, Any]:
        """Return the actual API call arguments for an event, from the capture's structured file.

        Critically useful for calls get_pipeline_state/get_resource_usages can't explain, like
        CopyTextureRegion/CopyBufferRegion -- this returns the real src/dst resources,
        subresources, and regions directly instead of having to infer them by cross-referencing
        usage lists. Works for any event, not just draws: state-setting calls (e.g.
        IASetVertexBuffers) have their own chunk too. Doesn't require SetFrameEvent -- structured
        data is static per-capture, not tied to replay position.
        """
        async with replay_execution():

            def _go() -> dict[str, Any]:
                rd = rdutil.get_renderdoc()
                sess = sessions.get(capture_id)
                if sess is None:
                    return R.err("unknown_capture", capture_id)
                chunk_index = sess.chunk_index_by_event.get(int(event_id))
                if chunk_index is None:
                    return R.err(
                        "no_chunk_for_event", "No structured-file chunk found for event {}".format(event_id)
                    )
                chunks = sess.structured_file.chunks
                if chunk_index < 0 or chunk_index >= len(chunks):
                    return R.err(
                        "chunk_index_out_of_range",
                        "chunk index {} out of range (0..{})".format(chunk_index, len(chunks)),
                    )
                try:
                    data = serialize_chunk(rd, chunks[chunk_index], sess.controller)
                except Exception as ex:
                    return R.err("get_event_details_failed", str(ex))
                data["event_id"] = int(event_id)
                data["chunk_index"] = int(chunk_index)
                return R.ok(data)

            return await asyncio.get_running_loop().run_in_executor(_replay_executor, _go)

    @mcp.tool()
    async def get_pipeline_state(capture_id: str, event_id: int) -> dict[str, Any]:
        async with replay_execution():

            def _go() -> dict[str, Any]:
                sess = sessions.get(capture_id)
                if sess is None:
                    return R.err("unknown_capture", capture_id)
                unsupported = _capability_error(sess, "PipelineState")
                if unsupported:
                    return unsupported
                try:
                    sessions.set_frame_event(sess, int(event_id))
                    snap = normalize_pipeline_state(sess.controller, sess.structured_file, int(event_id))
                except Exception as ex:
                    return R.err("pipeline_state_failed", str(ex))
                return R.ok(snap)

            return await asyncio.get_running_loop().run_in_executor(_replay_executor, _go)

    @mcp.tool()
    async def get_bound_resources(capture_id: str, event_id: int) -> dict[str, Any]:
        async with replay_execution():

            def _go() -> dict[str, Any]:
                sess = sessions.get(capture_id)
                if sess is None:
                    return R.err("unknown_capture", capture_id)
                unsupported = _capability_error(sess, "PipelineState")
                if unsupported:
                    return unsupported
                try:
                    sessions.set_frame_event(sess, int(event_id))
                    data = normalize_bound_resources(sess.controller, sess.structured_file, int(event_id))
                except Exception as ex:
                    return R.err("bound_resources_failed", str(ex))
                return R.ok(data)

            return await asyncio.get_running_loop().run_in_executor(_replay_executor, _go)

    @mcp.tool()
    async def get_descriptor(
        capture_id: str,
        event_id: int,
        heap_index: int,
        descriptor_store: str | None = None,
        is_sampler: bool = False,
        count: int = 1,
    ) -> dict[str, Any]:
        """Resolve descriptor-store slot(s) (e.g. a D3D12 shader-visible heap index, or a Vulkan
        descriptor-buffer offset) to the actual bound resource(s) via GetDescriptors().

        Answers "what resource is heap[N]?" for bindless engines that index into a descriptor
        store from a constant-buffer value. If descriptor_store is omitted: with exactly one
        descriptor store it's used automatically; with multiple and is_sampler=false, the store
        with the most descriptors is auto-picked (sampler heaps are capped small, e.g. 2048 on
        D3D12, so the largest store is almost always the shader-visible CBV/SRV/UAV heap) --
        the response then includes descriptor_store_auto_selected/_candidates so a caller who
        wanted a different store knows to pass descriptor_store explicitly. With multiple stores
        and is_sampler=true, the available stores are returned for the caller to pick from.
        """
        async with replay_execution():

            def _go() -> dict[str, Any]:
                rd = rdutil.get_renderdoc()
                sess = sessions.get(capture_id)
                if sess is None:
                    return R.err("unknown_capture", capture_id)
                sessions.set_frame_event(sess, int(event_id))

                stores = list(sess.controller.GetDescriptorStores())
                if not stores:
                    return R.err("no_descriptor_stores", "This capture has no descriptor stores")

                store = None
                auto_selected = False
                if descriptor_store:
                    try:
                        want = rdutil.parse_resource_id(descriptor_store)
                    except ValueError as ex:
                        return R.err("bad_resource_id", str(ex))
                    for s in stores:
                        if s.resourceId == want:
                            store = s
                            break
                    if store is None:
                        return R.err("unknown_descriptor_store", descriptor_store)
                elif len(stores) == 1:
                    store = stores[0]
                elif not is_sampler:
                    # Sampler heaps are capped small (e.g. 2048 on D3D12) while the shader-visible
                    # CBV/SRV/UAV heap in a bindless engine is typically far larger (tens of
                    # thousands+), so the largest store is almost always the one being asked
                    # about here. Note it was auto-picked so a caller who wanted a different one
                    # (e.g. a non-shader-visible staging heap) knows to pass descriptor_store.
                    store = max(stores, key=lambda s: int(s.descriptorCount))
                    auto_selected = True
                else:
                    return R.err(
                        "ambiguous_descriptor_store",
                        "Multiple descriptor stores exist; pass descriptor_store to pick one",
                        detail=[
                            {
                                "resource_id": rdutil.rid_str(s.resourceId),
                                "descriptor_count": int(s.descriptorCount),
                                "descriptor_byte_size": int(s.descriptorByteSize),
                            }
                            for s in stores
                        ],
                    )

                n = max(1, int(count))
                if store.descriptorCount and int(heap_index) + n > int(store.descriptorCount):
                    return R.err(
                        "index_out_of_range",
                        "heap_index {} + count {} exceeds store descriptor_count {}".format(
                            heap_index, n, store.descriptorCount
                        ),
                    )

                rng = rd.DescriptorRange()
                rng.offset = int(store.firstDescriptorOffset) + int(heap_index) * int(
                    store.descriptorByteSize
                )
                rng.descriptorSize = int(store.descriptorByteSize)
                rng.count = n
                rng.type = rd.DescriptorType.Unknown

                try:
                    if is_sampler:
                        descs = sess.controller.GetSamplerDescriptors(store.resourceId, [rng])
                        rows = [serialize_sampler_descriptor(sess.controller, d) for d in descs]
                    else:
                        descs = sess.controller.GetDescriptors(store.resourceId, [rng])
                        rows = [serialize_descriptor(sess.controller, d) for d in descs]
                except Exception as ex:
                    return R.err("get_descriptor_failed", str(ex))

                out: dict[str, Any] = {
                    "capture_id": capture_id,
                    "event_id": int(event_id),
                    "descriptor_store": rdutil.rid_str(store.resourceId),
                    "heap_index": int(heap_index),
                    "count": n,
                    "is_sampler": bool(is_sampler),
                    "descriptors": rows,
                }
                if auto_selected:
                    out["descriptor_store_auto_selected"] = True
                    out["descriptor_store_candidates"] = [
                        {"resource_id": rdutil.rid_str(s.resourceId), "descriptor_count": int(s.descriptorCount)}
                        for s in stores
                    ]
                return R.ok(out)

            return await asyncio.get_running_loop().run_in_executor(_replay_executor, _go)

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

            return await asyncio.get_running_loop().run_in_executor(_replay_executor, _go)

    @mcp.tool()
    async def get_resource_usages(capture_id: str, resource_id: str) -> dict[str, Any]:
        async with replay_execution():

            def _go() -> dict[str, Any]:
                rd = rdutil.get_renderdoc()
                sess = sessions.get(capture_id)
                if sess is None:
                    return R.err("unknown_capture", capture_id)
                unsupported = _capability_error(sess, "PipelineState")
                if unsupported:
                    return unsupported
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

            return await asyncio.get_running_loop().run_in_executor(_replay_executor, _go)

    @mcp.tool()
    async def trace_resource(
        capture_id: str,
        resource_id: str,
        event_id: int | None = None,
        direction: str = "backward",
        max_steps: int = 4,
        include_bound_resources: bool = True,
        include_event_details: bool = True,
    ) -> dict[str, Any]:
        """Trace a resource's producer/consumer chain through GetUsage().

        `direction="backward"` finds the latest writes before `event_id` (or before the end of
        the frame), then summarizes what each producer draw/copy read. This collapses the common
        manual chain of get_resource_usages -> get_event_details -> get_pipeline_state when
        tracking EDRAM/resolve resources. `direction="forward"` reports later consumers instead.
        """
        async with replay_execution():

            def _event_details(sess: Any, eid: int) -> dict[str, Any] | None:
                if not include_event_details:
                    return None
                chunk_index = sess.chunk_index_by_event.get(int(eid))
                if chunk_index is None:
                    return None
                chunks = sess.structured_file.chunks
                if chunk_index < 0 or chunk_index >= len(chunks):
                    return None
                try:
                    return serialize_chunk(rdutil.get_renderdoc(), chunks[chunk_index], sess.controller)
                except Exception as ex:
                    return {"error": str(ex)}

            def _producer_row(sess: Any, rid_s: str, usage: dict[str, Any]) -> dict[str, Any]:
                eid = int(usage["event_id"])
                row = _event_summary(sess, eid)
                row["resource_usage"] = usage["usage"]
                row["resource_id"] = rid_s
                try:
                    sessions.set_frame_event(sess, eid)
                    if include_bound_resources:
                        bound = normalize_bound_resources(sess.controller, sess.structured_file, eid)
                        targets = bound.get("targets")
                        write_ids = _target_resource_ids(targets)
                        row["targets"] = targets
                        row["write_targets"] = [
                            r for r in (bound.get("merged_resources", []) or [])
                            if str(r.get("resource_id", "")) in write_ids
                        ][:64]
                        row["inputs"] = [
                            r for r in (bound.get("merged_resources", []) or [])
                            if str(r.get("resource_id", "")) not in write_ids
                        ][:200]
                except Exception as ex:
                    row["bound_resources_error"] = str(ex)
                details = _event_details(sess, eid)
                if details is not None:
                    row["event_details"] = details
                return row

            def _go() -> dict[str, Any]:
                sess = sessions.get(capture_id)
                if sess is None:
                    return R.err("unknown_capture", capture_id)
                try:
                    rid = rdutil.parse_resource_id(resource_id)
                except ValueError as ex:
                    return R.err("bad_resource_id", str(ex))

                usages = _usage_rows(sess.controller, rid)
                target_eid = int(event_id) if event_id is not None else (
                    max((u["event_id"] for u in usages), default=0) + 1
                )
                direction_norm = direction.strip().lower()
                max_n = max(1, min(int(max_steps), 32))

                if direction_norm in ("backward", "back", "producer", "producers"):
                    candidates = [
                        u for u in usages if u["event_id"] <= target_eid and _is_write_usage(u["usage"])
                    ]
                    selected = list(reversed(candidates))[:max_n]
                    rows = [_producer_row(sess, resource_id, u) for u in selected]
                    mode = "backward"
                elif direction_norm in ("forward", "fwd", "consumer", "consumers"):
                    candidates = [
                        u for u in usages if u["event_id"] >= target_eid and _is_read_usage(u["usage"])
                    ]
                    selected = candidates[:max_n]
                    rows = []
                    for u in selected:
                        row = _event_summary(sess, int(u["event_id"]))
                        row["resource_usage"] = u["usage"]
                        details = _event_details(sess, int(u["event_id"]))
                        if details is not None:
                            row["event_details"] = details
                        rows.append(row)
                    mode = "forward"
                else:
                    return R.err("bad_direction", "Use backward or forward")

                payload: dict[str, Any] = {
                    "capture_id": capture_id,
                    "resource_id": resource_id,
                    "event_id": target_eid,
                    "direction": mode,
                    "steps": rows,
                    "step_count": len(rows),
                    "all_usages_near_event": [
                        u for u in usages if abs(int(u["event_id"]) - target_eid) <= 8
                    ][:100],
                    "truncated": len(candidates) > len(selected),
                }
                rn = rdutil.resource_name_for(sess.controller, rid)
                if rn:
                    payload["resource_name"] = rn
                return R.ok(payload)

            return await asyncio.get_running_loop().run_in_executor(_replay_executor, _go)

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
                unsupported = _capability_error(sess, "TextureFetch")
                if unsupported:
                    return unsupported
                try:
                    rid = rdutil.parse_resource_id(resource_id)
                except ValueError as ex:
                    return R.err("bad_resource_id", str(ex))
                try:
                    sessions.set_frame_event(sess, int(event_id))
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

            return await asyncio.get_running_loop().run_in_executor(_replay_executor, _go)

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
                unsupported = _capability_error(sess, "TextureFetch")
                if unsupported:
                    return unsupported
                try:
                    rid = rdutil.parse_resource_id(resource_id)
                except ValueError as ex:
                    return R.err("bad_resource_id", str(ex))
                sessions.set_frame_event(sess, int(event_id))
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

            return await asyncio.get_running_loop().run_in_executor(_replay_executor, _go)

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
                unsupported = _capability_error(sess, "TextureFetch")
                if unsupported:
                    return unsupported
                try:
                    rid = rdutil.parse_resource_id(resource_id)
                except ValueError as ex:
                    return R.err("bad_resource_id", str(ex))
                try:
                    sessions.set_frame_event(sess, int(event_id))
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

            return await asyncio.get_running_loop().run_in_executor(_replay_executor, _go)

    @mcp.tool()
    async def read_buffer(
        capture_id: str,
        resource_id: str,
        offset: int,
        length: int,
        event_id: int,
        max_bytes: int = 65536,
        out_file: str | None = None,
    ) -> dict[str, Any]:
        """Read raw bytes from a buffer resource.

        Pass out_file to write the bytes directly to disk instead of inlining base64 in the
        response -- skips the ~33% base64 inflation and avoids hitting per-call size limits for
        large reads. When out_file is set, max_bytes may go up to 256MB instead of the normal 4MB
        inline cap. hex_preview (first 512 bytes) is always included either way as a quick sanity
        check.
        """
        async with replay_execution():

            def _go() -> dict[str, Any]:
                sess = sessions.get(capture_id)
                if sess is None:
                    return R.err("unknown_capture", capture_id)
                unsupported = _capability_error(sess, "BufferFetch")
                if unsupported:
                    return unsupported
                try:
                    rid = rdutil.parse_resource_id(resource_id)
                except ValueError as ex:
                    return R.err("bad_resource_id", str(ex))
                sessions.set_frame_event(sess, int(event_id))
                hard_cap = 256 * 1024 * 1024 if out_file else 4 * 1024 * 1024
                ln = min(int(length), int(max_bytes), hard_cap)
                data = rdutil.controller_get_buffer_data(sess.controller, rid, int(offset), ln)
                preview = data[:512]
                buf_out: dict[str, Any] = {
                    "resource_id": resource_id,
                    "offset": int(offset),
                    "requested_length": ln,
                    "byte_length": len(data),
                    "hex_preview": preview.hex(),
                }
                if out_file:
                    try:
                        with open(out_file, "wb") as f:
                            f.write(data)
                    except OSError as ex:
                        return R.err("out_file_write_failed", str(ex))
                    buf_out["out_file"] = out_file
                else:
                    buf_out["base64"] = base64.b64encode(data).decode("ascii")
                bfname = rdutil.resource_name_for(sess.controller, rid)
                if bfname:
                    buf_out["resource_name"] = bfname
                return R.ok(buf_out)

            return await asyncio.get_running_loop().run_in_executor(_replay_executor, _go)

    @mcp.tool()
    async def diff_buffer_between_events(
        capture_id: str,
        resource_id: str,
        event_a: int,
        event_b: int,
        offset: int = 0,
        length: int = 65536,
        max_changed_ranges: int = 128,
        include_previews: bool = True,
    ) -> dict[str, Any]:
        """Compare a buffer's bytes at two events and return changed ranges.

        Useful for constant-buffer upload bugs: answers "did buffer X actually change between
        draw A and draw B?" without dumping base64 manually. Response is capped to avoid huge
        payloads; increase offset/length or page ranges for detailed inspection.
        """
        async with replay_execution():

            def _go() -> dict[str, Any]:
                sess = sessions.get(capture_id)
                if sess is None:
                    return R.err("unknown_capture", capture_id)
                unsupported = _capability_error(sess, "BufferFetch")
                if unsupported:
                    return unsupported
                try:
                    rid = rdutil.parse_resource_id(resource_id)
                except ValueError as ex:
                    return R.err("bad_resource_id", str(ex))

                off = max(0, int(offset))
                ln = max(0, min(int(length), 16 * 1024 * 1024))
                if ln == 0:
                    return R.err("bad_length", "length must be > 0")

                try:
                    sessions.set_frame_event(sess, int(event_a))
                    data_a = rdutil.controller_get_buffer_data(sess.controller, rid, off, ln)
                    sessions.set_frame_event(sess, int(event_b))
                    data_b = rdutil.controller_get_buffer_data(sess.controller, rid, off, ln)
                except Exception as ex:
                    return R.err("diff_buffer_failed", str(ex))

                ranges, truncated = _changed_ranges(
                    data_a, data_b, off, max(1, min(int(max_changed_ranges), 4096))
                )
                changed_bytes = sum(r["length"] for r in ranges)
                out: dict[str, Any] = {
                    "capture_id": capture_id,
                    "resource_id": resource_id,
                    "event_a": int(event_a),
                    "event_b": int(event_b),
                    "offset": off,
                    "requested_length": ln,
                    "byte_length_a": len(data_a),
                    "byte_length_b": len(data_b),
                    "changed": bool(ranges) or data_a != data_b,
                    "changed_ranges": ranges,
                    "changed_range_count": len(ranges),
                    "changed_ranges_truncated": truncated,
                    "changed_bytes_in_returned_ranges": changed_bytes,
                }
                if include_previews:
                    previews = []
                    for r in ranges[:16]:
                        rel = r["offset"] - off
                        plen = min(r["length"], 64)
                        previews.append(
                            {
                                "offset": r["offset"],
                                "length": r["length"],
                                "a_hex": data_a[rel:rel + plen].hex(),
                                "b_hex": data_b[rel:rel + plen].hex(),
                                "preview_truncated": r["length"] > plen,
                            }
                        )
                    out["previews"] = previews
                bfname = rdutil.resource_name_for(sess.controller, rid)
                if bfname:
                    out["resource_name"] = bfname
                return R.ok(out)

            return await asyncio.get_running_loop().run_in_executor(_replay_executor, _go)

    @mcp.tool()
    async def find_in_buffer(
        capture_id: str,
        event_id: int,
        resource_id: str,
        pattern_hex: str,
        start: int = 0,
        end: int | None = None,
        max_matches: int = 1000,
    ) -> dict[str, Any]:
        """Search a buffer resource for a byte pattern server-side, returning match offsets only.

        Avoids downloading a large buffer through repeated read_buffer calls just to grep it by
        hand. `end` defaults to the buffer's full length (from GetBuffers()). Matches may overlap
        each other; results are capped at max_matches (truncated=true if the cap was hit before
        the whole range was scanned).
        """
        async with replay_execution():

            def _go() -> dict[str, Any]:
                sess = sessions.get(capture_id)
                if sess is None:
                    return R.err("unknown_capture", capture_id)
                unsupported = _capability_error(sess, "BufferFetch")
                if unsupported:
                    return unsupported
                try:
                    rid = rdutil.parse_resource_id(resource_id)
                except ValueError as ex:
                    return R.err("bad_resource_id", str(ex))
                try:
                    pattern = bytes.fromhex(pattern_hex)
                except ValueError as ex:
                    return R.err("bad_pattern_hex", str(ex))
                if not pattern:
                    return R.err("bad_pattern_hex", "pattern_hex must not be empty")

                sessions.set_frame_event(sess, int(event_id))

                range_end = end
                if range_end is None:
                    for buf in sess.controller.GetBuffers():
                        if buf.resourceId == rid:
                            range_end = int(buf.length)
                            break
                    if range_end is None:
                        return R.err(
                            "unknown_buffer",
                            "Could not find buffer {} to determine its length; pass end explicitly".format(
                                resource_id
                            ),
                        )

                range_start = max(0, int(start))
                range_end = int(range_end)
                if range_end <= range_start:
                    return R.ok(
                        {
                            "resource_id": resource_id,
                            "start": range_start,
                            "end": range_end,
                            "matches": [],
                            "match_count": 0,
                            "truncated": False,
                        }
                    )

                def _fetch(off: int, ln: int) -> bytes:
                    return rdutil.controller_get_buffer_data(sess.controller, rid, off, ln)

                try:
                    matches, truncated = rdutil.find_pattern_offsets(
                        _fetch, range_start, range_end, pattern, max(1, int(max_matches))
                    )
                except Exception as ex:
                    return R.err("find_in_buffer_failed", str(ex))

                out: dict[str, Any] = {
                    "resource_id": resource_id,
                    "start": range_start,
                    "end": range_end,
                    "pattern_byte_length": len(pattern),
                    "matches": matches,
                    "match_count": len(matches),
                    "truncated": truncated,
                }
                bfname = rdutil.resource_name_for(sess.controller, rid)
                if bfname:
                    out["resource_name"] = bfname
                return R.ok(out)

            return await asyncio.get_running_loop().run_in_executor(_replay_executor, _go)

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
        include_interpolants: bool = False,
    ) -> dict[str, Any]:
        """Pixel history: every fragment that touched (x, y), with pre/post/shader-output values.

        Pass include_interpolants=true to also attach each entry's pixel-shader input values
        (interpolants) via DebugPixel -- cheap since it only reads trace.inputs from the initial
        debug state, without walking the full instruction trace. One extra SetFrameEvent +
        DebugPixel call per history entry, so off by default; entries where debugging fails (e.g.
        non-fragment-shader operations like a clear) get an interpolants_error instead.
        """
        async with replay_execution():

            def _go() -> dict[str, Any]:
                rd = rdutil.get_renderdoc()
                sess = sessions.get(capture_id)
                if sess is None:
                    return R.err("unknown_capture", capture_id)
                unsupported = _capability_error(sess, "PixelHistory")
                if unsupported:
                    return unsupported
                try:
                    rid = rdutil.parse_resource_id(resource_id)
                except ValueError as ex:
                    return R.err("bad_resource_id", str(ex))
                if event_id is not None:
                    sessions.set_frame_event(sess, int(event_id))
                sub = rd.Subresource(int(mip), int(slice_index), int(sample_index))
                cast = rd.CompType.Typeless
                tc = type_cast.strip()
                if hasattr(rd.CompType, tc):
                    cast = getattr(rd.CompType, tc)
                hist = sess.controller.PixelHistory(rid, int(x), int(y), sub, cast)
                norm = normalize_pixel_history(hist)

                if include_interpolants:
                    for entry in norm.get("entries", []):
                        prim = entry.get("primitive_id", -1)
                        eid = entry.get("event_id")
                        if eid is None or prim is None or prim < 0:
                            continue
                        trace = None
                        try:
                            sessions.set_frame_event(sess, int(eid))
                            dpi = rd.DebugPixelInputs()
                            dpi.sample = int(sample_index)
                            dpi.primitive = int(prim)
                            dpi.view = shader_debug.NO_PREFERENCE
                            trace = sess.controller.DebugPixel(int(x), int(y), dpi)
                            if trace is None or trace.debugger is None:
                                entry["interpolants_error"] = "DebugPixel unavailable for this fragment"
                            else:
                                entry["interpolants"] = [
                                    shader_debug.shader_variable_to_value(v) for v in trace.inputs
                                ]
                        except Exception as ex:
                            entry["interpolants_error"] = str(ex)
                        finally:
                            if trace is not None:
                                sess.controller.FreeTrace(trace)

                norm["capture_id"] = capture_id
                norm["resource_id"] = resource_id
                pxname = rdutil.resource_name_for(sess.controller, rid)
                if pxname:
                    norm["resource_name"] = pxname
                norm["coordinates"] = {"x": int(x), "y": int(y)}
                return R.ok(norm)

            return await asyncio.get_running_loop().run_in_executor(_replay_executor, _go)

    def _attach_provenance_debug_detail(
        sess: Any,
        rd: Any,
        producer: dict[str, Any],
        x: int,
        y: int,
        sample: int,
        primitive: int,
        include_interpolants: bool,
        include_resource_accesses: bool,
    ) -> None:
        """Re-run DebugPixel for one provenance hop's producer fragment to attach optional detail.

        Cheap path (include_interpolants only) reads trace.inputs without stepping the trace, same
        as pixel_history's existing include_interpolants option. Expensive path
        (include_resource_accesses) runs the full instruction trace via summarize_debug_trace,
        same as debug_pixel.
        """
        sessions.set_frame_event(sess, int(producer["event_id"]))
        pipe = sess.controller.GetPipelineState()
        refl = pipe.GetShaderReflection(rd.ShaderStage.Pixel)
        if refl is None:
            producer["debug_detail_error"] = "No pixel shader bound at this event"
            return
        dpi = rd.DebugPixelInputs()
        dpi.sample = int(sample)
        dpi.primitive = int(primitive)
        dpi.view = shader_debug.NO_PREFERENCE
        trace = sess.controller.DebugPixel(int(x), int(y), dpi)
        if trace is None or trace.debugger is None:
            if trace is not None:
                sess.controller.FreeTrace(trace)
            producer["debug_detail_error"] = "DebugPixel unavailable for this fragment"
            return
        try:
            if include_interpolants:
                producer["interpolants"] = [
                    shader_debug.shader_variable_to_value(v) for v in trace.inputs
                ]
            if include_resource_accesses:
                states, truncated = shader_debug.run_debug_trace(sess.controller, trace)
                disasm_lines: list[str] = []
                try:
                    pipe_obj = pipe.GetGraphicsPipelineObject()
                    disasm_text = shader_debug.best_disassembly(sess.controller, pipe_obj, refl)
                    disasm_lines = disasm_text.split("\n") if disasm_text else []
                except Exception:
                    disasm_lines = []
                summary = shader_debug.summarize_debug_trace(
                    rd, sess.controller, pipe, rd.ShaderStage.Pixel, refl, trace, states, disasm_lines
                )
                producer["resource_accesses"] = summary.get("resource_accesses", [])
                if truncated:
                    producer["resource_accesses_truncated"] = True
        except Exception as ex:
            producer["debug_detail_error"] = str(ex)
        finally:
            sess.controller.FreeTrace(trace)

    @mcp.tool()
    async def trace_pixel_provenance(
        capture_id: str,
        resource_id: str,
        x: int,
        y: int,
        event_id: int,
        mip: int = 0,
        slice_index: int = 0,
        sample_index: int = 0,
        type_cast: str = "Typeless",
        max_depth: int = 4,
        include_interpolants: bool = False,
        include_resource_accesses: bool = False,
    ) -> dict[str, Any]:
        """Walk a final pixel's provenance backward across draws, copies, and resolves.

        Starting at (resource_id, x, y, event_id), repeatedly runs PixelHistory, picks the last
        non-culled/clipped/depth-or-stencil-failed/predicate-failed entry as this hop's producer,
        and if that producer is a Copy/Resolve action, follows its copySource/copySourceSubresource
        to continue the walk on the source resource. Stops at max_depth, a draw/clear/no-history
        hop, or a copy whose source/destination dimensions don't match (can't safely assume
        aligned coordinates for a partial-rect copy). Pass include_interpolants and/or
        include_resource_accesses to additionally debug the terminal draw's fragment (same cost
        trade-off as pixel_history's include_interpolants and debug_pixel's full trace).
        """
        async with replay_execution():

            def _go() -> dict[str, Any]:
                rd = rdutil.get_renderdoc()
                sess = sessions.get(capture_id)
                if sess is None:
                    return R.err("unknown_capture", capture_id)
                unsupported = _capability_error(sess, "PixelHistory")
                if unsupported:
                    return unsupported
                try:
                    cur_rid = rdutil.parse_resource_id(resource_id)
                except ValueError as ex:
                    return R.err("bad_resource_id", str(ex))

                depth_limit = max(1, min(int(max_depth), 16))
                cast = rd.CompType.Typeless
                tc = type_cast.strip()
                if hasattr(rd.CompType, tc):
                    cast = getattr(rd.CompType, tc)

                chain: list[dict[str, Any]] = []
                cur_x, cur_y = int(x), int(y)
                cur_mip, cur_slice, cur_sample = int(mip), int(slice_index), int(sample_index)
                cur_event = int(event_id)
                stopped_reason = "max_depth_reached"

                for _ in range(depth_limit):
                    sessions.set_frame_event(sess, cur_event)
                    sub = rd.Subresource(cur_mip, cur_slice, cur_sample)
                    hist = sess.controller.PixelHistory(cur_rid, cur_x, cur_y, sub, cast)
                    norm = normalize_pixel_history(hist)
                    entries = norm.get("entries", [])

                    hop: dict[str, Any] = {
                        "capture_id": capture_id,
                        "resource_id": rdutil.rid_str(cur_rid),
                        "coordinates": {"x": cur_x, "y": cur_y},
                        "event_id": cur_event,
                        "entries": entries,
                    }
                    rname = rdutil.resource_name_for(sess.controller, cur_rid)
                    if rname:
                        hop["resource_name"] = rname
                    chain.append(hop)

                    if not entries:
                        stopped_reason = "no_history"
                        break

                    producer_entry = select_provenance_producer(entries)
                    if producer_entry is None:
                        stopped_reason = "no_visible_contributor"
                        break

                    producer_eid = int(producer_entry["event_id"])
                    ie = sess.events_by_id.get(producer_eid)
                    flags_names = ie.flags_names if ie is not None else []
                    kind = classify_producer_action(flags_names)

                    producer: dict[str, Any] = {
                        "event_id": producer_eid,
                        "kind": kind,
                        "shader_output": producer_entry.get("shader_output"),
                        "pre_mod": producer_entry.get("pre_mod"),
                        "post_mod": producer_entry.get("post_mod"),
                    }
                    hop["producer"] = producer

                    if kind == "draw":
                        if include_interpolants or include_resource_accesses:
                            prim = producer_entry.get("primitive_id", -1)
                            if prim is not None and prim >= 0:
                                _attach_provenance_debug_detail(
                                    sess, rd, producer, cur_x, cur_y, cur_sample, prim,
                                    include_interpolants, include_resource_accesses,
                                )
                        stopped_reason = "reached_draw"
                        break

                    if kind == "clear":
                        stopped_reason = "cleared"
                        break

                    if kind not in ("copy", "resolve"):
                        stopped_reason = "unsupported_producer_kind"
                        break

                    action = find_action(sess.controller, producer_eid)
                    if action is None or action.copySource == rd.ResourceId.Null():
                        stopped_reason = "unsupported_producer_kind"
                        break

                    src_rid = action.copySource
                    src_sub = action.copySourceSubresource
                    cur_tex = find_texture_description(sess.controller, cur_rid)
                    src_tex = find_texture_description(sess.controller, src_rid)
                    if cur_tex is None or src_tex is None:
                        stopped_reason = "unsupported_producer_kind"
                        break

                    dest_dims = mip_dims(int(cur_tex.width), int(cur_tex.height), cur_mip)
                    source_dims = mip_dims(
                        int(src_tex.width), int(src_tex.height), int(src_sub.mip)
                    )
                    if not copy_hop_is_safe(dest_dims, source_dims):
                        stopped_reason = "partial_rect_copy_unsupported"
                        break

                    cur_rid = src_rid
                    cur_mip = int(src_sub.mip)
                    cur_slice = int(src_sub.slice)
                    cur_sample = int(src_sub.sample)
                    cur_event = producer_eid
                    # x, y stay the same -- aligned-copy assumption verified above

                return R.ok(
                    {
                        "chain": chain,
                        "stopped_reason": stopped_reason,
                        "truncated": stopped_reason == "max_depth_reached",
                    }
                )

            return await asyncio.get_running_loop().run_in_executor(_replay_executor, _go)

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
                for sess in (g, b):
                    unsupported = _capability_error(sess, "PipelineState")
                    if unsupported:
                        return unsupported
                try:
                    sessions.set_frame_event(g, int(good_event_id))
                    sg = normalize_pipeline_state(g.controller, g.structured_file, int(good_event_id))
                    sessions.set_frame_event(b, int(bad_event_id))
                    sb = normalize_pipeline_state(b.controller, b.structured_file, int(bad_event_id))
                    diff = diff_pipeline_snapshots(sg, sb)
                    diff["good"] = {"capture_id": good_capture_id, "event_id": int(good_event_id)}
                    diff["bad"] = {"capture_id": bad_capture_id, "event_id": int(bad_event_id)}
                except Exception as ex:
                    return R.err("diff_failed", str(ex))
                return R.ok(diff)

            return await asyncio.get_running_loop().run_in_executor(_replay_executor, _go)

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
                for sess in (g, b):
                    unsupported = _capability_error(sess, "TextureFetch")
                    if unsupported:
                        return unsupported
                try:
                    gr = rdutil.parse_resource_id(good_resource_id)
                    br = rdutil.parse_resource_id(bad_resource_id)
                except ValueError as ex:
                    return R.err("bad_resource_id", str(ex))

                def analyze(sess: Any, eid: int, rid: Any, rid_s: str) -> dict[str, Any]:
                    sessions.set_frame_event(sess, int(eid))
                    tex = find_texture_description(sess.controller, rid)
                    if tex is None:
                        return {"error": "not_a_texture", "resource_id": rid_s}
                    sub = rd.Subresource(int(mip), int(slice_index), int(sample))
                    raw = sess.controller.GetTextureData(rid, sub)
                    return analyze_texture_bytes(tex, raw)

                ga = analyze(g, good_event_id, gr, good_resource_id)
                ba = analyze(b, bad_event_id, br, bad_resource_id)
                return R.ok(diff_texture_analysis(ga, ba))

            return await asyncio.get_running_loop().run_in_executor(_replay_executor, _go)

    @mcp.tool()
    async def decode_mesh_inputs(
        capture_id: str,
        event_id: int,
        preview_vertices: int = 8,
        out_file: str | None = None,
    ) -> dict[str, Any]:
        """Decode vertex attributes for a draw's input mesh (instancing not supported).

        preview_vertices controls how many vertices are decoded (up to 8192) -- it's no longer
        silently capped at 256 regardless of what you ask for. Pass out_file to write every
        decoded vertex to a CSV (one numeric column per vector component, e.g. POSITION[0..2])
        instead of inlining them all; the response still includes a small inline sample (first 8)
        plus vertex_count for context.
        """
        async with replay_execution():

            def _go() -> dict[str, Any]:
                sess = sessions.get(capture_id)
                if sess is None:
                    return R.err("unknown_capture", capture_id)
                for feature in ("PipelineState", "BufferFetch"):
                    unsupported = _capability_error(sess, feature)
                    if unsupported:
                        return unsupported
                try:
                    sessions.set_frame_event(sess, int(event_id))
                    data = decode_mesh_inputs_core(
                        sess.controller, sess.structured_file, int(event_id), preview_vertices, out_file
                    )
                except Exception as ex:
                    return R.err("decode_mesh_failed", str(ex))
                if isinstance(data, dict):
                    if data.get("error"):
                        return R.err(str(data.get("error")), str(data.get("message", data)))
                    if data.get("ok") is False:
                        return R.err("decode_mesh_failed", str(data.get("error", "")))
                return R.ok(data)

            return await asyncio.get_running_loop().run_in_executor(_replay_executor, _go)

    @mcp.tool()
    async def decode_post_vs_outputs(
        capture_id: str,
        event_id: int,
        stage: str = "vsout",
        instance: int = 0,
        view: int = 0,
        preview_vertices: int = 8,
        out_file: str | None = None,
    ) -> dict[str, Any]:
        """Decode a draw's post-vertex-shader (or post-geometry/tessellation) output into typed
        semantics (POSITION, COLOR0, COLOR1, texture coordinates, etc.) for selected vertices.

        stage is "vsout" (default) or "gsout" -- "gsout" requires a Geometry or Domain
        (tessellation) shader to be active for this draw and returns
        no_geometry_or_tessellation_stage otherwise, rather than silently falling back to VS data.
        POSITION additionally reports a perspective-divided NDC value (xyz/w) alongside the raw
        clip-space value when the underlying MeshFormat says this data is unprojectable -- the
        same perspective divide RenderDoc's own mesh-view shader uses, not a full camera/
        view-matrix reconstruction. preview_vertices controls how many vertices are decoded (up
        to 8192); pass out_file to write every decoded vertex to a CSV instead of inlining them.
        """
        async with replay_execution():

            def _go() -> dict[str, Any]:
                sess = sessions.get(capture_id)
                if sess is None:
                    return R.err("unknown_capture", capture_id)
                unsupported = _capability_error(sess, "PostVS")
                if unsupported:
                    return unsupported
                try:
                    sessions.set_frame_event(sess, int(event_id))
                    data = decode_post_vs_outputs_core(
                        sess.controller, sess.structured_file, int(event_id), stage, instance,
                        view, preview_vertices, out_file,
                    )
                except Exception as ex:
                    return R.err("decode_post_vs_failed", str(ex))
                if isinstance(data, dict):
                    if data.get("error"):
                        return R.err(str(data.get("error")), str(data.get("message", data)))
                    if data.get("ok") is False:
                        return R.err("decode_post_vs_failed", str(data.get("error", "")))
                return R.ok(data)

            return await asyncio.get_running_loop().run_in_executor(_replay_executor, _go)

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
                unsupported = _capability_error(sess, "ShaderSource")
                if unsupported:
                    return unsupported
                st = _stage_from_string(rd, stage)
                if st is None:
                    return R.err("bad_stage", stage)
                sessions.set_frame_event(sess, int(event_id))
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
                    targets = list(sess.controller.GetDisassemblyTargets(True))
                    out["available_targets"] = targets

                    text = ""
                    used_target = ""
                    failure_reason: str | None = None
                    # Record why each target was rejected so a total failure is diagnosable
                    # instead of silent. Previously the loop kept only the LAST reason, so when
                    # the source-level DXBC/DXIL target failed and an ISA target returned an
                    # "Unsupported encoding" sentinel, the caller saw neither the real cause nor
                    # (before the sentinel was added to the failure markers) that it had failed.
                    attempts: list[dict[str, str]] = []
                    if not targets:
                        failure_reason = "No disassembly targets available for this capture's driver"
                    else:
                        for t in targets:
                            candidate = sess.controller.DisassembleShader(pipe_obj, refl, t)
                            reason = shader_debug.disassembly_failure_reason(candidate)
                            if reason is None:
                                text = candidate
                                used_target = t
                                failure_reason = None
                                break
                            attempts.append({"target": t, "reason": reason[:200]})
                            failure_reason = reason

                    if failure_reason is not None:
                        out["disassembly_available"] = False
                        out["disassembly_error"] = failure_reason
                        if attempts:
                            out["disassembly_attempts"] = attempts
                    else:
                        out["disassembly_available"] = True
                        out["disassembly_target"] = used_target
                        if len(text) > max_disassembly_chars:
                            out["disassembly"] = text[:max_disassembly_chars]
                            out["disassembly_truncated"] = True
                        else:
                            out["disassembly"] = text
                return R.ok(out)

            return await asyncio.get_running_loop().run_in_executor(_replay_executor, _go)

    @mcp.tool()
    async def get_shader_reflection(capture_id: str, event_id: int, stage: str) -> dict[str, Any]:
        async with replay_execution():

            def _go() -> dict[str, Any]:
                rd = rdutil.get_renderdoc()
                sess = sessions.get(capture_id)
                if sess is None:
                    return R.err("unknown_capture", capture_id)
                unsupported = _capability_error(sess, "ShaderSource")
                if unsupported:
                    return unsupported
                st = _stage_from_string(rd, stage)
                if st is None:
                    return R.err("bad_stage", stage)
                sessions.set_frame_event(sess, int(event_id))
                pipe = sess.controller.GetPipelineState()
                refl = pipe.GetShaderReflection(st)
                if refl is None:
                    return R.ok({"bound": False, "stage": stage})
                sref: dict[str, Any] = {"bound": True, "stage": stage}
                rdutil.enrich_resource_dict(sess.controller, sref, refl.resourceId)
                sref["reflection"] = serialize_shader_reflection_summary(refl, sess.controller)
                return R.ok(sref)

            return await asyncio.get_running_loop().run_in_executor(_replay_executor, _go)

    def _collect_comparable_shader_trace(
        sess: Any,
        event_id: int,
        stage_name: str,
        invocation: dict[str, int],
    ) -> tuple[dict[str, Any], bool]:
        """Debug one invocation and retain the executed steps needed by the cross-capture diff."""
        rd = rdutil.get_renderdoc()
        stage_key = stage_name.strip().lower()
        if stage_key == "vertex":
            stage = rd.ShaderStage.Vertex
        elif stage_key in ("pixel", "fragment"):
            stage = rd.ShaderStage.Pixel
        else:
            raise ValueError("stage must be 'vertex' or 'pixel'/'fragment'")

        sessions.set_frame_event(sess, int(event_id))
        pipe = sess.controller.GetPipelineState()
        refl = pipe.GetShaderReflection(stage)
        if refl is None:
            raise ValueError("No {} shader is bound at event {}".format(stage_name, event_id))

        if stage_key == "vertex":
            vertex_id = int(invocation.get("vertex_id", 0))
            instance_id = int(invocation.get("instance_id", 0))
            index = int(invocation.get("index", vertex_id))
            view = int(invocation.get("view", 0))
            trace = sess.controller.DebugVertex(vertex_id, instance_id, index, view)
        else:
            if "x" not in invocation or "y" not in invocation:
                raise ValueError("pixel invocations require integer 'x' and 'y' keys")
            inputs = rd.DebugPixelInputs()
            inputs.sample = int(invocation.get("sample", 0))
            inputs.primitive = int(invocation.get("primitive", shader_debug.NO_PREFERENCE))
            inputs.view = int(invocation.get("view", 0))
            trace = sess.controller.DebugPixel(int(invocation["x"]), int(invocation["y"]), inputs)

        if trace is None or trace.debugger is None:
            if trace is not None:
                sess.controller.FreeTrace(trace)
            raise RuntimeError("Shader debugging is unavailable for this invocation")

        try:
            states, truncated = shader_debug.run_debug_trace(sess.controller, trace)
            disasm_lines: list[str] = []
            try:
                pipe_obj = pipe.GetGraphicsPipelineObject()
                disasm_text = shader_debug.best_disassembly(sess.controller, pipe_obj, refl)
                disasm_lines = disasm_text.split("\n") if disasm_text else []
            except Exception:
                pass
            data = shader_debug.build_comparable_debug_trace(
                rd, refl, trace, states, disasm_lines
            )
            return data, truncated
        finally:
            sess.controller.FreeTrace(trace)

    @mcp.tool()
    async def diff_shader_invocations(
        capture_a: str,
        event_id_a: int,
        invocation_a: dict[str, int],
        capture_b: str,
        event_id_b: int,
        invocation_b: dict[str, int],
        stage: str,
        abs_tolerance: float = 1e-6,
        rel_tolerance: float = 1e-5,
        max_differences: int = 100,
    ) -> dict[str, Any]:
        """Compare two vertex or pixel shader invocations and find the first divergent step.

        For vertex traces, each invocation object accepts ``vertex_id`` (default 0),
        ``instance_id`` (default 0), ``index`` (default vertex_id), and ``view``. For pixel traces,
        each object requires ``x``/``y`` and optionally accepts ``sample``, ``primitive``, and
        ``view``. Inputs and typed constant blocks are compared before execution; executed
        instructions are aligned by normalized disassembly (falling back to execution position),
        then temporary/register changes and final semantic outputs are compared with configurable
        float tolerances. The response reports the first divergent instruction automatically.
        """
        async with replay_execution():

            def _go() -> dict[str, Any]:
                sess_a = sessions.get(capture_a)
                sess_b = sessions.get(capture_b)
                if sess_a is None or sess_b is None:
                    return R.err("unknown_capture", "capture_a or capture_b invalid")
                for sess in (sess_a, sess_b):
                    unsupported = _capability_error(sess, "ShaderDebugging")
                    if unsupported:
                        return unsupported
                try:
                    trace_a, truncated_a = _collect_comparable_shader_trace(
                        sess_a, int(event_id_a), stage, invocation_a
                    )
                    trace_b, truncated_b = _collect_comparable_shader_trace(
                        sess_b, int(event_id_b), stage, invocation_b
                    )
                    out = shader_debug.compare_debug_traces(
                        trace_a,
                        trace_b,
                        abs_tolerance=max(0.0, float(abs_tolerance)),
                        rel_tolerance=max(0.0, float(rel_tolerance)),
                        max_differences=max(1, min(int(max_differences), 1000)),
                    )
                except Exception as ex:
                    return R.err("diff_shader_invocations_failed", str(ex))

                out.update(
                    {
                        "capture_a": capture_a,
                        "event_id_a": int(event_id_a),
                        "invocation_a": invocation_a,
                        "capture_b": capture_b,
                        "event_id_b": int(event_id_b),
                        "invocation_b": invocation_b,
                        "stage": stage,
                        "trace_truncated_a": truncated_a,
                        "trace_truncated_b": truncated_b,
                    }
                )
                return R.ok(out)

            return await asyncio.get_running_loop().run_in_executor(_replay_executor, _go)

    @mcp.tool()
    async def debug_pixel(
        capture_id: str,
        event_id: int,
        x: int,
        y: int,
        sample: int = 0,
        primitive: int | None = None,
        view: int = 0,
        full_trace: bool = False,
        full_trace_path: str | None = None,
        timeout_seconds: float = DEFAULT_SHADER_DEBUG_TIMEOUT_SECONDS,
    ) -> dict[str, Any]:
        """Debug the fragment invocation that wrote pixel (x, y) and summarize the trace.

        Uses ReplayController.DebugPixel() + ContinueDebug(). Returns shader inputs, every
        resource access (sample/load instructions with best-effort resolved descriptor/resource
        identity — reliable for DXBC-style disassembly, best-effort text-only elsewhere),
        constant buffer values, and the final output register values (answers "bad sample, bad
        constant, or no sample at all?" in one call). Pass full_trace=true to additionally dump
        the complete instruction-by-instruction trace to a JSON file and return its path.
        Native shader debugging runs in a disposable one-capture worker. If it exceeds
        timeout_seconds, that worker is terminated and the MCP server remains usable.
        """
        async with replay_execution():
            sess = sessions.get(capture_id)
            if sess is None:
                return R.err("unknown_capture", capture_id)
            unsupported = _capability_error(sess, "ShaderDebugging")
            if unsupported:
                return unsupported
            timeout = clamp_shader_debug_timeout(timeout_seconds)
            request = {
                "operation": "debug_pixel",
                "capture_id": capture_id,
                "capture_path": sess.path,
                "event_id": int(event_id),
                "x": int(x),
                "y": int(y),
                "sample": shader_debug.NO_PREFERENCE if sample is None else int(sample),
                "primitive": primitive,
                "view": view,
                "full_trace": bool(full_trace),
                "full_trace_path": full_trace_path,
            }
            _log.info(
                "debug_pixel: isolated worker start capture_id=%s event=%d pixel=(%d,%d) timeout=%.1fs",
                capture_id, int(event_id), int(x), int(y), timeout,
            )
            try:
                result = await run_shader_debug_worker(request, timeout)
            except IsolatedReplayTimeout as ex:
                _log.warning("debug_pixel: %s", ex)
                return R.err("debug_pixel_timeout", str(ex))
            except IsolatedReplayError as ex:
                _log.exception("debug_pixel: isolated worker failed")
                return R.err("debug_pixel_failed", str(ex))
            except Exception as ex:
                _log.exception("debug_pixel: could not start isolated worker")
                return R.err("debug_pixel_failed", str(ex))
            _log.info(
                "debug_pixel: isolated worker complete capture_id=%s event=%d",
                capture_id,
                int(event_id),
            )
            return result

    @mcp.tool()
    async def debug_vertex(
        capture_id: str,
        event_id: int,
        vertex_id: int,
        instance_id: int = 0,
        index: int | None = None,
        view: int = 0,
        full_trace: bool = False,
        full_trace_path: str | None = None,
        timeout_seconds: float = DEFAULT_SHADER_DEBUG_TIMEOUT_SECONDS,
    ) -> dict[str, Any]:
        """Debug the vertex shader invocation for a given vertex and summarize the trace.

        Uses ReplayController.DebugVertex() + ContinueDebug(). ``index`` is the actual index used
        to look up vertex inputs (from an index buffer, with all drawcall offsets applied) — if
        omitted it defaults to ``vertex_id``, which is only correct for non-indexed draws with no
        base vertex offset. Same summary shape as debug_pixel: inputs, resource accesses,
        constant buffer values, and final output register values. Native shader debugging runs
        in a disposable worker so a replay hang can be terminated without wedging the MCP server.
        """
        async with replay_execution():
            sess = sessions.get(capture_id)
            if sess is None:
                return R.err("unknown_capture", capture_id)
            unsupported = _capability_error(sess, "ShaderDebugging")
            if unsupported:
                return unsupported
            timeout = clamp_shader_debug_timeout(timeout_seconds)
            request = {
                "operation": "debug_vertex",
                "capture_id": capture_id,
                "capture_path": sess.path,
                "event_id": int(event_id),
                "vertex_id": int(vertex_id),
                "instance_id": int(instance_id),
                "index": index,
                "view": int(view),
                "full_trace": bool(full_trace),
                "full_trace_path": full_trace_path,
            }
            _log.info(
                "debug_vertex: isolated worker start capture_id=%s event=%d vertex=%d timeout=%.1fs",
                capture_id, int(event_id), int(vertex_id), timeout,
            )
            try:
                result = await run_shader_debug_worker(request, timeout)
            except IsolatedReplayTimeout as ex:
                _log.warning("debug_vertex: %s", ex)
                return R.err("debug_vertex_timeout", str(ex))
            except IsolatedReplayError as ex:
                _log.exception("debug_vertex: isolated worker failed")
                return R.err("debug_vertex_failed", str(ex))
            except Exception as ex:
                _log.exception("debug_vertex: could not start isolated worker")
                return R.err("debug_vertex_failed", str(ex))
            _log.info(
                "debug_vertex: isolated worker complete capture_id=%s event=%d",
                capture_id,
                int(event_id),
            )
            return result

    @mcp.tool()
    async def read_constant_buffer(
        capture_id: str,
        event_id: int,
        stage: str,
        slot: int,
        raw_offset: int = 0,
        raw_length: int = 256,
    ) -> dict[str, Any]:
        """Decode a constant buffer slot to typed named variables using shader reflection.

        Returns variable names and values (floats, matrices, vectors). Useful for finding
        wrong transform matrices or emulator data-upload bugs. Falls back to raw hex if
        reflection is unavailable.

        `variables` is always decoded from the full constant buffer (up to 65536 bytes), but
        `raw_bytes_hex` is a windowed preview — use `raw_offset`/`raw_length` (capped at 8192
        bytes) to page through bytes beyond the default 256-byte preview, e.g. the tail of a
        large constant buffer. `raw_byte_length` reports the total decoded size to page against.
        """
        async with replay_execution():

            def _go() -> dict[str, Any]:
                rd = rdutil.get_renderdoc()
                sess = sessions.get(capture_id)
                if sess is None:
                    return R.err("unknown_capture", capture_id)
                unsupported = _capability_error(sess, "PipelineState")
                if unsupported:
                    return unsupported
                st = _stage_from_string(rd, stage)
                if st is None:
                    return R.err("bad_stage", stage)

                sessions.set_frame_event(sess, int(event_id))
                pipe = sess.controller.GetPipelineState()
                refl = pipe.GetShaderReflection(st)
                if refl is None:
                    return R.ok({"bound": False, "stage": stage, "slot": int(slot)})

                cb_blocks = list(getattr(refl, "constantBlocks", []) or [])

                # Find by fixed bind number first, then fall back to index
                cb_block, cb_index = _select_constant_block(cb_blocks, int(slot))
                if cb_block is None:
                    return R.err("no_cb_at_slot", "No constant block at slot {}".format(slot))

                # Read raw buffer bytes
                raw = b""
                raw_hex = ""
                try:
                    # GetConstantBlock's second argument is the reflection-array index,
                    # not the register/bind slot.
                    cb_desc = pipe.GetConstantBlock(st, cb_index, 0)
                    desc = getattr(cb_desc, "descriptor", None)
                    if desc is not None:
                        buf_rid = getattr(desc, "resource", None)
                        byte_offset = int(getattr(desc, "byteOffset", 0))
                        byte_size = int(getattr(desc, "byteSize", 0)) or 65536
                        raw = rdutil.controller_get_buffer_data(
                            sess.controller, buf_rid, byte_offset, min(byte_size, 65536)
                        )
                        preview_off = max(0, int(raw_offset))
                        preview_len = max(0, min(int(raw_length), 8192))
                        raw_hex = raw[preview_off:preview_off + preview_len].hex()
                except Exception:
                    pass

                variables: list[dict] = []
                anomaly_list: list[str] = []

                if raw:
                    constants = list(getattr(cb_block, "variables", []) or [])
                    for var in decode_cb_bytes(raw, constants):
                        anom = detect_variable_anomalies(var)
                        if anom:
                            var["anomaly"] = anom
                            anomaly_list.append("{}:{}".format(anom, var["name"]))
                        variables.append(var)

                out: dict[str, Any] = {
                    "stage": stage,
                    "slot": int(slot),
                    "name": str(getattr(cb_block, "name", "") or ""),
                    "variables": variables,
                    "anomalies": anomaly_list,
                    "raw_bytes_hex": raw_hex,
                    "raw_offset": max(0, int(raw_offset)),
                    "raw_byte_length": len(raw),
                }
                if variables:
                    real_names = [
                        str(v.get("name", "") or "")
                        for v in variables
                        if str(v.get("name", "") or "") and not str(v.get("name", "") or "").startswith("unknown[")
                    ]
                    out["reflection_names_available"] = bool(real_names)
                    if not real_names:
                        out["reflection_names_note"] = (
                            "RenderDoc shader reflection did not provide original constant names; "
                            "values are decoded from byte offsets with generated unknown[N] names."
                        )
                if not raw:
                    out["buffer_unavailable"] = True

                return R.ok(out)

            return await asyncio.get_running_loop().run_in_executor(_replay_executor, _go)

    @mcp.tool()
    async def get_debug_messages(
        capture_id: str,
        severity_filter: str | None = None,
    ) -> dict[str, Any]:
        """Return GPU validation layer messages (errors, warnings) from the capture.

        Pass severity_filter='Error' to see only errors. Emulators frequently trigger
        validation messages that directly name the root cause of rendering bugs.
        """
        async with replay_execution():

            def _go() -> dict[str, Any]:
                sess = sessions.get(capture_id)
                if sess is None:
                    return R.err("unknown_capture", capture_id)
                unsupported = _capability_error(sess, "PipelineState")
                if unsupported:
                    return unsupported
                try:
                    msgs = list(sess.controller.GetDebugMessages())
                except Exception as ex:
                    return R.err("debug_messages_failed", str(ex))

                counts: dict[str, int] = {}
                rows: list[dict[str, Any]] = []

                for m in msgs:
                    sev_raw = str(getattr(m, "severity", "") or "")
                    sev = sev_raw.split(".")[-1] if "." in sev_raw else sev_raw
                    cat_raw = str(getattr(m, "category", "") or "")
                    cat = cat_raw.split(".")[-1] if "." in cat_raw else cat_raw
                    msg_str = str(getattr(m, "message", "") or "")
                    eid = int(getattr(m, "eventId", 0) or 0)

                    counts[sev] = counts.get(sev, 0) + 1

                    if severity_filter and sev.lower() != severity_filter.lower():
                        continue

                    rows.append({
                        "event_id": eid,
                        "severity": sev,
                        "category": cat,
                        "message": msg_str,
                    })

                return R.ok({
                    "error_count": counts.get("Error", 0),
                    "warning_count": counts.get("Warning", 0),
                    "info_count": counts.get("Info", 0),
                    "messages": rows,
                })

            return await asyncio.get_running_loop().run_in_executor(_replay_executor, _go)

    @mcp.tool()
    async def get_frame_overview(capture_id: str) -> dict[str, Any]:
        """Frame structure map: render passes, render targets, draw counts.

        Fast — uses no SetFrameEvent calls. Call this first on any capture to orient
        the agent. A render target with write_event_count=0 after its clear_event_ids
        is the primary signal for 'black screen' bugs.
        """
        async with replay_execution():

            def _go() -> dict[str, Any]:
                rd = rdutil.get_renderdoc()
                sess = sessions.get(capture_id)
                if sess is None:
                    return R.err("unknown_capture", capture_id)
                try:
                    overview = build_frame_overview(sess.controller, sess.structured_file)
                except Exception as ex:
                    return R.err("frame_overview_failed", str(ex))
                overview["api"] = enum_api(rd, sess.controller)
                overview["capture_id"] = capture_id
                return R.ok(overview)

            return await asyncio.get_running_loop().run_in_executor(_replay_executor, _go)

    @mcp.tool()
    async def analyze_draw_visibility(capture_id: str, event_id: int) -> dict[str, Any]:
        async with replay_execution():

            def _go() -> dict[str, Any]:
                sess = sessions.get(capture_id)
                if sess is None:
                    return R.err("unknown_capture", capture_id)
                try:
                    sessions.set_frame_event(sess, int(event_id))
                    snap = normalize_pipeline_state(sess.controller, sess.structured_file, int(event_id))
                    vis = draw_visibility_analysis(sess.controller, sess.structured_file, snap, int(event_id))
                except Exception as ex:
                    return R.err("visibility_failed", str(ex))
                return R.ok(vis, evidence=vis.get("evidence"))

            return await asyncio.get_running_loop().run_in_executor(_replay_executor, _go)

    def _draw_rows(sess: Any, event_ids: list[int] | None, name_contains: str | None, limit: int) -> tuple[list[dict[str, Any]], bool]:
        if event_ids is not None:
            candidates = [int(e) for e in event_ids]
        else:
            candidates = []
            for eid in sess.events_ordered:
                ie = sess.events_by_id[eid]
                if "Drawcall" not in ie.flags_names:
                    continue
                if name_contains and name_contains.lower() not in ie.name.lower():
                    continue
                candidates.append(eid)

        capped = candidates[: max(1, int(limit))]
        rows: list[dict[str, Any]] = []
        for eid in capped:
            try:
                sessions.set_frame_event(sess, eid)
                rows.append(build_draw_state_row(sess.controller, sess.structured_file, eid))
            except Exception as ex:
                rows.append({"event_id": eid, "name": "", "error": str(ex)})
        return rows, len(candidates) > len(capped)

    @mcp.tool()
    async def list_draws_with_state(
        capture_id: str,
        event_ids: list[int] | None = None,
        name_contains: str | None = None,
        limit: int = 200,
    ) -> dict[str, Any]:
        """One compact row per draw: VB/IB bindings, RT/DS, topology, shader CB block names.

        Built to replace dozens of individual get_pipeline_state calls (~10KB each) when scanning
        or comparing many draws. Pass event_ids to restrict to specific draws (e.g. from a prior
        list_events call); otherwise every Drawcall event is included, up to limit.
        """
        async with replay_execution():

            def _go() -> dict[str, Any]:
                sess = sessions.get(capture_id)
                if sess is None:
                    return R.err("unknown_capture", capture_id)
                unsupported = _capability_error(sess, "PipelineState")
                if unsupported:
                    return unsupported
                rows, truncated = _draw_rows(sess, event_ids, name_contains, limit)
                return R.ok(
                    {"capture_id": capture_id, "draws": rows, "count": len(rows), "truncated": truncated}
                )

            return await asyncio.get_running_loop().run_in_executor(_replay_executor, _go)

    @mcp.tool()
    async def diff_draw_sequences(
        capture_a: str,
        capture_b: str,
        event_ids_a: list[int] | None = None,
        event_ids_b: list[int] | None = None,
        limit: int = 200,
    ) -> dict[str, Any]:
        """Align and diff the draw-state tables of two captures (e.g. two backends of the same
        content, or good-vs-bad). Draws are aligned by structural shape (topology, buffer/target
        counts) via sequence alignment -- not by event_id or resource id, which are never
        comparable across captures -- so insertions/deletions on one side are reported separately
        from field-level changes on aligned pairs. resource_id/event_id fields are excluded from
        the per-pair change list (always different across captures, pure noise); resource_name is
        kept since it's often preserved and meaningfully comparable.
        """
        async with replay_execution():

            def _go() -> dict[str, Any]:
                sess_a = sessions.get(capture_a)
                sess_b = sessions.get(capture_b)
                if sess_a is None or sess_b is None:
                    return R.err("unknown_capture", "capture_a or capture_b invalid")
                for sess in (sess_a, sess_b):
                    unsupported = _capability_error(sess, "PipelineState")
                    if unsupported:
                        return unsupported

                rows_a, _ = _draw_rows(sess_a, event_ids_a, None, limit)
                rows_b, _ = _draw_rows(sess_b, event_ids_b, None, limit)

                def shape_key(row: dict[str, Any]) -> tuple[Any, ...]:
                    return draw_shape_key(row)

                def strip_noise(obj: Any) -> Any:
                    # event_id/resource_id are never comparable across captures; name is the raw
                    # API call name (e.g. "vkCmdDrawIndexed" vs "DrawIndexedInstanced") so it
                    # differs by backend even for the same semantic draw -- all three would just
                    # be noise in every pair's diff. name_a/name_b stay visible at the pair level.
                    if isinstance(obj, dict):
                        return {
                            k: strip_noise(v) for k, v in obj.items() if k not in ("event_id", "resource_id", "name")
                        }
                    if isinstance(obj, list):
                        return [strip_noise(v) for v in obj]
                    return obj

                keys_a = [shape_key(r) for r in rows_a]
                keys_b = [shape_key(r) for r in rows_b]
                matcher = difflib.SequenceMatcher(a=keys_a, b=keys_b, autojunk=False)

                paired: list[tuple[dict[str, Any], dict[str, Any]]] = []
                only_a: list[dict[str, Any]] = []
                only_b: list[dict[str, Any]] = []
                for tag, i1, i2, j1, j2 in matcher.get_opcodes():
                    if tag == "equal":
                        paired.extend(zip(rows_a[i1:i2], rows_b[j1:j2]))
                    elif tag == "replace":
                        n = min(i2 - i1, j2 - j1)
                        paired.extend(zip(rows_a[i1:i1 + n], rows_b[j1:j1 + n]))
                        only_a.extend(rows_a[i1 + n:i2])
                        only_b.extend(rows_b[j1 + n:j2])
                    elif tag == "delete":
                        only_a.extend(rows_a[i1:i2])
                    elif tag == "insert":
                        only_b.extend(rows_b[j1:j2])

                pairs = []
                changed_count = 0
                for ra, rb in paired:
                    changes = deep_diff(strip_noise(ra), strip_noise(rb))
                    if changes:
                        changed_count += 1
                    pairs.append(
                        {
                            "event_id_a": ra.get("event_id"),
                            "event_id_b": rb.get("event_id"),
                            "name_a": ra.get("name"),
                            "name_b": rb.get("name"),
                            "changes": changes,
                        }
                    )

                return R.ok(
                    {
                        "capture_a": capture_a,
                        "capture_b": capture_b,
                        "draw_count_a": len(rows_a),
                        "draw_count_b": len(rows_b),
                        "aligned_count": len(paired),
                        "changed_count": changed_count,
                        "only_in_a": [{"event_id": r.get("event_id"), "name": r.get("name")} for r in only_a],
                        "only_in_b": [{"event_id": r.get("event_id"), "name": r.get("name")} for r in only_b],
                        "pairs": pairs,
                    }
                )

            return await asyncio.get_running_loop().run_in_executor(_replay_executor, _go)

    @mcp.tool()
    async def find_corresponding_draws(
        capture_a: str,
        event_id_a: int,
        capture_b: str,
        event_ids_b: list[int] | None = None,
        limit: int = 200,
        top_k: int = 5,
        timeout_seconds: float = DEFAULT_DRAW_MATCHING_TIMEOUT_SECONDS,
    ) -> dict[str, Any]:
        """Rank the most likely corresponding draw(s) in capture_b for one draw in capture_a.

        Content-based fingerprint matching (post-VS position bounding-box shape, primary
        render-target dimensions/content, constant-buffer values, shader output-signature/
        constant-block name sets) -- deliberately does not compare shader bytecode/disassembly,
        which is meaningless across different graphics APIs. Pass event_ids_b to restrict the
        candidate pool (e.g. from a prior list_draws_with_state/list_events call); otherwise
        every Drawcall event in capture_b is considered, up to limit. Returns up to top_k
        candidates sorted by confidence, each with a per-signal breakdown so the result can be
        judged rather than trusted blindly -- this is a heuristic ranking, not a guaranteed match.
        """
        async with replay_execution():
            def _request() -> dict[str, Any]:
                sess_a = sessions.get(capture_a)
                sess_b = sessions.get(capture_b)
                if sess_a is None or sess_b is None:
                    return R.err("unknown_capture", "capture_a or capture_b invalid")
                for sess in (sess_a, sess_b):
                    unsupported = _capability_error(sess, "PipelineState")
                    if unsupported:
                        return unsupported
                return {
                    "operation": "find_corresponding_draws",
                    "capture_path_a": sess_a.path,
                    "event_id_a": int(event_id_a),
                    "capture_path_b": sess_b.path,
                    "event_ids_b": event_ids_b,
                    "limit": max(1, min(int(limit), 1000)),
                    "top_k": max(1, min(int(top_k), 50)),
                }

            request = await asyncio.get_running_loop().run_in_executor(
                _replay_executor, _request
            )
            if request.get("ok") is False:
                return request
            try:
                return await run_draw_matching_worker(request, timeout_seconds)
            except IsolatedReplayTimeout as ex:
                return R.err("find_corresponding_draws_timeout", str(ex))
            except IsolatedReplayError as ex:
                return R.err("find_corresponding_draws_worker_failed", str(ex))

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
