"""Serialize RenderDoc's SDFile/SDChunk/SDObject structured-data tree to JSON-safe dicts.

Only a small slice of SDObject's C++ API is exposed to Python: ``AsBool``/``AsFloat``/``AsInt``/
``AsResourceId``/``AsString`` and no ``IsXxx()`` type-check helpers at all -- confirmed directly
against the built pymodules rather than assumed from the C++ header, since the SWIG-exposed surface
is narrower than what the header's ``#if !defined(SWIG)`` guards would suggest (most of the
convenience accessors, e.g. ``AsInt64``/``AsUInt64``/``IsString``/``IsArray``, are C++-only). Object
kind is determined via ``type.basetype`` (an ``SDBasic`` enum value) instead.
"""

from __future__ import annotations

from typing import Any

from renderdoc_mcp import rdutil

DEFAULT_MAX_DEPTH = 12
DEFAULT_MAX_CHILDREN = 256


def serialize_sdobject(
    rd: Any,
    obj: Any,
    controller: Any | None = None,
    max_depth: int = DEFAULT_MAX_DEPTH,
    max_children: int = DEFAULT_MAX_CHILDREN,
    _depth: int = 0,
) -> dict[str, Any]:
    """Convert a single SDObject (a chunk parameter, or any nested struct/array/value) to a dict."""
    basetype = obj.type.basetype
    out: dict[str, Any] = {"name": str(obj.name), "type_name": str(obj.type.name)}

    has_custom_string = bool(int(obj.type.flags) & int(rd.SDTypeFlags.HasCustomString))

    if _depth >= max_depth:
        out["kind"] = "truncated"
        out["truncated"] = "max_depth"
        return out

    if basetype in (rd.SDBasic.Chunk, rd.SDBasic.Struct):
        n = obj.NumChildren()
        out["kind"] = "struct"
        out["fields"] = [
            serialize_sdobject(rd, obj.GetChild(i), controller, max_depth, max_children, _depth + 1)
            for i in range(min(n, max_children))
        ]
        if n > max_children:
            out["truncated"] = "max_children"
    elif basetype == rd.SDBasic.Array:
        n = obj.NumChildren()
        out["kind"] = "array"
        out["count"] = int(n)
        out["items"] = [
            serialize_sdobject(rd, obj.GetChild(i), controller, max_depth, max_children, _depth + 1)
            for i in range(min(n, max_children))
        ]
        if n > max_children:
            out["truncated"] = "max_children"
    elif basetype == rd.SDBasic.Null:
        out["kind"] = "null"
        out["value"] = None
    elif basetype == rd.SDBasic.Buffer:
        out["kind"] = "buffer"
        out["value"] = None
    elif basetype == rd.SDBasic.String:
        out["kind"] = "string"
        out["value"] = str(obj.AsString())
    elif basetype == rd.SDBasic.Boolean:
        out["kind"] = "bool"
        out["value"] = bool(obj.AsBool())
    elif basetype == rd.SDBasic.Resource:
        out["kind"] = "resource"
        rid = obj.AsResourceId()
        out["value"] = rdutil.rid_str(rid)
        if controller is not None:
            name = rdutil.resource_name_for(controller, rid)
            if name:
                out["resource_name"] = name
    elif basetype == rd.SDBasic.Float:
        out["kind"] = "float"
        out["value"] = float(obj.AsFloat())
    elif basetype in (rd.SDBasic.UnsignedInteger, rd.SDBasic.SignedInteger, rd.SDBasic.GPUAddress):
        out["kind"] = "int"
        out["value"] = int(obj.AsInt())
    elif basetype == rd.SDBasic.Enum:
        out["kind"] = "enum"
        out["value"] = int(obj.AsInt())
        if has_custom_string:
            s = str(getattr(obj.data, "string", "") or "")
            if s:
                out["enum_name"] = s
    elif basetype == rd.SDBasic.Character:
        out["kind"] = "char"
        try:
            out["value"] = obj.data.basic.c
        except Exception:
            out["value"] = None
    else:
        out["kind"] = "unknown"
        out["value"] = None

    if has_custom_string and out.get("kind") != "enum":
        s = str(getattr(obj.data, "string", "") or "")
        if s:
            out["display"] = s

    return out


def serialize_chunk(
    rd: Any,
    chunk: Any,
    controller: Any | None = None,
    max_depth: int = DEFAULT_MAX_DEPTH,
    max_children: int = DEFAULT_MAX_CHILDREN,
) -> dict[str, Any]:
    """Serialize an SDChunk: its call name, timing metadata, and full parameter tree."""
    meta = chunk.metadata
    n = chunk.NumChildren()
    out: dict[str, Any] = {
        "name": str(chunk.name),
        "thread_id": int(meta.threadID),
        "duration_micros": int(meta.durationMicro),
        "parameters": [
            serialize_sdobject(rd, chunk.GetChild(i), controller, max_depth, max_children)
            for i in range(min(n, max_children))
        ],
    }
    if n > max_children:
        out["parameters_truncated"] = True
    return out
