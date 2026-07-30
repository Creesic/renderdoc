from __future__ import annotations

import asyncio
import json

from renderdoc_mcp import isolated_replay


class _FakeProcess:
    def __init__(self, *, stdout: bytes = b"", stderr: bytes = b"", returncode: int = 0):
        self.stdout = stdout
        self.stderr = stderr
        self.returncode = returncode
        self.killed = False
        self.communicate_calls = 0

    async def communicate(self, payload=None):
        self.communicate_calls += 1
        return self.stdout, self.stderr

    def kill(self):
        self.killed = True


def test_worker_result_round_trips(monkeypatch):
    expected = {"ok": True, "data": {"step_count": 5}}
    stdout = (
        "native diagnostic\n"
        + isolated_replay._WORKER_RESULT_PREFIX
        + json.dumps(expected)
        + "\n"
    ).encode()
    process = _FakeProcess(stdout=stdout)

    async def create(*args, **kwargs):
        return process

    monkeypatch.setattr(asyncio, "create_subprocess_exec", create)

    result = asyncio.run(
        isolated_replay.run_shader_debug_worker({"operation": "debug_pixel"}, 2.0)
    )

    assert result == expected
    assert process.killed is False


def test_draw_matching_uses_the_same_killable_worker_boundary(monkeypatch):
    expected = {"ok": True, "data": {"candidates": [{"event_id": 7}]}}
    stdout = (
        isolated_replay._WORKER_RESULT_PREFIX + json.dumps(expected) + "\n"
    ).encode()
    process = _FakeProcess(stdout=stdout)
    command = []

    async def create(*args, **kwargs):
        command.extend(args)
        return process

    monkeypatch.setattr(asyncio, "create_subprocess_exec", create)

    result = asyncio.run(
        isolated_replay.run_draw_matching_worker(
            {"operation": "find_corresponding_draws"}, 2.0
        )
    )

    assert result == expected
    assert "renderdoc_mcp.shader_debug_worker" in command
    assert process.killed is False


def test_timeout_kills_worker_and_returns_control(monkeypatch):
    class HangingProcess(_FakeProcess):
        async def communicate(self, payload=None):
            self.communicate_calls += 1
            if self.killed:
                return b"", b""
            await asyncio.Future()

    process = HangingProcess()

    async def create(*args, **kwargs):
        return process

    async def immediate_timeout(awaitable, timeout):
        awaitable.close()
        raise asyncio.TimeoutError

    monkeypatch.setattr(asyncio, "create_subprocess_exec", create)
    monkeypatch.setattr(asyncio, "wait_for", immediate_timeout)

    async def run():
        try:
            await isolated_replay.run_shader_debug_worker({"operation": "debug_pixel"}, 2.0)
        except isolated_replay.IsolatedReplayTimeout:
            return
        raise AssertionError("expected IsolatedReplayTimeout")

    asyncio.run(run())

    assert process.killed is True
    assert process.communicate_calls == 1


def test_cancellation_does_not_orphan_worker(monkeypatch):
    class HangingProcess(_FakeProcess):
        async def communicate(self, payload=None):
            self.communicate_calls += 1
            if self.killed:
                return b"", b""
            await asyncio.Future()

    process = HangingProcess()

    async def create(*args, **kwargs):
        return process

    monkeypatch.setattr(asyncio, "create_subprocess_exec", create)

    async def run():
        task = asyncio.create_task(
            isolated_replay.run_shader_debug_worker({"operation": "debug_pixel"}, 2.0)
        )
        await asyncio.sleep(0)
        task.cancel()
        try:
            await task
        except asyncio.CancelledError:
            pass

    asyncio.run(run())

    assert process.killed is True
    assert process.communicate_calls == 2


def test_timeout_is_bounded():
    assert isolated_replay.clamp_shader_debug_timeout(-1) == 1.0
    assert isolated_replay.clamp_shader_debug_timeout(12.5) == 12.5
    assert (
        isolated_replay.clamp_shader_debug_timeout(999)
        == isolated_replay.MAX_SHADER_DEBUG_TIMEOUT_SECONDS
    )
