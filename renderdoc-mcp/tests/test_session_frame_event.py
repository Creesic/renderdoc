from __future__ import annotations


class _Ctrl:
    def __init__(self):
        self.calls = []

    def SetFrameEvent(self, event_id, force):
        self.calls.append((event_id, force))


class _Sess:
    def __init__(self):
        self.controller = _Ctrl()


def test_set_frame_event_defaults_to_no_forced_replay():
    """ReplayController.SetFrameEvent skips the full frame re-replay when the eventId is
    unchanged unless force is set; the MCP session owns its controller exclusively, so
    forcing on every tool call is pure waste (replay_controller.cpp:76)."""
    from renderdoc_mcp.session import CaptureSessionManager

    mgr = CaptureSessionManager()
    sess = _Sess()
    mgr.set_frame_event(sess, 42)
    assert sess.controller.calls == [(42, False)]
