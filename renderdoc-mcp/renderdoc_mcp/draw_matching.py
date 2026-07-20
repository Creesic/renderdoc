"""Content-based cross-capture draw matching: pure scoring helpers + orchestration."""

from __future__ import annotations

import math
from typing import Any


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
