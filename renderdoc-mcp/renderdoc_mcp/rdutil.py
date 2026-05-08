"""Lazy import and helpers for the RenderDoc Python module (pymodules)."""

from __future__ import annotations

import os
import re
import sys
from typing import Any

_RESOURCE_ID_PREFIX_RE = re.compile(r"(?i)^ResourceId::\s*")


def _resource_id_from_uint64(rd: Any, raw: int) -> Any:
    """Build ResourceId from capture/API numeric id (SWIG has no public ResourceId(uint64_t) ctor)."""
    val = int(raw)
    if val < 0:
        raise ValueError("resource id must be non-negative")
    maker = getattr(rd.ResourceId, "FromUInt64", None)
    if callable(maker):
        return maker(val)
    try:
        return rd.ResourceId(val)
    except Exception:
        raise ValueError(
            "Cannot construct ResourceId from integer on this pymodules build — rebuild RenderDoc "
            "so pyrenderdoc exposes ResourceId.FromUInt64 (see cosmetics.i), or use a matching "
            "development pymodules folder."
        ) from None


_rd: Any | None = None
_import_error: Exception | None = None
_win32_dll_prep_done = False


def _ensure_windows_renderdoc_dll_search_path() -> None:
    """Windows + Python 3.8+: extension DLL deps ignore PATH; add folders that hold renderdoc.dll."""
    global _win32_dll_prep_done
    if _win32_dll_prep_done or sys.platform != "win32":
        return
    _win32_dll_prep_done = True
    add = getattr(os, "add_dll_directory", None)
    if add is None:
        return

    to_add: list[str] = []
    appdir = os.environ.get("RENDERDOC_MCP_APPDIR", "").strip()
    if appdir:
        to_add.append(appdir)
    for entry in os.environ.get("PYTHONPATH", "").split(os.pathsep):
        entry = entry.strip().rstrip("\\/")
        if not entry:
            continue
        if os.path.basename(entry).lower() == "pymodules":
            parent = os.path.dirname(entry)
            if parent:
                to_add.append(parent)

    seen: set[str] = set()
    for d in to_add:
        try:
            d_abs = os.path.normpath(os.path.abspath(d))
        except OSError:
            continue
        if d_abs in seen or not os.path.isdir(d_abs):
            continue
        seen.add(d_abs)
        try:
            add(d_abs)
        except OSError:
            pass


def get_renderdoc():
    """Import renderdoc once; raises RuntimeError with setup hints if missing."""
    global _rd, _import_error
    if _rd is not None:
        return _rd
    if _import_error is not None:
        raise RuntimeError(
            "renderdoc module unavailable: {}. "
            "Add RenderDoc's pymodules to PYTHONPATH (e.g. .../bin/x64/Development/pymodules)."
            .format(_import_error)
        ) from _import_error
    _ensure_windows_renderdoc_dll_search_path()
    try:
        import renderdoc as rd  # type: ignore
    except Exception as e:  # pragma: no cover
        _import_error = e
        hint = ""
        if sys.platform == "win32" and "DLL load failed" in str(e):
            hint = (
                " On Windows, renderdoc.pyd must match this interpreter's Python ABI "
                "(see dumpbin /dependents on renderdoc.pyd vs sys.version). "
                "After the MCP bundle stages x64/$(Configuration)/python, rebuild the solution so "
                "pyrenderdoc_module links against that runtime."
            )
        raise RuntimeError(
            "Failed to import renderdoc: {}. "
            "Install/use RenderDoc build pymodules on PYTHONPATH.{}".format(e, hint)
        ) from e
    _rd = rd
    return _rd


def rid_str(rid: Any) -> str:
    rd = get_renderdoc()
    if rid is None:
        return ""
    if rid == rd.ResourceId.Null():
        return "Null"
    return str(rid)


def resource_name_map(controller: Any) -> dict[str, str]:
    """Map ``rid_str`` -> capture resource name (from ``GetResources``), cached on ``controller``."""
    cached = getattr(controller, "_mcp_resource_name_by_rid_str", None)
    if isinstance(cached, dict):
        return cached
    m: dict[str, str] = {}
    try:
        for res in controller.GetResources():
            key = rid_str(getattr(res, "resourceId", None))
            if key in ("", "Null"):
                continue
            raw = getattr(res, "name", None)
            m[key] = str(raw) if raw is not None else ""
    except Exception:
        m = {}
    setattr(controller, "_mcp_resource_name_by_rid_str", m)
    return m


def resource_name_for(controller: Any, rid: Any) -> str:
    """Human-readable resource name from capture metadata; empty string if unknown or null."""
    rd = get_renderdoc()
    if rid is None or rid == rd.ResourceId.Null():
        return ""
    key = rid_str(rid)
    if key in ("", "Null"):
        return ""
    return resource_name_map(controller).get(key, "")


def enrich_resource_dict(controller: Any, d: dict[str, Any], rid: Any) -> None:
    """Set ``resource_id`` and optional ``resource_name`` on dict ``d`` from ``rid``."""
    rs = rid_str(rid)
    d["resource_id"] = rs
    if rs and rs != "Null":
        name = resource_name_for(controller, rid)
        if name:
            d["resource_name"] = name


def enrich_resource_id_field(controller: Any, d: dict[str, Any]) -> None:
    """If ``d`` has ``resource_id`` string, add ``resource_name`` when known."""
    rid_s = d.get("resource_id")
    if not rid_s or rid_s == "Null":
        return
    name = resource_name_map(controller).get(str(rid_s), "")
    if name:
        d["resource_name"] = name


def _looks_like_swig_resource_id(obj: Any) -> bool:
    return getattr(obj.__class__, "__name__", "") == "ResourceId"


def parse_resource_id(s: Any) -> Any:
    """Accept ResourceId, int, or strings like '123', '0x…', 'ResourceId::456'."""
    rd = get_renderdoc()
    if s is None:
        return rd.ResourceId.Null()
    if isinstance(s, bool):
        raise ValueError("Invalid resource_id: expected string or integer, not boolean")
    if isinstance(s, float):
        try:
            return _resource_id_from_uint64(rd, int(s))
        except ValueError:
            raise
        except Exception:
            raise ValueError("Invalid resource_id: {}".format(s)) from None
    if isinstance(s, int):
        if s == 0:
            return rd.ResourceId.Null()
        return _resource_id_from_uint64(rd, s)
    if _looks_like_swig_resource_id(s):
        return s
    if not s or s in ("Null", "null", ""):
        return rd.ResourceId.Null()
    t = str(s).strip().strip("<>").strip()
    t = _RESOURCE_ID_PREFIX_RE.sub("", t)
    t = re.sub(r"^[\"\']|[\"\']$", "", t)
    t = t.replace(",", "").strip()
    try:
        return _resource_id_from_uint64(rd, int(t, 0))
    except ValueError:
        raise
    except Exception:
        raise ValueError("Invalid resource_id: {}".format(s)) from None


def enum_name(obj: Any) -> str:
    """Format enum-like SWIG values; handles PEP435 enums, plain strings, and unknown wrappers."""
    if obj is None:
        return ""
    if isinstance(obj, str):
        return obj
    try:
        n = getattr(obj, "name", None)
        if isinstance(n, str) and n:
            return n
    except Exception:
        pass
    return str(obj)


def _looks_like_wrong_arg_count(exc: BaseException) -> bool:
    """SWIG/Boost.Python arity errors are often TypeError; some builds use other exception types."""
    if isinstance(exc, TypeError):
        return True
    msg = str(exc).lower()
    return ("positional" in msg and "argument" in msg) or ("takes" in msg and "given" in msg)


def controller_get_buffer_data(controller: Any, rid: Any, offset: int, length: int) -> bytes:
    """ReplayController.GetBufferData — bindings vary between (rid,off,len), (rid,off,0), or (rid,off)."""
    off = int(offset)
    ln = max(0, int(length))
    last_err: BaseException | None = None
    for args in ((rid, off, ln), (rid, off, 0), (rid, off)):
        try:
            data = controller.GetBufferData(*args)
            if ln > 0 and len(data) > ln:
                data = data[:ln]
            return data
        except Exception as e:
            if _looks_like_wrong_arg_count(e):
                last_err = e
                continue
            raise
    assert last_err is not None
    raise last_err


def capture_driver_name(capture_access: Any) -> str:
    """Driver string from ICaptureFile after OpenFile; tolerates DriverName vs GetDriverName across bindings."""
    if capture_access is None:
        return ""
    for attr in ("DriverName", "GetDriverName"):
        if not hasattr(capture_access, attr):
            continue
        try:
            val = getattr(capture_access, attr)
            val = val() if callable(val) else val
            if val is not None and str(val).strip():
                return str(val)
        except Exception:
            continue
    return ""


def replay_driver_hint(controller: Any) -> str:
    """Optional fallback when capture file did not expose a driver name (unusual builds)."""
    if controller is None:
        return ""
    for attr in ("GetDriverName", "DriverName"):
        if not hasattr(controller, attr):
            continue
        try:
            val = getattr(controller, attr)
            val = val() if callable(val) else val
            if val is not None and str(val).strip():
                return str(val)
        except Exception:
            continue
    return ""
