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
