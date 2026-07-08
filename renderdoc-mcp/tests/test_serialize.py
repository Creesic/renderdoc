from __future__ import annotations


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


def test_vertex_inputs_report_real_vertex_buffer_slot():
    """VertexInputAttribute's field is vertexBuffer; attributes from VB1+ must not report slot 0."""
    from renderdoc_mcp.serialize import serialize_vertex_inputs

    pipe = _FakePipe([_FakeAttr(location=0, vertex_buffer=0), _FakeAttr(location=1, vertex_buffer=1)])
    data = serialize_vertex_inputs(pipe, controller=None)

    slots = [a["vertex_buffer_slot"] for a in data["attributes"]]
    assert slots == [0, 1]
