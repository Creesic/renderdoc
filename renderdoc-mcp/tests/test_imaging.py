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
