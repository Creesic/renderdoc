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
