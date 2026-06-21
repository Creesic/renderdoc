from __future__ import annotations


def _snap(**overrides):
    """Build a minimal pipeline snapshot dict."""
    base = {
        "viewports": [{"width": 1280.0, "height": 720.0, "x": 0.0, "y": 0.0}],
        "scissors": [{"x": 0, "y": 0, "width": 1280, "height": 720, "enabled": True}],
        "depth": {"depth_enable": True, "depth_writes": True, "depth_function": "Less"},
        "blend": {
            "targets": [{
                "slot": 0, "blend_enable": False, "write_mask": 15,
                "src_color": "One", "dst_color": "Zero", "color_op": "Add",
            }]
        },
        "targets": {
            "color_targets": [{"resource_id": "ResourceId::1", "slot": 0}],
            "depth_target": {"resource_id": "ResourceId::2"},
        },
        "action": {"num_vertices": 100, "num_instances": 1, "num_indices": 0, "flags": ["Drawcall"]},
    }
    base.update(overrides)
    return base


def test_no_anomalies_on_clean_snapshot():
    from renderdoc_mcp.analysis import detect_pipeline_anomalies
    assert detect_pipeline_anomalies(_snap()) == []


def test_zero_viewport():
    from renderdoc_mcp.analysis import detect_pipeline_anomalies
    snap = _snap(viewports=[{"width": 0.0, "height": 720.0}])
    assert "zero_viewport" in detect_pipeline_anomalies(snap)


def test_scissor_clips_all():
    from renderdoc_mcp.analysis import detect_pipeline_anomalies
    snap = _snap(scissors=[{"x": 0, "y": 0, "width": 0, "height": 0, "enabled": True}])
    assert "scissor_clips_all" in detect_pipeline_anomalies(snap)


def test_depth_test_disabled_on_draw():
    from renderdoc_mcp.analysis import detect_pipeline_anomalies
    snap = _snap(depth={"depth_enable": False, "depth_writes": False, "depth_function": "Less"})
    assert "depth_test_disabled" in detect_pipeline_anomalies(snap)


def test_depth_write_disabled_when_test_on():
    from renderdoc_mcp.analysis import detect_pipeline_anomalies
    snap = _snap(depth={"depth_enable": True, "depth_writes": False, "depth_function": "Less"})
    assert "depth_write_disabled" in detect_pipeline_anomalies(snap)


def test_no_color_outputs():
    from renderdoc_mcp.analysis import detect_pipeline_anomalies
    snap = _snap(targets={"color_targets": [{"resource_id": "Null", "slot": 0}], "depth_target": None})
    assert "no_color_outputs" in detect_pipeline_anomalies(snap)


def test_additive_blend():
    from renderdoc_mcp.analysis import detect_pipeline_anomalies
    snap = _snap(blend={
        "targets": [{
            "slot": 0, "blend_enable": True, "write_mask": 15,
            "src_color": "SrcAlpha", "dst_color": "One", "color_op": "Add",
        }]
    })
    assert "additive_blend" in detect_pipeline_anomalies(snap)


def test_additive_blend_not_flagged_when_disabled():
    from renderdoc_mcp.analysis import detect_pipeline_anomalies
    snap = _snap(blend={
        "targets": [{
            "slot": 0, "blend_enable": False, "write_mask": 15,
            "src_color": "SrcAlpha", "dst_color": "One", "color_op": "Add",
        }]
    })
    assert "additive_blend" not in detect_pipeline_anomalies(snap)
