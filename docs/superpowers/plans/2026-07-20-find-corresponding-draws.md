# find_corresponding_draws Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a `find_corresponding_draws` MCP tool that ranks the most likely corresponding draw(s) in one capture for a reference draw in another, by content-based fingerprint similarity, per `docs/superpowers/specs/2026-07-20-find-corresponding-draws-design.md`.

**Architecture:** Five small, pure, independently-tested scoring functions (structural shape key, count/geometry/texture/constants/shader-shape sub-scores, weighted combination) in a new `renderdoc_mcp/draw_matching.py`, plus an orchestration layer in the same file that gathers a "fingerprint" per draw (reusing `build_draw_state_row`, `decode_post_vs_outputs`, `analyze_texture_bytes`, `cbuffer.decode_cb_bytes`, and shader reflection) and scores every candidate against one reference draw. A thin `server.py` tool wraps it.

**Tech Stack:** Python 3.10+, `renderdoc` Python module (`pymodules`), `pytest` for unit tests, `rdtest` for the integration test.

## Global Constraints

- No new Python dependencies.
- Follow the existing tool pattern exactly: `@mcp.tool()` → `async with replay_execution(): def _go(): ...; return await asyncio.get_running_loop().run_in_executor(_replay_executor, _go)`.
- Match existing code style: `from __future__ import annotations`, type hints on all new functions, no unrelated formatting changes.
- Weights are fixed at geometry=0.4, texture=0.25, constants=0.2, shader_shape=0.15 (sum to 1.0), not caller-tunable in this version.
- Shader bytecode/disassembly is never compared — only reflection-level signature/name sets (semantic names, constant-block names), per the spec's cross-API rationale.
- The texture signal only inspects the primary color target (`color_targets[0]`), never arbitrary bound input textures.
- Reuse existing tolerance convention for constant comparison: `abs_tolerance=1e-6, rel_tolerance=1e-5` (matches `diff_shader_invocations`).

---

### Task 1: Structural/count/set-similarity pure helpers

**Files:**
- Create: `renderdoc-mcp/renderdoc_mcp/draw_matching.py`
- Test: `renderdoc-mcp/tests/test_draw_matching.py` (new file)

**Interfaces:**
- Produces: `draw_shape_key(draw_row: dict[str, Any]) -> tuple[Any, ...]`, `count_closeness(count_a: int, count_b: int) -> float`, `jaccard_similarity(set_a: set[str], set_b: set[str]) -> float`. Task 6's orchestration calls all three; Task 9's integration test may call `draw_shape_key` directly.

- [ ] **Step 1: Write the failing tests**

Create `renderdoc-mcp/tests/test_draw_matching.py`:

```python
from __future__ import annotations


def test_draw_shape_key_captures_topology_and_counts():
    from renderdoc_mcp.draw_matching import draw_shape_key

    row = {
        "topology": "TriangleList",
        "vertex_buffers": [{"slot": 0}, {"slot": 1}],
        "index_buffer": {"resource_id": "ResourceId::1"},
        "color_targets": [{"slot": 0}],
        "depth_target": None,
    }
    assert draw_shape_key(row) == ("TriangleList", 2, True, 1, False)


def test_draw_shape_key_handles_missing_fields():
    from renderdoc_mcp.draw_matching import draw_shape_key

    assert draw_shape_key({}) == (None, 0, False, 0, False)


def test_count_closeness_equal_counts_is_one():
    from renderdoc_mcp.draw_matching import count_closeness

    assert count_closeness(100, 100) == 1.0


def test_count_closeness_degrades_with_divergence():
    from renderdoc_mcp.draw_matching import count_closeness

    assert count_closeness(100, 50) == 0.5
    assert count_closeness(0, 0) == 1.0


def test_jaccard_similarity_identical_sets():
    from renderdoc_mcp.draw_matching import jaccard_similarity

    assert jaccard_similarity({"a", "b"}, {"a", "b"}) == 1.0


def test_jaccard_similarity_partial_overlap():
    from renderdoc_mcp.draw_matching import jaccard_similarity

    assert jaccard_similarity({"a", "b"}, {"b", "c"}) == 1.0 / 3.0


def test_jaccard_similarity_both_empty_is_vacuously_one():
    from renderdoc_mcp.draw_matching import jaccard_similarity

    assert jaccard_similarity(set(), set()) == 1.0


def test_jaccard_similarity_disjoint_sets_is_zero():
    from renderdoc_mcp.draw_matching import jaccard_similarity

    assert jaccard_similarity({"a"}, {"b"}) == 0.0
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd renderdoc-mcp && python -m pytest tests/test_draw_matching.py -v`
Expected: FAIL with `ModuleNotFoundError: No module named 'renderdoc_mcp.draw_matching'`

- [ ] **Step 3: Write minimal implementation**

Create `renderdoc-mcp/renderdoc_mcp/draw_matching.py`:

```python
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
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd renderdoc-mcp && python -m pytest tests/test_draw_matching.py -v`
Expected: PASS (8 tests)

- [ ] **Step 5: Commit**

```bash
git add renderdoc-mcp/renderdoc_mcp/draw_matching.py renderdoc-mcp/tests/test_draw_matching.py
git commit -m "mcp: add shape/count/jaccard helpers for cross-capture draw matching"
```

---

### Task 2: Geometry (post-VS bounding-box) pure helpers

**Files:**
- Modify: `renderdoc-mcp/renderdoc_mcp/draw_matching.py`
- Test: `renderdoc-mcp/tests/test_draw_matching.py`

**Interfaces:**
- Consumes: nothing from Task 1.
- Produces: `bbox_extents(positions: list[list[float]]) -> tuple[float, float, float] | None` and `bbox_ratio_similarity(extents_a: tuple[float, float, float], extents_b: tuple[float, float, float]) -> float`. Task 6's `_draw_fingerprint` calls `bbox_extents` on decoded POSITION clip values; `_score_pair` calls `bbox_ratio_similarity` when both draws have non-`None` extents.

- [ ] **Step 1: Write the failing tests**

Append to `renderdoc-mcp/tests/test_draw_matching.py`:

```python
def test_bbox_extents_computes_per_axis_range():
    from renderdoc_mcp.draw_matching import bbox_extents

    positions = [[0.0, 0.0, 0.0, 1.0], [2.0, 4.0, 1.0, 1.0], [1.0, 1.0, 0.5, 1.0]]
    assert bbox_extents(positions) == (2.0, 4.0, 1.0)


def test_bbox_extents_ignores_fourth_component():
    from renderdoc_mcp.draw_matching import bbox_extents

    positions = [[0.0, 0.0, 0.0, 999.0], [1.0, 1.0, 1.0, -999.0]]
    assert bbox_extents(positions) == (1.0, 1.0, 1.0)


def test_bbox_extents_none_for_empty_positions():
    from renderdoc_mcp.draw_matching import bbox_extents

    assert bbox_extents([]) is None


def test_bbox_ratio_similarity_identical_shapes_is_one():
    from renderdoc_mcp.draw_matching import bbox_ratio_similarity

    assert bbox_ratio_similarity((2.0, 4.0, 1.0), (2.0, 4.0, 1.0)) == 1.0


def test_bbox_ratio_similarity_scale_invariant():
    """Same proportions at a different absolute scale (cross-API clip-space scale can differ)
    must still score 1.0 -- this is the whole point of normalizing to ratios."""
    from renderdoc_mcp.draw_matching import bbox_ratio_similarity

    assert bbox_ratio_similarity((2.0, 4.0, 1.0), (20.0, 40.0, 10.0)) == 1.0


def test_bbox_ratio_similarity_different_proportions_scores_lower():
    from renderdoc_mcp.draw_matching import bbox_ratio_similarity

    wide_flat = (10.0, 1.0, 1.0)
    tall_thin = (1.0, 10.0, 1.0)
    assert bbox_ratio_similarity(wide_flat, tall_thin) < 0.5


def test_bbox_ratio_similarity_handles_degenerate_zero_extent():
    """A flat quad (zero depth) must not raise a division error."""
    from renderdoc_mcp.draw_matching import bbox_ratio_similarity

    result = bbox_ratio_similarity((2.0, 4.0, 0.0), (2.0, 4.0, 0.0))
    assert result == 1.0
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd renderdoc-mcp && python -m pytest tests/test_draw_matching.py -v`
Expected: FAIL with `ImportError: cannot import name 'bbox_extents'`

- [ ] **Step 3: Write minimal implementation**

Add to `renderdoc-mcp/renderdoc_mcp/draw_matching.py`, directly below `jaccard_similarity`:

```python
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
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd renderdoc-mcp && python -m pytest tests/test_draw_matching.py -v`
Expected: PASS (15 tests)

- [ ] **Step 5: Commit**

```bash
git add renderdoc-mcp/renderdoc_mcp/draw_matching.py renderdoc-mcp/tests/test_draw_matching.py
git commit -m "mcp: add bbox_extents/bbox_ratio_similarity for draw-matching geometry signal"
```

---

### Task 3: Texture signal pure helpers

**Files:**
- Modify: `renderdoc-mcp/renderdoc_mcp/draw_matching.py`
- Test: `renderdoc-mcp/tests/test_draw_matching.py`

**Interfaces:**
- Consumes: nothing from Tasks 1-2.
- Produces: `texture_dimension_score(dims_a, dims_b) -> float`, `texture_content_score(mean_a, mean_b) -> float | None`, `texture_signal(dims_a, dims_b, mean_a, mean_b) -> float`. Task 6's `_score_pair` calls only `texture_signal` (the other two are internal building blocks, still exported for direct testing).

- [ ] **Step 1: Write the failing tests**

Append to `renderdoc-mcp/tests/test_draw_matching.py`:

```python
def test_texture_dimension_score_both_absent_is_one():
    from renderdoc_mcp.draw_matching import texture_dimension_score

    assert texture_dimension_score(None, None) == 1.0


def test_texture_dimension_score_one_absent_is_zero():
    from renderdoc_mcp.draw_matching import texture_dimension_score

    assert texture_dimension_score((256, 256), None) == 0.0
    assert texture_dimension_score(None, (256, 256)) == 0.0


def test_texture_dimension_score_matching_dims_is_one():
    from renderdoc_mcp.draw_matching import texture_dimension_score

    assert texture_dimension_score((256, 256), (256, 256)) == 1.0


def test_texture_dimension_score_differing_dims_is_half():
    from renderdoc_mcp.draw_matching import texture_dimension_score

    assert texture_dimension_score((256, 256), (512, 512)) == 0.5


def test_texture_content_score_none_when_either_missing():
    from renderdoc_mcp.draw_matching import texture_content_score

    assert texture_content_score(None, [0.1, 0.2, 0.3, 1.0]) is None
    assert texture_content_score([0.1, 0.2, 0.3, 1.0], None) is None


def test_texture_content_score_identical_means_is_one():
    from renderdoc_mcp.draw_matching import texture_content_score

    assert texture_content_score([0.1, 0.2, 0.3, 1.0], [0.1, 0.2, 0.3, 1.0]) == 1.0


def test_texture_content_score_degrades_with_difference():
    from renderdoc_mcp.draw_matching import texture_content_score

    result = texture_content_score([0.0, 0.0, 0.0, 1.0], [1.0, 1.0, 1.0, 1.0])
    assert result == 0.25  # mean_abs_diff = (1+1+1+0)/4 = 0.75 -> 1 - 0.75


def test_texture_signal_uses_dimension_score_alone_when_content_unavailable():
    from renderdoc_mcp.draw_matching import texture_signal

    assert texture_signal((256, 256), (256, 256), None, None) == 1.0
    assert texture_signal((256, 256), (512, 512), None, None) == 0.5


def test_texture_signal_averages_both_halves_when_available():
    from renderdoc_mcp.draw_matching import texture_signal

    result = texture_signal((256, 256), (256, 256), [0.0, 0.0, 0.0, 1.0], [1.0, 1.0, 1.0, 1.0])
    # dimension half = 1.0, content half = 0.25 (per test above) -> average 0.625
    assert result == 0.625
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd renderdoc-mcp && python -m pytest tests/test_draw_matching.py -v`
Expected: FAIL with `ImportError: cannot import name 'texture_dimension_score'`

- [ ] **Step 3: Write minimal implementation**

Add to `renderdoc-mcp/renderdoc_mcp/draw_matching.py`, directly below `bbox_ratio_similarity`:

```python
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
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd renderdoc-mcp && python -m pytest tests/test_draw_matching.py -v`
Expected: PASS (24 tests)

- [ ] **Step 5: Commit**

```bash
git add renderdoc-mcp/renderdoc_mcp/draw_matching.py renderdoc-mcp/tests/test_draw_matching.py
git commit -m "mcp: add texture_signal helpers for draw matching"
```

---

### Task 4: Constants signal pure helpers

**Files:**
- Modify: `renderdoc-mcp/renderdoc_mcp/draw_matching.py`
- Test: `renderdoc-mcp/tests/test_draw_matching.py`

**Interfaces:**
- Consumes: nothing from Tasks 1-3.
- Produces: `constants_score(values_a: dict[str, Any], values_b: dict[str, Any], abs_tolerance: float = 1e-6, rel_tolerance: float = 1e-5) -> float`. Task 6's `_score_pair` calls this with each draw's `{name: value}` constants dict.

- [ ] **Step 1: Write the failing tests**

Append to `renderdoc-mcp/tests/test_draw_matching.py`:

```python
def test_constants_score_all_shared_values_match():
    from renderdoc_mcp.draw_matching import constants_score

    a = {"scale": 2.0, "offset": [1.0, 2.0, 3.0]}
    b = {"scale": 2.0, "offset": [1.0, 2.0, 3.0]}
    assert constants_score(a, b) == 1.0


def test_constants_score_partial_match():
    from renderdoc_mcp.draw_matching import constants_score

    a = {"scale": 2.0, "offset": 5.0}
    b = {"scale": 2.0, "offset": 999.0}
    assert constants_score(a, b) == 0.5


def test_constants_score_neutral_when_no_shared_names():
    from renderdoc_mcp.draw_matching import constants_score

    assert constants_score({"a": 1.0}, {"b": 2.0}) == 0.5
    assert constants_score({}, {}) == 0.5


def test_constants_score_respects_tolerance():
    from renderdoc_mcp.draw_matching import constants_score

    a = {"x": 1.0}
    b = {"x": 1.0000001}
    assert constants_score(a, b, abs_tolerance=1e-6, rel_tolerance=1e-5) == 1.0

    b_far = {"x": 1.1}
    assert constants_score(a, b_far, abs_tolerance=1e-6, rel_tolerance=1e-5) == 0.0


def test_constants_score_handles_matrix_values():
    from renderdoc_mcp.draw_matching import constants_score

    a = {"mvp": [[1.0, 0.0], [0.0, 1.0]]}
    b = {"mvp": [[1.0, 0.0], [0.0, 1.0]]}
    assert constants_score(a, b) == 1.0

    b_diff = {"mvp": [[1.0, 0.0], [0.0, 2.0]]}
    assert constants_score(a, b_diff) == 0.0


def test_constants_score_only_counts_names_present_in_both():
    from renderdoc_mcp.draw_matching import constants_score

    a = {"shared": 1.0, "only_in_a": 2.0}
    b = {"shared": 1.0, "only_in_b": 3.0}
    assert constants_score(a, b) == 1.0
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd renderdoc-mcp && python -m pytest tests/test_draw_matching.py -v`
Expected: FAIL with `ImportError: cannot import name 'constants_score'`

- [ ] **Step 3: Write minimal implementation**

Add to `renderdoc-mcp/renderdoc_mcp/draw_matching.py`, directly below `texture_signal`:

```python
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
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd renderdoc-mcp && python -m pytest tests/test_draw_matching.py -v`
Expected: PASS (30 tests)

- [ ] **Step 5: Commit**

```bash
git add renderdoc-mcp/renderdoc_mcp/draw_matching.py renderdoc-mcp/tests/test_draw_matching.py
git commit -m "mcp: add constants_score for draw matching"
```

---

### Task 5: Weighted combination

**Files:**
- Modify: `renderdoc-mcp/renderdoc_mcp/draw_matching.py`
- Test: `renderdoc-mcp/tests/test_draw_matching.py`

**Interfaces:**
- Consumes: nothing from Tasks 1-4 directly (takes plain floats).
- Produces: `combine_signals(geometry: float, texture: float, constants: float, shader_shape: float) -> float`. Task 6's `_score_pair` calls this as the final step to produce `confidence`.

- [ ] **Step 1: Write the failing tests**

Append to `renderdoc-mcp/tests/test_draw_matching.py`:

```python
def test_combine_signals_all_ones_is_one():
    from renderdoc_mcp.draw_matching import combine_signals

    assert combine_signals(1.0, 1.0, 1.0, 1.0) == 1.0


def test_combine_signals_all_zeros_is_zero():
    from renderdoc_mcp.draw_matching import combine_signals

    assert combine_signals(0.0, 0.0, 0.0, 0.0) == 0.0


def test_combine_signals_uses_documented_weights():
    """Locks in the exact weights the spec commits to: geometry=0.4, texture=0.25,
    constants=0.2, shader_shape=0.15."""
    from renderdoc_mcp.draw_matching import combine_signals

    result = combine_signals(geometry=1.0, texture=0.0, constants=0.0, shader_shape=0.0)
    assert result == 0.4

    result = combine_signals(geometry=0.0, texture=1.0, constants=0.0, shader_shape=0.0)
    assert result == 0.25

    result = combine_signals(geometry=0.0, texture=0.0, constants=1.0, shader_shape=0.0)
    assert result == 0.2

    result = combine_signals(geometry=0.0, texture=0.0, constants=0.0, shader_shape=1.0)
    assert result == 0.15
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd renderdoc-mcp && python -m pytest tests/test_draw_matching.py -v`
Expected: FAIL with `ImportError: cannot import name 'combine_signals'`

- [ ] **Step 3: Write minimal implementation**

Add to `renderdoc-mcp/renderdoc_mcp/draw_matching.py`, directly below `constants_score`:

```python
_SIGNAL_WEIGHTS = {"geometry": 0.4, "texture": 0.25, "constants": 0.2, "shader_shape": 0.15}


def combine_signals(geometry: float, texture: float, constants: float, shader_shape: float) -> float:
    """Weighted confidence score: geometry 0.4, texture 0.25, constants 0.2, shader_shape 0.15."""
    return (
        _SIGNAL_WEIGHTS["geometry"] * geometry
        + _SIGNAL_WEIGHTS["texture"] * texture
        + _SIGNAL_WEIGHTS["constants"] * constants
        + _SIGNAL_WEIGHTS["shader_shape"] * shader_shape
    )
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd renderdoc-mcp && python -m pytest tests/test_draw_matching.py -v`
Expected: PASS (33 tests)

- [ ] **Step 5: Commit**

```bash
git add renderdoc-mcp/renderdoc_mcp/draw_matching.py renderdoc-mcp/tests/test_draw_matching.py
git commit -m "mcp: add combine_signals weighted confidence score for draw matching"
```

---

### Task 6: Orchestration — `find_corresponding_draws`

**Files:**
- Modify: `renderdoc-mcp/renderdoc_mcp/draw_matching.py`

**Interfaces:**
- Consumes: `draw_shape_key`, `count_closeness`, `jaccard_similarity` (Task 1), `bbox_extents`, `bbox_ratio_similarity` (Task 2), `texture_signal` (Task 3), `constants_score` (Task 4), `combine_signals` (Task 5). Also `serialize.build_draw_state_row`, `mesh_decode.decode_post_vs_outputs`, `analysis.analyze_texture_bytes`, `analysis.find_texture_description`, `cbuffer.decode_cb_bytes`, `session.find_action`, `session.expand_action_flags`, `rdutil.get_renderdoc`, `rdutil.parse_resource_id`, `rdutil.controller_get_buffer_data`.
- Produces: `find_corresponding_draws(controller_a, structured_file_a, event_id_a, controller_b, structured_file_b, event_ids_b=None, limit=200, top_k=5) -> dict[str, Any]`. **Takes two separate (controller, structured_file) pairs, not one** — `capture_a` and `capture_b` are ordinarily two different open replay sessions (different `CaptureSession.controller`s), unlike every single-capture tool in this codebase (`decode_mesh_inputs`, `decode_post_vs_outputs`) that only ever reads one. The reference draw (`event_id_a`) is always read via `controller_a`/`structured_file_a`; every candidate is always read via `controller_b`/`structured_file_b`. Passing the same pair for both is valid too (e.g. finding a similar draw within one capture) — the function doesn't need to know or care whether they're the same session. Task 7's server.py tool calls this with `sess_a.controller`/`sess_a.structured_file` and `sess_b.controller`/`sess_b.structured_file` directly.

There is no automated unit test for this task's replay-API-calling body — consistent with `decode_mesh_inputs`/`decode_post_vs_outputs`, which also have no direct unit test in this codebase (only their pure helpers are tested that way). Verification is the existing test suite staying green.

- [ ] **Step 1: Write the implementation**

Add to `renderdoc-mcp/renderdoc_mcp/draw_matching.py`, directly below `combine_signals`. First, add these imports at the top of the file (below the existing `from __future__ import annotations` / `import math` / `from typing import Any`):

```python
from renderdoc_mcp.analysis import analyze_texture_bytes, find_texture_description
from renderdoc_mcp.cbuffer import decode_cb_bytes
from renderdoc_mcp.mesh_decode import decode_post_vs_outputs
from renderdoc_mcp.rdutil import controller_get_buffer_data, get_renderdoc, parse_resource_id
from renderdoc_mcp.serialize import build_draw_state_row
from renderdoc_mcp.session import expand_action_flags, find_action
```

Then add:

```python
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
```

- [ ] **Step 2: Run the full test suite to confirm no regressions**

Run: `cd renderdoc-mcp && python -m pytest -q`
Expected: PASS, same count as before plus the 33 new tests from Tasks 1-5 (no regressions)

- [ ] **Step 3: Commit**

```bash
git add renderdoc-mcp/renderdoc_mcp/draw_matching.py
git commit -m "mcp: add find_corresponding_draws orchestration function"
```

---

### Task 7: Wire the `find_corresponding_draws` tool

**Files:**
- Modify: `renderdoc-mcp/renderdoc_mcp/server.py`

**Interfaces:**
- Consumes: `find_corresponding_draws` (Task 6).
- Produces: the `find_corresponding_draws` MCP tool. No other task depends on its internals.

- [ ] **Step 1: Add the import**

In `renderdoc-mcp/renderdoc_mcp/server.py`, find the existing import block that includes `from renderdoc_mcp.mesh_decode import (...)`, and add a new import line directly after it:

```python
from renderdoc_mcp.draw_matching import find_corresponding_draws as find_corresponding_draws_core
```

- [ ] **Step 2: Add the tool, immediately after `diff_draw_sequences`'s closing line, before the `return mcp` at the end of `build_mcp()`**

In `renderdoc-mcp/renderdoc_mcp/server.py`, `diff_draw_sequences` currently ends with (search for this exact text — line numbers may have shifted from other work):

```python
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

    return mcp
```

Insert the new tool between the closing `run_in_executor` line and `return mcp`, so it reads:

```python
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

            def _go() -> dict[str, Any]:
                sess_a = sessions.get(capture_a)
                sess_b = sessions.get(capture_b)
                if sess_a is None or sess_b is None:
                    return R.err("unknown_capture", "capture_a or capture_b invalid")
                try:
                    result = find_corresponding_draws_core(
                        sess_a.controller, sess_a.structured_file, int(event_id_a),
                        sess_b.controller, sess_b.structured_file, event_ids_b, limit, top_k,
                    )
                except Exception as ex:
                    return R.err("find_corresponding_draws_failed", str(ex))
                if isinstance(result, dict):
                    if result.get("error"):
                        return R.err(str(result.get("error")), str(result.get("message", result)))
                    if result.get("ok") is False:
                        return R.err("find_corresponding_draws_failed", str(result.get("error", "")))
                return R.ok(result)

            return await asyncio.get_running_loop().run_in_executor(_replay_executor, _go)

    return mcp
```

Note: `sess_a`/`sess_b` may be the *same* `CaptureSession` object if the caller passes the same `capture_id` for both `capture_a` and `capture_b` (e.g. finding a similar draw within one capture) — `find_corresponding_draws_core` handles this transparently since it just reads whatever `(controller, structured_file)` pair it's given for each side; no special-casing needed in this wrapper.

- [ ] **Step 3: Run the full test suite**

Run: `cd renderdoc-mcp && python -m pytest -q`
Expected: PASS, no regressions

- [ ] **Step 4: Run the smoke check**

Run: `python -c "from renderdoc_mcp.server import build_mcp; build_mcp(); print('ok')"`
Expected: `ok`

- [ ] **Step 5: Commit**

```bash
git add renderdoc-mcp/renderdoc_mcp/server.py renderdoc-mcp/renderdoc_mcp/draw_matching.py
git commit -m "mcp: add find_corresponding_draws tool"
```

---

### Task 8: Document the new tool in the README

**Files:**
- Modify: `renderdoc-mcp/README.md`

**Interfaces:**
- Consumes: nothing (documentation only).

- [ ] **Step 1: Add a tools-table row**

In `renderdoc-mcp/README.md`, find the `diff_draw_sequences` row in the Tools table and add a new row directly after it:

```markdown
| `find_corresponding_draws` | Rank the most likely corresponding draw(s) in another capture by content-fingerprint similarity |
```

- [ ] **Step 2: Add a Limitations bullet**

Search the current `## Limitations` section for its actual last bullet before `## License` (do not assume a specific prior bullet is last — this section has grown across several unrelated plans; find the true final bullet by reading the file directly). Add a new bullet directly after it:

```markdown
- **`find_corresponding_draws`** never compares shader bytecode or disassembly (meaningless across
  different graphics APIs) -- it ranks by post-VS position bounding-box shape, primary
  render-target dimensions/content, constant-buffer values, and shader reflection name sets only.
  It only inspects the primary color target (`color_targets[0]`), never arbitrary bound input
  textures, and signal weights are fixed, not caller-tunable. This is a heuristic ranking to
  narrow a search, not a guaranteed-correct match.
```

- [ ] **Step 3: Commit**

```bash
git add renderdoc-mcp/README.md
git commit -m "docs(mcp): document find_corresponding_draws"
```

---

### Task 9: Integration test against a real multi-draw capture

**Files:**
- Create: a new rdtest test file under `util/test/tests/` targeting an existing demo with multiple, distinguishable draws (exact demo/file TBD by the implementer's investigation — see Step 1).

**Interfaces:**
- Consumes: `find_corresponding_draws` (Task 6), or its lower-level pieces (`_draw_fingerprint`, `_score_pair`) if directly testing the scoring logic is more tractable than the full tool.

This validates the scoring pipeline against real replay data, not just synthetic stand-ins for the pure functions.

- [ ] **Step 1: Confirm the environment can produce a suitable capture, and find a suitable fixture**

A suitable fixture needs at least two *distinguishable* draws in the same capture — e.g. `D3D11_Mesh_Zoo`'s "Quad" vs. "Points" vs. "Lines" sections (`util/test/rdtest/shared/Mesh_Zoo.py`, already used by the `decode_post_vs_outputs` plan's integration test) are strong candidates: different topologies, so a reference draw from "Quad" should score its own kind's draws highest and rank "Points"/"Lines" draws lower. Confirm the environment can build/replay this demo the same way the two most recent plans on this branch already did (bundled 64-bit Python at `x64/Development/python/python.exe`, prebuilt `demos_x64.exe`, working GPU) before writing anything.

If infeasible in this environment, **stop here and report this task as skipped with the reason** — do not fabricate a capture. Tasks 1-8 do not depend on this task.

- [ ] **Step 2: Write the integration test**

Using `D3D11_Mesh_Zoo` (or whatever fixture Step 1 confirmed works) as the target: locate two draws of the *same* kind (e.g. two triangles within "Quad", if the demo issues more than one draw there — confirm by reading the demo source and/or walking `GetRootActions()` at that marker) and one draw of a clearly *different* kind (e.g. "Points"). Call `find_corresponding_draws` with the same-kind draw as `event_id_a` and `event_ids_b` restricted to `[the other same-kind draw's event, the different-kind draw's event]` (or omit `event_ids_b` and let it scan the whole capture if that's more robust against the exact draw layout). Assert: the same-kind draw ranks with strictly higher `confidence` than the different-kind draw, and `signals["geometry"]` for the same-kind pair is higher than for the different-kind pair (a concrete, checkable claim, not just an opaque total-confidence comparison).

If the real capture's draws don't cleanly support this comparison (e.g. only one draw of each kind exists, or geometry signals don't cleanly separate as expected), **do not force a misleading assertion** — investigate why, and if `find_corresponding_draws`/its scoring functions have a real bug, report BLOCKED with specifics rather than adjusting the test to hide it.

- [ ] **Step 3: Run the new test**

Run it the same way prior plans' integration tests on this branch were run (bundled Python + `PYTHONPATH` pointed at the live `renderdoc-mcp/` source, per the known pre-existing harness gap already documented for this kind of test).
Expected: PASS.

- [ ] **Step 4: Commit**

```bash
git add <the new test file path>
git commit -m "test: verify find_corresponding_draws ranks a real capture's draws correctly"
```

---

## Self-Review Notes

- **Spec coverage:** Purpose/Algorithm (prefilter + 4 weighted signals) → Tasks 1-6. Parameters/Response shape → Tasks 6-7. Error Handling/Limitations (`not_a_drawcall`, primary-color-target-only, fixed weights, no bytecode) → Task 6's branches + Task 8's README bullet. Testing (unit tests on pure logic, one integration test) → Tasks 1-5 and Task 9. Out-of-scope items (full bipartite assignment) are explicitly not touched by any task.
- **Type consistency checked:** `_draw_fingerprint`'s output dict keys (`event_id`, `name`, `shape_key`, `num_elements`, `position_extents`, `texture_dims`, `texture_mean`, `constants`, `constant_block_names`, `output_semantics`) are consumed identically by `_score_pair` and by `find_corresponding_draws`'s prefilter/response-building — no renaming. `_score_pair`'s output dict keys (`geometry`, `texture`, `constants`, `shader_shape`, `confidence`) match what `find_corresponding_draws` reads to build each candidate's `signals` sub-dict.
- **No placeholders:** every step has complete code. Task 9's conditional skip is an explicit, justified escape hatch (missing build/GPU environment), not a vague TBD.
- **Design gap caught and fixed during this plan's own self-review (not left for the implementer):** an early draft of Task 6 gave `find_corresponding_draws` a single `(controller, structured_file)` pair, mirroring `decode_post_vs_outputs`'s/`decode_mesh_inputs`'s single-capture convention too closely. Since `capture_a`/`capture_b` are ordinarily two different open replay sessions, that signature couldn't correctly read the reference draw from one capture and candidates from another. Fixed by giving `find_corresponding_draws` two separate `(controller, structured_file)` parameter pairs — the reference always reads from the `_a` pair, every candidate from the `_b` pair — which also transparently supports the same-capture case (caller passes the same pair twice) with no special-casing. Task 7's wiring reflects this directly; no unresolved gap remains.
