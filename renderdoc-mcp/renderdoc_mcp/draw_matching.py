"""Content-based cross-capture draw matching: pure scoring helpers + orchestration."""

from __future__ import annotations

import math
from typing import Any

from renderdoc_mcp.analysis import analyze_texture_bytes, find_texture_description
from renderdoc_mcp.cbuffer import decode_cb_bytes
from renderdoc_mcp.mesh_decode import decode_post_vs_outputs
from renderdoc_mcp.rdutil import controller_get_buffer_data, get_renderdoc, parse_resource_id
from renderdoc_mcp.serialize import build_draw_state_row
from renderdoc_mcp.session import expand_action_flags, find_action


def draw_shape_key(draw_row: dict[str, Any]) -> tuple[Any, ...]:
    """Structural prefilter key: topology, has-index-buffer, VB count, target counts.

    Same shape `diff_draw_sequences` already aligns draws by (that tool's inline `shape_key`
    closure in server.py) -- reimplemented here as a standalone, unit-testable function rather
    than reaching into that closure, which isn't importable without refactoring the existing tool.
    """
    return (
        draw_row.get("topology"),
        len(draw_row.get("vertex_buffers") or []),
        draw_row.get("index_buffer") is not None,
        len(draw_row.get("color_targets") or []),
        draw_row.get("depth_target") is not None,
    )


def count_closeness(count_a: int, count_b: int) -> float:
    """0.0-1.0 closeness of two counts; 1.0 if equal, degrading toward 0.0 as they diverge."""
    denom = max(int(count_a), int(count_b), 1)
    return 1.0 - min(1.0, abs(int(count_a) - int(count_b)) / denom)


def jaccard_similarity(set_a: set[str], set_b: set[str]) -> float:
    """|intersection| / |union|; vacuously 1.0 if both sets are empty."""
    if not set_a and not set_b:
        return 1.0
    union = set_a | set_b
    return len(set_a & set_b) / len(union)


def bbox_extents(positions: list[list[float]]) -> tuple[float, float, float] | None:
    """Per-axis (x, y, z) extent (max - min) across a list of clip-space position vectors.

    Returns None if positions is empty. Each position must have at least 3 components; a 4th
    (w) is ignored -- this measures a clip-space bounding-box shape, not a perspective-divided
    volume (unprojection is not needed for a proportions-only comparison).
    """
    if not positions:
        return None
    mins = [math.inf, math.inf, math.inf]
    maxs = [-math.inf, -math.inf, -math.inf]
    for p in positions:
        for i in range(3):
            v = float(p[i])
            if v < mins[i]:
                mins[i] = v
            if v > maxs[i]:
                maxs[i] = v
    return (maxs[0] - mins[0], maxs[1] - mins[1], maxs[2] - mins[2])


def bbox_ratio_similarity(
    extents_a: tuple[float, float, float], extents_b: tuple[float, float, float]
) -> float:
    """Similarity of two bounding boxes' *proportions* (not absolute scale).

    Each extents triple is floored at a small epsilon (degenerate/flat geometry) and normalized
    to sum to 1, then compared via L1 distance -- the max possible L1 distance between two
    sum-to-1 non-negative 3-vectors is 2, so `1 - distance/2` maps cleanly to [0, 1].
    """
    eps = 1e-6

    def normalize(extents: tuple[float, float, float]) -> tuple[float, float, float]:
        floored = tuple(max(eps, float(v)) for v in extents)
        total = sum(floored)
        return (floored[0] / total, floored[1] / total, floored[2] / total)

    ratio_a = normalize(extents_a)
    ratio_b = normalize(extents_b)
    l1 = sum(abs(a - b) for a, b in zip(ratio_a, ratio_b))
    return 1.0 - min(1.0, l1 / 2.0)


def texture_dimension_score(dims_a: tuple[int, int] | None, dims_b: tuple[int, int] | None) -> float:
    """1.0 if both draws lack a color target (nothing to disagree on), 0.0 if only one has one,
    1.0 if both have one with identical (width, height), else 0.5 (both present, differ)."""
    if dims_a is None and dims_b is None:
        return 1.0
    if dims_a is None or dims_b is None:
        return 0.0
    return 1.0 if tuple(dims_a) == tuple(dims_b) else 0.5


def texture_content_score(mean_a: list[float] | None, mean_b: list[float] | None) -> float | None:
    """1 - mean-abs-difference across channels; None (skip) if either side has no content stat."""
    if mean_a is None or mean_b is None:
        return None
    n = min(len(mean_a), len(mean_b))
    if n == 0:
        return None
    avg_diff = sum(abs(float(mean_a[i]) - float(mean_b[i])) for i in range(n)) / n
    return 1.0 - min(1.0, avg_diff)


def texture_signal(
    dims_a: tuple[int, int] | None,
    dims_b: tuple[int, int] | None,
    mean_a: list[float] | None,
    mean_b: list[float] | None,
) -> float:
    """Combines dimension-match and content-stat halves. If the content stat is unavailable for
    either side, dimension-match alone is used (not averaged with a fabricated 0)."""
    dim_score = texture_dimension_score(dims_a, dims_b)
    content_score = texture_content_score(mean_a, mean_b)
    if content_score is None:
        return dim_score
    return (dim_score + content_score) / 2.0


def _scalar_close(a: Any, b: Any, abs_tolerance: float, rel_tolerance: float) -> bool:
    """Numeric/list/list-of-list closeness check for cbuffer.decode_cb_bytes' value shapes
    (scalar, flat list, or nested list for matrices) -- deliberately narrower than
    shader_debug._values_equal, which also handles dict-shaped values this module never sees."""
    if isinstance(a, bool) or isinstance(b, bool):
        return a == b
    if isinstance(a, (int, float)) and isinstance(b, (int, float)):
        return math.isclose(float(a), float(b), abs_tol=abs_tolerance, rel_tol=rel_tolerance)
    if isinstance(a, list) and isinstance(b, list):
        return len(a) == len(b) and all(
            _scalar_close(av, bv, abs_tolerance, rel_tolerance) for av, bv in zip(a, b)
        )
    return a == b


def constants_score(
    values_a: dict[str, Any],
    values_b: dict[str, Any],
    abs_tolerance: float = 1e-6,
    rel_tolerance: float = 1e-5,
) -> float:
    """Fraction of name-matched constants (present in both draws) that agree within tolerance.
    Neutral (0.5) if no names are shared -- no evidence either way shouldn't swing the score."""
    shared = set(values_a) & set(values_b)
    if not shared:
        return 0.5
    matches = sum(
        1 for name in shared
        if _scalar_close(values_a[name], values_b[name], abs_tolerance, rel_tolerance)
    )
    return matches / len(shared)


_SIGNAL_WEIGHTS = {"geometry": 0.4, "texture": 0.25, "constants": 0.2, "shader_shape": 0.15}


def combine_signals(geometry: float, texture: float, constants: float, shader_shape: float) -> float:
    """Weighted confidence score: geometry 0.4, texture 0.25, constants 0.2, shader_shape 0.15."""
    return (
        _SIGNAL_WEIGHTS["geometry"] * geometry
        + _SIGNAL_WEIGHTS["texture"] * texture
        + _SIGNAL_WEIGHTS["constants"] * constants
        + _SIGNAL_WEIGHTS["shader_shape"] * shader_shape
    )


def _iter_drawcall_events(controller: Any, rd: Any, limit: int) -> list[int]:
    """Walk the action tree collecting Drawcall event IDs, in frame order, capped at limit."""
    out: list[int] = []

    def visit(action: Any) -> None:
        if len(out) >= limit:
            return
        if "Drawcall" in expand_action_flags(rd, int(action.flags)):
            out.append(int(action.eventId))
        for child in action.children:
            if len(out) >= limit:
                return
            visit(child)

    for root in controller.GetRootActions():
        if len(out) >= limit:
            break
        visit(root)
    return out


def _draw_fingerprint(controller: Any, structured_file: Any, event_id: int) -> dict[str, Any] | None:
    """Gather every signal needed to score one draw against another.

    Returns None if the event isn't found or isn't a Drawcall -- the reference draw's caller
    turns this into an explicit error; a non-drawcall candidate id is simply skipped.
    """
    rd = get_renderdoc()
    action = find_action(controller, event_id)
    if action is None:
        return None
    if "Drawcall" not in expand_action_flags(rd, int(action.flags)):
        return None

    controller.SetFrameEvent(int(event_id), False)
    pipe = controller.GetPipelineState()
    draw_row = build_draw_state_row(controller, structured_file, event_id)

    # Geometry: element count (RenderDoc's ActionDescription.numIndices is the generic per-draw
    # element count for both indexed and non-indexed draws) + post-VS position bounding box.
    num_elements = int(getattr(action, "numIndices", 0) or 0)
    positions: list[list[float]] = []
    postvs = decode_post_vs_outputs(
        controller, structured_file, event_id, stage="vsout", instance=0, view=0,
        preview_vertices=32, out_file=None,
    )
    if not postvs.get("error") and postvs.get("ok") is not False:
        pos_name = next(
            (s["name"] for s in postvs.get("semantics", []) if s.get("system_value") == "Position"),
            None,
        )
        if pos_name is not None:
            for row in postvs.get("vertex_previews", []):
                pos = row.get("values", {}).get(pos_name)
                clip = pos.get("clip") if isinstance(pos, dict) else None
                if clip and len(clip) >= 3:
                    positions.append(clip)

    # Texture: primary color target dimensions + mean channel content.
    tex_dims: tuple[int, int] | None = None
    tex_mean: list[float] | None = None
    color_targets = draw_row.get("color_targets") or []
    if color_targets:
        rid_str_val = color_targets[0].get("resource_id")
        if rid_str_val and rid_str_val != "Null":
            try:
                rid = parse_resource_id(rid_str_val)
                tex = find_texture_description(controller, rid)
                if tex is not None:
                    tex_dims = (int(tex.width), int(tex.height))
                    sub = rd.Subresource(0, 0, 0)
                    raw = controller.GetTextureData(rid, sub)
                    stats = analyze_texture_bytes(tex, raw)
                    if stats.get("supported_stats") and "mean_channels" in stats:
                        tex_mean = stats["mean_channels"]
            except Exception:
                pass

    # Constants: decode Vertex + Pixel constant buffers, keyed by variable name.
    constants: dict[str, Any] = {}
    cb_names: set[str] = set()
    for stage in (rd.ShaderStage.Vertex, rd.ShaderStage.Pixel):
        refl = pipe.GetShaderReflection(stage)
        if refl is None:
            continue
        for cb_index, cb_block in enumerate(list(getattr(refl, "constantBlocks", []) or [])):
            cb_names.add(str(getattr(cb_block, "name", "") or ""))
            try:
                cb_desc = pipe.GetConstantBlock(stage, cb_index, 0)
                desc = getattr(cb_desc, "descriptor", None)
                if desc is None:
                    continue
                buf_rid = getattr(desc, "resource", None)
                byte_offset = int(getattr(desc, "byteOffset", 0))
                byte_size = int(getattr(desc, "byteSize", 0)) or 65536
                raw = controller_get_buffer_data(controller, buf_rid, byte_offset, min(byte_size, 65536))
                for var in decode_cb_bytes(raw, list(getattr(cb_block, "variables", []) or [])):
                    constants[var["name"]] = var["value"]
            except Exception:
                continue

    # Shader shape: VS output-signature semantic names.
    output_semantics: set[str] = set()
    vs_refl = pipe.GetShaderReflection(rd.ShaderStage.Vertex)
    if vs_refl is not None:
        for sig in getattr(vs_refl, "outputSignature", []) or []:
            name = str(getattr(sig, "semanticName", "") or "")
            if name:
                output_semantics.add(name)

    return {
        "event_id": int(event_id),
        "name": draw_row.get("name", ""),
        "shape_key": draw_shape_key(draw_row),
        "num_elements": num_elements,
        "position_extents": bbox_extents(positions),
        "texture_dims": tex_dims,
        "texture_mean": tex_mean,
        "constants": constants,
        "constant_block_names": cb_names,
        "output_semantics": output_semantics,
    }


def _score_pair(ref: dict[str, Any], cand: dict[str, Any]) -> dict[str, float]:
    count_score = count_closeness(ref["num_elements"], cand["num_elements"])
    if ref["position_extents"] is not None and cand["position_extents"] is not None:
        bbox_score = bbox_ratio_similarity(ref["position_extents"], cand["position_extents"])
        geometry = (count_score + bbox_score) / 2.0
    else:
        geometry = count_score

    texture = texture_signal(
        ref["texture_dims"], cand["texture_dims"], ref["texture_mean"], cand["texture_mean"]
    )
    constants = constants_score(ref["constants"], cand["constants"])
    shader_shape = (
        jaccard_similarity(ref["output_semantics"], cand["output_semantics"])
        + jaccard_similarity(ref["constant_block_names"], cand["constant_block_names"])
    ) / 2.0

    return {
        "geometry": geometry,
        "texture": texture,
        "constants": constants,
        "shader_shape": shader_shape,
        "confidence": combine_signals(geometry, texture, constants, shader_shape),
    }


def find_corresponding_draws(
    controller_a: Any,
    structured_file_a: Any,
    event_id_a: int,
    controller_b: Any,
    structured_file_b: Any,
    event_ids_b: list[int] | None = None,
    limit: int = 200,
    top_k: int = 5,
) -> dict[str, Any]:
    """Rank draws in (controller_b, structured_file_b) against one reference draw in
    (controller_a, structured_file_a). These are two separate (controller, structured_file)
    pairs, not one -- capture_a and capture_b are ordinarily different open replay sessions.
    Passing the same pair for both is valid too (e.g. finding a similar draw within one capture).
    """
    rd = get_renderdoc()
    ref = _draw_fingerprint(controller_a, structured_file_a, event_id_a)
    if ref is None:
        return {"ok": False, "error": "not_a_drawcall", "event_id": event_id_a}

    limit = max(1, int(limit))
    candidate_ids = (
        [int(e) for e in event_ids_b] if event_ids_b is not None
        else _iter_drawcall_events(controller_b, rd, limit)
    )
    candidate_ids = candidate_ids[:limit]

    fingerprints = []
    for eid in candidate_ids:
        fp = _draw_fingerprint(controller_b, structured_file_b, eid)
        if fp is not None:
            fingerprints.append(fp)

    prefiltered = [fp for fp in fingerprints if fp["shape_key"] == ref["shape_key"]]
    shape_prefilter_applied = len(prefiltered) > 0
    pool = prefiltered if shape_prefilter_applied else fingerprints

    scored = []
    for fp in pool:
        signals = _score_pair(ref, fp)
        scored.append({
            "event_id": fp["event_id"],
            "name": fp["name"],
            "confidence": signals["confidence"],
            "signals": {k: signals[k] for k in ("geometry", "texture", "constants", "shader_shape")},
        })

    scored.sort(key=lambda c: c["confidence"], reverse=True)

    return {
        "reference": {"event_id": ref["event_id"], "name": ref["name"]},
        "prefiltered_count": len(prefiltered),
        "scored_count": len(pool),
        "shape_prefilter_applied": shape_prefilter_applied,
        "candidates": scored[: max(1, int(top_k))],
    }
