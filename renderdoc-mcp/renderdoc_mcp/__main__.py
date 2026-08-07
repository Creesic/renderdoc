"""CLI entry: `python -m renderdoc_mcp` or `renderdoc-mcp`."""

from __future__ import annotations

import argparse
import logging
import os
import sys

from renderdoc_mcp.logging_config import LoggingState, configure_logging


_logging_state: LoggingState | None = None


def _setup_crash_logging() -> None:
    """Write bounded service logs and native fault traces next to the application.

    MCPServerManager caps captured stdout/stderr at 220 chars, so normal
    tracebacks are silently truncated. Native faults use a dedicated stable file
    while normal logs rotate and exclude verbose dependency payloads.
    """
    global _logging_state
    log_dir = (
        os.environ.get("RENDERDOC_MCP_LOGDIR")
        or os.environ.get("RENDERDOC_MCP_APPDIR")
        or os.getcwd()
    )

    try:
        _logging_state = configure_logging(log_dir)
    except OSError:
        return  # can't open log — silent fallback

    # Unhandled Python exceptions: write full traceback before exit.
    _orig_excepthook = sys.excepthook

    def _excepthook(exc_type, exc_value, exc_tb):
        import traceback
        assert _logging_state is not None
        print("UNHANDLED EXCEPTION:", file=_logging_state.fault_file)
        traceback.print_exception(
            exc_type, exc_value, exc_tb, file=_logging_state.fault_file
        )
        _logging_state.fault_file.flush()
        _orig_excepthook(exc_type, exc_value, exc_tb)

    sys.excepthook = _excepthook

    logging.getLogger("renderdoc_mcp").info(
        "MCP server starting — service log: %s — native faults: %s",
        _logging_state.service_path,
        _logging_state.fault_path,
    )


def main(argv: list[str] | None = None) -> None:
    _setup_crash_logging()
    parser = argparse.ArgumentParser(description="RenderDoc MCP server (stdio or Streamable HTTP /mcp)")
    parser.add_argument(
        "--transport",
        choices=("streamable-http", "stdio"),
        default="streamable-http",
        help="stdio for editor local MCP drivers (e.g. OpenCode/Cursor spawn); "
        "streamable-http for TCP /mcp (default, used by RenderDoc GUI)",
    )
    parser.add_argument("--host", default="127.0.0.1", help="Bind host (streamable-http only)")
    parser.add_argument("--port", type=int, default=8765, help="Bind port (streamable-http only)")
    args = parser.parse_args(argv)

    try:
        from renderdoc_mcp.server import main as srv_main
    except ImportError as e:
        print("renderdoc_mcp import failed: {}".format(e), file=sys.stderr)
        sys.exit(1)

    srv_main(host=args.host, port=args.port, transport=args.transport)


if __name__ == "__main__":
    main()
