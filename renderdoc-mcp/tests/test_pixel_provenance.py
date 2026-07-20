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


def _entry(event_id, **overrides):
    base = {
        "event_id": event_id,
        "primitive_id": 0,
        "shader_depth": 0.0,
        "shader_output": None,
        "depth_test_failed": False,
        "backface_culled": False,
        "clipped": False,
        "stencil_test_failed": False,
        "predicate_failed": False,
        "pre_mod": None,
        "post_mod": None,
    }
    base.update(overrides)
    return base


def test_select_provenance_producer_picks_last_visible_entry():
    from renderdoc_mcp.analysis import select_provenance_producer

    entries = [_entry(10), _entry(20), _entry(30)]

    result = select_provenance_producer(entries)

    assert result is not None
    assert result["event_id"] == 30


def test_select_provenance_producer_skips_trailing_failed_entries():
    from renderdoc_mcp.analysis import select_provenance_producer

    entries = [_entry(10), _entry(20), _entry(30, depth_test_failed=True)]

    result = select_provenance_producer(entries)

    assert result is not None
    assert result["event_id"] == 20


def test_select_provenance_producer_checks_every_failure_reason():
    from renderdoc_mcp.analysis import select_provenance_producer

    for field in (
        "depth_test_failed",
        "backface_culled",
        "clipped",
        "stencil_test_failed",
        "predicate_failed",
    ):
        entries = [_entry(10), _entry(20, **{field: True})]
        result = select_provenance_producer(entries)
        assert result is not None and result["event_id"] == 10, field


def test_select_provenance_producer_returns_none_when_all_entries_failed():
    from renderdoc_mcp.analysis import select_provenance_producer

    entries = [_entry(10, clipped=True), _entry(20, depth_test_failed=True)]

    assert select_provenance_producer(entries) is None


def test_select_provenance_producer_returns_none_for_empty_entries():
    from renderdoc_mcp.analysis import select_provenance_producer

    assert select_provenance_producer([]) is None


def test_mip_dims_at_base_mip_is_unchanged():
    from renderdoc_mcp.analysis import mip_dims

    assert mip_dims(1920, 1080, 0) == (1920, 1080)


def test_mip_dims_halves_per_mip_level():
    from renderdoc_mcp.analysis import mip_dims

    assert mip_dims(1920, 1080, 1) == (960, 540)
    assert mip_dims(1920, 1080, 2) == (480, 270)


def test_mip_dims_floors_at_one_pixel():
    from renderdoc_mcp.analysis import mip_dims

    assert mip_dims(4, 4, 10) == (1, 1)


def test_copy_hop_is_safe_when_dims_match():
    from renderdoc_mcp.analysis import copy_hop_is_safe

    assert copy_hop_is_safe((1920, 1080), (1920, 1080)) is True


def test_copy_hop_is_safe_false_on_width_mismatch():
    from renderdoc_mcp.analysis import copy_hop_is_safe

    assert copy_hop_is_safe((1920, 1080), (960, 1080)) is False


def test_copy_hop_is_safe_false_on_height_mismatch():
    from renderdoc_mcp.analysis import copy_hop_is_safe

    assert copy_hop_is_safe((1920, 1080), (1920, 540)) is False
