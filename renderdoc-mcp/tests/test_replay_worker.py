from __future__ import annotations

from renderdoc_mcp import shader_debug_worker


class _ReplayModule:
    class _Environment:
        pass

    def __init__(self):
        self.initialised = False
        self.shutdown = False

    def GlobalEnvironment(self):
        return self._Environment()

    def InitialiseReplay(self, environment, args):
        self.initialised = True

    def ShutdownReplay(self):
        self.shutdown = True


class _Controller:
    def __init__(self, name):
        self.name = name
        self.shutdown = False

    def GetStructuredFile(self):
        return "structured-" + self.name

    def Shutdown(self):
        self.shutdown = True


def test_draw_matching_worker_opens_both_captures_and_cleans_up(monkeypatch):
    rd = _ReplayModule()
    controllers = {
        "a.rdc": _Controller("a"),
        "b.rdc": _Controller("b"),
    }
    received = {}

    monkeypatch.setattr(shader_debug_worker.rdutil, "get_renderdoc", lambda: rd)
    monkeypatch.setattr(
        shader_debug_worker,
        "_open_controller",
        lambda _rd, path: controllers[path],
    )

    def match(*args):
        received["args"] = args
        return {"reference": {"event_id": 11}, "candidates": []}

    monkeypatch.setattr(shader_debug_worker, "find_corresponding_draws", match)

    result = shader_debug_worker.run_request(
        {
            "operation": "find_corresponding_draws",
            "capture_path_a": "a.rdc",
            "event_id_a": 11,
            "capture_path_b": "b.rdc",
            "event_ids_b": [22, 33],
            "limit": 20,
            "top_k": 3,
        }
    )

    assert result["ok"] is True
    assert result["data"]["reference"]["event_id"] == 11
    assert received["args"] == (
        controllers["a.rdc"],
        "structured-a",
        11,
        controllers["b.rdc"],
        "structured-b",
        [22, 33],
        20,
        3,
    )
    assert controllers["a.rdc"].shutdown is True
    assert controllers["b.rdc"].shutdown is True
    assert rd.initialised is True
    assert rd.shutdown is True
