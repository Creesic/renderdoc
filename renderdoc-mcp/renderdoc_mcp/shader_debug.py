"""Pixel/vertex shader debug tracing: summarizes ReplayController.DebugPixel/DebugVertex traces.

Resource-access identification (which descriptor a sample/load instruction touched) is done by a
best-effort regex match against the disassembly line for that instruction, using DXBC-style
register tokens (``t#``/``s#``/``u#``/``cb#[#]``). This resolves cleanly for DXBC (and
similarly-styled DXIL) disassembly; other shader representations (SPIR-V, GLSL) still get the raw
instruction text and register value changes, just without resolved resource identity — callers
should treat ``resolved`` as best-effort and fall back to ``disassembly`` when it's empty. See
renderdoc-mcp/TODO.md #1.
"""

from __future__ import annotations

import bisect
import difflib
import json
import math
import re
from typing import Any

from renderdoc_mcp import rdutil
from renderdoc_mcp.serialize import serialize_used_descriptor

NO_PREFERENCE = 0xFFFFFFFF  # matches IReplayController::NoPreference (~0U)

# Safety ceiling on ContinueDebug() steps. The C++ UI (qrenderdoc/Windows/ShaderViewer.cpp) runs the
# same unbounded loop but lets the user cancel by closing the debug view; this server has no such
# escape hatch and serializes every tool call behind one replay thread/lock, so a shader that runs
# (near-)indefinitely -- a genuine infinite loop, or just a huge iteration count -- wedges every
# other tool call too, and the accumulated ShaderDebugState list can grow without bound. Observed in
# practice: the server process ran for 1000+s and grew to 20+GB before dying, taking the whole MCP
# server down with it.
MAX_DEBUG_STEPS = 20000

# DisassembleShader returns these exact sentinel strings (not exceptions) on failure — see
# e.g. renderdoc/driver/d3d12/d3d12_replay.cpp, d3d11_replay.cpp, vk_replay.cpp, gl_replay.cpp.
_INVALID_SHADER_MARKER = "; Invalid Shader Specified"
_DISASSEMBLY_FAILURE_MARKERS = (
    _INVALID_SHADER_MARKER,
    "; Invalid disassembly target",
    "; No pipeline specified,",
    "; Unknown error fetching disassembly",
    # ISA targets (AMDIL / GCN / RDNA) can only disassemble native DXBC/SPIR-V; for a
    # DXIL or SPIR-V shader they return this exact sentinel (no leading ';'). Without it
    # the target loop accepted this 38-char string as if it were real disassembly and
    # reported disassembly_available=true with useless text, hiding that the source-level
    # DXBC/DXIL target was the one that actually failed. Observed on FM2/plume DXIL SM6.0.
    "Unsupported encoding for shader",
    # AMD ISA paths (amd_isa.cpp / amd_isa_win32.cpp) — replay_controller.cpp routes GCN
    # targets there before the driver, so these are reachable from any DisassembleShader call.
    "; Invalid ISA Target specified",
    "; Failed to Disassemble - ",
    "; Cannot identify shader type",
    "; SPIR-V disassembly not supported,",
    "; Invalid ELF file generated",
    "; Error loading ",
    "; Shader disassembly for DXIL shaders is not supported.",
    "; Failed to disassemble shader",
    # d3d12_replay.cpp
    "; Unknown shader stage in shader reflection",
    "; Couldn't find disassembly for given shader stage",
    # replay_controller.cpp
    "; Error: No shader specified",
)


def disassembly_failure_reason(text: str) -> str | None:
    """None if `text` looks like real disassembly; otherwise a human-readable failure reason.

    ``DisassembleShader`` signals failure by returning a sentinel string rather than raising, so
    callers must inspect the text itself. ``_INVALID_SHADER_MARKER`` in particular means the
    replay driver couldn't resolve ``refl.resourceId`` to a tracked shader resource at all — this
    is a replay-side resource-tracking gap, not a target-selection or missing-DLL problem, and no
    amount of retrying other targets will fix it (every target hits the same check first). See
    renderdoc-mcp/TODO.md #2.
    """
    stripped = (text or "").strip()
    if not stripped:
        return "empty disassembly result"
    for marker in _DISASSEMBLY_FAILURE_MARKERS:
        if stripped.startswith(marker):
            if marker == _INVALID_SHADER_MARKER:
                return (
                    "RenderDoc could not resolve this shader's resource for disassembly "
                    "(replay-side resource-tracking gap for this shader's creation path, not a "
                    "target-selection or missing-DLL issue). See renderdoc-mcp/TODO.md #2."
                )
            return stripped
    return None


def best_disassembly(controller: Any, pipe_obj: Any, refl: Any) -> str:
    """First disassembly target that returns real text (not a failure sentinel), else ''.

    ISA targets (AMDIL/GCN/RDNA) return an "Unsupported encoding" sentinel for DXIL/SPIR-V
    shaders; taking ``targets[0]`` blindly and splitting it fed that sentinel to the resource
    resolver as if it were disassembly. Reuse the same failure detection get_shader uses so the
    debug-trace resource resolution only ever sees genuine disassembly.
    """
    try:
        targets = list(controller.GetDisassemblyTargets(True))
    except Exception:
        return ""
    for t in targets:
        try:
            candidate = controller.DisassembleShader(pipe_obj, refl, t)
        except Exception:
            continue
        if disassembly_failure_reason(candidate) is None:
            return candidate
    return ""

_RES_TOKEN_RE = re.compile(r"(?<![A-Za-z0-9_])([tsu])(\d+)(?![A-Za-z0-9_])")
_CB_TOKEN_RE = re.compile(r"(?<![A-Za-z0-9_])cb(\d+)\[(\d+)\]")
_OUTPUT_REG_RE = re.compile(r"^o(\d+)$", re.IGNORECASE)

_TYPE_ARRAY_ATTR = {
    "Float": "f32v",
    "Double": "f64v",
    "Half": "f16v",
    "SInt": "s32v",
    "UInt": "u32v",
    "SShort": "s16v",
    "UShort": "u16v",
    "SLong": "s64v",
    "ULong": "u64v",
    "SByte": "s8v",
    "UByte": "u8v",
    "Bool": "u32v",
    "GPUPointer": "u64v",
    "ConstantBlock": "u32v",
    "ReadOnlyResource": "u32v",
    "ReadWriteResource": "u32v",
    "Sampler": "u32v",
}
_FLOAT_ATTRS = ("f32v", "f64v", "f16v")


def _var_components(var: Any) -> list[Any]:
    tname = rdutil.enum_name(getattr(var, "type", None))
    attr = _TYPE_ARRAY_ATTR.get(tname, "u32v")
    rows = max(1, int(getattr(var, "rows", 1) or 1))
    cols = max(1, int(getattr(var, "columns", 1) or 1))
    n = min(16, rows * cols)
    value = getattr(var, "value", None)
    arr = getattr(value, attr, None) if value is not None else None
    if arr is None:
        return []
    out: list[Any] = []
    for i in range(n):
        try:
            x = arr[i]
            out.append(float(x) if attr in _FLOAT_ATTRS else int(x))
        except Exception:
            out.append(None)
    return out


def shader_variable_to_value(var: Any) -> dict[str, Any]:
    """Recursively convert a ShaderVariable (register/CB/input value) to a JSON-safe dict.

    Always includes a "value" key (None for composite types) so callers can safely do
    result["value"] without checking for "members" first -- DXIL traces commonly produce
    composite (struct/array) ShaderVariables with no single scalar value, which previously
    produced a dict missing "value" entirely and crashed callers that assumed it was always
    present (KeyError: 'value').
    """
    name = str(getattr(var, "name", "") or "")
    members = list(getattr(var, "members", []) or [])
    if members:
        return {
            "name": name,
            "type": rdutil.enum_name(getattr(var, "type", None)),
            "value": None,
            "members": [shader_variable_to_value(m) for m in members],
        }
    rows = max(1, int(getattr(var, "rows", 1) or 1))
    cols = max(1, int(getattr(var, "columns", 1) or 1))
    comps = _var_components(var)
    value: Any
    if rows > 1 and len(comps) >= rows * cols:
        value = [comps[r * cols:(r + 1) * cols] for r in range(rows)]
    elif cols > 1:
        value = comps[:cols]
    else:
        value = comps[0] if comps else None
    return {"name": name, "type": rdutil.enum_name(getattr(var, "type", None)), "value": value}


def run_debug_trace(controller: Any, trace: Any, max_steps: int = MAX_DEBUG_STEPS) -> tuple[list[Any], bool]:
    """Repeatedly call ContinueDebug until it returns no more steps (matches the C++ UI's loop).

    Stops early once `max_steps` states have been collected, returning what was gathered so far
    rather than looping forever -- see MAX_DEBUG_STEPS. Returns (states, truncated). Calling
    FreeTrace on a debugger that hasn't reached its final state is safe (ReplayController::FreeTrace
    just releases it unconditionally; the C++ UI relies on the same behavior when a user cancels).
    """
    states: list[Any] = []
    truncated = False
    while True:
        chunk = list(controller.ContinueDebug(trace.debugger))
        if not chunk:
            break
        states.extend(chunk)
        if len(states) >= max_steps:
            truncated = True
            break
    return states, truncated


def _instruction_line_info(inst_info: list[Any], inst_numbers: list[int], instruction: int) -> Any | None:
    idx = bisect.bisect_right(inst_numbers, instruction) - 1
    if idx < 0:
        return None
    return inst_info[idx]


def _disasm_line_text(disasm_lines: list[str], info: Any | None) -> str:
    if info is None or not disasm_lines:
        return ""
    line_info = getattr(info, "lineInfo", None)
    if line_info is None:
        return ""
    line_no = int(getattr(line_info, "disassemblyLine", 0) or 0)
    if line_no <= 0 or line_no > len(disasm_lines):
        return ""
    return disasm_lines[line_no - 1].strip()


def _heuristic_resource_refs(line_text: str) -> dict[str, list[Any]]:
    refs: dict[str, list[Any]] = {"srv": [], "sampler": [], "uav": [], "cb": []}
    if not line_text:
        return refs
    for m in _RES_TOKEN_RE.finditer(line_text):
        kind, idx = m.group(1), int(m.group(2))
        bucket = {"t": "srv", "s": "sampler", "u": "uav"}.get(kind)
        if bucket and idx not in refs[bucket]:
            refs[bucket].append(idx)
    for m in _CB_TOKEN_RE.finditer(line_text):
        refs["cb"].append({"slot": int(m.group(1)), "offset_vec4": int(m.group(2))})
    return refs


_REFL_LIST_FOR_KIND = {
    "srv": "readOnlyResources",
    "uav": "readWriteResources",
    "sampler": "samplers",
}


def _reflection_index_for_register(refl: Any, list_attr: str, register: int) -> int | None:
    """Map a bind/register number to its index in the shader reflection array.

    Disassembly register tokens (t#/s#/u#/cb#) are bind numbers; the PipeState accessors
    key descriptors by DescriptorAccess.index, which is the reflection-array position.
    """
    for i, res in enumerate(list(getattr(refl, list_attr, []) or [])):
        if int(getattr(res, "fixedBindNumber", -1)) == int(register):
            return i
    return None


def _resolve_binding(pipe: Any, stage: Any, kind: str, register: int, controller: Any,
                     refl: Any) -> dict[str, Any] | None:
    try:
        list_attr = _REFL_LIST_FOR_KIND.get(kind)
        if list_attr is None:
            return None
        refl_index = _reflection_index_for_register(refl, list_attr, register)
        if refl_index is None:
            return None
        if kind == "srv":
            lst = pipe.GetReadOnlyResources(stage)
        elif kind == "uav":
            lst = pipe.GetReadWriteResources(stage)
        else:
            lst = pipe.GetSamplers(stage)
        for used in lst:
            access = getattr(used, "access", None)
            if access is not None and int(getattr(access, "index", -1)) == refl_index:
                return serialize_used_descriptor(used, controller)
        return None
    except Exception:
        return None


def _resolve_cb(pipe: Any, stage: Any, slot: int, controller: Any,
                refl: Any) -> dict[str, Any] | None:
    try:
        refl_index = _reflection_index_for_register(refl, "constantBlocks", slot)
        if refl_index is None:
            return None
        cb = pipe.GetConstantBlock(stage, refl_index, 0)
        desc = getattr(cb, "descriptor", None)
        if desc is None:
            return None
        out: dict[str, Any] = {}
        rdutil.enrich_resource_dict(controller, out, getattr(desc, "resource", None))
        out["byte_offset"] = int(getattr(desc, "byteOffset", 0))
        return out
    except Exception:
        return None


def match_output_values(final_registers: dict[str, Any], refl: Any) -> list[dict[str, Any]]:
    """Best-effort match of final register values to the shader's output signature.

    Assumes DXBC/DXIL-style ``o<N>`` output register naming; falls back to nothing if the
    underlying representation names output registers differently (caller should also inspect
    ``final_registers`` directly in that case).
    """
    sig_by_index: dict[int, Any] = {}
    for sp in list(getattr(refl, "outputSignature", []) or []):
        sig_by_index[int(getattr(sp, "regIndex", -1))] = sp
    out: list[dict[str, Any]] = []
    for name, value in final_registers.items():
        m = _OUTPUT_REG_RE.match(name)
        if not m:
            continue
        row: dict[str, Any] = {"register": name, "value": value}
        sp = sig_by_index.get(int(m.group(1)))
        if sp is not None:
            sem = str(getattr(sp, "semanticName", "") or "")
            if sem:
                row["semantic"] = sem
            si = int(getattr(sp, "semanticIndex", 0) or 0)
            if si:
                row["semantic_index"] = si
        out.append(row)
    return out


def summarize_debug_trace(
    rd: Any,
    controller: Any,
    pipe: Any,
    stage: Any,
    refl: Any,
    trace: Any,
    states: list[Any],
    disasm_lines: list[str],
) -> dict[str, Any]:
    """Build the default (non-full) summary of a shader debug trace for debug_pixel/debug_vertex."""
    inst_info = list(getattr(trace, "instInfo", []) or [])
    inst_numbers = [int(getattr(i, "instruction", 0)) for i in inst_info]

    sample_flag = int(rd.ShaderEvents.SampleLoadGather)
    nan_flag = int(rd.ShaderEvents.GeneratedNanOrInf)

    inputs = [shader_variable_to_value(v) for v in getattr(trace, "inputs", []) or []]
    constant_blocks = [shader_variable_to_value(v) for v in getattr(trace, "constantBlocks", []) or []]

    final_registers: dict[str, Any] = {}
    resource_accesses: list[dict[str, Any]] = []
    nan_or_inf_steps: list[dict[str, Any]] = []

    prev_instruction = 0
    for step_index, state in enumerate(states):
        flags = int(getattr(state, "flags", 0) or 0)
        changes = list(getattr(state, "changes", []) or [])

        change_rows: list[dict[str, Any]] = []
        for change_index, ch in enumerate(changes):
            after = getattr(ch, "after", None)
            before = getattr(ch, "before", None)
            reg_name = str(getattr(after, "name", "") or getattr(before, "name", "") or "")
            if not reg_name:
                # DXIL traces commonly have unnamed intermediate ShaderVariables; fall back to a
                # positional key so distinct anonymous registers don't silently overwrite each
                # other in final_registers.
                reg_name = "_unnamed_{}_{}".format(step_index, change_index)
            row: dict[str, Any] = {"name": reg_name}
            if after is not None:
                after_val = shader_variable_to_value(after)
                # Preserve the full (possibly composite) representation instead of collapsing
                # struct/array changes to a bare None -- only unwrap to the plain scalar/vector
                # "value" when there's no member structure to lose.
                stored = after_val if after_val.get("members") else after_val.get("value")
                row["after"] = stored
                final_registers[reg_name] = stored
            change_rows.append(row)

        if flags & sample_flag:
            info = _instruction_line_info(inst_info, inst_numbers, prev_instruction)
            line_text = _disasm_line_text(disasm_lines, info)
            refs = _heuristic_resource_refs(line_text)
            resolved: dict[str, Any] = {}
            srv = [r for i in refs["srv"] if (r := _resolve_binding(pipe, stage, "srv", i, controller, refl))]
            if srv:
                resolved["srv"] = srv
            sampler = [r for i in refs["sampler"] if (r := _resolve_binding(pipe, stage, "sampler", i, controller, refl))]
            if sampler:
                resolved["sampler"] = sampler
            uav = [r for i in refs["uav"] if (r := _resolve_binding(pipe, stage, "uav", i, controller, refl))]
            if uav:
                resolved["uav"] = uav
            cb = [r for c in refs["cb"] if (r := _resolve_cb(pipe, stage, c["slot"], controller, refl))]
            if cb:
                resolved["cb"] = cb
            resource_accesses.append(
                {
                    "step_index": step_index,
                    "instruction": prev_instruction,
                    "disassembly": line_text,
                    "registers_changed": change_rows,
                    "resolved": resolved,
                }
            )

        if flags & nan_flag:
            nan_or_inf_steps.append({"step_index": step_index, "instruction": prev_instruction})

        prev_instruction = int(getattr(state, "nextInstruction", prev_instruction) or prev_instruction)

    return {
        "step_count": len(states),
        "inputs": inputs,
        "constant_blocks": constant_blocks,
        "resource_accesses": resource_accesses,
        "sample_count": len(resource_accesses),
        "nan_or_inf_steps": nan_or_inf_steps,
        "output_values": match_output_values(final_registers, refl),
        "final_registers": final_registers,
    }


def build_comparable_debug_trace(
    rd: Any,
    refl: Any,
    trace: Any,
    states: list[Any],
    disasm_lines: list[str],
) -> dict[str, Any]:
    """Serialize the parts of a shader trace needed for cross-capture comparison.

    Unlike ``summarize_debug_trace`` this deliberately keeps every executed instruction, but it
    omits resource binding resolution and the full before-state to keep two-trace comparisons
    reasonably small. Register changes retain their post-instruction values, which is enough to
    identify the first temporary/output that diverges.
    """
    inst_info = list(getattr(trace, "instInfo", []) or [])
    inst_numbers = [int(getattr(i, "instruction", 0)) for i in inst_info]
    final_registers: dict[str, Any] = {}
    steps: list[dict[str, Any]] = []
    prev_instruction = 0

    for step_index, state in enumerate(states):
        changes: list[dict[str, Any]] = []
        for change_index, change in enumerate(list(getattr(state, "changes", []) or [])):
            after = getattr(change, "after", None)
            before = getattr(change, "before", None)
            name = str(getattr(after, "name", "") or getattr(before, "name", "") or "")
            if not name:
                name = "_unnamed_{}".format(change_index)
            stored: Any = None
            if after is not None:
                converted = shader_variable_to_value(after)
                stored = converted if converted.get("members") else converted.get("value")
                final_registers[name] = stored
            changes.append({"name": name, "after": stored})

        info = _instruction_line_info(inst_info, inst_numbers, prev_instruction)
        steps.append(
            {
                "step_index": step_index,
                "instruction": prev_instruction,
                "next_instruction": int(getattr(state, "nextInstruction", 0) or 0),
                "flags": int(getattr(state, "flags", 0) or 0),
                "disassembly": _disasm_line_text(disasm_lines, info),
                "changes": changes,
            }
        )
        prev_instruction = int(getattr(state, "nextInstruction", prev_instruction) or prev_instruction)

    return {
        "stage": rdutil.enum_name(getattr(trace, "stage", None)),
        "inputs": [shader_variable_to_value(v) for v in getattr(trace, "inputs", []) or []],
        "constant_blocks": [
            shader_variable_to_value(v) for v in getattr(trace, "constantBlocks", []) or []
        ],
        "steps": steps,
        "output_values": match_output_values(final_registers, refl),
        "final_registers": final_registers,
    }


_DISASM_PREFIX_RE = re.compile(r"^\s*(?:(?:0x)?[0-9a-f]+\s*[:)]\s*)", re.IGNORECASE)
_MISSING = object()


def _normalise_instruction(text: str) -> str:
    text = _DISASM_PREFIX_RE.sub("", str(text or "").strip())
    return " ".join(text.split()).lower()


def _values_equal(a: Any, b: Any, abs_tolerance: float, rel_tolerance: float) -> bool:
    numeric_a = isinstance(a, (int, float)) and not isinstance(a, bool)
    numeric_b = isinstance(b, (int, float)) and not isinstance(b, bool)
    if numeric_a and numeric_b:
        af = float(a)
        bf = float(b)
        if math.isnan(af) or math.isnan(bf):
            return math.isnan(af) and math.isnan(bf)
        return math.isclose(af, bf, abs_tol=abs_tolerance, rel_tol=rel_tolerance)
    if type(a) is not type(b):
        return False
    if isinstance(a, list):
        return len(a) == len(b) and all(
            _values_equal(av, bv, abs_tolerance, rel_tolerance) for av, bv in zip(a, b)
        )
    if isinstance(a, dict):
        return a.keys() == b.keys() and all(
            _values_equal(a[key], b[key], abs_tolerance, rel_tolerance) for key in a
        )
    return a == b


def _named_value_map(rows: list[dict[str, Any]], fallback: str) -> dict[str, Any]:
    out: dict[str, Any] = {}

    def visit(row: dict[str, Any], path: str) -> None:
        members = list(row.get("members") or [])
        if members:
            for index, member in enumerate(members):
                member_name = str(member.get("name") or "[{}]".format(index))
                visit(member, "{}.{}".format(path, member_name) if path else member_name)
        else:
            out[path] = row.get("value")

    for index, row in enumerate(rows):
        name = str(row.get("name") or "{}[{}]".format(fallback, index))
        visit(row, name)
    return out


def _output_value_map(rows: list[dict[str, Any]]) -> dict[str, Any]:
    out: dict[str, Any] = {}
    for index, row in enumerate(rows):
        register = str(row.get("register") or "output[{}]".format(index))
        semantic = str(row.get("semantic") or "")
        semantic_index = int(row.get("semantic_index", 0) or 0)
        if semantic_index:
            semantic += str(semantic_index)
        key = "{}:{}".format(semantic, register) if semantic else register
        out[key] = row.get("value")
    return out


def _trace_output_value_map(trace: dict[str, Any]) -> dict[str, Any]:
    semantic_outputs = _output_value_map(list(trace.get("output_values") or []))
    if semantic_outputs:
        return semantic_outputs
    # Some backends expose anonymous/non-oN output registers that cannot be matched to reflection
    # semantics. Compare their final register state instead of claiming there were no outputs.
    return dict(trace.get("final_registers") or {})


def _register_value_map(step: dict[str, Any]) -> dict[str, Any]:
    return {
        str(change.get("name") or "change[{}]".format(index)): change.get("after")
        for index, change in enumerate(step.get("changes") or [])
    }


def _diff_value_maps(
    a: dict[str, Any],
    b: dict[str, Any],
    abs_tolerance: float,
    rel_tolerance: float,
    max_differences: int,
) -> list[dict[str, Any]]:
    differences: list[dict[str, Any]] = []
    for path in sorted(a.keys() | b.keys()):
        av = a.get(path, _MISSING)
        bv = b.get(path, _MISSING)
        if av is not _MISSING and bv is not _MISSING and _values_equal(
            av, bv, abs_tolerance, rel_tolerance
        ):
            continue
        row: dict[str, Any] = {"path": path}
        if av is _MISSING:
            row["only_in"] = "b"
            row["b"] = bv
        elif bv is _MISSING:
            row["only_in"] = "a"
            row["a"] = av
        else:
            row["a"] = av
            row["b"] = bv
        differences.append(row)
        if len(differences) >= max_differences:
            break
    return differences


def _align_debug_steps(
    steps_a: list[dict[str, Any]], steps_b: list[dict[str, Any]]
) -> tuple[str, list[tuple[str, dict[str, Any] | None, dict[str, Any] | None]]]:
    tokens_a = [_normalise_instruction(step.get("disassembly", "")) for step in steps_a]
    tokens_b = [_normalise_instruction(step.get("disassembly", "")) for step in steps_b]
    use_disassembly = bool(set(filter(None, tokens_a)) & set(filter(None, tokens_b)))
    aligned: list[tuple[str, dict[str, Any] | None, dict[str, Any] | None]] = []

    if not use_disassembly:
        common = min(len(steps_a), len(steps_b))
        aligned.extend(("paired", steps_a[i], steps_b[i]) for i in range(common))
        aligned.extend(("only_a", step, None) for step in steps_a[common:])
        aligned.extend(("only_b", None, step) for step in steps_b[common:])
        return "execution_position", aligned

    matcher = difflib.SequenceMatcher(a=tokens_a, b=tokens_b, autojunk=False)
    for tag, i1, i2, j1, j2 in matcher.get_opcodes():
        if tag == "equal":
            aligned.extend(("paired", a, b) for a, b in zip(steps_a[i1:i2], steps_b[j1:j2]))
        elif tag == "replace":
            count = min(i2 - i1, j2 - j1)
            aligned.extend(
                ("paired", steps_a[i1 + offset], steps_b[j1 + offset]) for offset in range(count)
            )
            aligned.extend(("only_a", step, None) for step in steps_a[i1 + count:i2])
            aligned.extend(("only_b", None, step) for step in steps_b[j1 + count:j2])
        elif tag == "delete":
            aligned.extend(("only_a", step, None) for step in steps_a[i1:i2])
        elif tag == "insert":
            aligned.extend(("only_b", None, step) for step in steps_b[j1:j2])
    return "disassembly_sequence", aligned


def compare_debug_traces(
    trace_a: dict[str, Any],
    trace_b: dict[str, Any],
    *,
    abs_tolerance: float = 1e-6,
    rel_tolerance: float = 1e-5,
    max_differences: int = 100,
) -> dict[str, Any]:
    """Compare two serialized shader invocations and locate their first divergent step."""
    max_differences = max(1, int(max_differences))
    pre_execution: list[dict[str, Any]] = []
    pre_execution_truncated = False
    for section, fallback in (("inputs", "input"), ("constants", "constant")):
        source_key = "constant_blocks" if section == "constants" else section
        values_a = _named_value_map(list(trace_a.get(source_key) or []), fallback)
        values_b = _named_value_map(list(trace_b.get(source_key) or []), fallback)
        remaining = max_differences - len(pre_execution)
        if remaining <= 0:
            if _diff_value_maps(values_a, values_b, abs_tolerance, rel_tolerance, 1):
                pre_execution_truncated = True
            continue
        differences = _diff_value_maps(
            values_a,
            values_b,
            abs_tolerance,
            rel_tolerance,
            remaining + 1,
        )
        if len(differences) > remaining:
            pre_execution_truncated = True
        pre_execution.extend(
            {"section": section, **difference} for difference in differences[:remaining]
        )
        if pre_execution_truncated:
            break

    steps_a = list(trace_a.get("steps") or [])
    steps_b = list(trace_b.get("steps") or [])
    method, alignment = _align_debug_steps(steps_a, steps_b)
    step_differences: list[dict[str, Any]] = []
    only_a_count = sum(1 for kind, _, _ in alignment if kind == "only_a")
    only_b_count = sum(1 for kind, _, _ in alignment if kind == "only_b")
    step_differences_truncated = False

    for kind, step_a, step_b in alignment:
        difference: dict[str, Any] | None = None
        if kind != "paired":
            present = step_a if step_a is not None else step_b
            difference = {
                "category": "instruction_only_in_a" if step_a is not None else "instruction_only_in_b",
                "instruction_a": step_a.get("instruction") if step_a is not None else None,
                "instruction_b": step_b.get("instruction") if step_b is not None else None,
                "disassembly_a": step_a.get("disassembly", "") if step_a is not None else None,
                "disassembly_b": step_b.get("disassembly", "") if step_b is not None else None,
                "step_index": present.get("step_index") if present is not None else None,
            }
        else:
            assert step_a is not None and step_b is not None
            token_a = _normalise_instruction(step_a.get("disassembly", ""))
            token_b = _normalise_instruction(step_b.get("disassembly", ""))
            register_differences = _diff_value_maps(
                _register_value_map(step_a),
                _register_value_map(step_b),
                abs_tolerance,
                rel_tolerance,
                max_differences + 1,
            )
            if token_a != token_b or register_differences:
                difference = {
                    "category": "instruction" if token_a != token_b else "register_change",
                    "instruction_a": step_a.get("instruction"),
                    "instruction_b": step_b.get("instruction"),
                    "disassembly_a": step_a.get("disassembly", ""),
                    "disassembly_b": step_b.get("disassembly", ""),
                    "register_differences": register_differences[:max_differences],
                }
                if len(register_differences) > max_differences:
                    difference["register_differences_truncated"] = True
        if difference is not None:
            if len(step_differences) < max_differences:
                step_differences.append(difference)
            else:
                step_differences_truncated = True

    all_output_differences = _diff_value_maps(
        _trace_output_value_map(trace_a),
        _trace_output_value_map(trace_b),
        abs_tolerance,
        rel_tolerance,
        max_differences + 1,
    )
    output_differences = all_output_differences[:max_differences]
    output_differences_truncated = len(all_output_differences) > max_differences
    first_divergence: dict[str, Any] | None = step_differences[0] if step_differences else None
    if first_divergence is None and pre_execution:
        first_divergence = {"category": pre_execution[0]["section"], **pre_execution[0]}
    if first_divergence is None and output_differences:
        first_divergence = {"category": "final_output", **output_differences[0]}

    equivalent = not pre_execution and not step_differences and not output_differences
    return {
        "equivalent": equivalent,
        "tolerances": {"absolute": abs_tolerance, "relative": rel_tolerance},
        "alignment": {
            "method": method,
            "step_count_a": len(steps_a),
            "step_count_b": len(steps_b),
            "aligned_count": sum(1 for kind, _, _ in alignment if kind == "paired"),
            "only_in_a_count": only_a_count,
            "only_in_b_count": only_b_count,
        },
        "pre_execution_differences": pre_execution,
        "first_divergence": first_divergence,
        "step_differences": step_differences,
        "output_differences": output_differences,
        "differences_truncated": (
            pre_execution_truncated
            or step_differences_truncated
            or output_differences_truncated
        ),
    }


def dump_full_trace(rd: Any, trace: Any, states: list[Any], disasm_lines: list[str], path: str) -> None:
    """Write the complete instruction-by-instruction trace to a JSON file."""
    inst_info = list(getattr(trace, "instInfo", []) or [])
    inst_numbers = [int(getattr(i, "instruction", 0)) for i in inst_info]

    steps: list[dict[str, Any]] = []
    prev_instruction = 0
    for step_index, state in enumerate(states):
        changes = []
        for ch in list(getattr(state, "changes", []) or []):
            before = getattr(ch, "before", None)
            after = getattr(ch, "after", None)
            changes.append(
                {
                    "before": shader_variable_to_value(before) if before is not None else None,
                    "after": shader_variable_to_value(after) if after is not None else None,
                }
            )
        info = _instruction_line_info(inst_info, inst_numbers, prev_instruction)
        steps.append(
            {
                "step_index": step_index,
                "instruction": prev_instruction,
                "next_instruction": int(getattr(state, "nextInstruction", 0) or 0),
                "flags": int(getattr(state, "flags", 0) or 0),
                "disassembly": _disasm_line_text(disasm_lines, info),
                "changes": changes,
                "callstack": [str(c) for c in getattr(state, "callstack", []) or []],
            }
        )
        prev_instruction = int(getattr(state, "nextInstruction", prev_instruction) or prev_instruction)

    with open(path, "w", encoding="utf-8") as f:
        json.dump(
            {
                "stage": rdutil.enum_name(getattr(trace, "stage", None)),
                "inputs": [shader_variable_to_value(v) for v in getattr(trace, "inputs", []) or []],
                "constant_blocks": [
                    shader_variable_to_value(v) for v in getattr(trace, "constantBlocks", []) or []
                ],
                "disassembly": disasm_lines,
                "steps": steps,
            },
            f,
        )
