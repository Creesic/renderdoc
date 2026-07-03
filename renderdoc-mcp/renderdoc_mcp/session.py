"""Capture sessions: open/close, flat event index, marker stacks."""

from __future__ import annotations

import base64
import json
import re
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


def expand_action_flags(rd: Any, flags: int) -> list[str]:
    names: list[str] = []
    for name in dir(rd.ActionFlags):
        if name.startswith("_"):
            continue
        try:
            val = getattr(rd.ActionFlags, name)
            if isinstance(val, int) and flags & val:
                names.append(name)
        except Exception:
            continue
    return sorted(names)


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
    events_by_id: dict[int, IndexedEvent] = field(default_factory=dict)
    events_ordered: list[int] = field(default_factory=list)
    chunk_index_by_event: dict[int, int] = field(default_factory=dict)

    def shutdown(self, rd: Any) -> bool:
        try:
            self.controller.Shutdown()
            return True
        except Exception:
            # Previously swallowed silently -- a failure here can mean the replay device (and its
            # GPU memory) never actually got released, which is exactly what would make opening
            # several more captures afterwards more likely to hit a DXGI device error. Surfacing
            # it doesn't fix anything by itself, but a silent failure here was actively hiding the
            # evidence needed to diagnose that. See renderdoc-mcp/TODO.md #10.
            _log.exception("shutdown: controller.Shutdown() failed for capture_id=%s", self.capture_id)
            return False


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
        result = cap.OpenFile(path, "", None)
        if result != rd.ResultCode.Succeeded:
            cap.Shutdown()
            raise RuntimeError("OpenFile failed: {} ({})".format(path, result))

        driver_name = rdutil.capture_driver_name(cap)
        _log.info("open_capture: driver=%s LocalReplaySupport=%s", driver_name, cap.LocalReplaySupport())

        if not cap.LocalReplaySupport():
            cap.Shutdown()
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
            raise RuntimeError("OpenCapture failed: {}".format(result))

        structured = controller.GetStructuredFile()
        capture_id = str(uuid.uuid4())

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
        return sess.shutdown(rd)

    def close_all(self) -> None:
        rd = rdutil.get_renderdoc()
        for sid in list(self._sessions.keys()):
            sess = self._sessions.pop(sid, None)
            if sess is not None:
                sess.shutdown(rd)

    def set_frame_event(self, sess: CaptureSession, event_id: int, force_complete_replay: bool = True) -> None:
        # SetFrameEvent is void in the native API; Python bindings typically return None — do not
        # treat falsy return values as failure.
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


def find_action(controller: Any, event_id: int) -> Any | None:
    found: list[Any] = []

    def visit(act: Any) -> None:
        if int(act.eventId) == event_id:
            found.append(act)

    for root in controller.GetRootActions():
        _walk_actions(root, visit)
    return found[0] if found else None


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
