"""Normalize RenderDoc PipeState and related objects to JSON-safe dicts."""

from __future__ import annotations

from typing import Any

from renderdoc_mcp.rdutil import enum_name, get_renderdoc, rid_str
from renderdoc_mcp.session import expand_action_flags, find_action


def _try(fn: Any, default: Any = None) -> Any:
    try:
        return fn()
    except Exception:
        return default


def serialize_viewport(vp: Any) -> dict[str, Any]:
    return {
        "x": float(vp.x),
        "y": float(vp.y),
        "width": float(vp.width),
        "height": float(vp.height),
        "mindepth": float(vp.minDepth),
        "maxdepth": float(vp.maxDepth),
    }


def serialize_scissor(sc: Any) -> dict[str, Any]:
    return {
        "x": int(sc.x),
        "y": int(sc.y),
        "width": int(sc.width),
        "height": int(sc.height),
        "enabled": bool(sc.enabled),
    }


def serialize_used_descriptor(d: Any) -> dict[str, Any]:
    out: dict[str, Any] = {
        "bind_type": enum_name(getattr(d, "bindType", None)),
        "direct_access": bool(getattr(d, "directAccess", False)),
    }
    desc = getattr(d, "descriptor", None)
    if desc is not None:
        out["resource_id"] = rid_str(getattr(desc, "resource", None))
        out["byte_offset"] = int(getattr(desc, "byteOffset", 0))
        out["byte_size"] = int(getattr(desc, "byteSize", 0))
        out["format"] = enum_name(getattr(desc, "format", None))
        tid = getattr(desc, "texelBufferStructureSize", None)
        if tid is not None:
            out["texel_buffer_structure_size"] = int(tid)
    return out


def serialize_shader_stage_summary(rd: Any, pipe: Any, stage: Any) -> dict[str, Any]:
    info: dict[str, Any] = {"stage": enum_name(stage)}
    refl = _try(lambda: pipe.GetShaderReflection(stage))
    if refl is None:
        info["bound"] = False
        return info
    info["bound"] = True
    info["resource_id"] = rid_str(getattr(refl, "resourceId", rd.ResourceId.Null()))
    ep = _try(lambda: pipe.GetShaderEntryPoint(stage))
    if ep is not None and ep != "":
        # PipeState returns rdcstr (Python str); older bindings may return ShaderEntryPoint with .name.
        if isinstance(ep, str):
            info["entry_point"] = ep
        else:
            info["entry_point"] = str(getattr(ep, "name", ep))
    dbg = getattr(refl, "debugInfo", None)
    if dbg is not None:
        info["debuggable"] = bool(getattr(dbg, "debuggable", False))
    return info


def collect_shader_stages(rd: Any) -> list[Any]:
    stages: list[Any] = []
    for name in (
        "Vertex",
        "Hull",
        "Domain",
        "Geometry",
        "Pixel",
        "Fragment",
        "Amplification",
        "Mesh",
        "Compute",
        "Task",
        "RayGeneration",
        "Intersection",
        "AnyHit",
        "ClosestHit",
        "Miss",
        "Callable",
    ):
        if hasattr(rd.ShaderStage, name):
            stages.append(getattr(rd.ShaderStage, name))
    # Dedup if aliases overlap
    seen = set()
    uniq = []
    for s in stages:
        key = int(s) if hasattr(s, "__int__") else str(s)
        if key not in seen:
            seen.add(key)
            uniq.append(s)
    return uniq


def serialize_graphics_targets(pipe: Any) -> dict[str, Any]:
    rd = get_renderdoc()
    data: dict[str, Any] = {}
    outs = _try(lambda: pipe.GetOutputTargets(), [])
    data["color_targets"] = []
    for i, o in enumerate(outs):
        data["color_targets"].append(
            {
                "slot": i,
                "resource_id": rid_str(getattr(o, "resource", None)),
                "slice": int(getattr(o, "slice", 0)),
                "mipslice": int(getattr(o, "mipslice", 0)),
            }
        )
    dt = _try(lambda: pipe.GetDepthTarget())
    if dt is not None:
        data["depth_target"] = {
            "resource_id": rid_str(getattr(dt, "resource", None)),
            "slice": int(getattr(dt, "slice", 0)),
            "mipslice": int(getattr(dt, "mipslice", 0)),
        }
    else:
        data["depth_target"] = None
    ss = _try(lambda: pipe.GetStencilTarget())
    if ss is not None:
        data["stencil_target"] = {"resource_id": rid_str(getattr(ss, "resource", None))}
    else:
        data["stencil_target"] = None

    # Clear colour bindings if API exposes them
    cc = _try(lambda: pipe.GetColorBlends())  # might be wrong method name
    # Skip if fails - ColorBlend is state not targets

    return data


def serialize_rasterizer(pipe: Any) -> dict[str, Any]:
    r = _try(lambda: pipe.GetRasterizer())
    if r is None:
        return {}
    return {
        "fill_mode": enum_name(getattr(r, "fillMode", None)),
        "cull_mode": enum_name(getattr(r, "cullMode", None)),
        "front_ccw": bool(getattr(r, "frontCCW", False)),
        "depth_clip": bool(getattr(r, "depthClip", True)),
        "depth_bias": float(getattr(r, "depthBias", 0.0)),
        "slope_scaled_depth_bias": float(getattr(r, "slopeScaledDepthBias", 0.0)),
        "line_width": float(getattr(r, "lineWidth", 1.0)),
    }


def serialize_depth_state(pipe: Any) -> dict[str, Any]:
    d = _try(lambda: pipe.GetDepthState())
    if d is None:
        return {}
    return {
        "depth_enable": bool(getattr(d, "depthEnable", False)),
        "depth_writes": bool(getattr(d, "depthWrites", False)),
        "depth_function": enum_name(getattr(d, "depthFunction", None)),
    }


def serialize_blend_state(pipe: Any) -> dict[str, Any]:
    b = _try(lambda: pipe.GetBlendState())
    if b is None:
        return {}
    targets = []
    bl = getattr(b, "blends", None) or []
    for i, bt in enumerate(bl):
        targets.append(
            {
                "slot": i,
                "blend_enable": bool(getattr(bt, "blendEnable", False)),
                "logic_operation": enum_name(getattr(bt, "logicOperation", None)),
                "write_mask": int(getattr(bt, "writeMask", 0)),
            }
        )
    return {
        "alpha_to_coverage": bool(getattr(b, "alphaToCoverage", False)),
        "independent_blend": bool(getattr(b, "independentBlend", False)),
        "targets": targets,
    }


def serialize_stencil_state(pipe: Any) -> dict[str, Any]:
    s = _try(lambda: pipe.GetStencilState())
    if s is None:
        return {}
    return {
        "stencil_enable": bool(getattr(s, "stencilEnable", False)),
    }


def serialize_vertex_inputs(pipe: Any) -> dict[str, Any]:
    ib = _try(lambda: pipe.GetIBuffer())
    vbs = _try(lambda: pipe.GetVBuffers(), [])
    attrs = _try(lambda: pipe.GetVertexInputs(), [])
    data: dict[str, Any] = {}
    if ib is not None:
        data["index_buffer"] = {
            "resource_id": rid_str(ib.resourceId),
            "byte_offset": int(ib.byteOffset),
            "byte_stride": int(ib.byteStride),
            "byte_size": int(ib.byteSize),
        }
    data["vertex_buffers"] = []
    for i, vb in enumerate(vbs):
        data["vertex_buffers"].append(
            {
                "slot": i,
                "resource_id": rid_str(vb.resourceId),
                "byte_offset": int(vb.byteOffset),
                "byte_stride": int(vb.byteStride),
                "byte_size": int(vb.byteSize),
            }
        )
    data["attributes"] = []
    for a in attrs:
        data["attributes"].append(
            {
                "location": int(getattr(a, "location", 0)),
                "vertex_buffer_slot": int(getattr(a, "vertexBufferSlot", 0)),
                "byte_offset": int(getattr(a, "byteOffset", 0)),
                "per_instance": bool(getattr(a, "perInstance", False)),
                "format": enum_name(getattr(a, "format", None)),
            }
        )
    topo = _try(lambda: pipe.GetTopology())
    data["topology"] = enum_name(topo) if topo is not None else None
    return data


def bindings_for_stage(pipe: Any, stage: Any) -> dict[str, Any]:
    ro = _try(lambda: pipe.GetReadOnlyResources(stage), [])
    rw = _try(lambda: pipe.GetReadWriteResources(stage), [])
    samp = _try(lambda: pipe.GetSamplers(stage), [])
    cblocks = []
    for idx in range(32):
        cb = _try(lambda i=idx: pipe.GetConstantBlock(stage, i, 0))
        if cb is None:
            break
        rid = rid_str(cb.descriptor.resource) if getattr(cb, "descriptor", None) else ""
        if rid in ("", "Null"):
            continue
        cblocks.append({"slot": idx, "resource_id": rid})
    return {
        "readonly": [serialize_used_descriptor(x) for x in ro],
        "readwrite": [serialize_used_descriptor(x) for x in rw],
        "samplers": [serialize_used_descriptor(x) for x in samp],
        "constant_blocks": cblocks,
    }


def summarize_action(rd: Any, controller: Any, structured_file: Any, event_id: int) -> dict[str, Any] | None:
    act = find_action(controller, event_id)
    if act is None:
        return None
    outs = [rid_str(o) for o in getattr(act, "outputs", [])]
    return {
        "event_id": event_id,
        "name": act.GetName(structured_file),
        "flags": expand_action_flags(rd, int(act.flags)),
        "outputs": outs,
        "topology": enum_name(act.topology) if getattr(act, "topology", None) is not None else None,
        "num_vertices": int(getattr(act, "numVertices", 0)),
        "num_instances": int(getattr(act, "numInstances", 0)),
        "num_indices": int(getattr(act, "numIndices", 0)),
        "vertex_offset": int(getattr(act, "vertexOffset", 0)),
        "instance_offset": int(getattr(act, "instanceOffset", 0)),
        "index_offset": int(getattr(act, "indexOffset", 0)),
        "base_vertex": int(getattr(act, "baseVertex", 0)),
    }


def normalize_pipeline_state(controller: Any, structured_file: Any, event_id: int) -> dict[str, Any]:
    rd = get_renderdoc()
    pipe = controller.GetPipelineState()
    data: dict[str, Any] = {"event_id": event_id}
    data["action"] = summarize_action(rd, controller, structured_file, event_id)

    data["graphics_pipeline"] = rid_str(_try(lambda: pipe.GetGraphicsPipelineObject()))
    data["compute_pipeline"] = rid_str(_try(lambda: pipe.GetComputePipelineObject()))

    vps = []
    for i in range(16):
        vp = _try(lambda idx=i: pipe.GetViewport(idx))
        if vp is None:
            break
        vps.append(serialize_viewport(vp))
    data["viewports"] = vps

    scissors = []
    for i in range(16):
        sc = _try(lambda idx=i: pipe.GetScissor(idx))
        if sc is None:
            break
        scissors.append(serialize_scissor(sc))
    data["scissors"] = scissors

    data["targets"] = serialize_graphics_targets(pipe)
    data["rasterizer"] = serialize_rasterizer(pipe)
    data["depth"] = serialize_depth_state(pipe)
    data["stencil"] = serialize_stencil_state(pipe)
    data["blend"] = serialize_blend_state(pipe)
    data["vertex_inputs"] = serialize_vertex_inputs(pipe)

    stages = collect_shader_stages(rd)
    data["shaders"] = [serialize_shader_stage_summary(rd, pipe, st) for st in stages]

    data["bindings_by_stage"] = {}
    for st in stages:
        try:
            if pipe.GetShaderReflection(st) is None:
                continue
        except Exception:
            continue
        key = enum_name(st).lower()
        data["bindings_by_stage"][key] = bindings_for_stage(pipe, st)

    data["probable_causes"] = heuristic_pipeline_issues(data)
    return data


def heuristic_pipeline_issues(snapshot: dict[str, Any]) -> list[str]:
    hints: list[str] = []
    vps = snapshot.get("viewports") or []
    if vps:
        vp = vps[0]
        if vp["width"] <= 0 or vp["height"] <= 0:
            hints.append("Viewport has zero width or height at slot 0.")
    scissors = snapshot.get("scissors") or []
    if scissors:
        sc = scissors[0]
        if sc.get("enabled") and (sc["width"] == 0 or sc["height"] == 0):
            hints.append("Enabled scissor has zero area at slot 0.")
    depth = snapshot.get("depth") or {}
    if depth.get("depth_enable") and depth.get("depth_function") == "Never":
        hints.append("Depth test enabled with CompareFunction Never.")
    blend = snapshot.get("blend") or {}
    for t in blend.get("targets") or []:
        if int(t.get("write_mask") or 0xF) == 0:
            hints.append("Color write mask is 0 for at least one RT slot.")
    colors = (snapshot.get("targets") or {}).get("color_targets") or []
    if colors and all(c.get("resource_id") in ("Null", "") for c in colors):
        hints.append("No bound color targets (all Null).")
    return hints


def normalize_bound_resources(controller: Any, structured_file: Any, event_id: int) -> dict[str, Any]:
    rd = get_renderdoc()
    pipe = controller.GetPipelineState()
    stages = collect_shader_stages(rd)
    out: dict[str, Any] = {"event_id": event_id, "by_stage": {}, "merged_resources": []}
    merged: set[str] = set()

    for st in stages:
        try:
            if pipe.GetShaderReflection(st) is None:
                continue
        except Exception:
            continue
        key = enum_name(st).lower()
        bd = bindings_for_stage(pipe, st)
        out["by_stage"][key] = bd
        for bucket in ("readonly", "readwrite", "constant_blocks"):
            if bucket == "constant_blocks":
                for cb in bd.get("constant_blocks", []):
                    rid = cb.get("resource_id")
                    if rid and rid != "Null":
                        merged.add(rid)
            else:
                for u in bd.get(bucket, []):
                    rid = u.get("resource_id")
                    if rid and rid != "Null":
                        merged.add(rid)

    tg = serialize_graphics_targets(pipe)
    for c in tg.get("color_targets") or []:
        if c["resource_id"] not in ("Null", ""):
            merged.add(c["resource_id"])
    dt = tg.get("depth_target")
    if dt and dt.get("resource_id") not in ("Null", "", None):
        merged.add(dt["resource_id"])

    vi = serialize_vertex_inputs(pipe)
    if vi.get("index_buffer"):
        ib = vi["index_buffer"]["resource_id"]
        if ib not in ("Null", ""):
            merged.add(ib)
    for vb in vi.get("vertex_buffers") or []:
        if vb["resource_id"] not in ("Null", ""):
            merged.add(vb["resource_id"])

    out["merged_resources"] = sorted(merged)
    out["targets"] = tg
    out["vertex_inputs"] = vi
    return out


def serialize_shader_reflection_summary(refl: Any, *, max_resources: int = 64) -> dict[str, Any]:
    if refl is None:
        return {}
    rd = get_renderdoc()
    out: dict[str, Any] = {
        "resource_id": rid_str(getattr(refl, "resourceId", rd.ResourceId.Null())),
    }
    cb = getattr(refl, "constantBlocks", None) or []
    out["constant_blocks"] = []
    for i, block in enumerate(cb[:max_resources]):
        out["constant_blocks"].append(
            {
                "index": i,
                "name": getattr(block, "name", ""),
                "bind_point": int(getattr(block, "fixedBindNumber", -1)),
            }
        )
    ro = getattr(refl, "readOnlyResources", None) or []
    rw = getattr(refl, "readWriteResources", None) or []
    out["readonly_bindings"] = [getattr(x, "name", str(x)) for x in ro[:max_resources]]
    out["readwrite_bindings"] = [getattr(x, "name", str(x)) for x in rw[:max_resources]]
    return out
