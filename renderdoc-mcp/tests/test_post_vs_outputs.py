from __future__ import annotations


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
