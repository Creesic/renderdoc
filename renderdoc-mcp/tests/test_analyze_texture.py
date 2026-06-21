"""Tests for analyze_texture_bytes packed-format support (no RenderDoc import needed)."""

from __future__ import annotations

import struct


class _FmtType:
    def __init__(self, name):
        self.name = name

    def __str__(self):
        return self.name


class _Fmt:
    """Minimal fake ResourceFormat for R10G10B10A2."""

    def __init__(self, type_name="R10G10B10A2", comp_type_name="UNorm", comp_count=4, comp_byte_width=0):
        self.type = _FmtType(type_name)
        self.compType = _FmtType(comp_type_name)
        self.compCount = comp_count
        self.compByteWidth = comp_byte_width

    def Special(self):
        return True


class _Tex:
    def __init__(self, w, h, fmt):
        self.width = w
        self.height = h
        self.format = fmt


def _pack_r10g10b10a2(r10, g10, b10, a2):
    """Pack channel values into a 4-byte little-endian R10G10B10A2 word."""
    v = (r10 & 0x3FF) | ((g10 & 0x3FF) << 10) | ((b10 & 0x3FF) << 20) | ((a2 & 0x3) << 30)
    return struct.pack("<I", v)


def _make_raw(*pixels):
    """Concatenate packed pixels into a raw byte buffer."""
    return b"".join(pixels)


# ---- helper to call analyze_texture_bytes without a live RenderDoc --------

def _analyze(tex, raw):
    """Invoke analyze_texture_bytes with a stubbed get_renderdoc."""
    import renderdoc_mcp.analysis as ana

    class _FakeRd:
        class CompType:
            UNorm = _FmtType("UNorm")
            UInt = _FmtType("UInt")
            Float = _FmtType("Float")
            SInt = _FmtType("SInt")
            SNorm = _FmtType("SNorm")

    orig = ana.get_renderdoc
    try:
        ana.get_renderdoc = lambda: _FakeRd()
        return ana.analyze_texture_bytes(tex, raw)
    finally:
        ana.get_renderdoc = orig


# ---- tests ----------------------------------------------------------------

def test_r10g10b10a2_supported():
    fmt = _Fmt("R10G10B10A2")
    tex = _Tex(1, 1, fmt)
    raw = _pack_r10g10b10a2(0, 0, 0, 0)
    result = _analyze(tex, raw)
    assert result["supported_stats"] is True


def test_r10g10b10a2_midpoint_rgb():
    """R=512 → ~0.500, G=0 → 0.0, B=1023 → 1.0, A=3 → 1.0."""
    fmt = _Fmt("R10G10B10A2")
    tex = _Tex(1, 1, fmt)
    raw = _pack_r10g10b10a2(512, 0, 1023, 3)
    result = _analyze(tex, raw)
    means = result["mean_channels"]
    assert abs(means[0] - 512 / 1023) < 1e-4   # R
    assert abs(means[1] - 0.0) < 1e-4           # G
    assert abs(means[2] - 1.0) < 1e-4           # B
    assert abs(means[3] - 1.0) < 1e-4           # A


def test_r10g10b10a2_black_pixel_counted():
    fmt = _Fmt("R10G10B10A2")
    tex = _Tex(1, 1, fmt)
    raw = _pack_r10g10b10a2(0, 0, 0, 0)
    result = _analyze(tex, raw)
    assert result["near_black_pixel_count"] == 1
    assert result["near_black_ratio"] == 1.0


def test_r10g10b10a2_multi_pixel_mean():
    """Two-pixel image: one black, one full-white RGB."""
    fmt = _Fmt("R10G10B10A2")
    tex = _Tex(2, 1, fmt)
    raw = _pack_r10g10b10a2(0, 0, 0, 0) + _pack_r10g10b10a2(1023, 1023, 1023, 3)
    result = _analyze(tex, raw)
    means = result["mean_channels"]
    assert abs(means[0] - 0.5) < 0.01
    assert abs(means[1] - 0.5) < 0.01
    assert abs(means[2] - 0.5) < 0.01


def test_r10g10b10a2_packed_key_in_format():
    fmt = _Fmt("R10G10B10A2")
    tex = _Tex(1, 1, fmt)
    raw = _pack_r10g10b10a2(0, 0, 0, 0)
    result = _analyze(tex, raw)
    assert result["format"].get("packed") == "R10G10B10A2"
    assert result["format"]["comp_count"] == 4


def test_r10g10b10a2_uint_not_normalized():
    """When comp_type is UInt, values should be raw integers (not divided by 1023)."""
    fmt = _Fmt("R10G10B10A2", comp_type_name="UInt")
    tex = _Tex(1, 1, fmt)
    raw = _pack_r10g10b10a2(512, 0, 1023, 3)
    result = _analyze(tex, raw)
    # With UInt the means should be raw counts, not normalized
    means = result["mean_channels"]
    assert abs(means[0] - 512.0) < 1e-4
    assert abs(means[2] - 1023.0) < 1e-4


def test_non_r10g10b10a2_packed_still_unsupported():
    """Other packed formats (e.g. BC1) still return supported_stats=False."""
    fmt = _Fmt("BC1_UNORM")
    tex = _Tex(4, 4, fmt)
    raw = bytes(8)  # BC1 block is 8 bytes
    result = _analyze(tex, raw)
    assert result["supported_stats"] is False
    assert result["reason"] == "complex_or_packed_format"
