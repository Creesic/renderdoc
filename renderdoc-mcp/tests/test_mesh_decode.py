"""decode_mesh_inputs primitive-restart handling (no RenderDoc import needed)."""

from __future__ import annotations

import struct


class _RdFake:
    class ResourceId:
        @staticmethod
        def Null():
            return "NULL"

    class ActionFlags:
        Indexed = 0x1

    class CompType:
        UInt = "UInt"
        SInt = "SInt"
        Float = "Float"
        UNorm = "UNorm"
        SNorm = "SNorm"
        UScaled = "UScaled"
        SScaled = "SScaled"


class _Fmt:
    def __init__(self):
        self.compType = _RdFake.CompType.Float
        self.compCount = 2
        self.compByteWidth = 4

    def Special(self):
        return False

    def BGRAOrder(self):
        return False


class _Obj:
    def __init__(self, **kw):
        self.__dict__.update(kw)


class _Draw:
    def __init__(self, num_indices):
        self.flags = _RdFake.ActionFlags.Indexed
        self.numIndices = num_indices
        self.baseVertex = 0
        self.indexOffset = 0
        self.vertexOffset = 0

    def GetName(self, sf):
        return "draw"


class _Pipe:
    def GetRestartIndex(self):
        return 0xFFFFFFFF

    def IsRestartEnabled(self):
        return True

    def GetIBuffer(self):
        return _Obj(resourceId="IB", byteOffset=0, byteStride=2)

    def GetVBuffers(self):
        return [_Obj(resourceId="VB", byteOffset=0, byteStride=8)]

    def GetVertexInputs(self):
        return [_Obj(used=True, perInstance=False, vertexBuffer=0, byteOffset=0,
                     format=_Fmt(), name="POS")]


class _Controller:
    def GetPipelineState(self):
        return _Pipe()


def _fake_buffer_data(ib_bytes, vb_bytes):
    def fetch(controller, rid, offset, size):
        data = ib_bytes if rid == "IB" else vb_bytes
        return data[offset:offset + size]
    return fetch


def test_restart_indices_not_decoded_as_vertices(monkeypatch):
    """0xFFFF entries in a 16-bit index buffer with restart enabled are strip markers,
    not vertex indices — they must not fetch attribute data (qrenderdoc omits them)."""
    import renderdoc_mcp.mesh_decode as md

    # index buffer: 0, restart, 1 ; vertex buffer: two float2 vertices
    ib = struct.pack("<3H", 0, 0xFFFF, 1)
    vb = struct.pack("<2f", 1.0, 2.0) + struct.pack("<2f", 3.0, 4.0)

    monkeypatch.setattr(md, "get_renderdoc", lambda: _RdFake())
    monkeypatch.setattr(md, "find_action", lambda controller, eid: _Draw(3))
    monkeypatch.setattr(md, "expand_action_flags", lambda rd, flags: ["Drawcall"])
    monkeypatch.setattr(md, "controller_get_buffer_data", _fake_buffer_data(ib, vb))
    monkeypatch.setattr(md, "resource_name_for", lambda controller, rid: None)
    monkeypatch.setattr(md, "rid_str", lambda rid: str(rid))
    monkeypatch.setattr(md, "enum_name", lambda v: str(v))

    out = md.decode_mesh_inputs(_Controller(), structured_file=None, event_id=42,
                                preview_vertices=3)

    rows = out["vertex_previews"]
    real = [r for r in rows if not r.get("restart")]
    # The two real vertices decode normally
    assert [r["index"] for r in real] == [0, 1]
    # The restart entry is marked, carries no decoded attributes, and 65535 never
    # appears as a vertex index
    assert all(r["index"] != 0xFFFF for r in real)
    restarts = [r for r in rows if r.get("restart")]
    assert len(restarts) == 1
    assert out["vertex_count"] == 2
