"""Decode mesh vertex inputs at a draw event (instancing not supported — matches decode_mesh sample)."""

from __future__ import annotations

import csv
import struct
from typing import Any

# Safety ceiling on how many vertices decode_mesh_inputs will fetch/decode in one call, regardless
# of what preview_vertices requests (each vertex does one GetBufferData call per attribute).
MAX_PREVIEW_VERTICES = 8192

# VarType -> byte size / CompType, replicated from renderdoc/api/replay/replay_enums.h's
# VarTypeByteSize()/VarTypeCompType() constexpr functions (304-414) -- these are free functions,
# not virtual interface methods, so there's no confirmed SWIG binding to call into instead.
_VAR_TYPE_BYTE_SIZE = {
    "UByte": 1, "SByte": 1,
    "Half": 2, "UShort": 2, "SShort": 2,
    "Float": 4, "UInt": 4, "SInt": 4, "Bool": 4, "Enum": 4,
    "Double": 8, "ULong": 8, "SLong": 8, "GPUPointer": 8,
}

_VAR_TYPE_COMP_TYPE = {
    "Float": "Float", "Double": "Float", "Half": "Float",
    "UInt": "UInt", "ULong": "UInt", "UShort": "UInt", "UByte": "UInt", "Bool": "UInt",
    "Enum": "UInt", "GPUPointer": "UInt",
    "SInt": "SInt", "SLong": "SInt", "SShort": "SInt", "SByte": "SInt",
}


def var_type_byte_size(var_type_name: str) -> int:
    """Byte size of a VarType; 0 for types with no fixed scalar size (Struct, Unknown, etc.)."""
    return _VAR_TYPE_BYTE_SIZE.get(var_type_name, 0)


def var_type_comp_type(var_type_name: str) -> str:
    """Component type ('Float'/'UInt'/'SInt') of a VarType; 'Typeless' if not one of those."""
    return _VAR_TYPE_COMP_TYPE.get(var_type_name, "Typeless")


def select_gsout_reflection_stage(has_geometry: bool, has_domain: bool) -> str | None:
    """Which shader's reflection describes GSOut data for this draw.

    Only a bound Geometry shader, or a bound Domain (tessellation-eval) shader when tessellation
    is active with no Geometry shader, produces the final pre-rasterization output -- matches the
    real constraint in qrenderdoc's BufferViewer.cpp:5683 ("if geometry/tessellation is enabled,
    only the GS out stage is rasterized output"). Returns None if GSOut doesn't apply to this draw.
    """
    if has_geometry:
        return "geometry"
    if has_domain:
        return "domain"
    return None


def _align_up(value: int, alignment: int) -> int:
    return (value + alignment - 1) // alignment * alignment


def build_output_column_layout(
    sig_params: list[dict[str, Any]], aligned: bool, stream: int = 0
) -> list[dict[str, Any]]:
    """Compute each output semantic's byte offset within one post-VS/GS vertex.

    Replicates qrenderdoc/Windows/BufferViewer.cpp:1665-1759's ConfigureColumnsForShader: filter
    to one output stream, skip the OutputIndices system value, move the POSITION-tagged parameter
    to the front (keeping the rest in original order), then pack fields tightly -- except when
    `aligned` is True (Vulkan VSOut only, via PipeState.HasAlignedPostVSData), where 1-component
    fields align to their own element size, 2-component fields align to a 2x-element boundary,
    and 3-/4-component fields align to a 4x-element boundary.
    """
    columns: list[dict[str, Any]] = []
    for sig in sig_params:
        if int(sig.get("stream", 0)) != stream:
            continue
        if sig.get("system_value") == "OutputIndices":
            continue

        var_type = sig.get("var_type", "Float")
        elem_byte_width = 8 if var_type_byte_size(var_type) > 4 else 4
        columns.append({
            "name": sig.get("name", ""),
            "semantic_name": sig.get("semantic_name", ""),
            "semantic_index": int(sig.get("semantic_index", 0)),
            "system_value": sig.get("system_value", "Undefined"),
            "comp_type": var_type_comp_type(var_type),
            "comp_count": int(sig.get("comp_count", 1)),
            "elem_byte_width": elem_byte_width,
            "byte_offset": 0,
        })

    posidx = next((i for i, c in enumerate(columns) if c["system_value"] == "Position"), -1)
    if posidx > 0:
        columns.insert(0, columns.pop(posidx))

    offset = 0
    for col in columns:
        num_comps = col["comp_count"]
        elem_size = col["elem_byte_width"]
        if aligned:
            if num_comps == 1:
                offset = _align_up(offset, elem_size)
            elif num_comps == 2:
                offset = _align_up(offset, 2 * elem_size)
            elif num_comps > 2:
                offset = _align_up(offset, 4 * elem_size)
        col["byte_offset"] = offset
        offset += num_comps * elem_size

    return columns


def perspective_divide_position(clip_xyzw: list[float]) -> list[float] | None:
    """NDC position via perspective divide -- matches renderdoc/data/hlsl/mesh.hlsl's own
    unprojection (wpos.xyz /= wpos.www) exactly; not a camera/view-matrix reconstruction.
    Returns None if w is zero or fewer than 4 components were given (nothing meaningful to divide).
    """
    if len(clip_xyzw) < 4 or clip_xyzw[3] == 0:
        return None
    w = clip_xyzw[3]
    return [clip_xyzw[0] / w, clip_xyzw[1] / w, clip_xyzw[2] / w]


_SEMANTIC_STRUCT_CHARS = {
    ("Float", 4): "f", ("Float", 8): "d",
    ("UInt", 4): "I", ("UInt", 8): "Q",
    ("SInt", 4): "i", ("SInt", 8): "q",
}


def decode_semantic_bytes(
    data: bytes, offset: int, comp_type: str, comp_count: int, elem_byte_width: int
) -> tuple[Any, ...] | None:
    """Unpack comp_count tightly-packed elem_byte_width-byte values of comp_type starting at
    offset. Post-VS/GS output registers are always plain typed values (never packed/normalized
    vertex-input formats), so no UNorm/SNorm/BGRA handling is needed here -- contrast this
    module's unpack_data(), which does need that for vertex *inputs*.
    """
    char = _SEMANTIC_STRUCT_CHARS.get((comp_type, elem_byte_width))
    if char is None or comp_count <= 0:
        return None
    end = offset + comp_count * elem_byte_width
    if end > len(data):
        return None
    fmt = "=" + str(comp_count) + char
    return struct.unpack_from(fmt, data, offset)


def _write_post_vs_csv(path: str, columns: list[dict[str, Any]], rows: list[dict[str, Any]]) -> None:
    """Write decoded post-VS/GS semantics to CSV, one numeric column per vector component.
    POSITION additionally gets 3 NDC[0..2] columns (blank if not unprojected)."""
    with open(path, "w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        header = ["vertex_index"]
        for col in columns:
            comp_count = max(1, int(col["comp_count"]))
            if comp_count == 1:
                header.append(col["name"])
            else:
                header.extend("{}[{}]".format(col["name"], i) for i in range(comp_count))
            if col["system_value"] == "Position":
                header.extend("{}_ndc[{}]".format(col["name"], i) for i in range(3))
        writer.writerow(header)

        for row in rows:
            csv_row: list[Any] = [row["vertex_index"]]
            for col in columns:
                comp_count = max(1, int(col["comp_count"]))
                value = row["values"].get(col["name"])
                if col["system_value"] == "Position":
                    clip = value.get("clip") if isinstance(value, dict) else None
                    ndc = value.get("ndc") if isinstance(value, dict) else None
                    vals = list(clip) if clip else []
                    vals += [""] * (comp_count - len(vals))
                    csv_row.extend(vals[:comp_count])
                    ndc_vals = (list(ndc) if ndc else []) + ["", "", ""]
                    csv_row.extend(ndc_vals[:3])
                else:
                    vals = list(value) if isinstance(value, (list, tuple)) else []
                    vals += [""] * (comp_count - len(vals))
                    csv_row.extend(vals[:comp_count])
            writer.writerow(csv_row)


def _fetch_postvs_indices(controller: Any, mesh_fmt: Any, fetch_count: int) -> list[int | None]:
    """Resolve up to fetch_count post-VS/GS vertex-buffer slots in primitive order.

    GetPostVSData streams out only the unique vertices referenced by an indexed draw and
    provides a rebased index buffer (mesh_fmt.indexResourceId) mapping primitive order to that
    compact vertex buffer -- baseVertex/indexByteOffset are always 0 for post-VS's own rebased
    buffer (confirmed in GetPostVSBuffers' D3D11/Vulkan implementations), so raw index values are
    used as-is. Non-indexed draws have no separate index buffer; primitive order already equals
    vertex-buffer order directly, so a plain sequential range is returned unchanged.
    A resolved value of None means this row is a primitive-restart marker preserved verbatim in
    the rebased index buffer, not a real vertex -- skip it rather than decoding garbage.
    """
    rd = get_renderdoc()
    if mesh_fmt.indexResourceId == rd.ResourceId.Null():
        return list(range(fetch_count))

    stride = int(mesh_fmt.indexByteStride)
    if stride not in (2, 4):
        return list(range(fetch_count))

    raw = controller_get_buffer_data(
        controller, mesh_fmt.indexResourceId, int(mesh_fmt.indexByteOffset), stride * fetch_count
    )
    fmt_char = "H" if stride == 2 else "I"
    restart = 0xFFFF if stride == 2 else 0xFFFFFFFF
    avail = len(raw) // stride
    n = min(avail, fetch_count)
    values = struct.unpack_from("=" + str(n) + fmt_char, raw, 0) if n else ()
    resolved: list[int | None] = [None if v == restart else int(v) for v in values]
    resolved += [None] * (fetch_count - len(resolved))
    return resolved


def decode_post_vs_outputs(
    controller: Any,
    structured_file: Any,
    event_id: int,
    stage: str = "vsout",
    instance: int = 0,
    view: int = 0,
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
            "note": "Not a draw call; post-VS outputs undefined.",
            "flags": flags,
        }

    stage_key = stage.strip().lower()
    if stage_key not in ("vsout", "gsout"):
        return {"ok": False, "error": "bad_stage",
                "message": "stage must be 'vsout' or 'gsout'", "event_id": event_id}

    pipe = controller.GetPipelineState()

    if stage_key == "vsout":
        refl = pipe.GetShaderReflection(rd.ShaderStage.Vertex)
        mesh_stage = rd.MeshDataStage.VSOut
    else:
        has_geometry = pipe.GetShader(rd.ShaderStage.Geometry) != rd.ResourceId.Null()
        has_domain = pipe.GetShader(rd.ShaderStage.Domain) != rd.ResourceId.Null()
        resolved = select_gsout_reflection_stage(has_geometry, has_domain)
        if resolved is None:
            return {
                "ok": False, "error": "no_geometry_or_tessellation_stage",
                "message": "No Geometry or Domain (tessellation) shader is active for this draw",
                "event_id": event_id,
            }
        gsout_stage = rd.ShaderStage.Geometry if resolved == "geometry" else rd.ShaderStage.Domain
        refl = pipe.GetShaderReflection(gsout_stage)
        mesh_stage = rd.MeshDataStage.GSOut

    if refl is None:
        return {
            "ok": False, "error": "no_shader_reflection",
            "message": "Could not get shader reflection for stage {}".format(stage_key),
            "event_id": event_id,
        }

    mesh_fmt = controller.GetPostVSData(int(instance), int(view), mesh_stage)
    if mesh_fmt.vertexResourceId == rd.ResourceId.Null():
        return {
            "ok": False, "error": "no_post_vs_data",
            "message": "No post-VS data available for this draw (status: {})".format(mesh_fmt.status),
            "event_id": event_id,
        }

    sig_params = []
    for sig in refl.outputSignature:
        sig_params.append({
            "name": sig.varName if sig.varName else sig.semanticIdxName,
            "semantic_name": sig.semanticName,
            "semantic_index": int(sig.semanticIndex),
            "system_value": enum_name(sig.systemValue),
            "var_type": enum_name(sig.varType),
            "comp_count": int(sig.compCount),
            "stream": int(sig.stream),
        })

    aligned = bool(pipe.HasAlignedPostVSData(mesh_stage))
    columns = build_output_column_layout(sig_params, aligned)
    # GetPostVSData's own vertexByteStride is the authoritative per-vertex record size in the
    # physical buffer -- on Vulkan's aligned path the driver pads the *whole* per-vertex struct up
    # to a 16-byte boundary (vk_postvs.cpp's AlignUp16(memberOffset)), which can exceed the sum of
    # the individual decoded field sizes. Using a recomputed sum here instead of this driver value
    # would silently misalign every vertex from index 1 onward.
    vertex_stride = int(mesh_fmt.vertexByteStride)

    fetch_count = min(int(mesh_fmt.numIndices), max(0, int(preview_vertices)), MAX_PREVIEW_VERTICES)

    resolved_indices = _fetch_postvs_indices(controller, mesh_fmt, fetch_count)

    decoded_rows: list[dict[str, Any]] = []
    for vi, idx in enumerate(resolved_indices):
        if idx is None:
            decoded_rows.append({"vertex_index": vi, "index": None, "values": {}, "restart": True})
            continue

        offset = int(mesh_fmt.vertexByteOffset) + vertex_stride * idx
        try:
            raw = controller_get_buffer_data(controller, mesh_fmt.vertexResourceId, offset, vertex_stride)
        except Exception as ex:
            decoded_rows.append({"vertex_index": vi, "index": idx, "values": {}, "error": str(ex)})
            continue

        values: dict[str, Any] = {}
        for col in columns:
            decoded = decode_semantic_bytes(
                raw, col["byte_offset"], col["comp_type"], col["comp_count"], col["elem_byte_width"]
            )
            if decoded is None:
                values[col["name"]] = None
                continue
            if col["system_value"] == "Position":
                entry: dict[str, Any] = {"clip": list(decoded)}
                if mesh_fmt.unproject:
                    ndc = perspective_divide_position(list(decoded))
                    if ndc is not None:
                        entry["ndc"] = ndc
                values[col["name"]] = entry
            else:
                values[col["name"]] = list(decoded)

        decoded_rows.append({"vertex_index": vi, "index": idx, "values": values})

    out: dict[str, Any] = {
        "event_id": event_id,
        "stage": stage_key,
        "draw_name": draw.GetName(structured_file),
        "semantics": [
            {k: v for k, v in col.items() if k != "elem_byte_width"} for col in columns
        ],
        "vertex_count": int(mesh_fmt.numIndices),
        "unproject": bool(mesh_fmt.unproject),
        "flip_y": bool(mesh_fmt.flipY),
        "near_plane": float(mesh_fmt.nearPlane),
        "far_plane": float(mesh_fmt.farPlane),
    }

    if out_file:
        try:
            _write_post_vs_csv(out_file, columns, decoded_rows)
        except OSError as ex:
            return {"ok": False, "error": "out_file_write_failed", "message": str(ex), "event_id": event_id}
        out["out_file"] = out_file
        out["vertex_previews"] = decoded_rows[:8]
        out["vertex_previews_truncated"] = len(decoded_rows) > 8
    else:
        out["vertex_previews"] = decoded_rows

    return out


from renderdoc_mcp.rdutil import (
    controller_get_buffer_data,
    enum_name,
    get_renderdoc,
    resource_name_for,
    rid_str,
)
from renderdoc_mcp.session import expand_action_flags, find_action


class _RestartMarker:
    """Sentinel returned by fetch_indices for primitive-restart index values."""


RESTART_INDEX = _RestartMarker()


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

        return [RESTART_INDEX if restart_enabled and i == restart_idx else i + mesh.baseVertex
                for i in indices] + extra

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
            if idx_val is RESTART_INDEX:
                decoded_rows.append({"vertex_index": vi, "index": None, "restart": True,
                                     "attributes": {}})
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
            **({"restart": True} if decoded_row.get("restart") else {}),
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
        "vertex_count": sum(1 for r in decoded_rows if not r.get("restart")),
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
