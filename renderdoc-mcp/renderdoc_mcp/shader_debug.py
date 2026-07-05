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
import json
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


def _resolve_binding(pipe: Any, stage: Any, kind: str, index: int, controller: Any) -> dict[str, Any] | None:
    try:
        if kind == "srv":
            lst = pipe.GetReadOnlyResources(stage)
        elif kind == "uav":
            lst = pipe.GetReadWriteResources(stage)
        elif kind == "sampler":
            lst = pipe.GetSamplers(stage)
        else:
            return None
        if index < 0 or index >= len(lst):
            return None
        return serialize_used_descriptor(lst[index], controller)
    except Exception:
        return None


def _resolve_cb(pipe: Any, stage: Any, slot: int, controller: Any) -> dict[str, Any] | None:
    try:
        cb = pipe.GetConstantBlock(stage, int(slot), 0)
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
            srv = [r for i in refs["srv"] if (r := _resolve_binding(pipe, stage, "srv", i, controller))]
            if srv:
                resolved["srv"] = srv
            sampler = [r for i in refs["sampler"] if (r := _resolve_binding(pipe, stage, "sampler", i, controller))]
            if sampler:
                resolved["sampler"] = sampler
            uav = [r for i in refs["uav"] if (r := _resolve_binding(pipe, stage, "uav", i, controller))]
            if uav:
                resolved["uav"] = uav
            cb = [r for c in refs["cb"] if (r := _resolve_cb(pipe, stage, c["slot"], controller))]
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
