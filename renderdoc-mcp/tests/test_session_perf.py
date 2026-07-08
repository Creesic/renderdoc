"""Session indexing hot paths: flag expansion and action lookup must not re-scan per call.

open_capture visits every action (100k+ events in real captures) and tools like
list_draws_with_state call find_action per draw, so per-call dir() scans and full
tree walks are O(n^2) in practice.
"""

from __future__ import annotations


class _CountingFlags:
    """ActionFlags stand-in whose dir() enumeration is counted."""

    dir_calls = 0
    Clear = 0x1
    Drawcall = 0x2
    Indexed = 0x4

    def __dir__(self):
        type(self).dir_calls += 1
        return ["Clear", "Drawcall", "Indexed"]


class _RdFake:
    def __init__(self):
        self.ActionFlags = _CountingFlags()


class _Action:
    def __init__(self, eid, children=()):
        self.eventId = eid
        self.children = list(children)


class _Controller:
    def __init__(self, roots):
        self._roots = roots
        self.root_calls = 0

    def GetRootActions(self):
        self.root_calls += 1
        return self._roots


def test_expand_action_flags_enumerates_flag_table_once():
    from renderdoc_mcp.session import expand_action_flags

    rd = _RdFake()
    before = _CountingFlags.dir_calls
    assert expand_action_flags(rd, 0x3) == ["Clear", "Drawcall"]
    assert expand_action_flags(rd, 0x6) == ["Drawcall", "Indexed"]
    assert expand_action_flags(rd, 0x3) == ["Clear", "Drawcall"]
    assert _CountingFlags.dir_calls - before == 1


def test_find_action_walks_tree_once_per_controller():
    from renderdoc_mcp.session import find_action

    roots = [_Action(1, [_Action(2), _Action(3)]), _Action(4)]
    ctrl = _Controller(roots)

    assert find_action(ctrl, 3).eventId == 3
    assert find_action(ctrl, 4).eventId == 4
    assert find_action(ctrl, 999) is None
    assert ctrl.root_calls == 1
