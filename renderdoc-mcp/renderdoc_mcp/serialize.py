"""Normalize RenderDoc PipeState and related objects to JSON-safe dicts."""

from __future__ import annotations

from typing import Any

from renderdoc_mcp.rdutil import (
    enrich_resource_dict,
    enum_name,
    get_renderdoc,
    resource_name_for,
    resource_name_map,
    rid_str,
)
from renderdoc_mcp.session import expand_action_flags, find_action
from renderdoc_mcp.analysis import detect_pipeline_anomalies


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


def serialize_used_descriptor(d: Any, controller: Any) -> dict[str, Any]:
    out: dict[str, Any] = {
        "bind_type": enum_name(getattr(d, "bindType", None)),
        "direct_access": bool(getattr(d, "directAccess", False)),
    }
    desc = getattr(d, "descriptor", None)
    if desc is not None:
        enrich_resource_dict(controller, out, getattr(desc, "resource", None))
        out["byte_offset"] = int(getattr(desc, "byteOffset", 0))
        out["byte_size"] = int(getattr(desc, "byteSize", 0))
        # desc.format is a ResourceFormat struct, not an enum -- enum_name(fmt) on the struct
        # itself falls through to str(fmt), a raw "<Swig Object ...>" repr, since ResourceFormat
        # has no `.name` attribute (only `.type`/`.compCount`/`.compByteWidth` and a Name() method
        # enum_name() doesn't call). See renderdoc-mcp/TODO.md #11.
        fmt = getattr(desc, "format", None)
        if fmt is not None:
            out["format"] = enum_name(getattr(fmt, "type", None))
            out["format_compcount"] = int(getattr(fmt, "compCount", 0) or 0)
            out["format_bytewidth"] = int(getattr(fmt, "compByteWidth", 0) or 0)
        tid = getattr(desc, "texelBufferStructureSize", None)
        if tid is not None:
            out["texel_buffer_structure_size"] = int(tid)
    return out


def serialize_shader_stage_summary(rd: Any, pipe: Any, stage: Any, controller: Any) -> dict[str, Any]:
    info: dict[str, Any] = {"stage": enum_name(stage)}
    refl = _try(lambda: pipe.GetShaderReflection(stage))
    if refl is None:
        info["bound"] = False
        return info
    info["bound"] = True
    enrich_resource_dict(controller, info, getattr(refl, "resourceId", rd.ResourceId.Null()))
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


def serialize_graphics_targets(pipe: Any, controller: Any) -> dict[str, Any]:
    rd = get_renderdoc()
    data: dict[str, Any] = {}
    outs = _try(lambda: pipe.GetOutputTargets(), [])
    data["color_targets"] = []
    for i, o in enumerate(outs):
        ct: dict[str, Any] = {"slot": i, "slice": int(getattr(o, "slice", 0)), "mipslice": int(getattr(o, "mipslice", 0))}
        enrich_resource_dict(controller, ct, getattr(o, "resource", None))
        data["color_targets"].append(ct)
    dt = _try(lambda: pipe.GetDepthTarget())
    if dt is not None:
        dd: dict[str, Any] = {
            "slice": int(getattr(dt, "slice", 0)),
            "mipslice": int(getattr(dt, "mipslice", 0)),
        }
        enrich_resource_dict(controller, dd, getattr(dt, "resource", None))
        data["depth_target"] = dd
    else:
        data["depth_target"] = None
    ss = _try(lambda: pipe.GetStencilTarget())
    if ss is not None:
        st: dict[str, Any] = {}
        enrich_resource_dict(controller, st, getattr(ss, "resource", None))
        data["stencil_target"] = st
    else:
        data["stencil_target"] = None

    return data


def _state_unavailable(methods: list[str]) -> dict[str, Any]:
    return {
        "available": False,
        "reason": "PipeState does not expose {}".format(" or ".join(methods)),
    }


def serialize_rasterizer(pipe: Any) -> dict[str, Any]:
    r = _try(lambda: pipe.GetRasterizer())
    if r is None:
        r = _try(lambda: pipe.GetRasterState())
    if r is None:
        return _state_unavailable(["GetRasterizer", "GetRasterState"])
    return {
        "available": True,
        "fill_mode": enum_name(getattr(r, "fillMode", None)),
        "cull_mode": enum_name(getattr(r, "cullMode", None)),
        "front_ccw": bool(getattr(r, "frontCCW", False)),
        "depth_clip": bool(getattr(r, "depthClip", True)),
        "depth_bias": float(getattr(r, "depthBias", 0.0)),
        "depth_bias_clamp": float(getattr(r, "depthBiasClamp", 0.0)),
        "slope_scaled_depth_bias": float(getattr(r, "slopeScaledDepthBias", 0.0)),
        "line_width": float(getattr(r, "lineWidth", 1.0)),
        "forced_sample_count": int(getattr(r, "forcedSampleCount", 0) or 0),
        "conservative_rasterization": enum_name(getattr(r, "conservativeRasterization", None)),
    }


def serialize_depth_state(pipe: Any) -> dict[str, Any]:
    d = _try(lambda: pipe.GetDepthState())
    if d is None:
        d = _try(lambda: pipe.GetDepthTestState())
    if d is None:
        return _state_unavailable(["GetDepthState", "GetDepthTestState"])
    return {
        "available": True,
        "depth_enable": bool(getattr(d, "depthEnable", False)),
        "depth_writes": bool(getattr(d, "depthWrites", False)),
        "depth_function": enum_name(getattr(d, "depthFunction", None)),
        "depth_bounds_enable": bool(getattr(d, "depthBoundsEnable", False)),
        "min_depth_bounds": float(getattr(d, "minDepthBounds", 0.0)),
        "max_depth_bounds": float(getattr(d, "maxDepthBounds", 1.0)),
    }


def _serialize_blend_target(bt: Any, slot: int) -> dict[str, Any]:
    cb_eq = getattr(bt, "colorBlend", None)
    ab_eq = getattr(bt, "alphaBlend", None)
    return {
        "slot": slot,
        "blend_enable": bool(getattr(bt, "blendEnable", getattr(bt, "enabled", False))),
        "logic_operation_enable": bool(getattr(bt, "logicOperationEnabled", False)),
        "logic_operation": enum_name(getattr(bt, "logicOperation", None)),
        "write_mask": int(getattr(bt, "writeMask", 0)),
        "src_color": enum_name(getattr(cb_eq, "source", None)) if cb_eq is not None else "",
        "dst_color": enum_name(getattr(cb_eq, "destination", None)) if cb_eq is not None else "",
        "color_op": enum_name(getattr(cb_eq, "operation", None)) if cb_eq is not None else "",
        "src_alpha": enum_name(getattr(ab_eq, "source", None)) if ab_eq is not None else "",
        "dst_alpha": enum_name(getattr(ab_eq, "destination", None)) if ab_eq is not None else "",
        "alpha_op": enum_name(getattr(ab_eq, "operation", None)) if ab_eq is not None else "",
    }


def serialize_blend_state(pipe: Any) -> dict[str, Any]:
    b = _try(lambda: pipe.GetBlendState())
    color_blends = _try(lambda: pipe.GetColorBlends())
    if b is None and color_blends is None:
        return _state_unavailable(["GetBlendState", "GetColorBlends"])
    targets = []
    bl = list(getattr(b, "blends", None) or color_blends or [])
    for i, bt in enumerate(bl):
        targets.append(_serialize_blend_target(bt, i))
    blend_factor = getattr(b, "blendFactor", None) if b is not None else _try(lambda: pipe.GetBlendFactor())
    return {
        "available": True,
        "alpha_to_coverage": bool(getattr(b, "alphaToCoverage", False)) if b is not None else None,
        "independent_blend": (
            bool(getattr(b, "independentBlend", False))
            if b is not None
            else bool(_try(lambda: pipe.IsIndependentBlendingEnabled(), False))
        ),
        "blend_factor": [float(x) for x in blend_factor] if blend_factor is not None else None,
        "targets": targets,
    }


def _serialize_stencil_face(face: Any) -> dict[str, Any]:
    if face is None:
        return {}
    return {
        "function": enum_name(getattr(face, "function", None)),
        "fail_operation": enum_name(getattr(face, "failOperation", None)),
        "depth_fail_operation": enum_name(getattr(face, "depthFailOperation", None)),
        "pass_operation": enum_name(getattr(face, "passOperation", None)),
        "compare_mask": int(getattr(face, "compareMask", 0) or 0),
        "write_mask": int(getattr(face, "writeMask", 0) or 0),
        "reference": int(getattr(face, "reference", 0) or 0),
    }


def serialize_stencil_state(pipe: Any) -> dict[str, Any]:
    s = _try(lambda: pipe.GetStencilState())
    if s is None:
        s = _try(lambda: pipe.GetDepthTestState())
    faces = _try(lambda: pipe.GetStencilFaces())
    if s is None:
        enabled = _try(lambda: pipe.IsStencilTestEnabled())
        if enabled is None and faces is None:
            return _state_unavailable(["GetStencilState", "GetDepthTestState", "GetStencilFaces"])
        return {
            "available": True,
            "stencil_enable": bool(enabled),
            "front_face": _serialize_stencil_face(faces[0] if faces else None),
            "back_face": _serialize_stencil_face(faces[1] if faces and len(faces) > 1 else None),
        }
    front = getattr(s, "frontFace", None)
    back = getattr(s, "backFace", None)
    if faces:
        front = front or faces[0]
        back = back or (faces[1] if len(faces) > 1 else None)
    return {
        "available": True,
        "stencil_enable": bool(getattr(s, "stencilEnable", False)),
        "front_face": _serialize_stencil_face(front),
        "back_face": _serialize_stencil_face(back),
    }


def serialize_vertex_inputs(pipe: Any, controller: Any) -> dict[str, Any]:
    ib = _try(lambda: pipe.GetIBuffer())
    vbs = _try(lambda: pipe.GetVBuffers(), [])
    attrs = _try(lambda: pipe.GetVertexInputs(), [])
    data: dict[str, Any] = {}
    if ib is not None:
        ibd: dict[str, Any] = {
            "byte_offset": int(ib.byteOffset),
            "byte_stride": int(ib.byteStride),
            "byte_size": int(ib.byteSize),
        }
        enrich_resource_dict(controller, ibd, ib.resourceId)
        data["index_buffer"] = ibd
    data["vertex_buffers"] = []
    for i, vb in enumerate(vbs):
        vbd: dict[str, Any] = {
            "slot": i,
            "byte_offset": int(vb.byteOffset),
            "byte_stride": int(vb.byteStride),
            "byte_size": int(vb.byteSize),
        }
        enrich_resource_dict(controller, vbd, vb.resourceId)
        data["vertex_buffers"].append(vbd)
    data["attributes"] = []
    for a in attrs:
        # a.format is a ResourceFormat struct, not an enum -- see the identical fix/comment in
        # serialize_used_descriptor above. Was previously serializing as a raw "<Swig Object ...>"
        # repr. See renderdoc-mcp/TODO.md #11.
        fmt = getattr(a, "format", None)
        attr_row: dict[str, Any] = {
            "location": int(getattr(a, "location", 0)),
            "vertex_buffer_slot": int(getattr(a, "vertexBuffer", 0)),
            "byte_offset": int(getattr(a, "byteOffset", 0)),
            "per_instance": bool(getattr(a, "perInstance", False)),
        }
        if fmt is not None:
            attr_row["format"] = enum_name(getattr(fmt, "type", None))
            attr_row["format_compcount"] = int(getattr(fmt, "compCount", 0) or 0)
            attr_row["format_bytewidth"] = int(getattr(fmt, "compByteWidth", 0) or 0)
        data["attributes"].append(attr_row)
    topo = _try(lambda: pipe.GetTopology())
    data["topology"] = enum_name(topo) if topo is not None else None
    return data


def bindings_for_stage(pipe: Any, stage: Any, controller: Any) -> dict[str, Any]:
    ro = _try(lambda: pipe.GetReadOnlyResources(stage), [])
    rw = _try(lambda: pipe.GetReadWriteResources(stage), [])
    samp = _try(lambda: pipe.GetSamplers(stage), [])
    cblocks = []
    for idx in range(32):
        cb = _try(lambda i=idx: pipe.GetConstantBlock(stage, i, 0))
        if cb is None:
            break
        if not getattr(cb, "descriptor", None):
            continue
        res = cb.descriptor.resource
        rid = rid_str(res)
        if rid in ("", "Null"):
            continue
        row: dict[str, Any] = {"slot": idx, "resource_id": rid}
        name = resource_name_for(controller, res)
        if name:
            row["resource_name"] = name
        cblocks.append(row)
    return {
        "readonly": [serialize_used_descriptor(x, controller) for x in ro],
        "readwrite": [serialize_used_descriptor(x, controller) for x in rw],
        "samplers": [serialize_used_descriptor(x, controller) for x in samp],
        "constant_blocks": cblocks,
    }


def summarize_action(rd: Any, controller: Any, structured_file: Any, event_id: int) -> dict[str, Any] | None:
    act = find_action(controller, event_id)
    if act is None:
        return None
    outs: list[dict[str, Any]] = []
    for o in getattr(act, "outputs", []):
        one: dict[str, Any] = {}
        enrich_resource_dict(controller, one, o)
        outs.append(one)
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

    gp_rid = _try(lambda: pipe.GetGraphicsPipelineObject())
    cp_rid = _try(lambda: pipe.GetComputePipelineObject())
    data["graphics_pipeline"] = rid_str(gp_rid)
    gn = resource_name_for(controller, gp_rid)
    if gn:
        data["graphics_pipeline_name"] = gn
    data["compute_pipeline"] = rid_str(cp_rid)
    cn = resource_name_for(controller, cp_rid)
    if cn:
        data["compute_pipeline_name"] = cn

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

    data["targets"] = serialize_graphics_targets(pipe, controller)
    data["rasterizer"] = serialize_rasterizer(pipe)
    data["depth"] = serialize_depth_state(pipe)
    data["stencil"] = serialize_stencil_state(pipe)
    data["blend"] = serialize_blend_state(pipe)
    data["vertex_inputs"] = serialize_vertex_inputs(pipe, controller)

    stages = collect_shader_stages(rd)
    data["shaders"] = [serialize_shader_stage_summary(rd, pipe, st, controller) for st in stages]

    data["bindings_by_stage"] = {}
    for st in stages:
        try:
            if pipe.GetShaderReflection(st) is None:
                continue
        except Exception:
            continue
        key = enum_name(st).lower()
        data["bindings_by_stage"][key] = bindings_for_stage(pipe, st, controller)

    data["probable_causes"] = heuristic_pipeline_issues(data)
    data["anomalies"] = detect_pipeline_anomalies(data)
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
    if depth.get("available", True) and depth.get("depth_enable") and depth.get("depth_function") == "Never":
        hints.append("Depth test enabled with CompareFunction Never.")
    blend = snapshot.get("blend") or {}
    if blend.get("available", True):
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
        bd = bindings_for_stage(pipe, st, controller)
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

    tg = serialize_graphics_targets(pipe, controller)
    for c in tg.get("color_targets") or []:
        if c["resource_id"] not in ("Null", ""):
            merged.add(c["resource_id"])
    dt = tg.get("depth_target")
    if dt and dt.get("resource_id") not in ("Null", "", None):
        merged.add(dt["resource_id"])

    vi = serialize_vertex_inputs(pipe, controller)
    if vi.get("index_buffer"):
        ib = vi["index_buffer"]["resource_id"]
        if ib not in ("Null", ""):
            merged.add(ib)
    for vb in vi.get("vertex_buffers") or []:
        if vb["resource_id"] not in ("Null", ""):
            merged.add(vb["resource_id"])

    merged_rows: list[dict[str, str]] = []
    for rid_s in sorted(merged):
        rowm: dict[str, str] = {"resource_id": rid_s}
        name = resource_name_map(controller).get(rid_s, "")
        if name:
            rowm["resource_name"] = name
        merged_rows.append(rowm)
    out["merged_resources"] = merged_rows
    out["targets"] = tg
    out["vertex_inputs"] = vi
    return out


def build_draw_state_row(controller: Any, structured_file: Any, event_id: int) -> dict[str, Any]:
    """Compact per-draw pipeline snapshot for list_draws_with_state/diff_draw_sequences.

    Deliberately narrower than normalize_pipeline_state (~10KB/call): just VB/IB bindings, RT/DS,
    topology, and shader constant-block names, so scanning/diffing many draws stays cheap.
    """
    rd = get_renderdoc()
    pipe = controller.GetPipelineState()
    act = find_action(controller, event_id)
    name = act.GetName(structured_file) if act is not None else ""

    vi = serialize_vertex_inputs(pipe, controller)
    targets = serialize_graphics_targets(pipe, controller)
    topo = _try(lambda: pipe.GetTopology())

    shaders: dict[str, Any] = {}
    for st in collect_shader_stages(rd):
        refl = _try(lambda s=st: pipe.GetShaderReflection(s))
        if refl is None:
            continue
        entry: dict[str, Any] = {}
        enrich_resource_dict(controller, entry, getattr(refl, "resourceId", rd.ResourceId.Null()))
        entry["constant_blocks"] = [
            str(getattr(cb, "name", "") or "") for cb in (getattr(refl, "constantBlocks", None) or [])
        ]
        shaders[enum_name(st).lower()] = entry

    return {
        "event_id": int(event_id),
        "name": name,
        "topology": enum_name(topo) if topo is not None else None,
        "vertex_buffers": vi.get("vertex_buffers", []),
        "index_buffer": vi.get("index_buffer"),
        "color_targets": targets.get("color_targets", []),
        "depth_target": targets.get("depth_target"),
        "shaders": shaders,
    }


def serialize_descriptor(controller: Any, d: Any) -> dict[str, Any]:
    """Serialize a ``Descriptor`` (resource/image/buffer descriptor) from GetDescriptors()."""
    out: dict[str, Any] = {
        "type": enum_name(getattr(d, "type", None)),
        "flags": int(getattr(d, "flags", 0) or 0),
    }
    enrich_resource_dict(controller, out, getattr(d, "resource", None))
    sec = getattr(d, "secondary", None)
    if sec is not None:
        rid = rid_str(sec)
        if rid not in ("", "Null"):
            out["secondary_resource_id"] = rid
            name = resource_name_for(controller, sec)
            if name:
                out["secondary_resource_name"] = name
    view = getattr(d, "view", None)
    if view is not None:
        rid = rid_str(view)
        if rid not in ("", "Null"):
            out["view_resource_id"] = rid
    fmt = getattr(d, "format", None)
    if fmt is not None:
        out["format"] = enum_name(getattr(fmt, "type", None))
        out["format_compcount"] = int(getattr(fmt, "compCount", 0) or 0)
        out["format_bytewidth"] = int(getattr(fmt, "compByteWidth", 0) or 0)
    out["byte_offset"] = int(getattr(d, "byteOffset", 0) or 0)
    out["byte_size"] = int(getattr(d, "byteSize", 0) or 0)
    out["element_byte_size"] = int(getattr(d, "elementByteSize", 0) or 0)
    out["first_slice"] = int(getattr(d, "firstSlice", 0) or 0)
    out["num_slices"] = int(getattr(d, "numSlices", 1) or 1)
    out["first_mip"] = int(getattr(d, "firstMip", 0) or 0)
    out["num_mips"] = int(getattr(d, "numMips", 1) or 1)
    out["texture_type"] = enum_name(getattr(d, "textureType", None))
    return out


def serialize_sampler_descriptor(controller: Any, d: Any) -> dict[str, Any]:
    """Serialize a ``SamplerDescriptor`` from GetSamplerDescriptors()."""
    out: dict[str, Any] = {"type": enum_name(getattr(d, "type", None))}
    enrich_resource_dict(controller, out, getattr(d, "object", None))
    out["address_u"] = enum_name(getattr(d, "addressU", None))
    out["address_v"] = enum_name(getattr(d, "addressV", None))
    out["address_w"] = enum_name(getattr(d, "addressW", None))
    out["compare_function"] = enum_name(getattr(d, "compareFunction", None))
    out["max_anisotropy"] = float(getattr(d, "maxAnisotropy", 0) or 0)
    out["min_lod"] = float(getattr(d, "minLOD", 0) or 0)
    out["max_lod"] = float(getattr(d, "maxLOD", 0) or 0)
    out["mip_bias"] = float(getattr(d, "mipBias", 0) or 0)
    out["srgb_border"] = bool(getattr(d, "srgbBorder", False))
    out["seamless_cubemaps"] = bool(getattr(d, "seamlessCubemaps", True))
    out["unnormalized"] = bool(getattr(d, "unnormalized", False))
    return out


_VAR_TYPE_BYTE_WIDTH = {
    "Float": 4, "UInt": 4, "SInt": 4, "Bool": 4,
    "Double": 8, "ULong": 8, "SLong": 8, "GPUPointer": 8,
    "Half": 2, "UShort": 2, "SShort": 2,
    "UByte": 1, "SByte": 1,
}


def serialize_shader_constant(
    const: Any, *, max_depth: int = 8, max_members: int = 128, _depth: int = 0
) -> dict[str, Any]:
    """A single CB variable's name/offset/type/size, recursing into struct members.

    byteOffset is relative to the immediate parent (struct or CB root), matching the API's own
    documented semantics -- not accumulated into an absolute CB offset.
    """
    t = getattr(const, "type", None)
    rows = int(getattr(t, "rows", 1) or 1)
    cols = int(getattr(t, "columns", 1) or 1)
    elements = int(getattr(t, "elements", 1) or 1)
    array_stride = int(getattr(t, "arrayByteStride", 0) or 0)
    base_type_name = enum_name(getattr(t, "baseType", None))

    out: dict[str, Any] = {
        "name": str(getattr(const, "name", "") or ""),
        "byte_offset": int(getattr(const, "byteOffset", 0) or 0),
        "type_name": str(getattr(t, "name", "") or ""),
        "base_type": base_type_name,
        "rows": rows,
        "columns": cols,
        "elements": elements,
    }

    members = list(getattr(t, "members", None) or [])
    single_elem_size: int
    if members:
        if _depth >= max_depth:
            out["members_truncated"] = "max_depth"
            single_elem_size = 0
        else:
            out["members"] = [
                serialize_shader_constant(m, max_depth=max_depth, max_members=max_members, _depth=_depth + 1)
                for m in members[:max_members]
            ]
            if len(members) > max_members:
                out["members_truncated"] = "max_members"
            # a struct's own size isn't a direct field -- approximate as the sum of its
            # (already-computed) immediate members' sizes, rather than treating it as a scalar.
            single_elem_size = sum(m["byte_size"] for m in out["members"])
    else:
        single_elem_size = _VAR_TYPE_BYTE_WIDTH.get(base_type_name, 4) * rows * cols

    if elements > 1:
        out["byte_size"] = array_stride * elements if array_stride else single_elem_size * elements
    else:
        out["byte_size"] = single_elem_size
    return out


def serialize_sig_parameter(sp: Any) -> dict[str, Any]:
    """An input/output signature element -- semantic name distinguishes e.g. COLOR from TEXCOORD."""
    return {
        "var_name": str(getattr(sp, "varName", "") or ""),
        "semantic_name": str(getattr(sp, "semanticName", "") or ""),
        "semantic_index": int(getattr(sp, "semanticIndex", 0) or 0),
        "reg_index": int(getattr(sp, "regIndex", 0) or 0),
        "system_value": enum_name(getattr(sp, "systemValue", None)),
        "var_type": enum_name(getattr(sp, "varType", None)),
        "comp_count": int(getattr(sp, "compCount", 0) or 0),
    }


def serialize_shader_sampler(sampler: Any) -> dict[str, Any]:
    return {
        "name": str(getattr(sampler, "name", "") or ""),
        "bind_point": int(getattr(sampler, "fixedBindNumber", 0) or 0),
        "bind_set_or_space": int(getattr(sampler, "fixedBindSetOrSpace", 0) or 0),
        "array_size": int(getattr(sampler, "bindArraySize", 1) or 1),
    }


def serialize_shader_reflection_summary(
    refl: Any, controller: Any | None = None, *, max_resources: int = 64
) -> dict[str, Any]:
    if refl is None:
        return {}
    rd = get_renderdoc()
    out: dict[str, Any] = {}
    rid = getattr(refl, "resourceId", rd.ResourceId.Null())
    enrich_resource_dict(controller, out, rid) if controller is not None else out.update(
        {"resource_id": rid_str(rid)}
    )
    cb = getattr(refl, "constantBlocks", None) or []
    out["constant_blocks"] = []
    for i, block in enumerate(cb[:max_resources]):
        variables = list(getattr(block, "variables", None) or [])
        out["constant_blocks"].append(
            {
                "index": i,
                "name": getattr(block, "name", ""),
                "bind_point": int(getattr(block, "fixedBindNumber", -1)),
                "byte_size": int(getattr(block, "byteSize", 0) or 0),
                "variables": [serialize_shader_constant(v) for v in variables[:max_resources]],
            }
        )
    ro = getattr(refl, "readOnlyResources", None) or []
    rw = getattr(refl, "readWriteResources", None) or []
    out["readonly_bindings"] = [getattr(x, "name", str(x)) for x in ro[:max_resources]]
    out["readwrite_bindings"] = [getattr(x, "name", str(x)) for x in rw[:max_resources]]

    samplers = getattr(refl, "samplers", None) or []
    out["samplers"] = [serialize_shader_sampler(s) for s in samplers[:max_resources]]

    in_sig = getattr(refl, "inputSignature", None) or []
    out_sig = getattr(refl, "outputSignature", None) or []
    out["input_signature"] = [serialize_sig_parameter(sp) for sp in in_sig[:max_resources]]
    out["output_signature"] = [serialize_sig_parameter(sp) for sp in out_sig[:max_resources]]

    return out
