"""Bounded logging for the RenderDoc MCP subprocess.

Native fault traces and ordinary service logs have different retention needs:
faulthandler needs a stable file descriptor, while normal Python logs should
rotate. Keeping them separate avoids pinning an old rotated file and prevents
verbose MCP/SSE payload logging from growing without bound.
"""

from __future__ import annotations

import faulthandler
import logging
import os
from dataclasses import dataclass
from logging.handlers import RotatingFileHandler
from typing import TextIO


DEFAULT_SERVICE_LOG_MAX_BYTES = 2 * 1024 * 1024
DEFAULT_SERVICE_LOG_BACKUP_COUNT = 2
DEFAULT_FAULT_LOG_MAX_BYTES = 1024 * 1024
DEFAULT_LOG_RECORD_MAX_CHARS = 16 * 1024


def _bounded_env_int(
    name: str,
    default: int,
    *,
    minimum: int,
    maximum: int,
) -> int:
    try:
        value = int(os.environ.get(name, str(default)))
    except (TypeError, ValueError):
        value = default
    return max(minimum, min(value, maximum))


def _compact_fault_log(path: str, max_bytes: int) -> None:
    """Retain only the newest complete lines from an oversized fault log."""

    try:
        size = os.path.getsize(path)
    except OSError:
        return
    if size <= max_bytes:
        return

    marker = b"[older native fault log content compacted on startup]\n"
    tail_bytes = max(1, max_bytes - len(marker))
    try:
        with open(path, "rb") as src:
            src.seek(-min(size, tail_bytes), os.SEEK_END)
            tail = src.read(tail_bytes)
        newline = tail.find(b"\n")
        if newline >= 0:
            tail = tail[newline + 1 :]
        with open(path, "wb") as dst:
            dst.write(marker)
            dst.write(tail)
    except OSError:
        # Logging must never prevent the MCP server from starting.
        return


class _BoundedRecordFilter(logging.Filter):
    def __init__(self, max_chars: int) -> None:
        super().__init__()
        self._max_chars = max_chars

    def filter(self, record: logging.LogRecord) -> bool:
        message = record.getMessage()
        if len(message) > self._max_chars:
            omitted = len(message) - self._max_chars
            record.msg = "{}… [{} characters omitted]".format(
                message[: self._max_chars], omitted
            )
            record.args = ()
        return True


@dataclass
class LoggingState:
    service_path: str
    fault_path: str
    handler: RotatingFileHandler
    fault_file: TextIO


def configure_logging(
    app_dir: str,
    *,
    enable_faulthandler: bool = True,
    service_max_bytes: int | None = None,
    service_backup_count: int | None = None,
    fault_max_bytes: int | None = None,
    record_max_chars: int | None = None,
) -> LoggingState:
    """Configure compact rotating service logs plus a bounded native fault log."""

    os.makedirs(app_dir, exist_ok=True)
    service_path = os.path.join(app_dir, "renderdoc_mcp.log")
    fault_path = os.path.join(app_dir, "renderdoc_mcp_crash.log")

    service_max_bytes = service_max_bytes or _bounded_env_int(
        "RENDERDOC_MCP_LOG_MAX_BYTES",
        DEFAULT_SERVICE_LOG_MAX_BYTES,
        minimum=64 * 1024,
        maximum=64 * 1024 * 1024,
    )
    service_backup_count = (
        service_backup_count
        if service_backup_count is not None
        else _bounded_env_int(
            "RENDERDOC_MCP_LOG_BACKUP_COUNT",
            DEFAULT_SERVICE_LOG_BACKUP_COUNT,
            minimum=1,
            maximum=10,
        )
    )
    fault_max_bytes = fault_max_bytes or _bounded_env_int(
        "RENDERDOC_MCP_FAULT_LOG_MAX_BYTES",
        DEFAULT_FAULT_LOG_MAX_BYTES,
        minimum=64 * 1024,
        maximum=16 * 1024 * 1024,
    )
    record_max_chars = record_max_chars or _bounded_env_int(
        "RENDERDOC_MCP_LOG_RECORD_MAX_CHARS",
        DEFAULT_LOG_RECORD_MAX_CHARS,
        minimum=1024,
        maximum=1024 * 1024,
    )

    _compact_fault_log(fault_path, fault_max_bytes)
    fault_file = open(fault_path, "a", buffering=1, encoding="utf-8")
    if enable_faulthandler:
        faulthandler.enable(file=fault_file, all_threads=True)

    handler = RotatingFileHandler(
        service_path,
        maxBytes=service_max_bytes,
        backupCount=service_backup_count,
        encoding="utf-8",
    )
    handler.setFormatter(
        logging.Formatter("%(asctime)s %(levelname)s %(name)s: %(message)s")
    )
    handler.addFilter(_BoundedRecordFilter(record_max_chars))

    # Dependencies inherit WARNING, which suppresses full MCP response bodies,
    # SSE chunks, heartbeat pings, and tool-schema dumps. Keep our own lifecycle
    # breadcrumbs and uvicorn startup/errors at INFO.
    logging.basicConfig(level=logging.WARNING, handlers=[handler], force=True)
    logging.getLogger("renderdoc_mcp").setLevel(logging.INFO)
    logging.getLogger("uvicorn.error").setLevel(logging.INFO)

    return LoggingState(
        service_path=service_path,
        fault_path=fault_path,
        handler=handler,
        fault_file=fault_file,
    )
