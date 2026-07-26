"""Run native replay operations in a disposable subprocess.

RenderDoc replay calls are native and cannot be safely interrupted from a Python
thread.  A timeout around ``run_in_executor`` only abandons the awaiter while
leaving the replay thread, its global lock, and every later MCP call wedged.
Process isolation gives the timeout a real recovery boundary.
"""

from __future__ import annotations

import asyncio
import json
import os
import subprocess
import sys
from typing import Any


DEFAULT_SHADER_DEBUG_TIMEOUT_SECONDS = 30.0
MAX_SHADER_DEBUG_TIMEOUT_SECONDS = 300.0
_WORKER_RESULT_PREFIX = "RENDERDOC_MCP_WORKER_RESULT="
_MAX_DIAGNOSTIC_CHARS = 4000


class IsolatedReplayError(RuntimeError):
    """The isolated replay worker exited without a valid result."""


class IsolatedReplayTimeout(TimeoutError):
    """The isolated replay worker exceeded its deadline and was terminated."""


def clamp_shader_debug_timeout(value: float) -> float:
    return max(1.0, min(float(value), MAX_SHADER_DEBUG_TIMEOUT_SECONDS))


def _worker_creation_flags() -> int:
    if os.name == "nt":
        return int(getattr(subprocess, "CREATE_NO_WINDOW", 0))
    return 0


def _tail(text: str) -> str:
    if len(text) <= _MAX_DIAGNOSTIC_CHARS:
        return text
    return text[-_MAX_DIAGNOSTIC_CHARS:]


def _parse_worker_result(stdout: bytes, stderr: bytes, returncode: int) -> dict[str, Any]:
    stdout_text = stdout.decode("utf-8", errors="replace")
    stderr_text = stderr.decode("utf-8", errors="replace")
    payload: str | None = None
    for line in reversed(stdout_text.splitlines()):
        if line.startswith(_WORKER_RESULT_PREFIX):
            payload = line[len(_WORKER_RESULT_PREFIX):]
            break

    if payload is None:
        detail = _tail(stderr_text.strip() or stdout_text.strip())
        message = "isolated replay worker exited with code {}".format(returncode)
        if detail:
            message += ": " + detail
        raise IsolatedReplayError(message)

    try:
        result = json.loads(payload)
    except json.JSONDecodeError as ex:
        raise IsolatedReplayError("isolated replay worker returned invalid JSON: {}".format(ex)) from ex
    if not isinstance(result, dict):
        raise IsolatedReplayError("isolated replay worker returned a non-object result")
    return result


async def run_shader_debug_worker(
    request: dict[str, Any],
    timeout_seconds: float = DEFAULT_SHADER_DEBUG_TIMEOUT_SECONDS,
) -> dict[str, Any]:
    """Run one shader debug request in a killable, single-capture process."""

    timeout = clamp_shader_debug_timeout(timeout_seconds)
    payload = json.dumps(request, separators=(",", ":")).encode("utf-8")
    env = os.environ.copy()
    env["PYTHONUNBUFFERED"] = "1"
    process = await asyncio.create_subprocess_exec(
        sys.executable,
        "-m",
        "renderdoc_mcp.shader_debug_worker",
        stdin=asyncio.subprocess.PIPE,
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.PIPE,
        env=env,
        creationflags=_worker_creation_flags(),
    )

    try:
        stdout, stderr = await asyncio.wait_for(process.communicate(payload), timeout=timeout)
    except asyncio.TimeoutError as ex:
        process.kill()
        try:
            await process.communicate()
        except Exception:
            pass
        raise IsolatedReplayTimeout(
            "shader debugging exceeded {:.1f}s; the isolated replay worker was terminated".format(
                timeout
            )
        ) from ex
    except asyncio.CancelledError:
        # Client disconnects/cancellation must not orphan a native replay worker.
        process.kill()
        try:
            await asyncio.shield(process.communicate())
        except Exception:
            pass
        raise

    return _parse_worker_result(stdout, stderr, int(process.returncode or 0))
