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
