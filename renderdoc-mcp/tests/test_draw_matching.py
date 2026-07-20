from __future__ import annotations


def test_draw_shape_key_captures_topology_and_counts():
    from renderdoc_mcp.draw_matching import draw_shape_key

    row = {
        "topology": "TriangleList",
        "vertex_buffers": [{"slot": 0}, {"slot": 1}],
        "index_buffer": {"resource_id": "ResourceId::1"},
        "color_targets": [{"slot": 0}],
        "depth_target": None,
    }
    assert draw_shape_key(row) == ("TriangleList", 2, True, 1, False)


def test_draw_shape_key_handles_missing_fields():
    from renderdoc_mcp.draw_matching import draw_shape_key

    assert draw_shape_key({}) == (None, 0, False, 0, False)


def test_count_closeness_equal_counts_is_one():
    from renderdoc_mcp.draw_matching import count_closeness

    assert count_closeness(100, 100) == 1.0


def test_count_closeness_degrades_with_divergence():
    from renderdoc_mcp.draw_matching import count_closeness

    assert count_closeness(100, 50) == 0.5
    assert count_closeness(0, 0) == 1.0


def test_jaccard_similarity_identical_sets():
    from renderdoc_mcp.draw_matching import jaccard_similarity

    assert jaccard_similarity({"a", "b"}, {"a", "b"}) == 1.0


def test_jaccard_similarity_partial_overlap():
    from renderdoc_mcp.draw_matching import jaccard_similarity

    assert jaccard_similarity({"a", "b"}, {"b", "c"}) == 1.0 / 3.0


def test_jaccard_similarity_both_empty_is_vacuously_one():
    from renderdoc_mcp.draw_matching import jaccard_similarity

    assert jaccard_similarity(set(), set()) == 1.0


def test_jaccard_similarity_disjoint_sets_is_zero():
    from renderdoc_mcp.draw_matching import jaccard_similarity

    assert jaccard_similarity({"a"}, {"b"}) == 0.0
