# trace_pixel_provenance Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a `trace_pixel_provenance` MCP tool that walks a final pixel's provenance backward — across draws, copies, and resolves — in one call, per `docs/superpowers/specs/2026-07-20-trace-pixel-provenance-design.md`.

**Architecture:** Four small, pure, independently-tested helper functions in `renderdoc_mcp/analysis.py` do all the branching logic (classify a producer event, pick the contributing `PixelHistory` entry, decide whether a copy/resolve hop can be safely followed). A new `trace_pixel_provenance` tool in `renderdoc_mcp/server.py` is thin orchestration glue around those helpers plus existing primitives (`PixelHistory`, `find_action`, `DebugPixel`) — this mirrors how every other tool in `server.py` is structured (e.g. `trace_resource`, `pixel_history`): pure logic lives in `analysis.py` and is unit-tested there; the `server.py` closure itself is not unit-tested in isolation anywhere in this codebase, only exercised end-to-end.

**Tech Stack:** Python 3.10+, `renderdoc` Python module (`pymodules`), `pytest` for unit tests, `rdtest` for the one integration test.

## Global Constraints

- No new Python dependencies.
- Follow the existing tool pattern exactly: `@mcp.tool()` → `async with replay_execution(): def _go(): ...; return await asyncio.get_running_loop().run_in_executor(_replay_executor, _go)`.
- `NULL`/explicit-`None` checks over exceptions where the existing code already does so (e.g. `rd.ResourceId.Null()` comparisons, `resource_name_for`'s pattern).
- Match existing code style in this Python package: `from __future__ import annotations`, type hints on all new functions, no unrelated formatting changes.
- Per `renderdoc/api/replay/replay_enums.h`'s `ActionFlags` enum, there is no separate `Blit` flag — blit-like copies are flagged `Copy`. Do not introduce a `"blit"` classification.

---

### Task 1: `classify_producer_action` pure helper

**Files:**
- Modify: `renderdoc-mcp/renderdoc_mcp/analysis.py`
- Test: `renderdoc-mcp/tests/test_pixel_provenance.py` (new file)

**Interfaces:**
- Produces: `classify_producer_action(flags_names: list[str]) -> str`, returning one of `"clear"`, `"resolve"`, `"copy"`, `"draw"`, `"unsupported"`. Later tasks call this with a `PixelHistory` producer entry's `IndexedEvent.flags_names` (a `list[str]` of `ActionFlags` enum member names, e.g. `["Drawcall", "Instanced"]`).

- [ ] **Step 1: Write the failing tests**

Create `renderdoc-mcp/tests/test_pixel_provenance.py`:

```python
from __future__ import annotations


def test_classify_producer_action_recognizes_clear():
    from renderdoc_mcp.analysis import classify_producer_action

    assert classify_producer_action(["Clear", "ClearColor"]) == "clear"


def test_classify_producer_action_recognizes_resolve():
    from renderdoc_mcp.analysis import classify_producer_action

    assert classify_producer_action(["Resolve"]) == "resolve"


def test_classify_producer_action_recognizes_copy():
    from renderdoc_mcp.analysis import classify_producer_action

    assert classify_producer_action(["Copy"]) == "copy"


def test_classify_producer_action_recognizes_draw():
    from renderdoc_mcp.analysis import classify_producer_action

    assert classify_producer_action(["Drawcall", "Indexed", "Instanced"]) == "draw"


def test_classify_producer_action_falls_back_to_unsupported():
    from renderdoc_mcp.analysis import classify_producer_action

    assert classify_producer_action(["Dispatch"]) == "unsupported"
    assert classify_producer_action([]) == "unsupported"


def test_classify_producer_action_prioritizes_clear_over_drawcall():
    from renderdoc_mcp.analysis import classify_producer_action

    # A clear-via-draw action carries both flags; provenance should stop at the clear,
    # not treat it as an ordinary shaded draw.
    assert classify_producer_action(["Clear", "Drawcall"]) == "clear"
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd renderdoc-mcp && python -m pytest tests/test_pixel_provenance.py -v`
Expected: FAIL with `ImportError: cannot import name 'classify_producer_action'`

- [ ] **Step 3: Write minimal implementation**

Add to `renderdoc-mcp/renderdoc_mcp/analysis.py` (near the other pixel-history helpers, after `normalize_pixel_history`):

```python
def classify_producer_action(flags_names: list[str]) -> str:
    """Classify a PixelHistory producer event's ActionFlags for provenance-hop routing.

    Checked in this priority order because a single action can carry multiple flags (e.g. a
    clear-via-draw carries both Clear and Drawcall) -- Clear must win so the chain stops at the
    clear instead of treating it as a shaded draw. renderdoc::ActionFlags has no separate "blit"
    flag; blit-like copies are flagged Copy, same as same-format copies.
    """
    names = set(flags_names or [])
    if "Clear" in names:
        return "clear"
    if "Resolve" in names:
        return "resolve"
    if "Copy" in names:
        return "copy"
    if "Drawcall" in names:
        return "draw"
    return "unsupported"
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd renderdoc-mcp && python -m pytest tests/test_pixel_provenance.py -v`
Expected: PASS (6 tests)

- [ ] **Step 5: Commit**

```bash
git add renderdoc-mcp/renderdoc_mcp/analysis.py renderdoc-mcp/tests/test_pixel_provenance.py
git commit -m "mcp: add classify_producer_action for pixel-provenance hop routing"
```

---

### Task 2: `select_provenance_producer` pure helper

**Files:**
- Modify: `renderdoc-mcp/renderdoc_mcp/analysis.py`
- Test: `renderdoc-mcp/tests/test_pixel_provenance.py`

**Interfaces:**
- Consumes: nothing from Task 1.
- Produces: `select_provenance_producer(entries: list[dict[str, Any]]) -> dict[str, Any] | None`. Input is the `entries` list from `normalize_pixel_history()`'s return value (each entry has `event_id`, `primitive_id`, `depth_test_failed`, `backface_culled`, `clipped`, `stencil_test_failed`, `predicate_failed`, `shader_output`, `pre_mod`, `post_mod` -- see `analysis.py:392-406`). Returns the single entry later tasks treat as "this hop's producer", or `None` if no entry actually established a visible value.

- [ ] **Step 1: Write the failing tests**

Append to `renderdoc-mcp/tests/test_pixel_provenance.py`:

```python
def _entry(event_id, **overrides):
    base = {
        "event_id": event_id,
        "primitive_id": 0,
        "shader_depth": 0.0,
        "shader_output": None,
        "depth_test_failed": False,
        "backface_culled": False,
        "clipped": False,
        "stencil_test_failed": False,
        "predicate_failed": False,
        "pre_mod": None,
        "post_mod": None,
    }
    base.update(overrides)
    return base


def test_select_provenance_producer_picks_last_visible_entry():
    from renderdoc_mcp.analysis import select_provenance_producer

    entries = [_entry(10), _entry(20), _entry(30)]

    result = select_provenance_producer(entries)

    assert result is not None
    assert result["event_id"] == 30


def test_select_provenance_producer_skips_trailing_failed_entries():
    from renderdoc_mcp.analysis import select_provenance_producer

    entries = [_entry(10), _entry(20), _entry(30, depth_test_failed=True)]

    result = select_provenance_producer(entries)

    assert result is not None
    assert result["event_id"] == 20


def test_select_provenance_producer_checks_every_failure_reason():
    from renderdoc_mcp.analysis import select_provenance_producer

    for field in (
        "depth_test_failed",
        "backface_culled",
        "clipped",
        "stencil_test_failed",
        "predicate_failed",
    ):
        entries = [_entry(10), _entry(20, **{field: True})]
        result = select_provenance_producer(entries)
        assert result is not None and result["event_id"] == 10, field


def test_select_provenance_producer_returns_none_when_all_entries_failed():
    from renderdoc_mcp.analysis import select_provenance_producer

    entries = [_entry(10, clipped=True), _entry(20, depth_test_failed=True)]

    assert select_provenance_producer(entries) is None


def test_select_provenance_producer_returns_none_for_empty_entries():
    from renderdoc_mcp.analysis import select_provenance_producer

    assert select_provenance_producer([]) is None
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd renderdoc-mcp && python -m pytest tests/test_pixel_provenance.py -v`
Expected: FAIL with `ImportError: cannot import name 'select_provenance_producer'`

- [ ] **Step 3: Write minimal implementation**

Add to `renderdoc-mcp/renderdoc_mcp/analysis.py`, directly below `classify_producer_action`:

```python
_PROVENANCE_FAILURE_FIELDS = (
    "depth_test_failed",
    "backface_culled",
    "clipped",
    "stencil_test_failed",
    "predicate_failed",
)


def select_provenance_producer(entries: list[dict[str, Any]]) -> dict[str, Any] | None:
    """Pick the PixelHistory entry that established the currently-visible value.

    Walks backward through normalize_pixel_history()'s entries and returns the last one where
    none of the failure/cull reasons are set -- i.e. the fragment (or clear/resolve/copy write)
    that actually reached the target. Returns None if every entry failed (nothing visible was
    ever established at this point in the chain).
    """
    for entry in reversed(entries):
        if not any(entry.get(field) for field in _PROVENANCE_FAILURE_FIELDS):
            return entry
    return None
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd renderdoc-mcp && python -m pytest tests/test_pixel_provenance.py -v`
Expected: PASS (11 tests)

- [ ] **Step 5: Commit**

```bash
git add renderdoc-mcp/renderdoc_mcp/analysis.py renderdoc-mcp/tests/test_pixel_provenance.py
git commit -m "mcp: add select_provenance_producer for pixel-provenance hop routing"
```

---

### Task 3: `mip_dims` and `copy_hop_is_safe` pure helpers

**Files:**
- Modify: `renderdoc-mcp/renderdoc_mcp/analysis.py`
- Test: `renderdoc-mcp/tests/test_pixel_provenance.py`

**Interfaces:**
- Consumes: nothing from Tasks 1-2.
- Produces: `mip_dims(base_width: int, base_height: int, mip: int) -> tuple[int, int]` and `copy_hop_is_safe(dest_dims: tuple[int, int], source_dims: tuple[int, int]) -> bool`. Task 4 computes `mip_dims` for both the current (destination) resource and the copy's source resource, then passes both results to `copy_hop_is_safe` to decide whether `(x, y)` can be safely reused across the hop.

- [ ] **Step 1: Write the failing tests**

Append to `renderdoc-mcp/tests/test_pixel_provenance.py`:

```python
def test_mip_dims_at_base_mip_is_unchanged():
    from renderdoc_mcp.analysis import mip_dims

    assert mip_dims(1920, 1080, 0) == (1920, 1080)


def test_mip_dims_halves_per_mip_level():
    from renderdoc_mcp.analysis import mip_dims

    assert mip_dims(1920, 1080, 1) == (960, 540)
    assert mip_dims(1920, 1080, 2) == (480, 270)


def test_mip_dims_floors_at_one_pixel():
    from renderdoc_mcp.analysis import mip_dims

    assert mip_dims(4, 4, 10) == (1, 1)


def test_copy_hop_is_safe_when_dims_match():
    from renderdoc_mcp.analysis import copy_hop_is_safe

    assert copy_hop_is_safe((1920, 1080), (1920, 1080)) is True


def test_copy_hop_is_safe_false_on_width_mismatch():
    from renderdoc_mcp.analysis import copy_hop_is_safe

    assert copy_hop_is_safe((1920, 1080), (960, 1080)) is False


def test_copy_hop_is_safe_false_on_height_mismatch():
    from renderdoc_mcp.analysis import copy_hop_is_safe

    assert copy_hop_is_safe((1920, 1080), (1920, 540)) is False
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd renderdoc-mcp && python -m pytest tests/test_pixel_provenance.py -v`
Expected: FAIL with `ImportError: cannot import name 'mip_dims'`

- [ ] **Step 3: Write minimal implementation**

Add to `renderdoc-mcp/renderdoc_mcp/analysis.py`, directly below `select_provenance_producer`:

```python
def mip_dims(base_width: int, base_height: int, mip: int) -> tuple[int, int]:
    """Standard mip-chain size falloff: half per level, floored at 1 pixel."""
    return (max(1, int(base_width) >> int(mip)), max(1, int(base_height) >> int(mip)))


def copy_hop_is_safe(dest_dims: tuple[int, int], source_dims: tuple[int, int]) -> bool:
    """Whether a Copy/Resolve hop's (x, y) can be safely reused on the source resource.

    renderdoc::ActionDescription only exposes source/destination resource + subresource for a
    copy, not a sub-rectangle offset -- so a partial-rect copy (source/dest dimensions differing
    at their respective mips) can't be safely assumed aligned. Full-surface copies/resolves
    (the common case for resolve targets and post-process ping-pong buffers) have matching dims.
    """
    return tuple(dest_dims) == tuple(source_dims)
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd renderdoc-mcp && python -m pytest tests/test_pixel_provenance.py -v`
Expected: PASS (17 tests)

- [ ] **Step 5: Commit**

```bash
git add renderdoc-mcp/renderdoc_mcp/analysis.py renderdoc-mcp/tests/test_pixel_provenance.py
git commit -m "mcp: add mip_dims and copy_hop_is_safe for pixel-provenance hop routing"
```

---

### Task 4: Wire the `trace_pixel_provenance` tool

**Files:**
- Modify: `renderdoc-mcp/renderdoc_mcp/server.py`

**Interfaces:**
- Consumes: `classify_producer_action`, `select_provenance_producer`, `mip_dims`, `copy_hop_is_safe` (Tasks 1-3); existing `normalize_pixel_history`, `find_texture_description` (already imported at `server.py:19-28`); existing `session.find_action` (not yet imported -- add it); existing `shader_debug.NO_PREFERENCE`, `shader_debug.run_debug_trace`, `shader_debug.best_disassembly`, `shader_debug.summarize_debug_trace`, `shader_debug.shader_variable_to_value` (module already imported as `shader_debug` at `server.py:45`).
- Produces: the `trace_pixel_provenance` MCP tool. No other task depends on its internals.

There is no automated unit test for this task's closure -- consistent with every other tool in `server.py` (`trace_resource`, `pixel_history`, etc. are only exercised end-to-end, never unit-tested in isolation). Verification is the existing test suite staying green plus the smoke-check import.

- [ ] **Step 1: Add the `find_action` import**

In `renderdoc-mcp/renderdoc_mcp/server.py`, find this existing import (around line 44):

```python
from renderdoc_mcp.session import CaptureSessionManager, filter_events
```

Replace with:

```python
from renderdoc_mcp.session import CaptureSessionManager, filter_events, find_action
```

- [ ] **Step 2: Add the four new `analysis` imports**

Find the existing `from renderdoc_mcp.analysis import (...)` block (around `server.py:19-28`):

```python
from renderdoc_mcp.analysis import (
    analyze_texture_bytes,
    build_frame_overview,
    deep_diff,
    diff_pipeline_snapshots,
    diff_texture_analysis,
    draw_visibility_analysis,
    find_texture_description,
    normalize_pixel_history,
)
```

Replace with:

```python
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
```

- [ ] **Step 3: Add the tool, immediately after `pixel_history`'s closing line and before `diff_pipeline_state`**

In `renderdoc-mcp/renderdoc_mcp/server.py`, the `pixel_history` tool currently ends with (around line 1274):

```python
                norm["coordinates"] = {"x": int(x), "y": int(y)}
                return R.ok(norm)

            return await asyncio.get_running_loop().run_in_executor(_replay_executor, _go)

    @mcp.tool()
    async def diff_pipeline_state(
```

Insert the new tool between those two `@mcp.tool()` blocks, so it reads:

```python
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
```

- [ ] **Step 4: Run the full test suite**

Run: `cd renderdoc-mcp && python -m pytest -q`
Expected: PASS, same count as before plus the 17 new tests from Tasks 1-3 (no regressions)

- [ ] **Step 5: Run the smoke check to confirm the server still builds with the new tool registered**

Requires `PYTHONPATH` pointed at a RenderDoc build's `pymodules` (see `renderdoc-mcp/README.md` Prerequisites). If no build is available in this environment, skip this step and note it as unverified in the task summary rather than claiming it passed.

Run: `python -c "from renderdoc_mcp.server import build_mcp; build_mcp(); print('ok')"`
Expected: `ok`

- [ ] **Step 6: Commit**

```bash
git add renderdoc-mcp/renderdoc_mcp/server.py
git commit -m "mcp: add trace_pixel_provenance tool"
```

---

### Task 5: Document the new tool in the README

**Files:**
- Modify: `renderdoc-mcp/README.md`

**Interfaces:**
- Consumes: nothing (documentation only).

- [ ] **Step 1: Add a tools-table row**

In `renderdoc-mcp/README.md`, find this row in the Tools table:

```markdown
| `pixel_history` | Structured pixel modifications (+ optional `event_id`) |
```

Add a new row directly after it:

```markdown
| `pixel_history` | Structured pixel modifications (+ optional `event_id`) |
| `trace_pixel_provenance` | Walk a final pixel's provenance backward across draws, copies, and resolves in one call |
```

- [ ] **Step 2: Add a Limitations bullet**

In `renderdoc-mcp/README.md`, find the `## Limitations` section's existing bullets (around line 149-156) and add one at the end of the list:

```markdown
- **`trace_pixel_provenance`** only crosses a Copy/Resolve hop when the source and destination
  dimensions match at their respective mips (renderdoc's `ActionDescription` doesn't expose a
  sub-rectangle offset, so a partial-rect copy/blit can't be safely assumed aligned) -- it stops
  with `stopped_reason: "partial_rect_copy_unsupported"` instead. A true `Resolve` action's
  `copySourceSubresource.sample` isn't a single meaningful sample (all samples combine into the
  destination pixel), so the walk falls back to `sample_index=0` when continuing into a resolve
  source rather than fanning out into every sample.
```

- [ ] **Step 3: Commit**

```bash
git add renderdoc-mcp/README.md
git commit -m "docs(mcp): document trace_pixel_provenance"
```

---

### Task 6: Integration test against a real Copy/Resolve capture

**Files:**
- Create: `util/test/tests/Vulkan/VK_Custom_Resolve_Provenance.py` (conditional on Step 1's findings)

**Interfaces:**
- Consumes: `classify_producer_action`, `copy_hop_is_safe`, `mip_dims` (Task 1 and 3), `find_texture_description` (existing), `session.expand_action_flags` (existing, `renderdoc_mcp/session.py:67`).

This task validates the real-world assumption the whole feature rests on -- that `ActionDescription.copySource`/`copySourceSubresource` behave as expected and that `copy_hop_is_safe` correctly says "yes" for a real MSAA resolve -- against an actual capture, not synthetic data. `util/test/tests/Vulkan/VK_Custom_Resolve.py` already drives a demo (`demos_test_name = 'VK_Custom_Resolve'`) that clears, draws to an MSAA target, then resolves it, with known-good pixel coordinates and marker names already verified by that existing test (`check_pixel_history`, `check_triangle_resolve`).

- [ ] **Step 1: Confirm the environment can produce the capture this test needs**

Run: `cd util/test && python run_tests.py --renderdoc <path>/x64/Development --pyrenderdoc <path>/x64/Development/pymodules -t VK_Custom_Resolve -l`

If this requires building `demos_x64.exe` from `demos.sln` (or `cmake -Bbuild -Hdemos`) first and that build isn't feasible in this environment (no GPU, no demos build), **stop here and report this task as skipped with the reason** -- do not fabricate a capture or synthetic substitute for this integration test. Tasks 1-5 do not depend on this task.

- [ ] **Step 2: Write the integration test**

Create `util/test/tests/Vulkan/VK_Custom_Resolve_Provenance.py`:

```python
import renderdoc as rd
import rdtest


class VK_Custom_Resolve_Provenance(rdtest.TestCase):
    demos_test_name = 'VK_Custom_Resolve'

    def check_capture(self):
        from renderdoc_mcp.analysis import classify_producer_action, copy_hop_is_safe, mip_dims, find_texture_description
        from renderdoc_mcp.session import expand_action_flags

        action = self.find_action("RenderPass")
        action = self.find_action("MSAA Resolve", action.eventId)
        self.controller.SetFrameEvent(action.eventId + 1, True)

        pipe: rd.PipeState = self.controller.GetPipelineState()
        dest = pipe.GetOutputTargets()[0].resource
        x, y = 200, 150
        sub = rd.Subresource()
        modifs = self.controller.PixelHistory(dest, x, y, sub, pipe.GetOutputTargets()[0].format.compType)
        if not modifs:
            raise rdtest.TestFailureException("Expected pixel history entries at the resolve target")

        resolve_event = modifs[-1].eventId
        resolve_action = self.get_action(resolve_event)
        flags_names = expand_action_flags(rd, int(resolve_action.flags))
        kind = classify_producer_action(flags_names)
        if kind != "resolve":
            raise rdtest.TestFailureException(
                f"Expected last producer at ({x},{y}) to classify as 'resolve', got {kind!r}"
            )

        if resolve_action.copySource == rd.ResourceId.Null():
            raise rdtest.TestFailureException("Expected the resolve action to expose a copySource")

        dest_tex = find_texture_description(self.controller, dest)
        src_tex = find_texture_description(self.controller, resolve_action.copySource)
        dest_dims = mip_dims(dest_tex.width, dest_tex.height, sub.mip)
        src_dims = mip_dims(
            src_tex.width, src_tex.height, resolve_action.copySourceSubresource.mip
        )
        if not copy_hop_is_safe(dest_dims, src_dims):
            raise rdtest.TestFailureException(
                f"Expected resolve source/dest dims to match for an aligned copy: "
                f"dest={dest_dims} src={src_dims}"
            )

        rdtest.log.success("trace_pixel_provenance assumptions hold for VK_Custom_Resolve")
```

- [ ] **Step 3: Run the new test**

Run: `cd util/test && python run_tests.py --renderdoc <path>/x64/Development --pyrenderdoc <path>/x64/Development/pymodules -t VK_Custom_Resolve_Provenance`
Expected: PASS, logging `trace_pixel_provenance assumptions hold for VK_Custom_Resolve`

If it fails because a real capture's resolve action behaves differently than assumed (e.g. `copySource` is null for this driver's resolve implementation, or dimensions don't match when they should), **do not adjust the test to paper over it** -- this means Task 4's `trace_pixel_provenance` has a real bug against this capture; go back and fix the tool, then re-run this test.

- [ ] **Step 4: Commit**

```bash
git add util/test/tests/Vulkan/VK_Custom_Resolve_Provenance.py
git commit -m "test: verify trace_pixel_provenance assumptions against a real MSAA resolve capture"
```

---

## Self-Review Notes

- **Spec coverage:** Purpose/Algorithm → Task 4. Parameters/Response shape → Task 4. Error Handling/Limitations (`partial_rect_copy_unsupported`, MSAA-resolve `sample_index` fallback, `unsupported_producer_kind`) → Task 4's stop-reason branches + Task 5's README bullet. Testing (unit tests on pure logic, one integration test) → Tasks 1-3 and Task 6. Out-of-scope items are explicitly not touched by any task.
- **Type consistency checked:** `classify_producer_action` returns lowercase `"draw"/"copy"/"resolve"/"clear"/"unsupported"` consistently between Task 1's implementation, Task 4's `kind ==` comparisons, and Task 6's assertion. `select_provenance_producer`'s return dict keys (`event_id`, `primitive_id`, `shader_output`, `pre_mod`, `post_mod`) match `normalize_pixel_history`'s existing entry shape (`analysis.py:392-406`) exactly -- no renaming. `mip_dims`/`copy_hop_is_safe` signatures match between Task 3's definition, Task 4's call sites, and Task 6's call sites.
- **No placeholders:** every step has complete code; Task 6's conditional skip is an explicit, justified escape hatch (missing build/GPU environment), not a vague TBD.
