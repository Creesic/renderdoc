"""Decode RenderDoc constant buffer bytes using shader reflection metadata."""

from __future__ import annotations

import math
import struct
from typing import Any

# Byte widths keyed by VarType enum member names from the real RenderDoc API.
_VARTYPE_BYTES = {
    "Float": 4, "Double": 8, "Half": 2,
    "SInt": 4, "UInt": 4,
    "SShort": 2, "UShort": 2,
    "SLong": 8, "ULong": 8,
    "SByte": 1, "UByte": 1,
    "Bool": 4,
}


def _comp_type_name(comp_type: Any) -> str:
    if comp_type is None:
        return "Float"
    n = getattr(comp_type, "name", None)
    if n:
        return str(n)
    return str(comp_type)


def _type_str(rows: int, cols: int, base: str) -> str:
    short = {
        "Float": "float", "Double": "double", "Half": "half",
        "UInt": "uint", "SInt": "int",
        "SShort": "short", "UShort": "ushort",
        "SLong": "long", "ULong": "ulong",
        "SByte": "sbyte", "UByte": "ubyte",
        "Bool": "bool",
    }.get(base, base.lower())
    if rows == 1 and cols == 1:
        return short
    if rows == 1:
        return "{}{}".format(short, cols)
    return "{}{}x{}".format(short, rows, cols)


def _unpack_scalar(raw: bytes, offset: int, base: str, bw: int) -> Any:
    if offset + bw > len(raw):
        return None
    chunk = raw[offset : offset + bw]
    try:
        if base == "Half":
            return float(struct.unpack_from("<e", chunk)[0])
        if base in ("Float", "Double"):
            fmt = "<f" if bw == 4 else "<d" if bw == 8 else None
            return float(struct.unpack_from(fmt, chunk)[0]) if fmt else None
        if base == "UInt":
            fmt = {1: "<B", 2: "<H", 4: "<I"}.get(bw)
            return struct.unpack_from(fmt, chunk)[0] if fmt else None
        if base == "SInt":
            fmt = {1: "<b", 2: "<h", 4: "<i"}.get(bw)
            return struct.unpack_from(fmt, chunk)[0] if fmt else None
        if base == "UShort":
            return struct.unpack_from("<H", chunk)[0]
        if base == "SShort":
            return struct.unpack_from("<h", chunk)[0]
        if base == "ULong":
            return struct.unpack_from("<Q", chunk)[0]
        if base == "SLong":
            return struct.unpack_from("<q", chunk)[0]
        if base == "UByte":
            return struct.unpack_from("<B", chunk)[0]
        if base == "SByte":
            return struct.unpack_from("<b", chunk)[0]
        if base == "Bool":
            fmt = {1: "<B", 2: "<H", 4: "<I"}.get(bw)
            return bool(struct.unpack_from(fmt, chunk)[0]) if fmt else None
    except struct.error:
        return None
    return None


def decode_cb_bytes(raw: bytes, constants: list, row_major: bool = True,
                    base_offset: int = 0) -> list[dict]:
    """Decode constant buffer bytes into a list of typed variable dicts.

    Each output dict has keys: name (str), type (str), value (scalar/list/list-of-list).
    row_major=True matches HLSL cbuffer layout (rows padded to 16 bytes in memory).
    base_offset is the absolute offset of the enclosing struct: ShaderConstant.byteOffset
    is relative to the parent structure, so recursion accumulates it.
    """
    results: list[dict] = []
    for const in constants:
        name = str(getattr(const, "name", "") or "")
        offset = base_offset + int(getattr(const, "byteOffset", 0))
        ctype = getattr(const, "type", None)

        members = list(getattr(ctype, "members", None) or [])
        elements = int(getattr(ctype, "elements", 1) or 1)
        array_stride = int(getattr(ctype, "arrayByteStride", 0) or 0)

        if elements > 1:
            instances = [("{}[{}]".format(name, i), offset + i * array_stride)
                         for i in range(elements)]
        else:
            instances = [(name, offset)]

        for inst_name, inst_offset in instances:
            if members:
                nested_results = decode_cb_bytes(raw, members, row_major, base_offset=inst_offset)
                for nested in nested_results:
                    nested["name"] = "{}.{}".format(inst_name, nested["name"])
                results.extend(nested_results)
                continue

            rows = int(getattr(ctype, "rows", 1) or 1)
            cols = int(getattr(ctype, "columns", 1) or 1)
            base = _comp_type_name(getattr(ctype, "baseType", None))
            bw = _VARTYPE_BYTES.get(base, 4)
            type_name = _type_str(rows, cols, base)

            if rows == 1 and cols == 1:
                value: Any = _unpack_scalar(raw, inst_offset, base, bw)
            elif rows == 1:
                value = [_unpack_scalar(raw, inst_offset + c * bw, base, bw) for c in range(cols)]
            else:
                # Determine layout for this specific type (with fallback to function param)
                rm_method = getattr(ctype, "RowMajor", None)
                type_row_major = rm_method() if callable(rm_method) else row_major

                # matrixByteStride is the per-row byte stride; fall back to std140 default of 16
                row_stride = int(getattr(ctype, "matrixByteStride", 0) or 0) or 16
                if type_row_major:
                    value = [
                        [_unpack_scalar(raw, inst_offset + r * row_stride + c * bw, base, bw) for c in range(cols)]
                        for r in range(rows)
                    ]
                else:
                    # Column-major: column stride = row_stride, iterate columns then rows
                    value = [
                        [_unpack_scalar(raw, inst_offset + c * row_stride + r * bw, base, bw) for c in range(cols)]
                        for r in range(rows)
                    ]

            results.append({"name": inst_name, "type": type_name, "value": value})

    return results


def detect_variable_anomalies(var: dict) -> str | None:
    """Return an anomaly key string for a decoded variable, or None if nothing suspicious."""
    value = var.get("value")
    type_name = var.get("type", "")
    if value is None:
        return None

    def _flatten(v: Any) -> list:
        if isinstance(v, list):
            out: list = []
            for item in v:
                out.extend(_flatten(item))
            return out
        return [v]

    flat = [x for x in _flatten(value) if x is not None]
    if not flat:
        return None

    def _is_bad(v: Any) -> bool:
        try:
            f = float(v)
            return math.isnan(f) or math.isinf(f)
        except (TypeError, ValueError):
            return False

    if any(_is_bad(v) for v in flat):
        return "nan_or_inf"

    is_matrix = isinstance(value, list) and value and isinstance(value[0], list)

    if is_matrix:
        if all(abs(float(v)) < 1e-10 for v in flat):
            return "zero_matrix"
        rows = value
        n = len(rows)
        if all(len(r) == n for r in rows):
            is_identity = all(
                abs(float(rows[r][c]) - (1.0 if r == c else 0.0)) < 1e-5
                for r in range(n)
                for c in range(n)
            )
            if is_identity:
                return "identity_matrix"
        return None

    if isinstance(value, list):
        if all(v is not None and abs(float(v)) < 1e-10 for v in flat):
            return "zero_vector"
        return None

    return None
