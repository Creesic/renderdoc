"""Capture sessions: open/close, flat event index, marker stacks."""

from __future__ import annotations

import base64
import json
import os
import re
import shutil
import tempfile
import uuid
from dataclasses import dataclass, field
from typing import Any, Callable

import logging

from renderdoc_mcp import rdutil

_log = logging.getLogger("renderdoc_mcp.session")


def _walk_actions(action: Any, visitor: Callable[[Any], None]) -> None:
    visitor(action)
    for ch in action.children:
        _walk_actions(ch, visitor)


def marker_stack(rd: Any, action: Any, structured_file: Any) -> list[dict[str, Any]]:
    stack: list[dict[str, Any]] = []
    p = action.parent
    while p is not None:
        stack.append(
            {
                "event_id": int(p.eventId),
                "name": p.GetName(structured_file),
            }
        )
        p = p.parent
    stack.reverse()
    return stack


# Flag tables are static per renderdoc module, but expand_action_flags runs once per action
# during open_capture (100k+ events in real captures) — precompute the (name, value) list and
# memoize expanded name lists per flags value, since flag combinations repeat heavily.
# Keyed by id() with the object itself kept in the entry, so identity is verified and ids
# are never recycled for live entries.
_FLAG_TABLES: dict[int, tuple[Any, list[tuple[str, int]]]] = {}
_FLAG_NAMES: dict[tuple[int, int], list[str]] = {}


def _action_flag_table(rd: Any) -> list[tuple[str, int]]:
    entry = _FLAG_TABLES.get(id(rd))
    if entry is not None and entry[0] is rd:
        return entry[1]
    table: list[tuple[str, int]] = []
    for name in dir(rd.ActionFlags):
        if name.startswith("_"):
            continue
        try:
            val = getattr(rd.ActionFlags, name)
            if isinstance(val, int):
                table.append((name, int(val)))
        except Exception:
            continue
    _FLAG_TABLES[id(rd)] = (rd, table)
    return table


def expand_action_flags(rd: Any, flags: int) -> list[str]:
    key = (id(rd), int(flags))
    cached = _FLAG_NAMES.get(key)
    if cached is None:
        cached = sorted(name for name, val in _action_flag_table(rd) if flags & val)
        _FLAG_NAMES[key] = cached
    return list(cached)


@dataclass
class IndexedEvent:
    event_id: int
    name: str
    flags: int
    flags_names: list[str]
    marker_stack: list[dict[str, Any]]
    num_vertices: int = 0
    num_instances: int = 0
    num_indices: int = 0


@dataclass
class CaptureSession:
    capture_id: str
    path: str
    controller: Any
    structured_file: Any
    driver_name: str = ""
    api_name: str = "Unknown"
    degraded: bool = False
    capabilities: list[dict[str, Any]] = field(default_factory=list)
    owned_temp_dir: str | None = None
    events_by_id: dict[int, IndexedEvent] = field(default_factory=dict)
    events_ordered: list[int] = field(default_factory=list)
    chunk_index_by_event: dict[int, int] = field(default_factory=dict)

    def shutdown(self, rd: Any) -> bool:
        shutdown_ok = False
        try:
            self.controller.Shutdown()
            shutdown_ok = True
        except Exception:
            # Previously swallowed silently -- a failure here can mean the replay device (and its
            # GPU memory) never actually got released, which is exactly what would make opening
            # several more captures afterwards more likely to hit a DXGI device error. Surfacing
            # it doesn't fix anything by itself, but a silent failure here was actively hiding the
            # evidence needed to diagnose that. See renderdoc-mcp/TODO.md #10.
            _log.exception("shutdown: controller.Shutdown() failed for capture_id=%s", self.capture_id)
        finally:
            if self.owned_temp_dir:
                shutil.rmtree(self.owned_temp_dir, ignore_errors=True)
                self.owned_temp_dir = None
        return shutdown_ok

    def capability(self, feature_name: str) -> dict[str, Any] | None:
        wanted = feature_name.lower()
        for capability in self.capabilities:
            if str(capability.get("feature", "")).lower() == wanted:
                return capability
        return None


class CaptureSessionManager:
    def __init__(self) -> None:
        self._sessions: dict[str, CaptureSession] = {}

    def get(self, capture_id: str) -> CaptureSession | None:
        return self._sessions.get(capture_id)

    def open_capture(self, path: str) -> CaptureSession:
        if self._sessions:
            # Diagnostic breadcrumb, not a limit: each open replay device holds its own GPU
            # resources for as long as its session stays open (no eviction policy exists). If a
            # DXGI device error shows up later, this is the trail that shows how many replay
            # devices were live at the time. See renderdoc-mcp/TODO.md #10.
            _log.warning(
                "open_capture: %d other capture session(s) still open (%s) while opening %s",
                len(self._sessions), list(self._sessions.keys()), path,
            )
        _log.info("open_capture: OpenCaptureFile path=%s", path)
        rd = rdutil.get_renderdoc()
        cap = rd.OpenCaptureFile()
        owned_temp_dir: str | None = None
        input_type = "gputrace" if path.lower().endswith(".gputrace") else ""
        result = cap.OpenFile(path, input_type, None)
        if result != rd.ResultCode.Succeeded:
            cap.Shutdown()
            raise RuntimeError("OpenFile failed: {} ({})".format(path, result))

        if input_type == "gputrace":
            owned_temp_dir = tempfile.mkdtemp(prefix="renderdoc-metal-import-")
            converted_path = os.path.join(owned_temp_dir, "capture.rdc")
            try:
                try:
                    result = cap.Convert(converted_path, "rdc", None, None)
                except TypeError:
                    try:
                        result = cap.Convert(converted_path, "rdc", None)
                    except TypeError:
                        result = cap.Convert(converted_path, "rdc")
            except Exception:
                shutil.rmtree(owned_temp_dir, ignore_errors=True)
                raise
            finally:
                cap.Shutdown()
            if result != rd.ResultCode.Succeeded:
                shutil.rmtree(owned_temp_dir, ignore_errors=True)
                raise RuntimeError("gputrace conversion failed: {} ({})".format(path, result))

            cap = rd.OpenCaptureFile()
            result = cap.OpenFile(converted_path, "rdc", None)
            if result != rd.ResultCode.Succeeded:
                cap.Shutdown()
                shutil.rmtree(owned_temp_dir, ignore_errors=True)
                raise RuntimeError("Converted Metal capture could not be opened: {}".format(result))

        driver_name = rdutil.capture_driver_name(cap)
        _log.info("open_capture: driver=%s LocalReplaySupport=%s", driver_name, cap.LocalReplaySupport())

        if not cap.LocalReplaySupport():
            cap.Shutdown()
            if owned_temp_dir:
                shutil.rmtree(owned_temp_dir, ignore_errors=True)
            raise RuntimeError(
                "Capture cannot be replayed locally ({})".format(driver_name or "unknown driver")
            )

        opts = rd.ReplayOptions()
        _log.info("open_capture: calling OpenCapture")
        result, controller = cap.OpenCapture(opts, None)
        _log.info("open_capture: OpenCapture returned result=%s", result)
        cap.Shutdown()
        if result != rd.ResultCode.Succeeded:
            try:
                controller.Shutdown()
            except Exception:
                pass
            if owned_temp_dir:
                shutil.rmtree(owned_temp_dir, ignore_errors=True)
            raise RuntimeError("OpenCapture failed: {}".format(result))

        structured = controller.GetStructuredFile()
        capture_id = str(uuid.uuid4())

        api_name = "Unknown"
        degraded = False
        capabilities: list[dict[str, Any]] = []
        try:
            props = controller.GetAPIProperties()
            api_name = rdutil.enum_name(getattr(props, "pipelineType", "Unknown"))
            api_name = api_name.rsplit("::", 1)[-1].rsplit(".", 1)[-1]
            degraded = bool(getattr(props, "degraded", False))
            for capability in getattr(props, "features", []) or []:
                capabilities.append(
                    {
                        "feature": rdutil.enum_name(capability.feature)
                        .rsplit("::", 1)[-1]
                        .rsplit(".", 1)[-1],
                        "available": bool(capability.available),
                        "reason": str(capability.reason or ""),
                    }
                )
        except Exception:
            _log.exception("open_capture: could not read API feature capabilities")

        events_by_id: dict[int, IndexedEvent] = {}
        ordered: list[int] = []
        chunk_index_by_event: dict[int, int] = {}

        def visit(act: Any) -> None:
            eid = int(act.eventId)
            idx = IndexedEvent(
                event_id=eid,
                name=act.GetName(structured),
                flags=int(act.flags),
                flags_names=expand_action_flags(rd, int(act.flags)),
                marker_stack=marker_stack(rd, act, structured),
                num_vertices=int(getattr(act, "numVertices", 0) or 0),
                num_instances=int(getattr(act, "numInstances", 0) or 0),
                num_indices=int(getattr(act, "numIndices", 0) or 0),
            )
            events_by_id[eid] = idx
            ordered.append(eid)
            # Actions only cover draws/dispatches/copies/etc; act.events also lists the
            # state-setting API calls leading up to this action, each with its own eventId and a
            # chunkIndex into structured_file.chunks — this is the only place that mapping is
            # available, so build it here rather than re-walking the action tree later.
            for ev in getattr(act, "events", []) or []:
                chunk_index_by_event[int(ev.eventId)] = int(ev.chunkIndex)

        for root in controller.GetRootActions():
            _walk_actions(root, visit)

        sess = CaptureSession(
            capture_id=capture_id,
            path=path,
            controller=controller,
            structured_file=structured,
            driver_name=driver_name,
            api_name=api_name,
            degraded=degraded,
            capabilities=capabilities,
            owned_temp_dir=owned_temp_dir,
            events_by_id=events_by_id,
            events_ordered=ordered,
            chunk_index_by_event=chunk_index_by_event,
        )
        self._sessions[capture_id] = sess
        return sess

    def close_capture(self, capture_id: str) -> bool | None:
        """Returns True/False for whether the underlying device actually shut down cleanly, or
        None if there was no such session. A session is removed from tracking either way -- a
        failed Shutdown() leaves the device un-freed on the GPU/driver side, but there's nothing
        further this process can do about a handle that's already reported failure."""
        sess = self._sessions.pop(capture_id, None)
        if sess is None:
            return None
        rd = rdutil.get_renderdoc()
        _drop_action_map(sess.controller)
        return sess.shutdown(rd)

    def close_all(self) -> None:
        rd = rdutil.get_renderdoc()
        for sid in list(self._sessions.keys()):
            sess = self._sessions.pop(sid, None)
            if sess is not None:
                _drop_action_map(sess.controller)
                sess.shutdown(rd)

    def set_frame_event(self, sess: CaptureSession, event_id: int, force_complete_replay: bool = False) -> None:
        # SetFrameEvent is void in the native API; Python bindings typically return None — do not
        # treat falsy return values as failure.
        # force defaults False: the native call already no-ops when the eventId is unchanged,
        # and this process owns the controller exclusively, so forcing a full frame re-replay
        # on every tool call is pure waste.
        sess.controller.SetFrameEvent(event_id, force_complete_replay)


def decode_cursor(cursor: str | None) -> int:
    if not cursor:
        return 0
    try:
        raw = base64.urlsafe_b64decode(cursor.encode("ascii"))
        data = json.loads(raw.decode("utf-8"))
        return int(data["o"])
    except Exception:
        return 0


def encode_cursor(offset: int) -> str:
    return base64.urlsafe_b64encode(json.dumps({"o": offset}).encode("utf-8")).decode("ascii")


# eventId -> action map per controller, built once on first lookup. Tools like
# list_draws_with_state call find_action per draw, which was a full tree walk each time
# (O(draws x events)). Entries hold the controller so id() can't be recycled while cached;
# CaptureSessionManager drops the entry when the session closes.
_ACTION_MAPS: dict[int, tuple[Any, dict[int, Any]]] = {}


def _actions_by_event(controller: Any) -> dict[int, Any]:
    entry = _ACTION_MAPS.get(id(controller))
    if entry is not None and entry[0] is controller:
        return entry[1]
    mapping: dict[int, Any] = {}

    def visit(act: Any) -> None:
        mapping[int(act.eventId)] = act

    for root in controller.GetRootActions():
        _walk_actions(root, visit)
    _ACTION_MAPS[id(controller)] = (controller, mapping)
    return mapping


def _drop_action_map(controller: Any) -> None:
    entry = _ACTION_MAPS.get(id(controller))
    if entry is not None and entry[0] is controller:
        del _ACTION_MAPS[id(controller)]


def find_action(controller: Any, event_id: int) -> Any | None:
    return _actions_by_event(controller).get(int(event_id))


def filter_events(
    sess: CaptureSession,
    *,
    name_contains: str | None = None,
    name_regex: str | None = None,
    require_flags: list[str] | None = None,
    cursor: str | None = None,
    limit: int = 200,
) -> tuple[list[dict[str, Any]], str | None]:
    rd = rdutil.get_renderdoc()
    rx = re.compile(name_regex) if name_regex else None
    flags_mask = 0
    if require_flags:
        for fn in require_flags:
            if hasattr(rd.ActionFlags, fn):
                flags_mask |= int(getattr(rd.ActionFlags, fn))

    skip_matches = decode_cursor(cursor)
    out: list[dict[str, Any]] = []
    matched_before_page = 0

    for eid in sess.events_ordered:
        ie = sess.events_by_id[eid]
        if name_contains and name_contains.lower() not in ie.name.lower():
            continue
        if rx and not rx.search(ie.name):
            continue
        if flags_mask and (ie.flags & flags_mask) == 0:
            continue

        if matched_before_page < skip_matches:
            matched_before_page += 1
            continue

        out.append(
            {
                "event_id": ie.event_id,
                "name": ie.name,
                "flags": ie.flags_names,
                "marker_stack": ie.marker_stack,
                "draw": {
                    "num_vertices": ie.num_vertices,
                    "num_instances": ie.num_instances,
                    "num_indices": ie.num_indices,
                },
            }
        )
        if len(out) >= limit:
            break

    next_cursor: str | None = None
    if len(out) >= limit:
        next_cursor = encode_cursor(skip_matches + len(out))
    return out, next_cursor
