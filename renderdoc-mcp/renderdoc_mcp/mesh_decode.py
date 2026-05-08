"""Decode mesh vertex inputs at a draw event (instancing not supported — matches decode_mesh sample)."""

from __future__ import annotations

import struct
from typing import Any

from renderdoc_mcp.rdutil import (
    controller_get_buffer_data,
    enum_name,
    get_renderdoc,
    resource_name_for,
    rid_str,
)
from renderdoc_mcp.session import expand_action_flags, find_action


class MeshData:
    """Mesh fields compatible with fetch_indices (decode_mesh sample)."""

    def __init__(self) -> None:
        rd = get_renderdoc()
        self.indexResourceId = rd.ResourceId.Null()
        self.indexByteOffset = 0
        self.indexByteStride = 0
        self.baseVertex = 0
        self.indexOffset = 0
        self.numIndices = 0
        self.vertexByteOffset = 0
        self.vertexByteStride = 0
        self.vertexResourceId = rd.ResourceId.Null()
        self.format = None
        self.name = ""


def fetch_indices(controller: Any, action: Any, mesh: MeshData, index_offset: int, first_index: int, num_indices: int):
    rd = get_renderdoc()
    pipe = controller.GetPipelineState()
    restart_idx = pipe.GetRestartIndex() & ((1 << (mesh.indexByteStride * 8)) - 1)
    restart_enabled = pipe.IsRestartEnabled()

    if mesh.indexResourceId != rd.ResourceId.Null():
        offset = mesh.indexByteStride * (first_index + index_offset)
        read_bytes = mesh.indexByteStride * num_indices
        ibdata = controller_get_buffer_data(
            controller, mesh.indexResourceId, mesh.indexByteOffset + offset, read_bytes
        )

        index_fmt = "B"
        if mesh.indexByteStride == 2:
            index_fmt = "H"
        elif mesh.indexByteStride == 4:
            index_fmt = "I"

        avail_indices = int(len(ibdata) / mesh.indexByteStride) if mesh.indexByteStride else 0

        if avail_indices <= 0:
            indices = ()
        else:
            index_fmt = "=" + str(min(avail_indices, num_indices)) + index_fmt
            indices = struct.unpack_from(index_fmt, ibdata)

        extra = []
        if avail_indices < num_indices:
            extra = [None] * (num_indices - avail_indices)

        return [i if restart_enabled and i == restart_idx else i + mesh.baseVertex for i in indices] + extra

    return tuple(range(first_index, first_index + num_indices))


def unpack_data(fmt: Any, data: bytes, data_offset: int = 0):
    rd = get_renderdoc()
    special = getattr(fmt, "Special", None)
    if callable(special):
        try:
            is_special = bool(special())
        except TypeError:
            is_special = False
    else:
        is_special = bool(special)
    if is_special:
        raise RuntimeError("Packed formats are not supported")

    format_chars = {
        rd.CompType.UInt: "xBHxIxxxQ",
        rd.CompType.SInt: "xbhxixxxq",
        rd.CompType.Float: "xxexfxxxd",
    }
    format_chars[rd.CompType.UNorm] = format_chars[rd.CompType.UInt]
    format_chars[rd.CompType.UScaled] = format_chars[rd.CompType.UInt]
    format_chars[rd.CompType.SNorm] = format_chars[rd.CompType.SInt]
    format_chars[rd.CompType.SScaled] = format_chars[rd.CompType.SInt]

    vertex_format = "=" + str(fmt.compCount) + format_chars[fmt.compType][fmt.compByteWidth]

    if data_offset >= len(data):
        return None

    value = struct.unpack_from(vertex_format, data, data_offset)

    if fmt.compType == rd.CompType.UNorm:
        divisor = float((2 ** (fmt.compByteWidth * 8)) - 1)
        value = tuple(float(i) / divisor for i in value)
    elif fmt.compType == rd.CompType.SNorm:
        max_neg = -float(2 ** (fmt.compByteWidth * 8)) / 2
        divisor = float(-(max_neg - 1))
        value = tuple((float(i) if (i == max_neg) else (float(i) / divisor)) for i in value)

    if fmt.BGRAOrder():
        value = tuple(value[i] for i in [2, 1, 0, 3])

    return value


def get_mesh_inputs(controller: Any, draw: Any) -> list[MeshData]:
    rd = get_renderdoc()
    state = controller.GetPipelineState()
    ib = state.GetIBuffer()
    vbs = state.GetVBuffers()
    attrs = state.GetVertexInputs()

    mesh_inputs: list[MeshData] = []

    for attr in attrs:
        if not getattr(attr, "used", True):
            continue
        if attr.perInstance:
            raise RuntimeError("Instanced draws are not supported by decode_mesh_inputs")

        mesh_input = MeshData()
        mesh_input.indexResourceId = ib.resourceId
        mesh_input.indexByteOffset = ib.byteOffset
        mesh_input.indexByteStride = ib.byteStride
        mesh_input.baseVertex = draw.baseVertex
        mesh_input.indexOffset = draw.indexOffset
        mesh_input.numIndices = draw.numIndices

        if not (draw.flags & rd.ActionFlags.Indexed):
            mesh_input.indexResourceId = rd.ResourceId.Null()

        vb = vbs[attr.vertexBuffer]
        mesh_input.vertexByteOffset = attr.byteOffset + vb.byteOffset + draw.vertexOffset * vb.byteStride
        mesh_input.format = attr.format
        mesh_input.vertexResourceId = vb.resourceId
        mesh_input.vertexByteStride = vb.byteStride
        mesh_input.name = attr.name

        mesh_inputs.append(mesh_input)

    return mesh_inputs


def decode_mesh_inputs(controller: Any, structured_file: Any, event_id: int, preview_vertices: int = 8) -> dict[str, Any]:
    rd = get_renderdoc()
    draw = find_action(controller, event_id)
    if draw is None:
        return {"error": "event_not_found", "event_id": event_id}

    flags = expand_action_flags(rd, int(draw.flags))
    if "Drawcall" not in flags:
        return {
            "event_id": event_id,
            "note": "Not a draw call; mesh inputs undefined.",
            "flags": flags,
        }

    try:
        meshes = get_mesh_inputs(controller, draw)
    except RuntimeError as e:
        return {"ok": False, "error": str(e), "event_id": event_id}

    layouts = []
    for m in meshes:
        row = {
            "name": m.name,
            "vertex_buffer": rid_str(m.vertexResourceId),
            "vertex_stride": int(m.vertexByteStride),
            "vertex_byte_offset": int(m.vertexByteOffset),
            "format": {
                "comp_type": enum_name(m.format.compType),
                "comp_count": int(m.format.compCount),
                "comp_byte_width": int(m.format.compByteWidth),
            },
        }
        vbn = resource_name_for(controller, m.vertexResourceId)
        if vbn:
            row["vertex_buffer_name"] = vbn
        layouts.append(row)

    index_summary = {
        "indexed": bool(draw.flags & rd.ActionFlags.Indexed),
        "index_buffer": rid_str(meshes[0].indexResourceId) if meshes else "Null",
        "index_stride": int(meshes[0].indexByteStride) if meshes else 0,
        "num_indices": int(draw.numIndices),
    }
    if meshes:
        ibn = resource_name_for(controller, meshes[0].indexResourceId)
        if ibn:
            index_summary["index_buffer_name"] = ibn

    previews = []
    if meshes:
        m0 = meshes[0]
        try:
            indices = fetch_indices(controller, draw, m0, m0.indexOffset, 0, min(int(draw.numIndices), 256))
        except Exception as ex:
            indices = []
            index_summary["index_decode_error"] = str(ex)

        for vi in range(min(preview_vertices, len(indices))):
            idx_val = indices[vi]
            if idx_val is None:
                continue
            vertex_sample: dict[str, Any] = {"vertex_index": vi, "index": int(idx_val), "attributes": {}}
            try:
                idx_int = int(idx_val)
            except Exception:
                continue
            for attr_mesh in meshes:
                offset = attr_mesh.vertexByteOffset + attr_mesh.vertexByteStride * idx_int
                try:
                    raw = controller_get_buffer_data(
                        controller, attr_mesh.vertexResourceId, offset, attr_mesh.vertexByteStride
                    )
                    vertex_sample["attributes"][attr_mesh.name] = repr(unpack_data(attr_mesh.format, raw, 0))
                except Exception as ex:
                    vertex_sample["attributes"][attr_mesh.name] = "<error: {}>".format(ex)
            previews.append(vertex_sample)

    return {
        "event_id": event_id,
        "draw_name": draw.GetName(structured_file),
        "index": index_summary,
        "vertex_attributes": layouts,
        "vertex_previews": previews,
    }
