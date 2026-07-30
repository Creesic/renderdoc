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


def test_draw_shape_key_uses_slot_count_when_bindings_are_grouped():
    from renderdoc_mcp.draw_matching import draw_shape_key

    row = {
        "topology": "TriangleList",
        "vertex_buffer_count": 4,
        "vertex_buffers": [{"slots": [0, 1, 2, 3]}],
    }
    assert draw_shape_key(row) == ("TriangleList", 4, False, 0, False)


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


def test_bbox_extents_computes_per_axis_range():
    from renderdoc_mcp.draw_matching import bbox_extents

    positions = [[0.0, 0.0, 0.0, 1.0], [2.0, 4.0, 1.0, 1.0], [1.0, 1.0, 0.5, 1.0]]
    assert bbox_extents(positions) == (2.0, 4.0, 1.0)


def test_bbox_extents_ignores_fourth_component():
    from renderdoc_mcp.draw_matching import bbox_extents

    positions = [[0.0, 0.0, 0.0, 999.0], [1.0, 1.0, 1.0, -999.0]]
    assert bbox_extents(positions) == (1.0, 1.0, 1.0)


def test_bbox_extents_none_for_empty_positions():
    from renderdoc_mcp.draw_matching import bbox_extents

    assert bbox_extents([]) is None


def test_bbox_ratio_similarity_identical_shapes_is_one():
    from renderdoc_mcp.draw_matching import bbox_ratio_similarity

    assert bbox_ratio_similarity((2.0, 4.0, 1.0), (2.0, 4.0, 1.0)) == 1.0


def test_bbox_ratio_similarity_scale_invariant():
    """Same proportions at a different absolute scale (cross-API clip-space scale can differ)
    must still score 1.0 -- this is the whole point of normalizing to ratios."""
    from renderdoc_mcp.draw_matching import bbox_ratio_similarity

    assert bbox_ratio_similarity((2.0, 4.0, 1.0), (20.0, 40.0, 10.0)) == 1.0


def test_bbox_ratio_similarity_different_proportions_scores_lower():
    from renderdoc_mcp.draw_matching import bbox_ratio_similarity

    wide_flat = (10.0, 1.0, 1.0)
    tall_thin = (1.0, 10.0, 1.0)
    assert bbox_ratio_similarity(wide_flat, tall_thin) < 0.5


def test_bbox_ratio_similarity_handles_degenerate_zero_extent():
    """A flat quad (zero depth) must not raise a division error."""
    from renderdoc_mcp.draw_matching import bbox_ratio_similarity

    result = bbox_ratio_similarity((2.0, 4.0, 0.0), (2.0, 4.0, 0.0))
    assert result == 1.0


def test_texture_dimension_score_both_absent_is_one():
    from renderdoc_mcp.draw_matching import texture_dimension_score

    assert texture_dimension_score(None, None) == 1.0


def test_texture_dimension_score_one_absent_is_zero():
    from renderdoc_mcp.draw_matching import texture_dimension_score

    assert texture_dimension_score((256, 256), None) == 0.0
    assert texture_dimension_score(None, (256, 256)) == 0.0


def test_texture_dimension_score_matching_dims_is_one():
    from renderdoc_mcp.draw_matching import texture_dimension_score

    assert texture_dimension_score((256, 256), (256, 256)) == 1.0


def test_texture_dimension_score_differing_dims_is_half():
    from renderdoc_mcp.draw_matching import texture_dimension_score

    assert texture_dimension_score((256, 256), (512, 512)) == 0.5


def test_texture_content_score_none_when_either_missing():
    from renderdoc_mcp.draw_matching import texture_content_score

    assert texture_content_score(None, [0.1, 0.2, 0.3, 1.0]) is None
    assert texture_content_score([0.1, 0.2, 0.3, 1.0], None) is None


def test_texture_content_score_identical_means_is_one():
    from renderdoc_mcp.draw_matching import texture_content_score

    assert texture_content_score([0.1, 0.2, 0.3, 1.0], [0.1, 0.2, 0.3, 1.0]) == 1.0


def test_texture_content_score_degrades_with_difference():
    from renderdoc_mcp.draw_matching import texture_content_score

    result = texture_content_score([0.0, 0.0, 0.0, 1.0], [1.0, 1.0, 1.0, 1.0])
    assert result == 0.25  # mean_abs_diff = (1+1+1+0)/4 = 0.75 -> 1 - 0.75


def test_texture_signal_uses_dimension_score_alone_when_content_unavailable():
    from renderdoc_mcp.draw_matching import texture_signal

    assert texture_signal((256, 256), (256, 256), None, None) == 1.0
    assert texture_signal((256, 256), (512, 512), None, None) == 0.5


def test_texture_signal_averages_both_halves_when_available():
    from renderdoc_mcp.draw_matching import texture_signal

    result = texture_signal((256, 256), (256, 256), [0.0, 0.0, 0.0, 1.0], [1.0, 1.0, 1.0, 1.0])
    # dimension half = 1.0, content half = 0.25 (per test above) -> average 0.625
    assert result == 0.625


def test_constants_score_all_shared_values_match():
    from renderdoc_mcp.draw_matching import constants_score

    a = {"scale": 2.0, "offset": [1.0, 2.0, 3.0]}
    b = {"scale": 2.0, "offset": [1.0, 2.0, 3.0]}
    assert constants_score(a, b) == 1.0


def test_constants_score_partial_match():
    from renderdoc_mcp.draw_matching import constants_score

    a = {"scale": 2.0, "offset": 5.0}
    b = {"scale": 2.0, "offset": 999.0}
    assert constants_score(a, b) == 0.5


def test_constants_score_neutral_when_no_shared_names():
    from renderdoc_mcp.draw_matching import constants_score

    assert constants_score({"a": 1.0}, {"b": 2.0}) == 0.5
    assert constants_score({}, {}) == 0.5


def test_constants_score_respects_tolerance():
    from renderdoc_mcp.draw_matching import constants_score

    a = {"x": 1.0}
    b = {"x": 1.0000001}
    assert constants_score(a, b, abs_tolerance=1e-6, rel_tolerance=1e-5) == 1.0

    b_far = {"x": 1.1}
    assert constants_score(a, b_far, abs_tolerance=1e-6, rel_tolerance=1e-5) == 0.0


def test_constants_score_handles_matrix_values():
    from renderdoc_mcp.draw_matching import constants_score

    a = {"mvp": [[1.0, 0.0], [0.0, 1.0]]}
    b = {"mvp": [[1.0, 0.0], [0.0, 1.0]]}
    assert constants_score(a, b) == 1.0

    b_diff = {"mvp": [[1.0, 0.0], [0.0, 2.0]]}
    assert constants_score(a, b_diff) == 0.0


def test_constants_score_only_counts_names_present_in_both():
    from renderdoc_mcp.draw_matching import constants_score

    a = {"shared": 1.0, "only_in_a": 2.0}
    b = {"shared": 1.0, "only_in_b": 3.0}
    assert constants_score(a, b) == 1.0


def test_combine_signals_all_ones_is_one():
    from renderdoc_mcp.draw_matching import combine_signals

    assert combine_signals(1.0, 1.0, 1.0, 1.0) == 1.0


def test_combine_signals_all_zeros_is_zero():
    from renderdoc_mcp.draw_matching import combine_signals

    assert combine_signals(0.0, 0.0, 0.0, 0.0) == 0.0


def test_combine_signals_uses_documented_weights():
    """Locks in the exact weights the spec commits to: geometry=0.4, texture=0.25,
    constants=0.2, shader_shape=0.15."""
    from renderdoc_mcp.draw_matching import combine_signals

    result = combine_signals(geometry=1.0, texture=0.0, constants=0.0, shader_shape=0.0)
    assert result == 0.4

    result = combine_signals(geometry=0.0, texture=1.0, constants=0.0, shader_shape=0.0)
    assert result == 0.25

    result = combine_signals(geometry=0.0, texture=0.0, constants=1.0, shader_shape=0.0)
    assert result == 0.2

    result = combine_signals(geometry=0.0, texture=0.0, constants=0.0, shader_shape=1.0)
    assert result == 0.15
