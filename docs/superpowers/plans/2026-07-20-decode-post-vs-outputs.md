# decode_post_vs_outputs Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a `decode_post_vs_outputs` MCP tool that decodes a draw's post-vertex-shader (or post-geometry/tessellation) output into typed semantics, per `docs/superpowers/specs/2026-07-20-decode-post-vs-outputs-design.md`.

**Architecture:** Four small, pure, independently-tested helper functions (`VarType` lookup tables, GSOut stage-resolution rule, the output-column byte-offset builder, perspective divide + raw semantic decode) added to `renderdoc-mcp/renderdoc_mcp/mesh_decode.py` alongside the existing `decode_mesh_inputs` machinery. An impure orchestration function in the same file wires them together against the real replay API (`GetPostVSData`, shader reflection, buffer reads). A thin `server.py` tool wraps it, mirroring `decode_mesh_inputs`'s existing wrapper exactly.

**Tech Stack:** Python 3.10+, `renderdoc` Python module (`pymodules`), `pytest` for unit tests, `rdtest` for the integration test.

## Global Constraints

- No new Python dependencies.
- Follow the existing tool pattern exactly: `@mcp.tool()` → `async with replay_execution(): def _go(): ...; return await asyncio.get_running_loop().run_in_executor(_replay_executor, _go)`.
- Match existing code style: `from __future__ import annotations`, type hints on all new functions, no unrelated formatting changes.
- Reimplement `VarType`→byte-size/`CompType` mapping natively in Python rather than assume a matching SWIG-exposed helper exists for the C++ `constexpr` functions `VarTypeByteSize`/`VarTypeCompType` (`renderdoc/api/replay/replay_enums.h:304-414`) — same caution `mesh_decode.py`'s existing `unpack_data` already takes with format conversion.
- `stage="gsout"` resolves to the Geometry shader's reflection if bound, else the Domain (tessellation-eval) shader's if tessellation is active, else an explicit error — never silently falls back to VS data.
- POSITION's NDC value is a perspective divide only (`xyz/w`) — not a camera/view-matrix reconstruction.
- Reuse `MAX_PREVIEW_VERTICES` (already defined in `mesh_decode.py`) rather than redefining a cap.

---

### Task 1: `VarType` lookup tables

**Files:**
- Modify: `renderdoc-mcp/renderdoc_mcp/mesh_decode.py`
- Test: `renderdoc-mcp/tests/test_post_vs_outputs.py` (new file)

**Interfaces:**
- Produces: `var_type_byte_size(var_type_name: str) -> int` and `var_type_comp_type(var_type_name: str) -> str`. Both take the *string* name of a `VarType` enum member (e.g. `"Float"`, `"UByte"` — what `rdutil.enum_name(sig.varType)` returns), not a real `rd.VarType` object, so they're testable without a live `renderdoc` module. Task 3 calls both.

- [ ] **Step 1: Write the failing tests**

Create `renderdoc-mcp/tests/test_post_vs_outputs.py`:

```python
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
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd renderdoc-mcp && python -m pytest tests/test_post_vs_outputs.py -v`
Expected: FAIL with `ImportError: cannot import name 'var_type_byte_size'`

- [ ] **Step 3: Write minimal implementation**

Add to `renderdoc-mcp/renderdoc_mcp/mesh_decode.py`, after the `MAX_PREVIEW_VERTICES` constant near the top of the file:

```python
# VarType -> byte size / CompType, replicated from renderdoc/api/replay/replay_enums.h's
# VarTypeByteSize()/VarTypeCompType() constexpr functions (304-414) -- these are free functions,
# not virtual interface methods, so there's no confirmed SWIG binding to call into instead.
_VAR_TYPE_BYTE_SIZE = {
    "UByte": 1, "SByte": 1,
    "Half": 2, "UShort": 2, "SShort": 2,
    "Float": 4, "UInt": 4, "SInt": 4, "Bool": 4, "Enum": 4,
    "Double": 8, "ULong": 8, "SLong": 8, "GPUPointer": 8,
}

_VAR_TYPE_COMP_TYPE = {
    "Float": "Float", "Double": "Float", "Half": "Float",
    "UInt": "UInt", "ULong": "UInt", "UShort": "UInt", "UByte": "UInt", "Bool": "UInt",
    "Enum": "UInt", "GPUPointer": "UInt",
    "SInt": "SInt", "SLong": "SInt", "SShort": "SInt", "SByte": "SInt",
}


def var_type_byte_size(var_type_name: str) -> int:
    """Byte size of a VarType; 0 for types with no fixed scalar size (Struct, Unknown, etc.)."""
    return _VAR_TYPE_BYTE_SIZE.get(var_type_name, 0)


def var_type_comp_type(var_type_name: str) -> str:
    """Component type ('Float'/'UInt'/'SInt') of a VarType; 'Typeless' if not one of those."""
    return _VAR_TYPE_COMP_TYPE.get(var_type_name, "Typeless")
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd renderdoc-mcp && python -m pytest tests/test_post_vs_outputs.py -v`
Expected: PASS (4 tests)

- [ ] **Step 5: Commit**

```bash
git add renderdoc-mcp/renderdoc_mcp/mesh_decode.py renderdoc-mcp/tests/test_post_vs_outputs.py
git commit -m "mcp: add VarType byte-size/comp-type tables for post-VS decoding"
```

---

### Task 2: GSOut stage-resolution rule

**Files:**
- Modify: `renderdoc-mcp/renderdoc_mcp/mesh_decode.py`
- Test: `renderdoc-mcp/tests/test_post_vs_outputs.py`

**Interfaces:**
- Consumes: nothing from Task 1.
- Produces: `select_gsout_reflection_stage(has_geometry: bool, has_domain: bool) -> str | None`, returning `"geometry"`, `"domain"`, or `None`. Task 5's orchestration function calls this with `pipe.GetShader(ShaderStage.Geometry) != ResourceId.Null()` and `pipe.GetShader(ShaderStage.Domain) != ResourceId.Null()` to decide which shader's reflection to use for `stage="gsout"`.

- [ ] **Step 1: Write the failing tests**

Append to `renderdoc-mcp/tests/test_post_vs_outputs.py`:

```python
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
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd renderdoc-mcp && python -m pytest tests/test_post_vs_outputs.py -v`
Expected: FAIL with `ImportError: cannot import name 'select_gsout_reflection_stage'`

- [ ] **Step 3: Write minimal implementation**

Add to `renderdoc-mcp/renderdoc_mcp/mesh_decode.py`, directly below `var_type_comp_type`:

```python
def select_gsout_reflection_stage(has_geometry: bool, has_domain: bool) -> str | None:
    """Which shader's reflection describes GSOut data for this draw.

    Only a bound Geometry shader, or a bound Domain (tessellation-eval) shader when tessellation
    is active with no Geometry shader, produces the final pre-rasterization output -- matches the
    real constraint in qrenderdoc's BufferViewer.cpp:5683 ("if geometry/tessellation is enabled,
    only the GS out stage is rasterized output"). Returns None if GSOut doesn't apply to this draw.
    """
    if has_geometry:
        return "geometry"
    if has_domain:
        return "domain"
    return None
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd renderdoc-mcp && python -m pytest tests/test_post_vs_outputs.py -v`
Expected: PASS (7 tests)

- [ ] **Step 5: Commit**

```bash
git add renderdoc-mcp/renderdoc_mcp/mesh_decode.py renderdoc-mcp/tests/test_post_vs_outputs.py
git commit -m "mcp: add select_gsout_reflection_stage for post-VS decoding"
```

---

### Task 3: Output column layout builder

**Files:**
- Modify: `renderdoc-mcp/renderdoc_mcp/mesh_decode.py`
- Test: `renderdoc-mcp/tests/test_post_vs_outputs.py`

**Interfaces:**
- Consumes: `var_type_byte_size`, `var_type_comp_type` (Task 1).
- Produces: `build_output_column_layout(sig_params: list[dict[str, Any]], aligned: bool, stream: int = 0) -> list[dict[str, Any]]`. Each input dict has keys `name` (`str`), `semantic_name` (`str`), `semantic_index` (`int`), `system_value` (`str`, e.g. `"Position"` or `"Undefined"`), `var_type` (`str`, e.g. `"Float"`), `comp_count` (`int`), `stream` (`int`). Each output dict has all of those plus `comp_type` (`str`), `elem_byte_width` (`int`), and `byte_offset` (`int`). Task 5 feeds this real `SigParameter` fields converted to this dict shape, and uses the returned `byte_offset`/`elem_byte_width`/`comp_type`/`comp_count` to decode buffer bytes.

This is the core algorithm the whole feature depends on getting right — it replicates `qrenderdoc/Windows/BufferViewer.cpp:1665-1759`'s `ConfigureColumnsForShader` exactly, verified against a real, already-existing RenderDoc test fixture (`D3D11_Mesh_Zoo`, used again in Task 8): that fixture's own `rdtest/shared/Mesh_Zoo.py:89-98` manually computes the exact same offsets this function must produce (`POSITION` at byte 0, a `float2 COLOR0` at byte 16, then a `float4 COLOR1` at byte 24 if tightly packed or byte 32 if aligned).

- [ ] **Step 1: Write the failing tests**

Append to `renderdoc-mcp/tests/test_post_vs_outputs.py`:

```python
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
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd renderdoc-mcp && python -m pytest tests/test_post_vs_outputs.py -v`
Expected: FAIL with `ImportError: cannot import name 'build_output_column_layout'`

- [ ] **Step 3: Write minimal implementation**

Add to `renderdoc-mcp/renderdoc_mcp/mesh_decode.py`, directly below `select_gsout_reflection_stage`:

```python
def _align_up(value: int, alignment: int) -> int:
    return (value + alignment - 1) // alignment * alignment


def build_output_column_layout(
    sig_params: list[dict[str, Any]], aligned: bool, stream: int = 0
) -> list[dict[str, Any]]:
    """Compute each output semantic's byte offset within one post-VS/GS vertex.

    Replicates qrenderdoc/Windows/BufferViewer.cpp:1665-1759's ConfigureColumnsForShader: filter
    to one output stream, skip the OutputIndices system value, move the POSITION-tagged parameter
    to the front (keeping the rest in original order), then pack fields tightly -- except when
    `aligned` is True (Vulkan VSOut only, via PipeState.HasAlignedPostVSData), where 2-component
    fields align to a 2x-element boundary and 3-/4-component fields align to a 4x-element
    boundary.
    """
    columns: list[dict[str, Any]] = []
    for sig in sig_params:
        if int(sig.get("stream", 0)) != stream:
            continue
        if sig.get("system_value") == "OutputIndices":
            continue

        var_type = sig.get("var_type", "Float")
        elem_byte_width = 8 if var_type_byte_size(var_type) > 4 else 4
        columns.append({
            "name": sig.get("name", ""),
            "semantic_name": sig.get("semantic_name", ""),
            "semantic_index": int(sig.get("semantic_index", 0)),
            "system_value": sig.get("system_value", "Undefined"),
            "comp_type": var_type_comp_type(var_type),
            "comp_count": int(sig.get("comp_count", 1)),
            "elem_byte_width": elem_byte_width,
            "byte_offset": 0,
        })

    posidx = next((i for i, c in enumerate(columns) if c["system_value"] == "Position"), -1)
    if posidx > 0:
        columns.insert(0, columns.pop(posidx))

    offset = 0
    for col in columns:
        num_comps = col["comp_count"]
        elem_size = col["elem_byte_width"]
        if aligned:
            if num_comps == 2:
                offset = _align_up(offset, 2 * elem_size)
            elif num_comps > 2:
                offset = _align_up(offset, 4 * elem_size)
        col["byte_offset"] = offset
        offset += num_comps * elem_size

    return columns
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd renderdoc-mcp && python -m pytest tests/test_post_vs_outputs.py -v`
Expected: PASS (13 tests)

- [ ] **Step 5: Commit**

```bash
git add renderdoc-mcp/renderdoc_mcp/mesh_decode.py renderdoc-mcp/tests/test_post_vs_outputs.py
git commit -m "mcp: add build_output_column_layout for post-VS decoding"
```

---

### Task 4: Perspective divide + semantic byte decode

**Files:**
- Modify: `renderdoc-mcp/renderdoc_mcp/mesh_decode.py`
- Test: `renderdoc-mcp/tests/test_post_vs_outputs.py`

**Interfaces:**
- Consumes: nothing from Tasks 1-3.
- Produces: `perspective_divide_position(clip_xyzw: list[float]) -> list[float] | None` and `decode_semantic_bytes(data: bytes, offset: int, comp_type: str, comp_count: int, elem_byte_width: int) -> tuple[float, ...] | None`. Task 5 calls `decode_semantic_bytes` with each column's `comp_type`/`comp_count`/`elem_byte_width` from Task 3's output, and calls `perspective_divide_position` on the decoded POSITION column's 4 values.

- [ ] **Step 1: Write the failing tests**

Append to `renderdoc-mcp/tests/test_post_vs_outputs.py`:

```python
import struct


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
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd renderdoc-mcp && python -m pytest tests/test_post_vs_outputs.py -v`
Expected: FAIL with `ImportError: cannot import name 'perspective_divide_position'`

- [ ] **Step 3: Write minimal implementation**

Add to `renderdoc-mcp/renderdoc_mcp/mesh_decode.py`, directly below `build_output_column_layout`:

```python
def perspective_divide_position(clip_xyzw: list[float]) -> list[float] | None:
    """NDC position via perspective divide -- matches renderdoc/data/hlsl/mesh.hlsl's own
    unprojection (wpos.xyz /= wpos.www) exactly; not a camera/view-matrix reconstruction.
    Returns None if w is zero or fewer than 4 components were given (nothing meaningful to divide).
    """
    if len(clip_xyzw) < 4 or clip_xyzw[3] == 0:
        return None
    w = clip_xyzw[3]
    return [clip_xyzw[0] / w, clip_xyzw[1] / w, clip_xyzw[2] / w]


_SEMANTIC_STRUCT_CHARS = {
    ("Float", 4): "f", ("Float", 8): "d",
    ("UInt", 4): "I", ("UInt", 8): "Q",
    ("SInt", 4): "i", ("SInt", 8): "q",
}


def decode_semantic_bytes(
    data: bytes, offset: int, comp_type: str, comp_count: int, elem_byte_width: int
) -> tuple[Any, ...] | None:
    """Unpack comp_count tightly-packed elem_byte_width-byte values of comp_type starting at
    offset. Post-VS/GS output registers are always plain typed values (never packed/normalized
    vertex-input formats), so no UNorm/SNorm/BGRA handling is needed here -- contrast this
    module's unpack_data(), which does need that for vertex *inputs*.
    """
    char = _SEMANTIC_STRUCT_CHARS.get((comp_type, elem_byte_width))
    if char is None or comp_count <= 0:
        return None
    end = offset + comp_count * elem_byte_width
    if end > len(data):
        return None
    fmt = "=" + str(comp_count) + char
    return struct.unpack_from(fmt, data, offset)
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd renderdoc-mcp && python -m pytest tests/test_post_vs_outputs.py -v`
Expected: PASS (20 tests)

- [ ] **Step 5: Commit**

```bash
git add renderdoc-mcp/renderdoc_mcp/mesh_decode.py renderdoc-mcp/tests/test_post_vs_outputs.py
git commit -m "mcp: add perspective_divide_position and decode_semantic_bytes"
```

---

### Task 5: Orchestration function `decode_post_vs_outputs`

**Files:**
- Modify: `renderdoc-mcp/renderdoc_mcp/mesh_decode.py`

**Interfaces:**
- Consumes: `var_type_byte_size`/`var_type_comp_type` (Task 1, used indirectly via Task 3), `select_gsout_reflection_stage` (Task 2), `build_output_column_layout` (Task 3), `perspective_divide_position`/`decode_semantic_bytes` (Task 4). Also existing module functions `find_action`, `expand_action_flags` (both already imported at the top of `mesh_decode.py`), `get_renderdoc`, `controller_get_buffer_data`, `enum_name` (already imported), and `MAX_PREVIEW_VERTICES` (already defined).
- Produces: `decode_post_vs_outputs(controller, structured_file, event_id, stage="vsout", instance=0, view=0, preview_vertices=8, out_file=None) -> dict[str, Any]`. Task 6's server.py tool calls this exactly the way it already calls `decode_mesh_inputs`.

There is no automated unit test for this function's replay-API-calling body — consistent with `decode_mesh_inputs` itself, which also has no direct unit test (only its pure helpers, `unpack_data`/`fetch_indices`, would be testable in isolation, and aren't tested that way either in this codebase). Verification is the existing test suite staying green.

- [ ] **Step 1: Write the implementation**

Add to `renderdoc-mcp/renderdoc_mcp/mesh_decode.py`, directly below `decode_semantic_bytes`:

```python
def _write_post_vs_csv(path: str, columns: list[dict[str, Any]], rows: list[dict[str, Any]]) -> None:
    """Write decoded post-VS/GS semantics to CSV, one numeric column per vector component.
    POSITION additionally gets 3 NDC[0..2] columns (blank if not unprojected)."""
    with open(path, "w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        header = ["vertex_index"]
        for col in columns:
            comp_count = max(1, int(col["comp_count"]))
            if comp_count == 1:
                header.append(col["name"])
            else:
                header.extend("{}[{}]".format(col["name"], i) for i in range(comp_count))
            if col["system_value"] == "Position":
                header.extend("{}_ndc[{}]".format(col["name"], i) for i in range(3))
        writer.writerow(header)

        for row in rows:
            csv_row: list[Any] = [row["vertex_index"]]
            for col in columns:
                comp_count = max(1, int(col["comp_count"]))
                value = row["values"].get(col["name"])
                if col["system_value"] == "Position":
                    clip = value.get("clip") if isinstance(value, dict) else None
                    ndc = value.get("ndc") if isinstance(value, dict) else None
                    vals = list(clip) if clip else []
                    vals += [""] * (comp_count - len(vals))
                    csv_row.extend(vals[:comp_count])
                    ndc_vals = (list(ndc) if ndc else []) + ["", "", ""]
                    csv_row.extend(ndc_vals[:3])
                else:
                    vals = list(value) if isinstance(value, (list, tuple)) else []
                    vals += [""] * (comp_count - len(vals))
                    csv_row.extend(vals[:comp_count])
            writer.writerow(csv_row)


def decode_post_vs_outputs(
    controller: Any,
    structured_file: Any,
    event_id: int,
    stage: str = "vsout",
    instance: int = 0,
    view: int = 0,
    preview_vertices: int = 8,
    out_file: str | None = None,
) -> dict[str, Any]:
    rd = get_renderdoc()
    draw = find_action(controller, event_id)
    if draw is None:
        return {"error": "event_not_found", "event_id": event_id}

    flags = expand_action_flags(rd, int(draw.flags))
    if "Drawcall" not in flags:
        return {
            "event_id": event_id,
            "note": "Not a draw call; post-VS outputs undefined.",
            "flags": flags,
        }

    stage_key = stage.strip().lower()
    if stage_key not in ("vsout", "gsout"):
        return {"ok": False, "error": "bad_stage",
                "message": "stage must be 'vsout' or 'gsout'", "event_id": event_id}

    pipe = controller.GetPipelineState()

    if stage_key == "vsout":
        refl = pipe.GetShaderReflection(rd.ShaderStage.Vertex)
        mesh_stage = rd.MeshDataStage.VSOut
    else:
        has_geometry = pipe.GetShader(rd.ShaderStage.Geometry) != rd.ResourceId.Null()
        has_domain = pipe.GetShader(rd.ShaderStage.Domain) != rd.ResourceId.Null()
        resolved = select_gsout_reflection_stage(has_geometry, has_domain)
        if resolved is None:
            return {
                "ok": False, "error": "no_geometry_or_tessellation_stage",
                "message": "No Geometry or Domain (tessellation) shader is active for this draw",
                "event_id": event_id,
            }
        gsout_stage = rd.ShaderStage.Geometry if resolved == "geometry" else rd.ShaderStage.Domain
        refl = pipe.GetShaderReflection(gsout_stage)
        mesh_stage = rd.MeshDataStage.GSOut

    if refl is None:
        return {
            "ok": False, "error": "no_shader_reflection",
            "message": "Could not get shader reflection for stage {}".format(stage_key),
            "event_id": event_id,
        }

    mesh_fmt = controller.GetPostVSData(int(instance), int(view), mesh_stage)
    if mesh_fmt.vertexResourceId == rd.ResourceId.Null():
        return {
            "ok": False, "error": "no_post_vs_data",
            "message": "No post-VS data available for this draw (status: {})".format(mesh_fmt.status),
            "event_id": event_id,
        }

    sig_params = []
    for sig in refl.outputSignature:
        sig_params.append({
            "name": sig.varName if sig.varName else sig.semanticIdxName,
            "semantic_name": sig.semanticName,
            "semantic_index": int(sig.semanticIndex),
            "system_value": enum_name(sig.systemValue),
            "var_type": enum_name(sig.varType),
            "comp_count": int(sig.compCount),
            "stream": int(sig.stream),
        })

    aligned = bool(pipe.HasAlignedPostVSData(mesh_stage))
    columns = build_output_column_layout(sig_params, aligned)
    vertex_stride = sum(c["comp_count"] * c["elem_byte_width"] for c in columns)

    fetch_count = min(int(mesh_fmt.numIndices), max(0, int(preview_vertices)), MAX_PREVIEW_VERTICES)

    decoded_rows: list[dict[str, Any]] = []
    for vi in range(fetch_count):
        offset = int(mesh_fmt.vertexByteOffset) + vertex_stride * vi
        try:
            raw = controller_get_buffer_data(controller, mesh_fmt.vertexResourceId, offset, vertex_stride)
        except Exception as ex:
            decoded_rows.append({"vertex_index": vi, "values": {}, "error": str(ex)})
            continue

        values: dict[str, Any] = {}
        for col in columns:
            decoded = decode_semantic_bytes(
                raw, col["byte_offset"], col["comp_type"], col["comp_count"], col["elem_byte_width"]
            )
            if decoded is None:
                values[col["name"]] = None
                continue
            if col["system_value"] == "Position":
                entry: dict[str, Any] = {"clip": list(decoded)}
                if mesh_fmt.unproject:
                    ndc = perspective_divide_position(list(decoded))
                    if ndc is not None:
                        entry["ndc"] = ndc
                values[col["name"]] = entry
            else:
                values[col["name"]] = list(decoded)

        decoded_rows.append({"vertex_index": vi, "values": values})

    out: dict[str, Any] = {
        "event_id": event_id,
        "stage": stage_key,
        "draw_name": draw.GetName(structured_file),
        "semantics": [
            {k: v for k, v in col.items() if k != "elem_byte_width"} for col in columns
        ],
        "vertex_count": int(mesh_fmt.numIndices),
        "unproject": bool(mesh_fmt.unproject),
        "flip_y": bool(mesh_fmt.flipY),
        "near_plane": float(mesh_fmt.nearPlane),
        "far_plane": float(mesh_fmt.farPlane),
    }

    if out_file:
        try:
            _write_post_vs_csv(out_file, columns, decoded_rows)
        except OSError as ex:
            return {"ok": False, "error": "out_file_write_failed", "message": str(ex), "event_id": event_id}
        out["out_file"] = out_file
        out["vertex_previews"] = decoded_rows[:8]
        out["vertex_previews_truncated"] = len(decoded_rows) > 8
    else:
        out["vertex_previews"] = decoded_rows

    return out
```

- [ ] **Step 2: Run the full test suite to confirm no regressions**

Run: `cd renderdoc-mcp && python -m pytest -q`
Expected: PASS, same count as before plus the 20 new tests from Tasks 1-4 (no regressions)

- [ ] **Step 3: Commit**

```bash
git add renderdoc-mcp/renderdoc_mcp/mesh_decode.py
git commit -m "mcp: add decode_post_vs_outputs orchestration function"
```

---

### Task 6: Wire the `decode_post_vs_outputs` tool

**Files:**
- Modify: `renderdoc-mcp/renderdoc_mcp/server.py`

**Interfaces:**
- Consumes: `decode_post_vs_outputs` (Task 5).
- Produces: the `decode_post_vs_outputs` MCP tool. No other task depends on its internals.

- [ ] **Step 1: Add the import**

In `renderdoc-mcp/renderdoc_mcp/server.py`, find this existing import (around line 38):

```python
from renderdoc_mcp.mesh_decode import decode_mesh_inputs as decode_mesh_inputs_core
```

Replace with:

```python
from renderdoc_mcp.mesh_decode import (
    decode_mesh_inputs as decode_mesh_inputs_core,
    decode_post_vs_outputs as decode_post_vs_outputs_core,
)
```

- [ ] **Step 2: Add the tool, immediately after `decode_mesh_inputs`'s closing line and before `get_shader`**

In `renderdoc-mcp/renderdoc_mcp/server.py`, the `decode_mesh_inputs` tool currently ends with (search for this exact text — line numbers may have shifted from other work):

```python
                if data.get("ok") is False:
                    return R.err("decode_mesh_failed", str(data.get("error", "")))
                return R.ok(data)

            return await asyncio.get_running_loop().run_in_executor(_replay_executor, _go)

    @mcp.tool()
    async def get_shader(
```

Insert the new tool between those two `@mcp.tool()` blocks, so it reads:

```python
                if data.get("ok") is False:
                    return R.err("decode_mesh_failed", str(data.get("error", "")))
                return R.ok(data)

            return await asyncio.get_running_loop().run_in_executor(_replay_executor, _go)

    @mcp.tool()
    async def decode_post_vs_outputs(
        capture_id: str,
        event_id: int,
        stage: str = "vsout",
        instance: int = 0,
        view: int = 0,
        preview_vertices: int = 8,
        out_file: str | None = None,
    ) -> dict[str, Any]:
        """Decode a draw's post-vertex-shader (or post-geometry/tessellation) output into typed
        semantics (POSITION, COLOR0, COLOR1, texture coordinates, etc.) for selected vertices.

        stage is "vsout" (default) or "gsout" -- "gsout" requires a Geometry or Domain
        (tessellation) shader to be active for this draw and returns
        no_geometry_or_tessellation_stage otherwise, rather than silently falling back to VS data.
        POSITION additionally reports a perspective-divided NDC value (xyz/w) alongside the raw
        clip-space value when the underlying MeshFormat says this data is unprojectable -- the
        same perspective divide RenderDoc's own mesh-view shader uses, not a full camera/
        view-matrix reconstruction. preview_vertices controls how many vertices are decoded (up
        to 8192); pass out_file to write every decoded vertex to a CSV instead of inlining them.
        """
        async with replay_execution():

            def _go() -> dict[str, Any]:
                sess = sessions.get(capture_id)
                if sess is None:
                    return R.err("unknown_capture", capture_id)
                try:
                    sessions.set_frame_event(sess, int(event_id))
                    data = decode_post_vs_outputs_core(
                        sess.controller, sess.structured_file, int(event_id), stage, instance,
                        view, preview_vertices, out_file,
                    )
                except Exception as ex:
                    return R.err("decode_post_vs_failed", str(ex))
                if isinstance(data, dict):
                    if data.get("error"):
                        return R.err(str(data.get("error")), str(data.get("message", data)))
                    if data.get("ok") is False:
                        return R.err("decode_post_vs_failed", str(data.get("error", "")))
                return R.ok(data)

            return await asyncio.get_running_loop().run_in_executor(_replay_executor, _go)

    @mcp.tool()
    async def get_shader(
```

- [ ] **Step 3: Run the full test suite**

Run: `cd renderdoc-mcp && python -m pytest -q`
Expected: PASS, no regressions

- [ ] **Step 4: Run the smoke check to confirm the server still builds with the new tool registered**

Requires `PYTHONPATH` pointed at a RenderDoc build's `pymodules` (see `renderdoc-mcp/README.md` Prerequisites) *or* just confirm `build_mcp()` doesn't require a live `renderdoc` import at module scope (it doesn't — tool registration is lazy). If no build is available in this environment, run the check anyway; it should still print `ok` since no replay call happens during registration.

Run: `python -c "from renderdoc_mcp.server import build_mcp; build_mcp(); print('ok')"`
Expected: `ok`

- [ ] **Step 5: Commit**

```bash
git add renderdoc-mcp/renderdoc_mcp/server.py
git commit -m "mcp: add decode_post_vs_outputs tool"
```

---

### Task 7: Document the new tool in the README

**Files:**
- Modify: `renderdoc-mcp/README.md`

**Interfaces:**
- Consumes: nothing (documentation only).

- [ ] **Step 1: Add a tools-table row**

In `renderdoc-mcp/README.md`, find this row in the Tools table:

```markdown
| `decode_mesh_inputs` | Vertex layout + indexbuffer summary + vertex previews |
```

Add a new row directly after it:

```markdown
| `decode_mesh_inputs` | Vertex layout + indexbuffer summary + vertex previews |
| `decode_post_vs_outputs` | Decode a draw's post-VS/GS output into typed semantics (POSITION, COLORn, texcoords) for selected vertices |
```

- [ ] **Step 2: Add a Limitations bullet**

In `renderdoc-mcp/README.md`, find the `## Limitations` section's last bullet (`- Replay APIs must run serialized; the server uses a lock around all tools.`) and add a new bullet directly after it (i.e. at the true end of the list, before `## License`):

```markdown
- **`decode_post_vs_outputs`** only decodes the primary output stream (`stream=0`) of a Geometry
  shader's multi-stream output, and only supports `VSOut`/`GSOut` -- mesh-shader/task-shader
  stages (`MeshOut`/`TaskOut`) aren't exposed by this tool. POSITION's reported NDC value is a
  perspective divide only (`xyz/w`), not a full camera/view-matrix reconstruction into screen
  pixel coordinates -- use `pixel_history`/`trace_pixel_provenance` for that.
```

- [ ] **Step 3: Commit**

```bash
git add renderdoc-mcp/README.md
git commit -m "docs(mcp): document decode_post_vs_outputs"
```

---

### Task 8: Integration test against a real post-VS capture

**Files:**
- Create: `util/test/tests/D3D11/D3D11_Mesh_Zoo_PostVS.py`

**Interfaces:**
- Consumes: `build_output_column_layout` (Task 3), `select_gsout_reflection_stage` (Task 2) — imported directly from `renderdoc_mcp.mesh_decode`.

This validates the column-layout algorithm (Task 3) against a real capture's actual reflection data, not just synthetic stand-ins. `util/test/demos/d3d11/d3d11_mesh_zoo.cpp:31-44` defines a vertex shader whose output signature is exactly: `SV_POSITION` (float4, `system_value="Position"`), `COLOR0` (float2), `COLOR1` (float4) — and `util/test/rdtest/shared/Mesh_Zoo.py:89-98` (already an established, passing test) manually hardcodes the exact same byte offsets this task's test asserts (`POSITION` at 0, `COLOR0` at 16, `COLOR1` at 24 unaligned / 32 aligned), so there is strong independent confirmation this fixture exercises exactly the alignment-padding path Task 3's logic must get right.

- [ ] **Step 1: Confirm the environment can produce the capture this test needs**

Run: `cd util/test && python run_tests.py --renderdoc <path>/x64/Development --pyrenderdoc <path>/x64/Development/pymodules -t D3D11_Mesh_Zoo -l`

If this requires building `demos_x64.exe` and that build isn't feasible in this environment, **stop here and report this task as skipped with the reason** — do not fabricate a capture or synthetic substitute. Tasks 1-7 do not depend on this task. (Per prior experience on this project: a bundled 64-bit Python interpreter under the RenderDoc build's `x64/Development/python/` directory is usually required — the system default `python`/`python3` is commonly 32-bit and fails to import `renderdoc`.)

- [ ] **Step 2: Write the integration test**

Create `util/test/tests/D3D11/D3D11_Mesh_Zoo_PostVS.py`:

```python
import renderdoc as rd
import rdtest


class D3D11_Mesh_Zoo_PostVS(rdtest.TestCase):
    demos_test_name = 'D3D11_Mesh_Zoo'

    def check_capture(self):
        from renderdoc_mcp.mesh_decode import build_output_column_layout
        from renderdoc_mcp.rdutil import enum_name

        action = self.find_action("Quad")
        self.controller.SetFrameEvent(action.next.eventId, False)

        pipe: rd.PipeState = self.controller.GetPipelineState()
        refl = pipe.GetShaderReflection(rd.ShaderStage.Vertex)
        if refl is None:
            raise rdtest.TestFailureException("Expected a bound vertex shader reflection")

        sig_params = []
        for sig in refl.outputSignature:
            sig_params.append({
                "name": sig.varName if sig.varName else sig.semanticIdxName,
                "semantic_name": sig.semanticName,
                "semantic_index": int(sig.semanticIndex),
                "system_value": enum_name(sig.systemValue),
                "var_type": enum_name(sig.varType),
                "comp_count": int(sig.compCount),
                "stream": int(sig.stream),
            })

        aligned = bool(pipe.HasAlignedPostVSData(rd.MeshDataStage.VSOut))
        columns = build_output_column_layout(sig_params, aligned)

        def find_column(**criteria):
            for c in columns:
                if all(c.get(k) == v for k, v in criteria.items()):
                    return c
            raise rdtest.TestFailureException(
                "Expected a column matching {} in {}".format(criteria, columns))

        pos_col = find_column(system_value="Position")
        color0_col = find_column(semantic_name="COLOR", semantic_index=0)
        color1_col = find_column(semantic_name="COLOR", semantic_index=1)

        if pos_col["byte_offset"] != 0:
            raise rdtest.TestFailureException(
                "Expected POSITION at byte 0, got {}".format(pos_col["byte_offset"]))
        if color0_col["byte_offset"] != 16:
            raise rdtest.TestFailureException(
                "Expected COLOR0 at byte 16, got {}".format(color0_col["byte_offset"]))

        expected_color1_offset = 32 if aligned else 24
        if color1_col["byte_offset"] != expected_color1_offset:
            raise rdtest.TestFailureException(
                "Expected COLOR1 at byte {} (aligned={}), got {}".format(
                    expected_color1_offset, aligned, color1_col["byte_offset"]))

        # Confirm GetPostVSData actually agrees this layout is real: the reported vertex stride
        # (sum of all column sizes) should not exceed the driver's own reported stride.
        mesh_fmt = self.controller.GetPostVSData(0, 0, rd.MeshDataStage.VSOut)
        computed_stride = sum(c["comp_count"] * c["elem_byte_width"] for c in columns)
        if mesh_fmt.vertexResourceId == rd.ResourceId.Null():
            raise rdtest.TestFailureException("Expected valid post-VS data for the Quad draw")
        if computed_stride > mesh_fmt.vertexByteStride:
            raise rdtest.TestFailureException(
                "Computed stride {} exceeds driver-reported stride {}".format(
                    computed_stride, mesh_fmt.vertexByteStride))

        rdtest.log.success(
            "decode_post_vs_outputs column layout matches Mesh_Zoo's own reference offsets "
            "(POSITION=0, COLOR0=16, COLOR1={}, aligned={})".format(expected_color1_offset, aligned))
```

- [ ] **Step 3: Run the new test**

Run: `cd util/test && python run_tests.py --renderdoc <path>/x64/Development --pyrenderdoc <path>/x64/Development/pymodules -t D3D11_Mesh_Zoo_PostVS`
Expected: PASS, logging the success message above

If it fails because the real capture's reflection/alignment behaves differently than assumed, **do not adjust the test to paper over it** — this means Task 3's `build_output_column_layout` has a real bug against this capture; go back and fix it, then re-run this test.

- [ ] **Step 4: Commit**

```bash
git add util/test/tests/D3D11/D3D11_Mesh_Zoo_PostVS.py
git commit -m "test: verify decode_post_vs_outputs column layout against a real D3D11 capture"
```

---

## Self-Review Notes

- **Spec coverage:** Purpose/Algorithm → Tasks 1-5. Parameters/Response shape → Tasks 5-6. Error Handling/Limitations (`bad_stage`, `no_geometry_or_tessellation_stage`, `no_shader_reflection`, `no_post_vs_data`, multi-stream skip, `MeshOut`/`TaskOut` out of scope, NDC-only) → Task 5's branches + Task 7's README bullet. Testing (unit tests on pure logic, one integration test) → Tasks 1-4 and Task 8. Out-of-scope items are explicitly not touched by any task.
- **Type consistency checked:** `build_output_column_layout`'s output dict keys (`name`, `semantic_name`, `semantic_index`, `system_value`, `comp_type`, `comp_count`, `elem_byte_width`, `byte_offset`) are used identically in Task 5's orchestration function and Task 8's integration test — no renaming. `decode_semantic_bytes`'s `(comp_type, elem_byte_width)` key pairs in its lookup table cover every combination `var_type_byte_size`/`var_type_comp_type` (Task 1) can produce (4- or 8-byte width × Float/UInt/SInt). `select_gsout_reflection_stage`'s three return values (`"geometry"`, `"domain"`, `None`) match exactly what Task 5's `if resolved == "geometry" else ...`/`if resolved is None` branches check.
- **No placeholders:** every step has complete code; Task 8's conditional skip is an explicit, justified escape hatch (missing build/GPU environment), not a vague TBD, and is backed by a concrete, already-passing reference fixture (`Mesh_Zoo`) rather than a search for one.
- **Deviation from spec's illustrative response JSON:** the spec's example response didn't include an `"index"` field separate from `"vertex_index"` for post-VS previews (unlike `decode_mesh_inputs`'s input-side response) — post-VS data is a flat, already-materialized buffer with no separate index-buffer indirection to report, so Task 5 only emits `vertex_index`. This is a resolved implementation ambiguity, not a scope change.
