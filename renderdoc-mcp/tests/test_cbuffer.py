from __future__ import annotations

import math
import struct

import pytest


def _make_base_type(name: str):
    """Return a fake VarType-like object with a .name attribute."""
    class FakeVarType:
        pass
    vt = FakeVarType()
    vt.name = name
    return vt


def _make_const(name, byte_offset, rows=1, cols=1, base_type_name="Float", members=None,
                matrix_byte_stride=0, row_major=None):
    """Build a mock ShaderConstant-like object mirroring the real RenderDoc API.

    Note: no ``baseByteWidth`` field — byte width is derived from baseType.name via _VARTYPE_BYTES.
    """
    class FakeType:
        pass

    class FakeConst:
        pass

    ct = FakeType()
    ct.rows = rows
    ct.columns = cols
    ct.baseType = _make_base_type(base_type_name)
    ct.members = members or []
    ct.matrixByteStride = matrix_byte_stride
    if row_major is not None:
        _rm = row_major
        ct.RowMajor = lambda: _rm
        ct.ColMajor = lambda: not _rm

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


def test_decode_sint_scalar():
    from renderdoc_mcp.cbuffer import decode_cb_bytes
    raw = struct.pack("<i", -7)
    result = decode_cb_bytes(raw, [_make_const("Signed", 0, rows=1, cols=1, base_type_name="SInt")])
    assert result[0]["value"] == -7
    assert result[0]["type"] == "int"


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


# --- New type tests (Half, SShort, UShort) ---

def test_decode_half_scalar():
    from renderdoc_mcp.cbuffer import decode_cb_bytes
    # Pack a half-float value
    raw = struct.pack("<e", 3.14)
    result = decode_cb_bytes(raw, [_make_const("HalfVal", 0, rows=1, cols=1, base_type_name="Half")])
    assert result[0]["type"] == "half"
    # Half precision is limited; 3.14 round-trips to ~3.14 within half-float accuracy
    assert abs(result[0]["value"] - 3.14) < 0.01


def test_decode_sshort_scalar():
    from renderdoc_mcp.cbuffer import decode_cb_bytes
    raw = struct.pack("<h", -300)
    result = decode_cb_bytes(raw, [_make_const("ShortVal", 0, rows=1, cols=1, base_type_name="SShort")])
    assert result[0]["value"] == -300
    assert result[0]["type"] == "short"


def test_decode_ushort_scalar():
    from renderdoc_mcp.cbuffer import decode_cb_bytes
    raw = struct.pack("<H", 65000)
    result = decode_cb_bytes(raw, [_make_const("UShortVal", 0, rows=1, cols=1, base_type_name="UShort")])
    assert result[0]["value"] == 65000
    assert result[0]["type"] == "ushort"


def test_decode_slong_scalar():
    from renderdoc_mcp.cbuffer import decode_cb_bytes
    raw = struct.pack("<q", -9_000_000_000)
    result = decode_cb_bytes(raw, [_make_const("LongVal", 0, rows=1, cols=1, base_type_name="SLong")])
    assert result[0]["value"] == -9_000_000_000
    assert result[0]["type"] == "long"


def test_decode_ulong_scalar():
    from renderdoc_mcp.cbuffer import decode_cb_bytes
    raw = struct.pack("<Q", 18_000_000_000)
    result = decode_cb_bytes(raw, [_make_const("ULongVal", 0, rows=1, cols=1, base_type_name="ULong")])
    assert result[0]["value"] == 18_000_000_000
    assert result[0]["type"] == "ulong"


def test_decode_sbyte_scalar():
    from renderdoc_mcp.cbuffer import decode_cb_bytes
    raw = struct.pack("<b", -120)
    result = decode_cb_bytes(raw, [_make_const("SByteVal", 0, rows=1, cols=1, base_type_name="SByte")])
    assert result[0]["value"] == -120
    assert result[0]["type"] == "sbyte"


def test_decode_ubyte_scalar():
    from renderdoc_mcp.cbuffer import decode_cb_bytes
    raw = struct.pack("<B", 200)
    result = decode_cb_bytes(raw, [_make_const("UByteVal", 0, rows=1, cols=1, base_type_name="UByte")])
    assert result[0]["value"] == 200
    assert result[0]["type"] == "ubyte"


def test_decode_matrix_with_stride():
    """Matrix byte stride from matrixByteStride is honoured."""
    from renderdoc_mcp.cbuffer import decode_cb_bytes
    # 2x2 row-major matrix with stride of 8 bytes (2 floats per row, no padding)
    raw = struct.pack("<ffff", 1.0, 2.0, 3.0, 4.0)
    result = decode_cb_bytes(
        raw,
        [_make_const("M", 0, rows=2, cols=2, matrix_byte_stride=8, row_major=True)],
        row_major=True,
    )
    mat = result[0]["value"]
    assert mat[0][0] == 1.0
    assert mat[0][1] == 2.0
    assert mat[1][0] == 3.0
    assert mat[1][1] == 4.0


def test_decode_column_major_matrix():
    """Column-major 2x2: memory holds columns, not rows."""
    from renderdoc_mcp.cbuffer import decode_cb_bytes
    # Column-major 2x2: col0=(1,3), col1=(2,4) packed at stride 8
    raw = struct.pack("<ffff", 1.0, 3.0, 2.0, 4.0)
    result = decode_cb_bytes(
        raw,
        [_make_const("M", 0, rows=2, cols=2, matrix_byte_stride=8, row_major=False)],
        row_major=False,
    )
    mat = result[0]["value"]
    # Logical matrix should be [[1,2],[3,4]]
    assert mat[0][0] == 1.0
    assert mat[0][1] == 2.0
    assert mat[1][0] == 3.0
    assert mat[1][1] == 4.0


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
