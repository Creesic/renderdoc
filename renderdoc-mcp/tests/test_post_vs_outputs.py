from __future__ import annotations

import struct


def test_var_type_byte_size_matches_replay_enums_table():
    from renderdoc_mcp.mesh_decode import var_type_byte_size

    assert var_type_byte_size("UByte") == 1
    assert var_type_byte_size("SByte") == 1
    assert var_type_byte_size("Half") == 2
    assert var_type_byte_size("UShort") == 2
    assert var_type_byte_size("SShort") == 2
    assert var_type_byte_size("Float") == 4
    assert var_type_byte_size("UInt") == 4
    assert var_type_byte_size("SInt") == 4
    assert var_type_byte_size("Bool") == 4
    assert var_type_byte_size("Enum") == 4
    assert var_type_byte_size("Double") == 8
    assert var_type_byte_size("ULong") == 8
    assert var_type_byte_size("SLong") == 8
    assert var_type_byte_size("GPUPointer") == 8


def test_var_type_byte_size_unknown_type_is_zero():
    from renderdoc_mcp.mesh_decode import var_type_byte_size

    assert var_type_byte_size("Struct") == 0
    assert var_type_byte_size("Unknown") == 0


def test_var_type_comp_type_matches_replay_enums_table():
    from renderdoc_mcp.mesh_decode import var_type_comp_type

    assert var_type_comp_type("Float") == "Float"
    assert var_type_comp_type("Double") == "Float"
    assert var_type_comp_type("Half") == "Float"
    assert var_type_comp_type("UInt") == "UInt"
    assert var_type_comp_type("ULong") == "UInt"
    assert var_type_comp_type("UShort") == "UInt"
    assert var_type_comp_type("UByte") == "UInt"
    assert var_type_comp_type("Bool") == "UInt"
    assert var_type_comp_type("Enum") == "UInt"
    assert var_type_comp_type("GPUPointer") == "UInt"
    assert var_type_comp_type("SInt") == "SInt"
    assert var_type_comp_type("SLong") == "SInt"
    assert var_type_comp_type("SShort") == "SInt"
    assert var_type_comp_type("SByte") == "SInt"


def test_var_type_comp_type_unknown_type_is_typeless():
    from renderdoc_mcp.mesh_decode import var_type_comp_type

    assert var_type_comp_type("Struct") == "Typeless"


def test_select_gsout_reflection_stage_prefers_geometry():
    from renderdoc_mcp.mesh_decode import select_gsout_reflection_stage

    assert select_gsout_reflection_stage(has_geometry=True, has_domain=True) == "geometry"
    assert select_gsout_reflection_stage(has_geometry=True, has_domain=False) == "geometry"


def test_select_gsout_reflection_stage_falls_back_to_domain():
    from renderdoc_mcp.mesh_decode import select_gsout_reflection_stage

    assert select_gsout_reflection_stage(has_geometry=False, has_domain=True) == "domain"


def test_select_gsout_reflection_stage_none_when_neither_bound():
    from renderdoc_mcp.mesh_decode import select_gsout_reflection_stage

    assert select_gsout_reflection_stage(has_geometry=False, has_domain=False) is None


def _sig(name, semantic_name="", semantic_index=0, system_value="Undefined",
         var_type="Float", comp_count=4, stream=0):
    return {
        "name": name,
        "semantic_name": semantic_name,
        "semantic_index": semantic_index,
        "system_value": system_value,
        "var_type": var_type,
        "comp_count": comp_count,
        "stream": stream,
    }


def test_build_output_column_layout_packs_tightly_when_not_aligned():
    from renderdoc_mcp.mesh_decode import build_output_column_layout

    sigs = [
        _sig("pos", system_value="Position", comp_count=4),
        _sig("col2", semantic_name="COLOR", semantic_index=0, comp_count=2),
        _sig("col", semantic_name="COLOR", semantic_index=1, comp_count=4),
    ]

    columns = build_output_column_layout(sigs, aligned=False)

    assert [c["name"] for c in columns] == ["pos", "col2", "col"]
    assert columns[0]["byte_offset"] == 0
    assert columns[1]["byte_offset"] == 16
    assert columns[2]["byte_offset"] == 24


def test_build_output_column_layout_pads_for_alignment():
    """Matches util/test/rdtest/shared/Mesh_Zoo.py's own manual offset math exactly: after a
    float2 (8 bytes) at offset 16, a following float4 needs +8 padding when aligned (24 -> 32)."""
    from renderdoc_mcp.mesh_decode import build_output_column_layout

    sigs = [
        _sig("pos", system_value="Position", comp_count=4),
        _sig("col2", semantic_name="COLOR", semantic_index=0, comp_count=2),
        _sig("col", semantic_name="COLOR", semantic_index=1, comp_count=4),
    ]

    columns = build_output_column_layout(sigs, aligned=True)

    assert columns[0]["byte_offset"] == 0
    assert columns[1]["byte_offset"] == 16
    assert columns[2]["byte_offset"] == 32


def test_build_output_column_layout_moves_position_to_front():
    from renderdoc_mcp.mesh_decode import build_output_column_layout

    sigs = [
        _sig("col", semantic_name="COLOR", comp_count=4),
        _sig("pos", system_value="Position", comp_count=4),
    ]

    columns = build_output_column_layout(sigs, aligned=False)

    assert [c["name"] for c in columns] == ["pos", "col"]
    assert columns[0]["byte_offset"] == 0
    assert columns[1]["byte_offset"] == 16


def test_build_output_column_layout_skips_output_indices_and_other_streams():
    from renderdoc_mcp.mesh_decode import build_output_column_layout

    sigs = [
        _sig("pos", system_value="Position", comp_count=4),
        _sig("indices", system_value="OutputIndices", comp_count=1),
        _sig("other_stream_col", semantic_name="COLOR", comp_count=4, stream=1),
    ]

    columns = build_output_column_layout(sigs, aligned=False, stream=0)

    assert [c["name"] for c in columns] == ["pos"]


def test_build_output_column_layout_uses_eight_byte_elements_for_wide_types():
    from renderdoc_mcp.mesh_decode import build_output_column_layout

    sigs = [
        _sig("d", semantic_name="DOUBLE0", var_type="Double", comp_count=1),
        _sig("f", semantic_name="FLOAT0", var_type="Float", comp_count=1),
    ]

    columns = build_output_column_layout(sigs, aligned=False)

    assert columns[0]["elem_byte_width"] == 8
    assert columns[0]["byte_offset"] == 0
    assert columns[1]["byte_offset"] == 8
    assert columns[1]["comp_type"] == "Float"


def test_build_output_column_layout_reports_comp_type_and_counts():
    from renderdoc_mcp.mesh_decode import build_output_column_layout

    sigs = [_sig("id", semantic_name="ID0", var_type="UInt", comp_count=1)]

    columns = build_output_column_layout(sigs, aligned=False)

    assert columns[0]["comp_type"] == "UInt"
    assert columns[0]["comp_count"] == 1


def test_perspective_divide_position_divides_by_w():
    from renderdoc_mcp.mesh_decode import perspective_divide_position

    assert perspective_divide_position([2.0, 4.0, 6.0, 2.0]) == [1.0, 2.0, 3.0]


def test_perspective_divide_position_none_when_w_is_zero():
    from renderdoc_mcp.mesh_decode import perspective_divide_position

    assert perspective_divide_position([1.0, 2.0, 3.0, 0.0]) is None


def test_perspective_divide_position_none_when_too_short():
    from renderdoc_mcp.mesh_decode import perspective_divide_position

    assert perspective_divide_position([1.0, 2.0, 3.0]) is None


def test_decode_semantic_bytes_float4():
    from renderdoc_mcp.mesh_decode import decode_semantic_bytes

    data = struct.pack("=4f", 1.5, 2.5, 3.5, 4.5)
    result = decode_semantic_bytes(data, 0, "Float", 4, 4)

    assert result == (1.5, 2.5, 3.5, 4.5)


def test_decode_semantic_bytes_respects_offset():
    from renderdoc_mcp.mesh_decode import decode_semantic_bytes

    data = struct.pack("=2f2I", 1.5, 2.5, 10, 20)
    result = decode_semantic_bytes(data, 8, "UInt", 2, 4)

    assert result == (10, 20)


def test_decode_semantic_bytes_sint_and_double_width():
    from renderdoc_mcp.mesh_decode import decode_semantic_bytes

    data = struct.pack("=2i", -5, 7)
    assert decode_semantic_bytes(data, 0, "SInt", 2, 4) == (-5, 7)

    data8 = struct.pack("=d", 3.25)
    assert decode_semantic_bytes(data8, 0, "Float", 1, 8) == (3.25,)


def test_decode_semantic_bytes_none_when_out_of_range():
    from renderdoc_mcp.mesh_decode import decode_semantic_bytes

    data = struct.pack("=2f", 1.0, 2.0)
    assert decode_semantic_bytes(data, 4, "Float", 4, 4) is None


def test_decode_semantic_bytes_none_for_typeless():
    from renderdoc_mcp.mesh_decode import decode_semantic_bytes

    data = struct.pack("=4f", 1.0, 2.0, 3.0, 4.0)
    assert decode_semantic_bytes(data, 0, "Typeless", 4, 4) is None
