"""disassembly_failure_reason must recognize every sentinel the C++ replay drivers emit.

DisassembleShader signals failure by returning sentinel strings, not exceptions. Any
sentinel missing from the marker list is accepted as genuine disassembly and fed to the
resource resolver. Sources: renderdoc/driver/ihv/amd/amd_isa.cpp, amd_isa_win32.cpp,
d3d12_replay.cpp, replay_controller.cpp.
"""

from __future__ import annotations

import pytest

from renderdoc_mcp.shader_debug import disassembly_failure_reason


_CPP_SENTINELS = [
    # already covered before this test existed
    "; Invalid Shader Specified",
    "; Invalid disassembly target foo",
    "; No pipeline specified, cannot disassemble",
    "; Unknown error fetching disassembly",
    "Unsupported encoding for shader 'x'",
    # AMD ISA paths (amd_isa.cpp / amd_isa_win32.cpp) — reachable from every
    # DisassembleShader call via replay_controller.cpp's GCN target routing
    "; Invalid ISA Target specified",
    "; Failed to Disassemble - some tool error",
    "; Cannot identify shader type",
    "; SPIR-V disassembly not supported, couldn't locate spirv-cross",
    "; Invalid ELF file generated",
    "; Error loading amdspv.exe",
    "; Shader disassembly for DXIL shaders is not supported.",
    "; Failed to disassemble shader",
    # d3d12_replay.cpp
    "; Unknown shader stage in shader reflection",
    "; Couldn't find disassembly for given shader stage in returned string",
    # replay_controller.cpp
    "; Error: No shader specified",
]


@pytest.mark.parametrize("sentinel", _CPP_SENTINELS)
def test_cpp_failure_sentinels_detected(sentinel):
    assert disassembly_failure_reason(sentinel) is not None


def test_real_disassembly_not_flagged():
    dxbc = "ps_5_0\ndcl_globalFlags refactoringAllowed\nsample r0.xyzw, v1.xyxx, t0.xyzw, s0"
    assert disassembly_failure_reason(dxbc) is None
