from __future__ import annotations


def test_classify_producer_action_recognizes_clear():
    from renderdoc_mcp.analysis import classify_producer_action

    assert classify_producer_action(["Clear", "ClearColor"]) == "clear"


def test_classify_producer_action_recognizes_resolve():
    from renderdoc_mcp.analysis import classify_producer_action

    assert classify_producer_action(["Resolve"]) == "resolve"


def test_classify_producer_action_recognizes_copy():
    from renderdoc_mcp.analysis import classify_producer_action

    assert classify_producer_action(["Copy"]) == "copy"


def test_classify_producer_action_recognizes_draw():
    from renderdoc_mcp.analysis import classify_producer_action

    assert classify_producer_action(["Drawcall", "Indexed", "Instanced"]) == "draw"


def test_classify_producer_action_falls_back_to_unsupported():
    from renderdoc_mcp.analysis import classify_producer_action

    assert classify_producer_action(["Dispatch"]) == "unsupported"
    assert classify_producer_action([]) == "unsupported"


def test_classify_producer_action_prioritizes_clear_over_drawcall():
    from renderdoc_mcp.analysis import classify_producer_action

    # A clear-via-draw action carries both flags; provenance should stop at the clear,
    # not treat it as an ordinary shaded draw.
    assert classify_producer_action(["Clear", "Drawcall"]) == "clear"
