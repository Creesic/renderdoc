"""Decode mesh vertex inputs at a draw event (instancing not supported — matches decode_mesh sample)."""

from __future__ import annotations

import csv
import struct
from typing import Any

# Safety ceiling on how many vertices decode_mesh_inputs will fetch/decode in one call, regardless
# of what preview_vertices requests (each vertex does one GetBufferData call per attribute).
MAX_PREVIEW_VERTICES = 8192

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


def _write_vertex_csv(path: str, layouts: list[dict[str, Any]], rows: list[dict[str, Any]]) -> None:
    """Write decoded vertex attributes to CSV, one numeric column per vector component."""
    with open(path, "w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        header = ["vertex_index", "index"]
        for layout in layouts:
            comp_count = max(1, int(layout["format"]["comp_count"]))
            if comp_count == 1:
                header.append(layout["name"])
            else:
                header.extend("{}[{}]".format(layout["name"], i) for i in range(comp_count))
        writer.writerow(header)

        for row in rows:
            csv_row: list[Any] = [row["vertex_index"], row["index"]]
            for layout in layouts:
                comp_count = max(1, int(layout["format"]["comp_count"]))
                value = row["attributes"].get(layout["name"])
                if value is None:
                    csv_row.extend([""] * comp_count)
                    continue
                vals = list(value) if isinstance(value, tuple) else [value]
                vals += [""] * (comp_count - len(vals))
                csv_row.extend(vals[:comp_count])
            writer.writerow(csv_row)


def decode_mesh_inputs(
    controller: Any,
    structured_file: Any,
    event_id: int,
    preview_vertices: int = 8,
    out_file: str | None = None,
) -> dict[str, Any]:
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

    # Fetch exactly as many indices as were actually requested (capped for safety), instead of a
    # fixed 256 disconnected from preview_vertices — previously requesting more than 256 previews
    # silently got no more than 256 with no indication why.
    fetch_count = min(int(draw.numIndices), max(0, int(preview_vertices)), MAX_PREVIEW_VERTICES)

    decoded_rows: list[dict[str, Any]] = []
    if meshes and fetch_count:
        m0 = meshes[0]
        try:
            indices = fetch_indices(controller, draw, m0, m0.indexOffset, 0, fetch_count)
        except Exception as ex:
            indices = []
            index_summary["index_decode_error"] = str(ex)

        for vi in range(len(indices)):
            idx_val = indices[vi]
            if idx_val is None:
                continue
            try:
                idx_int = int(idx_val)
            except Exception:
                continue
            decoded_row: dict[str, Any] = {"vertex_index": vi, "index": idx_int, "attributes": {}}
            for attr_mesh in meshes:
                offset = attr_mesh.vertexByteOffset + attr_mesh.vertexByteStride * idx_int
                try:
                    raw = controller_get_buffer_data(
                        controller, attr_mesh.vertexResourceId, offset, attr_mesh.vertexByteStride
                    )
                    decoded_row["attributes"][attr_mesh.name] = unpack_data(attr_mesh.format, raw, 0)
                except Exception as ex:
                    decoded_row["attributes"][attr_mesh.name] = None
                    decoded_row.setdefault("errors", {})[attr_mesh.name] = str(ex)
            decoded_rows.append(decoded_row)

    def _to_preview(decoded_row: dict[str, Any]) -> dict[str, Any]:
        preview: dict[str, Any] = {
            "vertex_index": decoded_row["vertex_index"],
            "index": decoded_row["index"],
            "attributes": {
                name: ("<error: {}>".format(decoded_row.get("errors", {}).get(name)) if value is None
                       and name in decoded_row.get("errors", {}) else repr(value))
                for name, value in decoded_row["attributes"].items()
            },
        }
        return preview

    out: dict[str, Any] = {
        "event_id": event_id,
        "draw_name": draw.GetName(structured_file),
        "index": index_summary,
        "vertex_attributes": layouts,
        "vertex_count": len(decoded_rows),
    }

    if out_file:
        try:
            _write_vertex_csv(out_file, layouts, decoded_rows)
        except OSError as ex:
            return {"ok": False, "error": "out_file_write_failed", "message": str(ex), "event_id": event_id}
        out["out_file"] = out_file
        out["vertex_previews"] = [_to_preview(r) for r in decoded_rows[:8]]
        out["vertex_previews_truncated"] = len(decoded_rows) > 8
    else:
        out["vertex_previews"] = [_to_preview(r) for r in decoded_rows]

    return out
