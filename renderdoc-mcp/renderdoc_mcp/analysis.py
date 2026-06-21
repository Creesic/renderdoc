"""Texture statistics, pipeline diffing, pixel history normalization, visibility helpers."""

from __future__ import annotations

import math
import struct
from typing import Any

from renderdoc_mcp.rdutil import enum_name, get_renderdoc, parse_resource_id, resource_name_for, rid_str


def find_texture_description(controller: Any, tex_id: Any) -> Any | None:
    for t in controller.GetTextures():
        if t.resourceId == tex_id:
            return t
    return None


def _pixel_stride(fmt: Any) -> int | None:
    """Return bytes per pixel if layout is a simple fixed RGBA-like format."""
    rd = get_renderdoc()
    try:
        special = getattr(fmt, "Special", None)
        if callable(special):
            try:
                if special():
                    return None
            except TypeError:
                pass
        elif special:
            return None
        count = int(fmt.compCount)
        bw = int(fmt.compByteWidth)
        return count * bw
    except Exception:
        return None


def _as_floats_r10g10b10a2(px: bytes, normalize: bool) -> tuple[float, float, float, float]:
    """Unpack a 4-byte R10G10B10A2 pixel into (R, G, B, A) floats."""
    v = struct.unpack_from("<I", px, 0)[0]
    r = (v >> 0) & 0x3FF
    g = (v >> 10) & 0x3FF
    b = (v >> 20) & 0x3FF
    a = (v >> 30) & 0x003
    if normalize:
        return r / 1023.0, g / 1023.0, b / 1023.0, a / 3.0
    return float(r), float(g), float(b), float(a)


def analyze_texture_bytes(tex: Any, raw: bytes, *, max_pixels: int = 4_194_304) -> dict[str, Any]:
    rd = get_renderdoc()
    fmt = tex.format
    w = max(1, int(tex.width))
    h = max(1, int(tex.height))
    stride = _pixel_stride(fmt)

    # R10G10B10A2: 4-byte packed format, 10+10+10+2 bits
    packed_r10g10b10a2 = False
    if stride is None:
        fmt_type_name = enum_name(fmt.type) if fmt else ""
        if "R10G10B10A2" in fmt_type_name:
            stride = 4
            packed_r10g10b10a2 = True

    if stride is None or len(raw) < stride:
        return {
            "supported_stats": False,
            "reason": "complex_or_packed_format",
            "byte_length": len(raw),
            "format": enum_name(fmt.type) if fmt else None,
        }

    comp_type = fmt.compType
    normalize_r10 = enum_name(comp_type) == "UNorm"
    bpp = stride
    row_pitch = w * bpp
    expected = row_pitch * h
    sampled = False
    step = 1
    total_px = w * h
    if total_px > max_pixels:
        step = int(math.ceil(math.sqrt(total_px / max_pixels)))
        sampled = True

    mins = [math.inf, math.inf, math.inf, math.inf]
    maxs = [-math.inf, -math.inf, -math.inf, -math.inf]
    sums = [0.0, 0.0, 0.0, 0.0]
    count_px = 0
    nan_ct = 0
    black_ct = 0

    def as_floats(px: bytes) -> tuple[float, float, float, float]:
        if packed_r10g10b10a2:
            return _as_floats_r10g10b10a2(px, normalize_r10)
        ct = comp_type
        bw = int(fmt.compByteWidth)
        cc = int(fmt.compCount)
        vals = []
        off = 0
        for _ in range(cc):
            chunk = px[off : off + bw]
            off += bw
            if ct == rd.CompType.Float:
                if bw == 4:
                    vals.append(float(struct.unpack_from("<f", chunk, 0)[0]))
                elif bw == 8:
                    vals.append(float(struct.unpack_from("<d", chunk, 0)[0]))
                else:
                    vals.append(float("nan"))
            elif ct in (rd.CompType.UInt, rd.CompType.UNorm):
                if bw == 1:
                    v = struct.unpack_from("<B", chunk, 0)[0]
                elif bw == 2:
                    v = struct.unpack_from("<H", chunk, 0)[0]
                elif bw == 4:
                    v = struct.unpack_from("<I", chunk, 0)[0]
                else:
                    v = 0
                if ct == rd.CompType.UNorm:
                    denom = float((2 ** (bw * 8)) - 1)
                    vals.append(float(v) / denom if denom else 0.0)
                else:
                    vals.append(float(v))
            elif ct in (rd.CompType.SInt, rd.CompType.SNorm):
                if bw == 1:
                    v = struct.unpack_from("<b", chunk, 0)[0]
                elif bw == 2:
                    v = struct.unpack_from("<h", chunk, 0)[0]
                elif bw == 4:
                    v = struct.unpack_from("<i", chunk, 0)[0]
                else:
                    v = 0
                if ct == rd.CompType.SNorm:
                    max_neg = -float(2 ** (bw * 8)) / 2
                    divisor = float(-(max_neg - 1))
                    vals.append(float(v) / divisor if v != max_neg else float(v))
                else:
                    vals.append(float(v))
            else:
                vals.append(float("nan"))
        while len(vals) < 4:
            vals.append(0.0)
        return vals[0], vals[1], vals[2], vals[3]

    for y in range(0, h, step):
        row_off = y * row_pitch
        if row_off + row_pitch > len(raw):
            break
        for x in range(0, w, step):
            px_off = row_off + x * bpp
            if px_off + bpp > len(raw):
                break
            px = raw[px_off : px_off + bpp]
            r, g, b, a = as_floats(px)
            if any(math.isnan(v) for v in (r, g, b, a)):
                nan_ct += 1
            if not math.isnan(r + g + b) and abs(r) < 1e-6 and abs(g) < 1e-6 and abs(b) < 1e-6:
                black_ct += 1
            for i, v in enumerate((r, g, b, a)):
                if not math.isnan(v):
                    mins[i] = min(mins[i], v)
                    maxs[i] = max(maxs[i], v)
                    sums[i] += v
            count_px += 1

    mean = [sums[i] / max(1, count_px) for i in range(4)]
    black_ratio = float(black_ct) / max(1, count_px)

    return {
        "supported_stats": True,
        "dimensions": {"width": w, "height": h},
        "sampled": sampled,
        "sample_step": step,
        "pixels_considered": count_px,
        "byte_length": len(raw),
        "expected_min_bytes": expected,
        "format": {
            "type": enum_name(fmt.type),
            "comp_type": enum_name(fmt.compType),
            "comp_count": 4 if packed_r10g10b10a2 else int(fmt.compCount),
            "comp_byte_width": int(fmt.compByteWidth),
            **({"packed": "R10G10B10A2"} if packed_r10g10b10a2 else {}),
        },
        "min_channels": [None if math.isinf(m) else m for m in mins],
        "max_channels": [None if math.isinf(-m) else m for m in maxs],
        "mean_channels": mean,
        "nan_pixel_count": nan_ct,
        "near_black_pixel_count": black_ct,
        "near_black_ratio": black_ratio,
    }


def normalize_pixel_history(hist: Any) -> dict[str, Any]:
    entries = []
    for mod in list(hist):
        shader_out = getattr(mod, "shaderOut", None)
        pre = getattr(mod, "preMod", None)
        post = getattr(mod, "postMod", None)
        entries.append(
            {
                "event_id": int(getattr(mod, "eventId", 0)),
                "primitive_id": int(getattr(mod, "primitiveID", -1)),
                "shader_depth": float(getattr(mod, "shaderDepth", 0.0)),
                "shader_output": _summarize_modification_value(shader_out),
                "depth_test_failed": bool(getattr(mod, "depthTestFailed", False)),
                "backface_culled": bool(getattr(mod, "backfaceCulled", False)),
                "clipped": bool(getattr(mod, "clipped", False)),
                "stencil_test_failed": bool(getattr(mod, "stencilTestFailed", False)),
                "predicate_failed": bool(getattr(mod, "predicateFailed", False)),
                "pre_mod": _summarize_modification_value(pre),
                "post_mod": _summarize_modification_value(post),
            }
        )
    hypotheses = []
    if entries:
        last = entries[-1]
        if last.get("depth_test_failed"):
            hypotheses.append("Last contributing stage failed depth test.")
        if last.get("backface_culled"):
            hypotheses.append("Primitive was backface culled.")
        if last.get("clipped"):
            hypotheses.append("Primitive clipped before reaching target.")
    return {"entries": entries, "hypotheses": hypotheses, "count": len(entries)}


def _summarize_pixel_value_union(pv: Any) -> dict[str, Any]:
    """PixelValue is a C++ union exposed as floatValue / uintValue / intValue (not .value)."""
    if pv is None:
        return {}
    fv = getattr(pv, "floatValue", None)
    if fv is not None:
        try:
            return {"kind": "float", "rgba": [float(x) for x in fv]}
        except Exception:
            pass
    uv = getattr(pv, "uintValue", None)
    if uv is not None:
        try:
            return {"kind": "uint", "rgba": [int(x) for x in uv]}
        except Exception:
            pass
    iv = getattr(pv, "intValue", None)
    if iv is not None:
        try:
            return {"kind": "int", "rgba": [int(x) for x in iv]}
        except Exception:
            pass
    # Rare / older aliases
    for alt in ("f32Value", "u32Value"):
        chunk = getattr(pv, alt, None)
        if chunk is not None:
            try:
                return {"kind": alt.rstrip("Value"), "rgba": list(chunk)}
            except Exception:
                pass
    return {"kind": "unknown"}


def _summarize_modification_value(mv: Any) -> dict[str, Any] | None:
    """ModificationValue uses .col for RGBA (PixelValue); depth/stencil are scalar fields."""
    if mv is None:
        return None
    col = getattr(mv, "col", None)
    if col is None:
        col = getattr(mv, "color", None)
    valid = True
    is_valid_fn = getattr(mv, "IsValid", None)
    if callable(is_valid_fn):
        try:
            valid = bool(is_valid_fn())
        except Exception:
            valid = True
    return {
        "color": _summarize_pixel_value_union(col),
        "depth": float(getattr(mv, "depth", -1.0)),
        "stencil": int(getattr(mv, "stencil", -1)),
        "valid": valid,
    }


def _summarize_modval(v: Any) -> dict[str, Any] | None:
    """Accept PixelValue or ModificationValue (discriminate via ModificationValue.IsValid)."""
    if v is None:
        return None
    if callable(getattr(v, "IsValid", None)):
        return _summarize_modification_value(v)
    return {"color": _summarize_pixel_value_union(v)}


def deep_diff(a: Any, b: Any, path: str = "") -> list[dict[str, Any]]:
    changes: list[dict[str, Any]] = []
    if type(a) != type(b):
        changes.append({"path": path or ".", "before": a, "after": b})
        return changes
    if isinstance(a, dict):
        keys = set(a.keys()) | set(b.keys())
        for k in sorted(keys):
            p = "{}.{}".format(path, k) if path else str(k)
            if k not in a:
                changes.append({"path": p, "before": None, "after": b[k]})
            elif k not in b:
                changes.append({"path": p, "before": a[k], "after": None})
            else:
                changes.extend(deep_diff(a[k], b[k], p))
        return changes
    if isinstance(a, list):
        min_len = min(len(a), len(b))
        for i in range(min_len):
            changes.extend(deep_diff(a[i], b[i], "{}[{}]".format(path, i)))
        if len(a) != len(b):
            changes.append({"path": path + ".length", "before": len(a), "after": len(b)})
        return changes
    if a != b:
        changes.append({"path": path or ".", "before": a, "after": b})
    return changes


def diff_pipeline_snapshots(before: dict[str, Any], after: dict[str, Any]) -> dict[str, Any]:
    changes = deep_diff(before, after)
    important = [c for c in changes if any(x in c["path"] for x in ("targets", "depth", "blend", "viewports", "bindings_by_stage", "shaders"))]
    return {
        "change_count": len(changes),
        "changes": changes[:400],
        "truncated": len(changes) > 400,
        "important_changes": important[:120],
    }


def draw_visibility_analysis(
    controller: Any,
    structured_file: Any,
    pipe_snapshot: dict[str, Any],
    event_id: int,
) -> dict[str, Any]:
    rd = get_renderdoc()
    evidence: list[dict[str, Any]] = []
    unsupported_reason: str | None = None
    samples_passed: int | None = None

    counters = _try_list(lambda: controller.EnumerateCounters())
    if counters is not None and rd.GPUCounter.SamplesPassed in counters:
        try:
            results = controller.FetchCounters([rd.GPUCounter.SamplesPassed])
            for r in results:
                eid = getattr(r, "eventId", getattr(r, "event_id", None))
                if eid is None:
                    continue
                if int(eid) != int(event_id):
                    continue
                desc = controller.DescribeCounter(rd.GPUCounter.SamplesPassed)
                if desc.resultByteWidth == 8:
                    samples_passed = int(r.value.u64)
                else:
                    samples_passed = int(r.value.u32)
                evidence.append(
                    {
                        "kind": "gpu_counter",
                        "counter": "SamplesPassed",
                        "event_id": event_id,
                        "value": samples_passed,
                    }
                )
                break
        except Exception as ex:
            unsupported_reason = "FetchCounters failed: {}".format(ex)
    else:
        unsupported_reason = "SamplesPassed counter not available for this replay."

    hypotheses: list[str] = []
    if samples_passed == 0:
        hypotheses.append("GPU reports zero samples passed (depth/stencil rejection or fully clipped).")

    vp = (pipe_snapshot.get("viewports") or [{}])[0]
    if vp and (vp.get("width", 1) <= 0 or vp.get("height", 1) <= 0):
        hypotheses.append("Viewport dimension is zero.")
        evidence.append({"kind": "viewport", "detail": vp})

    scissors = pipe_snapshot.get("scissors") or []
    if scissors and scissors[0].get("enabled") and (scissors[0].get("width", 1) == 0 or scissors[0].get("height", 1) == 0):
        hypotheses.append("Scissor enabled with zero area.")
        evidence.append({"kind": "scissor", "detail": scissors[0]})

    depth = pipe_snapshot.get("depth") or {}
    if depth.get("depth_enable") and depth.get("depth_function") == "Never":
        hypotheses.append("Depth compare Never rejects all fragments.")

    action = pipe_snapshot.get("action") or {}
    if int(action.get("num_instances") or 0) == 0:
        hypotheses.append("Draw has zero instances.")
    if int(action.get("num_indices") or 0) == 0 and int(action.get("num_vertices") or 0) == 0:
        flags = action.get("flags") or []
        if "Drawcall" in flags:
            hypotheses.append("Drawcall reports zero indices and zero vertices.")

    return {
        "event_id": event_id,
        "samples_passed": samples_passed,
        "unsupported_reason": unsupported_reason,
        "hypotheses": hypotheses,
        "evidence": evidence,
        "anomalies": detect_pipeline_anomalies(pipe_snapshot),
    }


def _try_list(fn: Any) -> Any | None:
    try:
        return fn()
    except Exception:
        return None


def detect_pipeline_anomalies(snapshot: dict[str, Any]) -> list[str]:
    """Return anomaly keys for suspicious pipeline state in a normalize_pipeline_state snapshot."""
    anomalies: list[str] = []

    # Viewport
    vps = snapshot.get("viewports") or []
    if vps:
        vp = vps[0]
        if float(vp.get("width", 1)) <= 0 or float(vp.get("height", 1)) <= 0:
            anomalies.append("zero_viewport")

    # Scissor
    scissors = snapshot.get("scissors") or []
    if scissors:
        sc = scissors[0]
        if sc.get("enabled") and (int(sc.get("width", 1)) == 0 or int(sc.get("height", 1)) == 0):
            anomalies.append("scissor_clips_all")

    # Depth
    depth = snapshot.get("depth") or {}
    depth_enable = bool(depth.get("depth_enable", False))
    depth_writes = bool(depth.get("depth_writes", False))
    action = snapshot.get("action") or {}
    is_draw = (
        int(action.get("num_vertices") or 0) > 0
        or int(action.get("num_indices") or 0) > 0
        or "Drawcall" in (action.get("flags") or [])
    )
    color_targets = (snapshot.get("targets") or {}).get("color_targets") or []
    has_color_output = any(
        c.get("resource_id") not in ("Null", "", None) for c in color_targets
    )
    if not depth_enable and is_draw and has_color_output:
        anomalies.append("depth_test_disabled")
    if depth_enable and not depth_writes:
        anomalies.append("depth_write_disabled")

    # Color outputs
    if color_targets and all(
        c.get("resource_id") in ("Null", "", None) for c in color_targets
    ):
        anomalies.append("no_color_outputs")

    # Additive blend
    blend = snapshot.get("blend") or {}
    for t in blend.get("targets") or []:
        if t.get("blend_enable") and t.get("dst_color") == "One" and t.get("color_op", "Add") == "Add":
            anomalies.append("additive_blend")
            break

    return anomalies


def diff_texture_analysis(a: dict[str, Any], b: dict[str, Any]) -> dict[str, Any]:
    """Compare outputs of analyze_texture."""
    out: dict[str, Any] = {"before": a, "after": b, "deltas": {}}
    if not a.get("supported_stats") or not b.get("supported_stats"):
        out["note"] = "One side lacked computable stats."
        return out
    for key in ("mean_channels", "near_black_ratio", "nan_pixel_count"):
        if key in a and key in b:
            try:
                if isinstance(a[key], list):
                    out["deltas"][key] = [float(b[key][i]) - float(a[key][i]) for i in range(min(len(a[key]), len(b[key])))]
                else:
                    out["deltas"][key] = float(b[key]) - float(a[key])
            except Exception:
                continue
    return out


def _walk_count(action: Any, draw_flag: int, dispatch_flag: int) -> tuple[int, int, int]:
    """Return (draws, dispatches, total_events) for an action subtree."""
    flags = int(action.flags)
    draws = 1 if (flags & draw_flag) else 0
    dispatches = 1 if (flags & dispatch_flag) else 0
    total = 1
    for ch in action.children:
        cd, cdi, ct = _walk_count(ch, draw_flag, dispatch_flag)
        draws += cd
        dispatches += cdi
        total += ct
    return draws, dispatches, total


def _max_event_in_subtree(action: Any) -> int:
    eid = int(action.eventId)
    for ch in action.children:
        eid = max(eid, _max_event_in_subtree(ch))
    return eid


def build_frame_overview(controller: Any, structured_file: Any) -> dict[str, Any]:
    """Build a frame structure map without seeking to any event.

    Uses only GetRootActions(), GetTextures(), and GetUsage() so it is fast
    even on large captures. Returns render passes (named marker groups), render
    targets (textures with ColourTarget/DepthStencilTarget usages), and counts.
    """
    rd = get_renderdoc()

    draw_flag = int(getattr(rd.ActionFlags, "Drawcall", 0))
    dispatch_flag = int(getattr(rd.ActionFlags, "Dispatch", 0))

    total_draws = 0
    total_dispatches = 0
    total_events = 0
    render_passes: list[dict] = []

    for root in controller.GetRootActions():
        cd, cdi, ct = _walk_count(root, draw_flag, dispatch_flag)
        total_draws += cd
        total_dispatches += cdi
        total_events += ct

        children = list(root.children)
        if children:
            name = root.GetName(structured_file)
            render_passes.append({
                "marker_name": name,
                "start_event_id": int(root.eventId),
                "end_event_id": _max_event_in_subtree(root),
                "draw_count": cd,
            })

    # Find render targets via resource usages (no SetFrameEvent needed)
    ColourTarget = getattr(rd.ResourceUsage, "ColourTarget", None)
    DepthStencilTarget = getattr(rd.ResourceUsage, "DepthStencilTarget", None)
    ClearUsage = getattr(rd.ResourceUsage, "Clear", None)

    render_targets: list[dict] = []
    warnings: list[str] = []

    try:
        textures = list(controller.GetTextures())[:2000]
    except Exception as ex:
        warnings.append("GetTextures failed: {}".format(ex))
        textures = []

    for tex in textures:
        tid = tex.resourceId
        try:
            usages = list(controller.GetUsage(tid))
        except Exception:
            continue

        rt_usages = [
            u for u in usages
            if (ColourTarget is not None and u.usage == ColourTarget)
            or (DepthStencilTarget is not None and u.usage == DepthStencilTarget)
        ]
        if not rt_usages:
            continue

        write_eids = [int(u.eventId) for u in rt_usages]
        clear_eids = [
            int(u.eventId) for u in usages
            if ClearUsage is not None and u.usage == ClearUsage
        ]

        entry: dict = {
            "resource_id": rid_str(tid),
            "format": enum_name(tex.format.type) if tex.format else "",
            "width": int(tex.width),
            "height": int(tex.height),
            "first_write_event_id": min(write_eids) if write_eids else None,
            "write_event_count": len(write_eids),
            "clear_event_ids": clear_eids[:20],
        }
        rname = resource_name_for(controller, tid)
        if rname:
            entry["resource_name"] = rname
        render_targets.append(entry)

    debug_count = 0
    try:
        debug_count = len(list(controller.GetDebugMessages()))
    except Exception:
        pass

    out: dict = {
        "total_events": total_events,
        "draw_count": total_draws,
        "dispatch_count": total_dispatches,
        "render_passes": render_passes[:100],
        "render_targets": render_targets[:200],
        "debug_message_count": debug_count,
    }
    if warnings:
        out["warnings"] = warnings
    return out
