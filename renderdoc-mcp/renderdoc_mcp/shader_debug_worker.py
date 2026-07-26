"""Disposable process entry point for pixel and vertex shader debugging."""

from __future__ import annotations

import json
import os
import sys
import tempfile
from typing import Any

from renderdoc_mcp import rdutil
from renderdoc_mcp import responses as R
from renderdoc_mcp import shader_debug
from renderdoc_mcp.isolated_replay import _WORKER_RESULT_PREFIX


def _open_controller(rd: Any, path: str) -> Any:
    capture = rd.OpenCaptureFile()
    result = capture.OpenFile(path, "", None)
    if result != rd.ResultCode.Succeeded:
        capture.Shutdown()
        raise RuntimeError("OpenFile failed: {} ({})".format(path, result))
    if not capture.LocalReplaySupport():
        capture.Shutdown()
        raise RuntimeError("Capture cannot be replayed locally")

    result, controller = capture.OpenCapture(rd.ReplayOptions(), None)
    capture.Shutdown()
    if result != rd.ResultCode.Succeeded:
        try:
            controller.Shutdown()
        except Exception:
            pass
        raise RuntimeError("OpenCapture failed: {}".format(result))
    return controller


def _trace_summary(
    rd: Any,
    controller: Any,
    pipe: Any,
    stage: Any,
    reflection: Any,
    trace: Any,
    *,
    full_trace: bool,
    full_trace_path: str | None,
) -> dict[str, Any]:
    states, truncated = shader_debug.run_debug_trace(controller, trace)
    disasm_lines: list[str] = []
    try:
        pipe_obj = pipe.GetGraphicsPipelineObject()
        disasm_text = shader_debug.best_disassembly(controller, pipe_obj, reflection)
        disasm_lines = disasm_text.split("\n") if disasm_text else []
    except Exception:
        disasm_lines = []

    out = shader_debug.summarize_debug_trace(
        rd, controller, pipe, stage, reflection, trace, states, disasm_lines
    )
    if full_trace:
        path = full_trace_path
        if not path:
            fd, path = tempfile.mkstemp(suffix=".json", prefix="renderdoc_mcp_trace_")
            os.close(fd)
        shader_debug.dump_full_trace(rd, trace, states, disasm_lines, path)
        out["full_trace_path"] = path
    if truncated:
        out["truncated"] = True
        out["truncated_reason"] = (
            "Trace exceeded {} steps (likely a long-running or infinite shader loop); "
            "stopped early. Results reflect only the first {} steps.".format(
                shader_debug.MAX_DEBUG_STEPS, shader_debug.MAX_DEBUG_STEPS
            )
        )
    return out


def run_request(request: dict[str, Any]) -> dict[str, Any]:
    rd = rdutil.get_renderdoc()
    rd.InitialiseReplay(rd.GlobalEnvironment(), [])
    controller = None
    trace = None
    try:
        controller = _open_controller(rd, str(request["capture_path"]))
        event_id = int(request["event_id"])
        controller.SetFrameEvent(event_id, False)
        pipe = controller.GetPipelineState()
        operation = str(request.get("operation", ""))

        if operation == "debug_pixel":
            stage = rd.ShaderStage.Pixel
            reflection = pipe.GetShaderReflection(stage)
            if reflection is None:
                return R.err("no_pixel_shader", "No pixel/fragment shader bound at this event")
            inputs = rd.DebugPixelInputs()
            inputs.sample = int(request.get("sample", 0))
            primitive = request.get("primitive")
            inputs.primitive = (
                shader_debug.NO_PREFERENCE if primitive is None else int(primitive)
            )
            view = request.get("view")
            inputs.view = shader_debug.NO_PREFERENCE if view is None else int(view)
            trace = controller.DebugPixel(int(request["x"]), int(request["y"]), inputs)
            if trace is None or trace.debugger is None:
                return R.err(
                    "debug_failed",
                    "No fragment writes pixel ({}, {}) at this event, or debugging is unsupported here".format(
                        request["x"], request["y"]
                    ),
                )
        elif operation == "debug_vertex":
            stage = rd.ShaderStage.Vertex
            reflection = pipe.GetShaderReflection(stage)
            if reflection is None:
                return R.err("no_vertex_shader", "No vertex shader bound at this event")
            vertex_id = int(request["vertex_id"])
            index = request.get("index")
            index = vertex_id if index is None else int(index)
            trace = controller.DebugVertex(
                vertex_id,
                int(request.get("instance_id", 0)),
                index,
                int(request.get("view", 0)),
            )
            if trace is None or trace.debugger is None:
                return R.err(
                    "debug_failed",
                    "Could not debug vertex {} (instance {}, index {})".format(
                        vertex_id, request.get("instance_id", 0), index
                    ),
                )
        else:
            return R.err("bad_worker_operation", operation)

        out = _trace_summary(
            rd,
            controller,
            pipe,
            stage,
            reflection,
            trace,
            full_trace=bool(request.get("full_trace", False)),
            full_trace_path=request.get("full_trace_path"),
        )
        out["capture_id"] = str(request["capture_id"])
        out["event_id"] = event_id
        out["stage"] = rdutil.enum_name(stage)
        if operation == "debug_pixel":
            out["coordinates"] = {"x": int(request["x"]), "y": int(request["y"])}
        else:
            vertex_id = int(request["vertex_id"])
            index = request.get("index")
            out["vertex_id"] = vertex_id
            out["instance_id"] = int(request.get("instance_id", 0))
            out["index"] = vertex_id if index is None else int(index)
        return R.ok(out)
    except Exception as ex:
        operation = str(request.get("operation", "shader_debug"))
        return R.err("{}_failed".format(operation), str(ex))
    finally:
        if trace is not None and controller is not None:
            try:
                controller.FreeTrace(trace)
            except Exception:
                pass
        if controller is not None:
            try:
                controller.Shutdown()
            except Exception:
                pass
        try:
            rd.ShutdownReplay()
        except Exception:
            pass


def main() -> None:
    try:
        request = json.loads(sys.stdin.buffer.read().decode("utf-8"))
        if not isinstance(request, dict):
            raise ValueError("worker request must be a JSON object")
        result = run_request(request)
    except Exception as ex:
        result = R.err("shader_debug_worker_failed", str(ex))
    print(
        _WORKER_RESULT_PREFIX + json.dumps(result, separators=(",", ":"), ensure_ascii=False),
        flush=True,
    )


if __name__ == "__main__":
    main()
