"""Register-number → descriptor resolution in shader_debug and server cb selection.

DXBC register numbers (t#/s#/u#/cb#) are bind numbers, NOT positions in
PipeState.GetReadOnlyResources()/etc (access-ordered, filtered) and NOT reflection-array
indices (GetConstantBlock's second argument). Resolution must go register →
reflection index (via fixedBindNumber) → descriptor (via access.index).
"""

from __future__ import annotations


class _Acc:
    def __init__(self, index):
        self.index = index


class _Used:
    def __init__(self, refl_index, tag):
        self.access = _Acc(refl_index)
        self.tag = tag


class _ReflRes:
    def __init__(self, fixed_bind_number):
        self.fixedBindNumber = fixed_bind_number


class _Refl:
    def __init__(self, ro=(), rw=(), samp=(), cbs=()):
        self.readOnlyResources = list(ro)
        self.readWriteResources = list(rw)
        self.samplers = list(samp)
        self.constantBlocks = list(cbs)


class _CbResult:
    def __init__(self, descriptor):
        self.descriptor = descriptor


class _Desc:
    def __init__(self, resource, byte_offset):
        self.resource = resource
        self.byteOffset = byte_offset


class _Pipe:
    def __init__(self, ro=(), rw=(), samp=(), cb_results=None):
        self._ro = list(ro)
        self._rw = list(rw)
        self._samp = list(samp)
        self._cb_results = cb_results or {}
        self.cb_calls = []

    def GetReadOnlyResources(self, stage):
        return self._ro

    def GetReadWriteResources(self, stage):
        return self._rw

    def GetSamplers(self, stage):
        return self._samp

    def GetConstantBlock(self, stage, index, array_idx):
        self.cb_calls.append(index)
        res = self._cb_results.get(index)
        if res is None:
            return _CbResult(None)
        return res


def _patch_serializer(monkeypatch):
    import renderdoc_mcp.shader_debug as sd
    monkeypatch.setattr(sd, "serialize_used_descriptor", lambda used, controller: {"tag": used.tag})


def test_resolve_binding_shader_using_only_t5(monkeypatch):
    """Shader samples only t5: the 1-element descriptor list must still resolve."""
    import renderdoc_mcp.shader_debug as sd
    _patch_serializer(monkeypatch)

    refl = _Refl(ro=[_ReflRes(5)])
    pipe = _Pipe(ro=[_Used(0, "TEX5")])
    out = sd._resolve_binding(pipe, "PS", "srv", 5, None, refl)
    assert out == {"tag": "TEX5"}


def test_resolve_binding_sparse_registers(monkeypatch):
    """t0 and t3 bound: register 3 must resolve to the t3 entry, not lst[3]."""
    import renderdoc_mcp.shader_debug as sd
    _patch_serializer(monkeypatch)

    refl = _Refl(ro=[_ReflRes(0), _ReflRes(3)])
    pipe = _Pipe(ro=[_Used(0, "TEX0"), _Used(1, "TEX3")])
    assert sd._resolve_binding(pipe, "PS", "srv", 3, None, refl) == {"tag": "TEX3"}
    assert sd._resolve_binding(pipe, "PS", "srv", 0, None, refl) == {"tag": "TEX0"}


def test_resolve_binding_unbound_register_returns_none(monkeypatch):
    import renderdoc_mcp.shader_debug as sd
    _patch_serializer(monkeypatch)

    refl = _Refl(ro=[_ReflRes(0)])
    pipe = _Pipe(ro=[_Used(0, "TEX0")])
    assert sd._resolve_binding(pipe, "PS", "srv", 7, None, refl) is None


def test_resolve_cb_sparse_slot(monkeypatch):
    """cbuffers at b0/b2: cb2 maps to reflection index 1, which is what GetConstantBlock takes."""
    import renderdoc_mcp.shader_debug as sd
    monkeypatch.setattr(
        "renderdoc_mcp.rdutil.enrich_resource_dict",
        lambda controller, out, resource: out.update({"resource_id": str(resource)}),
    )

    refl = _Refl(cbs=[_ReflRes(0), _ReflRes(2)])
    pipe = _Pipe(cb_results={1: _CbResult(_Desc("BUF2", 16))})
    out = sd._resolve_cb(pipe, "PS", 2, None, refl)
    assert out is not None
    assert out["byte_offset"] == 16
    assert pipe.cb_calls == [1]


def test_select_constant_block_by_fixed_bind_number():
    """server.read_constant_buffer: slot matches fixedBindNumber; returned index is the
    reflection-array position to pass to GetConstantBlock."""
    from renderdoc_mcp.server import _select_constant_block

    blocks = [_ReflRes(0), _ReflRes(2)]
    block, index = _select_constant_block(blocks, 2)
    assert block is blocks[1]
    assert index == 1


def test_select_constant_block_positional_fallback():
    from renderdoc_mcp.server import _select_constant_block

    class _NoFbn:
        pass

    blocks = [_NoFbn(), _NoFbn()]
    block, index = _select_constant_block(blocks, 1)
    assert block is blocks[1]
    assert index == 1

    block, index = _select_constant_block(blocks, 9)
    assert block is None
    assert index == -1
