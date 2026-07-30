from __future__ import annotations


def test_compact_draw_row_omits_null_slots_defaults_and_unbounded_sizes(monkeypatch):
    from renderdoc_mcp import serialize

    class _Action:
        def GetName(self, structured_file):
            return "Draw"

    class _Pipe:
        def GetPipelineState(self):
            return self

        def GetPrimitiveTopology(self):
            return "TriangleList"

    class _Controller:
        def GetPipelineState(self):
            return _Pipe()

    monkeypatch.setattr(serialize, "get_renderdoc", lambda: object())
    monkeypatch.setattr(serialize, "find_action", lambda controller, event_id: _Action())
    monkeypatch.setattr(serialize, "collect_shader_stages", lambda rd: [])
    monkeypatch.setattr(
        serialize,
        "serialize_vertex_inputs",
        lambda pipe, controller: {
            "vertex_buffers": [
                {
                    "slot": 0,
                    "resource_id": "ResourceId::1",
                    "resource_name": "positions",
                    "byte_offset": 0,
                    "byte_stride": 16,
                    "byte_size": 0xFFFFFFFFFFFFFFFF,
                },
                {
                    "slot": 1,
                    "resource_id": "ResourceId::1",
                    "resource_name": "positions",
                    "byte_offset": 16,
                    "byte_stride": 16,
                    "byte_size": 0xFFFFFFFFFFFFFFFF,
                },
                {
                    "slot": 2,
                    "resource_id": "Null",
                    "byte_offset": 0,
                    "byte_stride": 16,
                    "byte_size": 0xFFFFFFFFFFFFFFFF,
                },
            ],
            "index_buffer": None,
        },
    )
    monkeypatch.setattr(
        serialize,
        "serialize_graphics_targets",
        lambda pipe, controller: {
            "color_targets": [
                {
                    "slot": 0,
                    "resource_id": "ResourceId::2",
                    "slice": 0,
                    "mipslice": 0,
                },
                {"slot": 1, "resource_id": "Null", "slice": 0, "mipslice": 0},
            ],
            "depth_target": {"resource_id": "Null", "slice": 0, "mipslice": 0},
        },
    )

    row = serialize.build_draw_state_row(_Controller(), object(), 7)

    assert row["vertex_buffers"] == [
        {
            "resource_id": "ResourceId::1",
            "resource_name": "positions",
            "byte_stride": 16,
            "slots": [0, 1],
            "byte_offsets": [0, 16],
        }
    ]
    assert row["vertex_buffer_count"] == 2
    assert row["color_targets"] == [{"slot": 0, "resource_id": "ResourceId::2"}]
    assert row["depth_target"] is None


class _FakeAttr:
    def __init__(self, location, vertex_buffer, byte_offset=0, per_instance=False):
        self.location = location
        self.vertexBuffer = vertex_buffer
        self.byteOffset = byte_offset
        self.perInstance = per_instance
        self.format = None


class _FakePipe:
    def __init__(self, attrs):
        self._attrs = attrs

    def GetIBuffer(self):
        return None

    def GetVBuffers(self):
        return []

    def GetVertexInputs(self):
        return self._attrs

    def GetTopology(self):
        return None


class _FakeRasterState:
    fillMode = "Solid"
    cullMode = "Back"
    frontCCW = True
    depthClip = False
    depthBias = 2
    depthBiasClamp = 3.5
    slopeScaledDepthBias = 1.25
    forcedSampleCount = 4
    conservativeRasterization = "Disabled"


class _FakeDepthStencilState:
    depthEnable = True
    depthWrites = False
    depthFunction = "LessEqual"
    depthBoundsEnable = True
    minDepthBounds = 0.25
    maxDepthBounds = 0.75
    stencilEnable = True

    def __init__(self):
        self.frontFace = _FakeStencilFace("AlwaysTrue")
        self.backFace = _FakeStencilFace("Less")


class _FakeStencilFace:
    failOperation = "Keep"
    depthFailOperation = "Zero"
    passOperation = "Replace"
    compareMask = 0x7F
    writeMask = 0x3F
    reference = 5

    def __init__(self, function):
        self.function = function


class _FakeBlendEquation:
    source = "SrcAlpha"
    destination = "One"
    operation = "Add"


class _FakeBlendTarget:
    enabled = True
    logicOperationEnabled = False
    logicOperation = "NoOp"
    writeMask = 15

    def __init__(self):
        self.colorBlend = _FakeBlendEquation()
        self.alphaBlend = _FakeBlendEquation()


class _FakeD3D12Pipe:
    def GetRasterState(self):
        return _FakeRasterState()

    def GetDepthTestState(self):
        return _FakeDepthStencilState()

    def GetColorBlends(self):
        return [_FakeBlendTarget()]

    def GetBlendFactor(self):
        return [0.1, 0.2, 0.3, 0.4]

    def IsIndependentBlendingEnabled(self):
        return True


class _NoStatePipe:
    pass


def test_vertex_inputs_report_real_vertex_buffer_slot():
    """VertexInputAttribute's field is vertexBuffer; attributes from VB1+ must not report slot 0."""
    from renderdoc_mcp.serialize import serialize_vertex_inputs

    pipe = _FakePipe([_FakeAttr(location=0, vertex_buffer=0), _FakeAttr(location=1, vertex_buffer=1)])
    data = serialize_vertex_inputs(pipe, controller=None)

    slots = [a["vertex_buffer_slot"] for a in data["attributes"]]
    assert slots == [0, 1]


def test_d3d12_style_pipe_state_methods_are_serialized():
    from renderdoc_mcp.serialize import (
        serialize_blend_state,
        serialize_depth_state,
        serialize_rasterizer,
        serialize_stencil_state,
    )

    pipe = _FakeD3D12Pipe()

    raster = serialize_rasterizer(pipe)
    assert raster["available"] is True
    assert raster["front_ccw"] is True
    assert raster["depth_bias_clamp"] == 3.5

    depth = serialize_depth_state(pipe)
    assert depth["available"] is True
    assert depth["depth_enable"] is True
    assert depth["depth_writes"] is False
    assert depth["depth_function"] == "LessEqual"

    stencil = serialize_stencil_state(pipe)
    assert stencil["available"] is True
    assert stencil["stencil_enable"] is True
    assert stencil["front_face"]["function"] == "AlwaysTrue"

    blend = serialize_blend_state(pipe)
    assert blend["available"] is True
    assert blend["independent_blend"] is True
    assert blend["blend_factor"] == [0.1, 0.2, 0.3, 0.4]
    assert blend["targets"][0]["blend_enable"] is True
    assert blend["targets"][0]["dst_color"] == "One"


def test_unavailable_pipe_state_is_explicit_not_empty():
    from renderdoc_mcp.serialize import (
        serialize_blend_state,
        serialize_depth_state,
        serialize_rasterizer,
        serialize_stencil_state,
    )

    pipe = _NoStatePipe()
    for state in (
        serialize_rasterizer(pipe),
        serialize_depth_state(pipe),
        serialize_stencil_state(pipe),
        serialize_blend_state(pipe),
    ):
        assert state["available"] is False
        assert state["reason"]
