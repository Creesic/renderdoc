from __future__ import annotations


def _trace(*, inputs=None, constants=None, steps=None, outputs=None):
    return {
        "inputs": inputs or [],
        "constant_blocks": constants or [],
        "steps": steps or [],
        "output_values": outputs or [],
        "final_registers": {},
    }


def _step(instruction, disassembly, **changes):
    return {
        "instruction": instruction,
        "next_instruction": instruction + 1,
        "disassembly": disassembly,
        "changes": [{"name": name, "after": value} for name, value in changes.items()],
    }


def test_identical_shader_invocations_have_no_divergence():
    from renderdoc_mcp.shader_debug import compare_debug_traces

    trace = _trace(
        inputs=[{"name": "v0", "type": "Float", "value": [1.0, 2.0]}],
        constants=[{"name": "cb0", "type": "ConstantBlock", "value": [3.0]}],
        steps=[_step(0, "0: add r0, v0, cb0[0]", r0=[4.0, 2.0])],
        outputs=[{"register": "o0", "semantic": "SV_Position", "value": [4.0, 2.0]}],
    )

    result = compare_debug_traces(trace, trace)

    assert result["equivalent"] is True
    assert result["first_divergence"] is None
    assert result["pre_execution_differences"] == []
    assert result["step_differences"] == []
    assert result["output_differences"] == []


def test_reports_inputs_constants_and_first_divergent_instruction_result():
    from renderdoc_mcp.shader_debug import compare_debug_traces

    a = _trace(
        inputs=[{"name": "v0", "type": "Float", "value": [1.0, 2.0]}],
        constants=[{"name": "cb0", "type": "ConstantBlock", "value": [3.0]}],
        steps=[
            _step(0, "0: mov r0, v0", r0=[1.0, 2.0]),
            _step(1, "1: add r1, r0, cb0[0]", r1=[4.0, 2.0]),
        ],
        outputs=[{"register": "o0", "semantic": "SV_Position", "value": [4.0, 2.0]}],
    )
    b = _trace(
        inputs=[{"name": "v0", "type": "Float", "value": [1.0, 2.0]}],
        constants=[{"name": "cb0", "type": "ConstantBlock", "value": [4.0]}],
        steps=[
            _step(0, "0: mov r0, v0", r0=[1.0, 2.0]),
            _step(1, "1: add r1, r0, cb0[0]", r1=[5.0, 2.0]),
        ],
        outputs=[{"register": "o0", "semantic": "SV_Position", "value": [5.0, 2.0]}],
    )

    result = compare_debug_traces(a, b)

    assert result["equivalent"] is False
    assert result["pre_execution_differences"][0]["section"] == "constants"
    assert result["first_divergence"]["category"] == "register_change"
    assert result["first_divergence"]["instruction_a"] == 1
    assert result["first_divergence"]["instruction_b"] == 1
    assert result["first_divergence"]["register_differences"][0]["path"] == "r1"
    assert result["output_differences"][0]["path"] == "SV_Position:o0"


def test_instruction_alignment_skips_an_inserted_step_in_one_capture():
    from renderdoc_mcp.shader_debug import compare_debug_traces

    a = _trace(
        steps=[
            _step(0, "0: mov r0, v0", r0=1.0),
            _step(1, "1: add r1, r0, r0", r1=2.0),
        ]
    )
    b = _trace(
        steps=[
            _step(10, "10: mov r0, v0", r0=1.0),
            _step(11, "11: nop"),
            _step(12, "12: add r1, r0, r0", r1=2.0),
        ]
    )

    result = compare_debug_traces(a, b)

    assert result["alignment"]["method"] == "disassembly_sequence"
    assert result["alignment"]["only_in_b_count"] == 1
    assert result["first_divergence"]["category"] == "instruction_only_in_b"
    assert result["first_divergence"]["instruction_b"] == 11


def test_float_tolerance_avoids_noise_but_preserves_real_differences():
    from renderdoc_mcp.shader_debug import compare_debug_traces

    a = _trace(outputs=[{"register": "o0", "value": [1.0, 2.0]}])
    near = _trace(outputs=[{"register": "o0", "value": [1.0000001, 2.0]}])
    far = _trace(outputs=[{"register": "o0", "value": [1.01, 2.0]}])

    assert compare_debug_traces(a, near, abs_tolerance=1e-5)["equivalent"] is True
    assert compare_debug_traces(a, far, abs_tolerance=1e-5)["equivalent"] is False


def test_build_comparable_trace_keeps_steps_and_semantic_outputs():
    from types import SimpleNamespace

    from renderdoc_mcp.shader_debug import build_comparable_debug_trace

    value = SimpleNamespace(f32v=[1.0, 2.0, 3.0, 4.0])
    output = SimpleNamespace(
        name="o0", type="Float", rows=1, columns=4, value=value, members=[]
    )
    change = SimpleNamespace(before=None, after=output)
    state = SimpleNamespace(nextInstruction=1, flags=0, changes=[change])
    line_info = SimpleNamespace(disassemblyLine=1)
    inst_info = SimpleNamespace(instruction=0, lineInfo=line_info)
    trace = SimpleNamespace(
        stage="Vertex", inputs=[], constantBlocks=[], instInfo=[inst_info]
    )
    sig = SimpleNamespace(
        regIndex=0, semanticName="SV_Position", semanticIndex=0
    )
    reflection = SimpleNamespace(outputSignature=[sig])

    result = build_comparable_debug_trace(
        None, reflection, trace, [state], ["0: mov o0, v0"]
    )

    assert result["steps"][0]["instruction"] == 0
    assert result["steps"][0]["changes"] == [
        {"name": "o0", "after": [1.0, 2.0, 3.0, 4.0]}
    ]
    assert result["output_values"] == [
        {
            "register": "o0",
            "semantic": "SV_Position",
            "value": [1.0, 2.0, 3.0, 4.0],
        }
    ]
