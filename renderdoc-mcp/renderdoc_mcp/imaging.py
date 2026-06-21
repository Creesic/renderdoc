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
