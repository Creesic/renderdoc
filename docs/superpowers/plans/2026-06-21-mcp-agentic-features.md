# MCP Agentic Features Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add five features to `renderdoc-mcp/` for open-ended emulation debugging: `detect_pipeline_anomalies`, `get_texture_image`, `read_constant_buffer`, `get_debug_messages`, and `get_frame_overview`.

**Architecture:** Pure helper functions live in `analysis.py`, `imaging.py`, and `cbuffer.py`; server tools in `server.py`; serialization changes in `serialize.py`. Pure functions (no RenderDoc required) are unit-tested in `renderdoc-mcp/tests/`; replay-backed functions are verified via smoke import.

**Tech Stack:** Python 3.10+, FastMCP (existing), `struct` (stdlib), `tempfile`/`os` (stdlib), `pytest` (dev).

## Global Constraints

- No new runtime dependencies — no PIL, no numpy, no new packages in `pyproject.toml`.
- All tools follow the existing pattern: `async with replay_execution()` → `asyncio.to_thread(_go)` → `R.ok(...)` / `R.err(...)`.
- All new tool responses include `resource_name` when `rdutil.resource_name_for()` returns a non-empty string.
- `renderdoc` module is imported lazily via `rdutil.get_renderdoc()` only inside functions, never at module import time.
- Tests must pass without a RenderDoc `pymodules` directory — only test pure functions.
- Working directory for all commands: `renderdoc-mcp/`.

---

## File Map

| Action | Path | Responsibility |
|---|---|---|
| Modify | `renderdoc_mcp/serialize.py` | Expand `serialize_blend_state` with src/dst factors; add `anomalies` field to `normalize_pipeline_state` |
| Modify | `renderdoc_mcp/analysis.py` | Add `detect_pipeline_anomalies`, `build_frame_overview` |
| Create | `renderdoc_mcp/imaging.py` | `detect_texture_anomalies`, `describe_texture`, `save_texture_as_png_bytes` |
| Create | `renderdoc_mcp/cbuffer.py` | `decode_cb_bytes`, `detect_variable_anomalies` |
| Modify | `renderdoc_mcp/server.py` | Add 4 new tools: `get_texture_image`, `read_constant_buffer`, `get_debug_messages`, `get_frame_overview` |
| Create | `tests/__init__.py` | Empty — marks tests/ as a package |
| Create | `tests/test_pipeline_anomalies.py` | Unit tests for `detect_pipeline_anomalies` |
| Create | `tests/test_imaging.py` | Unit tests for `detect_texture_anomalies`, `describe_texture` |
| Create | `tests/test_cbuffer.py` | Unit tests for `decode_cb_bytes`, `detect_variable_anomalies` |

---

## Task 1: `detect_pipeline_anomalies` + blend state expansion

**Files:**
- Modify: `renderdoc_mcp/serialize.py` (lines 177–196: `serialize_blend_state`)
- Modify: `renderdoc_mcp/serialize.py` (lines 300–354: `normalize_pipeline_state`)
- Modify: `renderdoc_mcp/analysis.py` (add function after `draw_visibility_analysis`)
- Create: `tests/__init__.py`
- Create: `tests/test_pipeline_anomalies.py`

**Interfaces:**
- Produces: `detect_pipeline_anomalies(snapshot: dict[str, Any]) -> list[str]` in `analysis.py`
- Produces: `snapshot["blend"]["targets"][i]` now includes `"src_color"`, `"dst_color"`, `"color_op"` keys
- Produces: `snapshot["anomalies"]` in every `normalize_pipeline_state` result

- [ ] **Step 1: Create tests directory and write failing tests**

Create `tests/__init__.py` (empty file).

Create `tests/test_pipeline_anomalies.py`:

```python
from __future__ import annotations

import pytest


def _snap(**overrides):
    """Build a minimal pipeline snapshot dict."""
    base = {
        "viewports": [{"width": 1280.0, "height": 720.0, "x": 0.0, "y": 0.0}],
        "scissors": [{"x": 0, "y": 0, "width": 1280, "height": 720, "enabled": True}],
        "depth": {"depth_enable": True, "depth_writes": True, "depth_function": "Less"},
        "blend": {
            "targets": [{
                "slot": 0, "blend_enable": False, "write_mask": 15,
                "src_color": "One", "dst_color": "Zero", "color_op": "Add",
            }]
        },
        "targets": {
            "color_targets": [{"resource_id": "ResourceId::1", "slot": 0}],
            "depth_target": {"resource_id": "ResourceId::2"},
        },
        "action": {"num_vertices": 100, "num_instances": 1, "num_indices": 0, "flags": ["Drawcall"]},
    }
    base.update(overrides)
    return base


def test_no_anomalies_on_clean_snapshot():
    from renderdoc_mcp.analysis import detect_pipeline_anomalies
    assert detect_pipeline_anomalies(_snap()) == []


def test_zero_viewport():
    from renderdoc_mcp.analysis import detect_pipeline_anomalies
    snap = _snap(viewports=[{"width": 0.0, "height": 720.0}])
    assert "zero_viewport" in detect_pipeline_anomalies(snap)


def test_scissor_clips_all():
    from renderdoc_mcp.analysis import detect_pipeline_anomalies
    snap = _snap(scissors=[{"x": 0, "y": 0, "width": 0, "height": 0, "enabled": True}])
    assert "scissor_clips_all" in detect_pipeline_anomalies(snap)


def test_depth_test_disabled_on_draw():
    from renderdoc_mcp.analysis import detect_pipeline_anomalies
    snap = _snap(depth={"depth_enable": False, "depth_writes": False, "depth_function": "Less"})
    assert "depth_test_disabled" in detect_pipeline_anomalies(snap)


def test_depth_write_disabled_when_test_on():
    from renderdoc_mcp.analysis import detect_pipeline_anomalies
    snap = _snap(depth={"depth_enable": True, "depth_writes": False, "depth_function": "Less"})
    assert "depth_write_disabled" in detect_pipeline_anomalies(snap)


def test_no_color_outputs():
    from renderdoc_mcp.analysis import detect_pipeline_anomalies
    snap = _snap(targets={"color_targets": [{"resource_id": "Null", "slot": 0}], "depth_target": None})
    assert "no_color_outputs" in detect_pipeline_anomalies(snap)


def test_additive_blend():
    from renderdoc_mcp.analysis import detect_pipeline_anomalies
    snap = _snap(blend={
        "targets": [{
            "slot": 0, "blend_enable": True, "write_mask": 15,
            "src_color": "SrcAlpha", "dst_color": "One", "color_op": "Add",
        }]
    })
    assert "additive_blend" in detect_pipeline_anomalies(snap)


def test_additive_blend_not_flagged_when_disabled():
    from renderdoc_mcp.analysis import detect_pipeline_anomalies
    snap = _snap(blend={
        "targets": [{
            "slot": 0, "blend_enable": False, "write_mask": 15,
            "src_color": "SrcAlpha", "dst_color": "One", "color_op": "Add",
        }]
    })
    assert "additive_blend" not in detect_pipeline_anomalies(snap)
```

- [ ] **Step 2: Run tests — expect ImportError (function doesn't exist yet)**

```
python -m pytest tests/test_pipeline_anomalies.py -v
```

Expected: all tests fail with `ImportError: cannot import name 'detect_pipeline_anomalies'`.

- [ ] **Step 3: Expand `serialize_blend_state` in `serialize.py`**

Find the block inside `serialize_blend_state` that builds each target dict (around line 184). Replace:

```python
        targets.append(
            {
                "slot": i,
                "blend_enable": bool(getattr(bt, "blendEnable", False)),
                "logic_operation": enum_name(getattr(bt, "logicOperation", None)),
                "write_mask": int(getattr(bt, "writeMask", 0)),
            }
        )
```

With:

```python
        _cb_eq = getattr(bt, "colorBlend", None)
        targets.append(
            {
                "slot": i,
                "blend_enable": bool(getattr(bt, "blendEnable", False)),
                "logic_operation": enum_name(getattr(bt, "logicOperation", None)),
                "write_mask": int(getattr(bt, "writeMask", 0)),
                "src_color": enum_name(getattr(_cb_eq, "source", None)) if _cb_eq is not None else "",
                "dst_color": enum_name(getattr(_cb_eq, "destination", None)) if _cb_eq is not None else "",
                "color_op": enum_name(getattr(_cb_eq, "operation", None)) if _cb_eq is not None else "",
            }
        )
```

- [ ] **Step 4: Add `detect_pipeline_anomalies` to `analysis.py`**

Add this function after the `_try_list` helper (around line 381, before `diff_texture_analysis`):

```python
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
        if t.get("blend_enable") and t.get("dst_color") == "One":
            anomalies.append("additive_blend")
            break

    return anomalies
```

- [ ] **Step 5: Wire `detect_pipeline_anomalies` into `normalize_pipeline_state` in `serialize.py`**

Add this import at the top of `serialize.py` (after the existing `from renderdoc_mcp.session import ...` line):

```python
from renderdoc_mcp.analysis import detect_pipeline_anomalies
```

Then find the end of `normalize_pipeline_state` (around line 353) where it returns `data`. Change:

```python
    data["probable_causes"] = heuristic_pipeline_issues(data)
    return data
```

To:

```python
    data["probable_causes"] = heuristic_pipeline_issues(data)
    data["anomalies"] = detect_pipeline_anomalies(data)
    return data
```

Also add `anomalies` to `draw_visibility_analysis` in `analysis.py`. Find the return dict at the end of that function (around line 371) and add one field:

```python
    return {
        "event_id": event_id,
        "samples_passed": samples_passed,
        "unsupported_reason": unsupported_reason,
        "hypotheses": hypotheses,
        "evidence": evidence,
        "anomalies": detect_pipeline_anomalies(pipe_snapshot),
    }
```

- [ ] **Step 6: Run tests — expect all pass**

```
python -m pytest tests/test_pipeline_anomalies.py -v
```

Expected output: `7 passed`.

- [ ] **Step 7: Run smoke import**

```
python scripts/smoke_import.py
```

Expected: `OK: renderdoc_mcp.server.build_mcp`

- [ ] **Step 8: Commit**

```
git add renderdoc_mcp/serialize.py renderdoc_mcp/analysis.py tests/__init__.py tests/test_pipeline_anomalies.py
git commit -m "feat(mcp): add detect_pipeline_anomalies and blend factor serialization"
```

---

## Task 2: `imaging.py` — texture anomaly detection and description (pure functions)

**Files:**
- Create: `renderdoc_mcp/imaging.py`
- Create: `tests/test_imaging.py`

**Interfaces:**
- Produces: `detect_texture_anomalies(stats: dict) -> list[str]` — `stats` is the dict returned by `analyze_texture_bytes`
- Produces: `describe_texture(stats: dict, anomalies: list[str]) -> str`

- [ ] **Step 1: Write failing tests**

Create `tests/test_imaging.py`:

```python
from __future__ import annotations

import pytest


def _stats(**overrides):
    """Build a minimal analyze_texture_bytes-style stats dict."""
    base = {
        "supported_stats": True,
        "mean_channels": [0.5, 0.5, 0.5, 1.0],
        "min_channels": [0.1, 0.1, 0.1, 1.0],
        "max_channels": [0.9, 0.9, 0.9, 1.0],
        "near_black_ratio": 0.0,
        "nan_pixel_count": 0,
        "format": {"comp_count": 4},
    }
    base.update(overrides)
    return base


# --- detect_texture_anomalies ---

def test_no_anomalies_on_normal_texture():
    from renderdoc_mcp.imaging import detect_texture_anomalies
    assert detect_texture_anomalies(_stats()) == []


def test_blank_detected():
    from renderdoc_mcp.imaging import detect_texture_anomalies
    s = _stats(mean_channels=[0.001, 0.001, 0.001, 1.0], near_black_ratio=0.99)
    assert "blank" in detect_texture_anomalies(s)


def test_saturated_detected():
    from renderdoc_mcp.imaging import detect_texture_anomalies
    s = _stats(mean_channels=[0.999, 0.999, 0.999, 1.0])
    assert "saturated" in detect_texture_anomalies(s)


def test_nan_detected():
    from renderdoc_mcp.imaging import detect_texture_anomalies
    s = _stats(nan_pixel_count=5)
    assert "nan_present" in detect_texture_anomalies(s)


def test_uniform_detected():
    from renderdoc_mcp.imaging import detect_texture_anomalies
    # Not blank (mean ~0.5) but very tight range
    s = _stats(
        mean_channels=[0.5, 0.5, 0.5, 1.0],
        min_channels=[0.499, 0.499, 0.499, 1.0],
        max_channels=[0.501, 0.501, 0.501, 1.0],
    )
    assert "uniform" in detect_texture_anomalies(s)


def test_blank_not_also_uniform():
    from renderdoc_mcp.imaging import detect_texture_anomalies
    s = _stats(
        mean_channels=[0.0, 0.0, 0.0, 1.0],
        min_channels=[0.0, 0.0, 0.0, 1.0],
        max_channels=[0.0, 0.0, 0.0, 1.0],
        near_black_ratio=1.0,
    )
    result = detect_texture_anomalies(s)
    assert "blank" in result
    assert "uniform" not in result


def test_alpha_zero_detected():
    from renderdoc_mcp.imaging import detect_texture_anomalies
    s = _stats(mean_channels=[0.5, 0.5, 0.5, 0.0], format={"comp_count": 4})
    assert "alpha_zero" in detect_texture_anomalies(s)


def test_unsupported_stats_returns_empty():
    from renderdoc_mcp.imaging import detect_texture_anomalies
    assert detect_texture_anomalies({"supported_stats": False}) == []


# --- describe_texture ---

def test_describe_blank():
    from renderdoc_mcp.imaging import describe_texture
    s = _stats(mean_channels=[0.001, 0.001, 0.001, 1.0], near_black_ratio=0.98)
    desc = describe_texture(s, ["blank"])
    assert "black" in desc.lower()
    assert "98%" in desc


def test_describe_nan():
    from renderdoc_mcp.imaging import describe_texture
    s = _stats(nan_pixel_count=3)
    desc = describe_texture(s, ["nan_present"])
    assert "nan" in desc.lower() or "NaN" in desc


def test_describe_normal():
    from renderdoc_mcp.imaging import describe_texture
    s = _stats(mean_channels=[0.4, 0.5, 0.6, 1.0])
    desc = describe_texture(s, [])
    assert "R=0.400" in desc
    assert "G=0.500" in desc
    assert "B=0.600" in desc


def test_describe_unsupported():
    from renderdoc_mcp.imaging import describe_texture
    s = {"supported_stats": False, "reason": "complex_format"}
    desc = describe_texture(s, [])
    assert "complex_format" in desc
```

- [ ] **Step 2: Run tests — expect ImportError**

```
python -m pytest tests/test_imaging.py -v
```

Expected: all fail with `ImportError: No module named 'renderdoc_mcp.imaging'`.

- [ ] **Step 3: Create `renderdoc_mcp/imaging.py` with pure functions**

```python
"""Texture anomaly detection and description generation (no RenderDoc import at module level)."""

from __future__ import annotations

import base64
import os
import tempfile
from typing import Any


def detect_texture_anomalies(stats: dict) -> list[str]:
    """Return anomaly keys from an analyze_texture_bytes stats dict."""
    if not stats.get("supported_stats"):
        return []

    anomalies: list[str] = []
    means = stats.get("mean_channels") or []
    mins = stats.get("min_channels") or []
    maxs = stats.get("max_channels") or []
    fmt = stats.get("format") or {}
    comp_count = int(fmt.get("comp_count", 4))
    nan_count = stats.get("nan_pixel_count") or 0

    rgb_means = [float(m) for m in means[:3] if m is not None]

    if rgb_means and all(m < 0.01 for m in rgb_means):
        anomalies.append("blank")
    elif rgb_means and all(m > 0.99 for m in rgb_means):
        anomalies.append("saturated")

    if nan_count > 0:
        anomalies.append("nan_present")

    # Uniform: tight range across RGB (skip if already blank to avoid redundancy)
    if "blank" not in anomalies:
        rgb_mins = [float(v) for v in mins[:3] if v is not None]
        rgb_maxs = [float(v) for v in maxs[:3] if v is not None]
        if rgb_mins and rgb_maxs and len(rgb_mins) == len(rgb_maxs):
            ranges = [b - a for a, b in zip(rgb_mins, rgb_maxs)]
            if all(r < 0.01 for r in ranges):
                anomalies.append("uniform")

    # Alpha
    if comp_count >= 4 and len(means) >= 4 and means[3] is not None:
        if float(means[3]) < 0.01:
            anomalies.append("alpha_zero")

    return anomalies


def describe_texture(stats: dict, anomalies: list[str]) -> str:
    """Return a human-readable one-sentence description of the texture's content."""
    if not stats.get("supported_stats"):
        reason = stats.get("reason", "unsupported format")
        return "Statistics unavailable ({}).".format(reason)

    means = stats.get("mean_channels") or [0.0, 0.0, 0.0, 1.0]
    nan_count = stats.get("nan_pixel_count") or 0
    black_ratio = float(stats.get("near_black_ratio") or 0.0)

    def _mean(i: int) -> float:
        return float(means[i]) if len(means) > i and means[i] is not None else 0.0

    if "blank" in anomalies:
        pct = int(black_ratio * 100)
        avg = sum(_mean(i) for i in range(3)) / 3.0
        return (
            "Nearly entirely black ({}% blank pixels, mean brightness ~{:.3f}). "
            "Draw calls targeting this RT may not be executing, or the shader is outputting zero."
        ).format(pct, avg)

    if "nan_present" in anomalies:
        return (
            "Contains {} NaN pixel(s) — likely an uninitialized or incorrectly cleared "
            "float buffer, or a division-by-zero in the shader."
        ).format(nan_count)

    if "saturated" in anomalies:
        return (
            "Nearly entirely white/saturated (mean brightness ~1.0). "
            "Possible overexposure or uncleared HDR float buffer."
        )

    if "uniform" in anomalies:
        return (
            "Solid or near-solid color (very low pixel variance). "
            "Possible clear target or shader outputting a constant. "
            "Mean: R={:.3f} G={:.3f} B={:.3f}."
        ).format(_mean(0), _mean(1), _mean(2))

    return "Appears to contain normal image content. Channel means: R={:.3f} G={:.3f} B={:.3f}.".format(
        _mean(0), _mean(1), _mean(2)
    )


def save_texture_as_png_bytes(
    controller: Any,
    rd: Any,
    rid: Any,
    mip: int,
    slice_index: int,
    max_dimension: int,
) -> bytes | None:
    """Export a texture slice to PNG bytes via SaveTexture. Returns None on failure."""
    # Pick the smallest mip whose longest axis is still >= max_dimension
    best_mip = mip
    try:
        textures = controller.GetTextures()
        for tex in textures:
            if tex.resourceId == rid:
                num_mips = int(getattr(tex, "mips", 1))
                w = int(tex.width)
                h = int(tex.height)
                for m in range(num_mips):
                    mw = max(1, w >> m)
                    mh = max(1, h >> m)
                    if max(mw, mh) >= max_dimension:
                        best_mip = m
                    else:
                        break
                break
    except Exception:
        best_mip = mip

    ts = rd.TextureSave()
    ts.resourceId = rid
    ts.mip = best_mip
    ts.slice.sliceIndex = slice_index
    ts.alpha = rd.AlphaMapping.Preserve
    ts.destType = rd.FileType.PNG

    tmp = tempfile.NamedTemporaryFile(suffix=".png", delete=False)
    tmp_path = tmp.name
    tmp.close()
    try:
        res = controller.SaveTexture(ts, tmp_path)
        succeeded = True
        if isinstance(res, bool):
            succeeded = res
        elif hasattr(res, "code"):
            succeeded = res.code == rd.ResultCode.Succeeded
        if not succeeded:
            return None
        with open(tmp_path, "rb") as f:
            return f.read()
    except Exception:
        return None
    finally:
        try:
            os.unlink(tmp_path)
        except OSError:
            pass
```

- [ ] **Step 4: Run tests — expect all pass**

```
python -m pytest tests/test_imaging.py -v
```

Expected: `13 passed`.

- [ ] **Step 5: Commit**

```
git add renderdoc_mcp/imaging.py tests/test_imaging.py
git commit -m "feat(mcp): add imaging.py with texture anomaly detection and PNG export"
```

---

## Task 3: `get_texture_image` tool

**Files:**
- Modify: `renderdoc_mcp/server.py` (add tool inside `build_mcp`, after `save_texture`)

**Interfaces:**
- Consumes: `imaging.detect_texture_anomalies`, `imaging.describe_texture`, `imaging.save_texture_as_png_bytes`
- Consumes: `analysis.analyze_texture_bytes`, `analysis.find_texture_description`
- Produces: `get_texture_image` MCP tool

- [ ] **Step 1: Add `get_texture_image` to `server.py`**

Add this import at the top of `server.py`, after the existing `from renderdoc_mcp.analysis import (...)` block:

```python
from renderdoc_mcp.imaging import (
    detect_texture_anomalies,
    describe_texture,
    save_texture_as_png_bytes,
)
```

Inside `build_mcp()`, add the following tool definition after the `save_texture` tool (after line ~399):

```python
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
                try:
                    rid = rdutil.parse_resource_id(resource_id)
                except ValueError as ex:
                    return R.err("bad_resource_id", str(ex))
                try:
                    sessions.set_frame_event(sess, int(event_id), True)
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

            return await asyncio.to_thread(_go)
```

- [ ] **Step 2: Verify smoke import**

```
python scripts/smoke_import.py
```

Expected: `OK: renderdoc_mcp.server.build_mcp`

- [ ] **Step 3: Commit**

```
git add renderdoc_mcp/server.py
git commit -m "feat(mcp): add get_texture_image tool with inline PNG and anomaly description"
```

---

## Task 4: `cbuffer.py` — constant buffer byte decoder (pure functions)

**Files:**
- Create: `renderdoc_mcp/cbuffer.py`
- Create: `tests/test_cbuffer.py`

**Interfaces:**
- Produces: `decode_cb_bytes(raw: bytes, constants: list, row_major: bool = True) -> list[dict]`
  - `constants` is a list of objects with `.name`, `.byteOffset`, `.type` (having `.rows`, `.columns`, `.baseByteWidth`, `.baseType`, `.members`)
  - Returns list of `{"name": str, "type": str, "value": Any}` dicts
- Produces: `detect_variable_anomalies(var: dict) -> str | None`
  - Returns one of: `"zero_matrix"`, `"identity_matrix"`, `"nan_or_inf"`, `"zero_vector"`, or `None`

- [ ] **Step 1: Write failing tests**

Create `tests/test_cbuffer.py`:

```python
from __future__ import annotations

import math
import struct

import pytest


def _make_const(name, byte_offset, rows=1, cols=1, base_type_name="Float", byte_width=4, members=None):
    """Build a mock ShaderConstant-like object."""
    class FakeType:
        pass

    class FakeConst:
        pass

    ct = FakeType()
    ct.rows = rows
    ct.columns = cols
    ct.baseByteWidth = byte_width
    ct.members = members or []

    class FakeCompType:
        name = base_type_name

    ct.baseType = FakeCompType()

    c = FakeConst()
    c.name = name
    c.byteOffset = byte_offset
    c.type = ct
    return c


# --- decode_cb_bytes ---

def test_decode_float_scalar():
    from renderdoc_mcp.cbuffer import decode_cb_bytes
    raw = struct.pack("<f", 1.234)
    result = decode_cb_bytes(raw, [_make_const("Time", 0, rows=1, cols=1)])
    assert len(result) == 1
    assert result[0]["name"] == "Time"
    assert result[0]["type"] == "float"
    assert abs(result[0]["value"] - 1.234) < 1e-4


def test_decode_float3_vector():
    from renderdoc_mcp.cbuffer import decode_cb_bytes
    raw = struct.pack("<fff", 0.1, 0.2, 0.3)
    result = decode_cb_bytes(raw, [_make_const("LightDir", 0, rows=1, cols=3)])
    assert result[0]["type"] == "float3"
    assert len(result[0]["value"]) == 3
    assert abs(result[0]["value"][1] - 0.2) < 1e-5


def test_decode_uint_scalar():
    from renderdoc_mcp.cbuffer import decode_cb_bytes
    raw = struct.pack("<I", 42)
    result = decode_cb_bytes(raw, [_make_const("Flags", 0, rows=1, cols=1, base_type_name="UInt")])
    assert result[0]["value"] == 42
    assert result[0]["type"] == "uint"


def test_decode_float4x4_matrix():
    from renderdoc_mcp.cbuffer import decode_cb_bytes
    # Identity matrix: 4 rows, each 16 bytes (4 floats)
    row = struct.pack("<ffff", 1.0, 0.0, 0.0, 0.0)
    raw = row + struct.pack("<ffff", 0.0, 1.0, 0.0, 0.0)
    raw += struct.pack("<ffff", 0.0, 0.0, 1.0, 0.0)
    raw += struct.pack("<ffff", 0.0, 0.0, 0.0, 1.0)
    result = decode_cb_bytes(raw, [_make_const("MVP", 0, rows=4, cols=4)])
    assert result[0]["type"] == "float4x4"
    mat = result[0]["value"]
    assert len(mat) == 4
    assert len(mat[0]) == 4
    assert mat[0][0] == 1.0
    assert mat[1][1] == 1.0
    assert mat[0][1] == 0.0


def test_decode_with_byte_offset():
    from renderdoc_mcp.cbuffer import decode_cb_bytes
    # 4 padding bytes then the float
    raw = struct.pack("<f", 0.0) + struct.pack("<f", 9.99)
    result = decode_cb_bytes(raw, [_make_const("Val", byte_offset=4, rows=1, cols=1)])
    assert abs(result[0]["value"] - 9.99) < 1e-4


def test_decode_multiple_constants():
    from renderdoc_mcp.cbuffer import decode_cb_bytes
    raw = struct.pack("<ff", 1.0, 2.0)
    consts = [
        _make_const("A", 0, rows=1, cols=1),
        _make_const("B", 4, rows=1, cols=1),
    ]
    result = decode_cb_bytes(raw, consts)
    assert len(result) == 2
    assert abs(result[0]["value"] - 1.0) < 1e-5
    assert abs(result[1]["value"] - 2.0) < 1e-5


# --- detect_variable_anomalies ---

def test_zero_matrix_detected():
    from renderdoc_mcp.cbuffer import detect_variable_anomalies
    var = {"name": "MVP", "type": "float4x4", "value": [[0,0,0,0],[0,0,0,0],[0,0,0,0],[0,0,0,0]]}
    assert detect_variable_anomalies(var) == "zero_matrix"


def test_identity_matrix_detected():
    from renderdoc_mcp.cbuffer import detect_variable_anomalies
    identity = [[1,0,0,0],[0,1,0,0],[0,0,1,0],[0,0,0,1]]
    var = {"name": "MVP", "type": "float4x4", "value": identity}
    assert detect_variable_anomalies(var) == "identity_matrix"


def test_nan_in_vector_detected():
    from renderdoc_mcp.cbuffer import detect_variable_anomalies
    var = {"name": "Dir", "type": "float3", "value": [0.0, float("nan"), 1.0]}
    assert detect_variable_anomalies(var) == "nan_or_inf"


def test_inf_detected():
    from renderdoc_mcp.cbuffer import detect_variable_anomalies
    var = {"name": "Scale", "type": "float", "value": float("inf")}
    assert detect_variable_anomalies(var) == "nan_or_inf"


def test_zero_vector_detected():
    from renderdoc_mcp.cbuffer import detect_variable_anomalies
    var = {"name": "Color", "type": "float4", "value": [0.0, 0.0, 0.0, 0.0]}
    assert detect_variable_anomalies(var) == "zero_vector"


def test_normal_value_no_anomaly():
    from renderdoc_mcp.cbuffer import detect_variable_anomalies
    var = {"name": "Scale", "type": "float", "value": 1.5}
    assert detect_variable_anomalies(var) is None
```

- [ ] **Step 2: Run tests — expect ImportError**

```
python -m pytest tests/test_cbuffer.py -v
```

Expected: all fail with `ImportError: No module named 'renderdoc_mcp.cbuffer'`.

- [ ] **Step 3: Create `renderdoc_mcp/cbuffer.py`**

```python
"""Decode RenderDoc constant buffer bytes using shader reflection metadata."""

from __future__ import annotations

import math
import struct
from typing import Any


def _comp_type_name(comp_type: Any) -> str:
    if comp_type is None:
        return "Float"
    n = getattr(comp_type, "name", None)
    if n:
        return str(n)
    return str(comp_type)


def _type_str(rows: int, cols: int, base: str) -> str:
    short = {
        "Float": "float", "Double": "double",
        "UInt": "uint", "UNorm": "uint",
        "SInt": "int", "SNorm": "int",
        "Bool": "bool",
    }.get(base, base.lower())
    if rows == 1 and cols == 1:
        return short
    if rows == 1:
        return "{}{}".format(short, cols)
    return "{}{}x{}".format(short, rows, cols)


def _unpack_scalar(raw: bytes, offset: int, base: str, bw: int) -> Any:
    if offset + bw > len(raw):
        return None
    chunk = raw[offset : offset + bw]
    try:
        if base in ("Float", "Double"):
            fmt = "<f" if bw == 4 else "<d" if bw == 8 else None
            return float(struct.unpack_from(fmt, chunk)[0]) if fmt else None
        if base in ("UInt", "UNorm"):
            fmt = {1: "<B", 2: "<H", 4: "<I"}.get(bw)
            return struct.unpack_from(fmt, chunk)[0] if fmt else None
        if base in ("SInt", "SNorm"):
            fmt = {1: "<b", 2: "<h", 4: "<i"}.get(bw)
            return struct.unpack_from(fmt, chunk)[0] if fmt else None
        if base == "Bool":
            fmt = {1: "<B", 2: "<H", 4: "<I"}.get(bw)
            return bool(struct.unpack_from(fmt, chunk)[0]) if fmt else None
    except struct.error:
        return None
    return None


def decode_cb_bytes(raw: bytes, constants: list, row_major: bool = True) -> list[dict]:
    """Decode constant buffer bytes into a list of typed variable dicts.

    Each output dict has keys: name (str), type (str), value (scalar/list/list-of-list).
    row_major=True matches HLSL cbuffer layout (rows padded to 16 bytes in memory).
    """
    results: list[dict] = []
    for const in constants:
        name = str(getattr(const, "name", "") or "")
        base_offset = int(getattr(const, "byteOffset", 0))
        ctype = getattr(const, "type", None)

        members = list(getattr(ctype, "members", None) or [])
        if members:
            for nested in decode_cb_bytes(raw, members, row_major):
                nested["name"] = "{}.{}".format(name, nested["name"])
            results.extend(decode_cb_bytes(raw, members, row_major))
            continue

        rows = int(getattr(ctype, "rows", 1) or 1)
        cols = int(getattr(ctype, "columns", 1) or 1)
        bw = int(getattr(ctype, "baseByteWidth", 4) or 4)
        base = _comp_type_name(getattr(ctype, "baseType", None))
        type_name = _type_str(rows, cols, base)

        if rows == 1 and cols == 1:
            value: Any = _unpack_scalar(raw, base_offset, base, bw)
        elif rows == 1:
            value = [_unpack_scalar(raw, base_offset + c * bw, base, bw) for c in range(cols)]
        else:
            # HLSL cbuffer: each row aligned to 16 bytes
            row_stride = 16
            value = [
                [_unpack_scalar(raw, base_offset + r * row_stride + c * bw, base, bw) for c in range(cols)]
                for r in range(rows)
            ]

        results.append({"name": name, "type": type_name, "value": value})

    return results


def detect_variable_anomalies(var: dict) -> str | None:
    """Return an anomaly key string for a decoded variable, or None if nothing suspicious."""
    value = var.get("value")
    type_name = var.get("type", "")
    if value is None:
        return None

    def _flatten(v: Any) -> list:
        if isinstance(v, list):
            out: list = []
            for item in v:
                out.extend(_flatten(item))
            return out
        return [v]

    flat = [x for x in _flatten(value) if x is not None]
    if not flat:
        return None

    def _is_bad(v: Any) -> bool:
        try:
            f = float(v)
            return math.isnan(f) or math.isinf(f)
        except (TypeError, ValueError):
            return False

    if any(_is_bad(v) for v in flat):
        return "nan_or_inf"

    is_matrix = isinstance(value, list) and value and isinstance(value[0], list)

    if is_matrix:
        if all(abs(float(v)) < 1e-10 for v in flat):
            return "zero_matrix"
        rows = value
        n = len(rows)
        if all(len(r) == n for r in rows):
            is_identity = all(
                abs(float(rows[r][c]) - (1.0 if r == c else 0.0)) < 1e-5
                for r in range(n)
                for c in range(n)
            )
            if is_identity:
                return "identity_matrix"
        return None

    if isinstance(value, list):
        if all(v is not None and abs(float(v)) < 1e-10 for v in flat):
            return "zero_vector"
        return None

    return None
```

- [ ] **Step 4: Run tests — expect all pass**

```
python -m pytest tests/test_cbuffer.py -v
```

Expected: `12 passed`.

- [ ] **Step 5: Commit**

```
git add renderdoc_mcp/cbuffer.py tests/test_cbuffer.py
git commit -m "feat(mcp): add cbuffer.py with constant buffer byte decoder"
```

---

## Task 5: `read_constant_buffer` tool

**Files:**
- Modify: `renderdoc_mcp/server.py` (add tool inside `build_mcp`, after `get_shader_reflection`)

**Interfaces:**
- Consumes: `cbuffer.decode_cb_bytes`, `cbuffer.detect_variable_anomalies`
- Consumes: `serialize.bindings_for_stage` (existing, for locating the CB buffer resource)
- Produces: `read_constant_buffer` MCP tool

- [ ] **Step 1: Add import and tool to `server.py`**

Add this import at the top of `server.py` after the existing imports:

```python
from renderdoc_mcp.cbuffer import decode_cb_bytes, detect_variable_anomalies
```

Inside `build_mcp()`, add after `get_shader_reflection` (after the closing brace of that tool, around line ~641):

```python
    @mcp.tool()
    async def read_constant_buffer(
        capture_id: str,
        event_id: int,
        stage: str,
        slot: int,
    ) -> dict[str, Any]:
        """Decode a constant buffer slot to typed named variables using shader reflection.

        Returns variable names and values (floats, matrices, vectors). Useful for finding
        wrong transform matrices or emulator data-upload bugs. Falls back to raw hex if
        reflection is unavailable.
        """
        async with replay_execution():

            def _go() -> dict[str, Any]:
                rd = rdutil.get_renderdoc()
                sess = sessions.get(capture_id)
                if sess is None:
                    return R.err("unknown_capture", capture_id)
                st = _stage_from_string(rd, stage)
                if st is None:
                    return R.err("bad_stage", stage)

                sessions.set_frame_event(sess, int(event_id), True)
                pipe = sess.controller.GetPipelineState()
                refl = pipe.GetShaderReflection(st)
                if refl is None:
                    return R.ok({"bound": False, "stage": stage, "slot": int(slot)})

                cb_blocks = list(getattr(refl, "constantBlocks", []) or [])

                # Find by fixed bind number first, then fall back to index
                cb_block = None
                for block in cb_blocks:
                    if int(getattr(block, "fixedBindNumber", -1)) == int(slot):
                        cb_block = block
                        break
                if cb_block is None and int(slot) < len(cb_blocks):
                    cb_block = cb_blocks[int(slot)]
                if cb_block is None:
                    return R.err("no_cb_at_slot", "No constant block at slot {}".format(slot))

                # Read raw buffer bytes
                raw = b""
                raw_hex = ""
                try:
                    cb_desc = pipe.GetConstantBlock(st, int(slot), 0)
                    desc = getattr(cb_desc, "descriptor", None)
                    if desc is not None:
                        buf_rid = getattr(desc, "resource", None)
                        byte_offset = int(getattr(desc, "byteOffset", 0))
                        byte_size = int(getattr(desc, "byteSize", 0)) or 65536
                        raw = rdutil.controller_get_buffer_data(
                            sess.controller, buf_rid, byte_offset, min(byte_size, 65536)
                        )
                        raw_hex = raw[:256].hex()
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
                }
                if not raw:
                    out["reflection_unavailable"] = True

                return R.ok(out)

            return await asyncio.to_thread(_go)
```

- [ ] **Step 2: Verify smoke import**

```
python scripts/smoke_import.py
```

Expected: `OK: renderdoc_mcp.server.build_mcp`

- [ ] **Step 3: Commit**

```
git add renderdoc_mcp/server.py
git commit -m "feat(mcp): add read_constant_buffer tool with typed CB decode"
```

---

## Task 6: `get_debug_messages` tool

**Files:**
- Modify: `renderdoc_mcp/server.py` (add tool inside `build_mcp`, after `read_constant_buffer`)

**Interfaces:**
- Produces: `get_debug_messages` MCP tool

- [ ] **Step 1: Add tool to `server.py` inside `build_mcp()`**

Add after the `read_constant_buffer` tool:

```python
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

            return await asyncio.to_thread(_go)
```

- [ ] **Step 2: Verify smoke import**

```
python scripts/smoke_import.py
```

Expected: `OK: renderdoc_mcp.server.build_mcp`

- [ ] **Step 3: Commit**

```
git add renderdoc_mcp/server.py
git commit -m "feat(mcp): add get_debug_messages tool"
```

---

## Task 7: `build_frame_overview` + `get_frame_overview` tool

**Files:**
- Modify: `renderdoc_mcp/analysis.py` (add function at end of file)
- Modify: `renderdoc_mcp/server.py` (add tool inside `build_mcp`, after `get_debug_messages`)

**Interfaces:**
- Produces: `build_frame_overview(controller: Any, structured_file: Any) -> dict[str, Any]` in `analysis.py`
- Produces: `get_frame_overview` MCP tool in `server.py`

- [ ] **Step 1: Add `build_frame_overview` to `analysis.py`**

Add at the end of `analysis.py` (after `diff_texture_analysis`):

```python
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
```

- [ ] **Step 2: Add `get_frame_overview` tool to `server.py`**

Add this import to the `from renderdoc_mcp.analysis import (...)` block at the top of `server.py`:

```python
from renderdoc_mcp.analysis import (
    analyze_texture_bytes,
    diff_pipeline_snapshots,
    diff_texture_analysis,
    draw_visibility_analysis,
    find_texture_description,
    normalize_pixel_history,
    build_frame_overview,        # add this line
)
```

Inside `build_mcp()`, add after `get_debug_messages`:

```python
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

            return await asyncio.to_thread(_go)
```

- [ ] **Step 3: Verify smoke import**

```
python scripts/smoke_import.py
```

Expected: `OK: renderdoc_mcp.server.build_mcp`

- [ ] **Step 4: Run full test suite**

```
python -m pytest tests/ -v
```

Expected: `32 passed` (7 + 13 + 12 from previous tasks).

- [ ] **Step 5: Commit**

```
git add renderdoc_mcp/analysis.py renderdoc_mcp/server.py
git commit -m "feat(mcp): add build_frame_overview and get_frame_overview tool"
```

---

## Self-Review Checklist

- **Spec coverage:**
  - `get_frame_overview` → Task 7 ✓
  - `get_texture_image` → Task 3 ✓
  - `read_constant_buffer` → Task 5 ✓
  - `get_debug_messages` → Task 6 ✓
  - Anomaly hints on `get_pipeline_state` → Task 1 (`anomalies` field added to `normalize_pipeline_state`) ✓
  - Anomaly hints on `analyze_draw_visibility` → Task 1 (Step 5 adds `anomalies` to that return dict) ✓
  - `imaging.py` as separate module → Task 2 ✓
  - No new dependencies → all tasks use stdlib only ✓
  - `resource_name` fields → carried through in Task 3 and Task 7 ✓

- **Types consistent across tasks:**
  - `detect_pipeline_anomalies(snapshot: dict) -> list[str]` used consistently in Task 1
  - `decode_cb_bytes(raw: bytes, constants: list) -> list[dict]` signature matches test mocks in Task 4 and usage in Task 5
  - `save_texture_as_png_bytes(controller, rd, rid, mip, slice_index, max_dimension) -> bytes | None` matches call site in Task 3
  - `build_frame_overview(controller, structured_file) -> dict` matches import and call in Task 7
