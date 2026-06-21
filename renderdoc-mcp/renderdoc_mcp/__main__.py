"""CLI entry: `python -m renderdoc_mcp` or `renderdoc-mcp`."""

from __future__ import annotations

import argparse
import faulthandler
import logging
import os
import sys


def _setup_crash_logging() -> None:
    """Write Python tracebacks and native crash info to a persistent log file.

    MCPServerManager caps captured stdout/stderr at 220 chars, so normal
    tracebacks are silently truncated. This bypasses that limit by writing
    directly to a file.
    """
    app_dir = os.environ.get("RENDERDOC_MCP_APPDIR") or os.getcwd()
    log_path = os.path.join(app_dir, "renderdoc_mcp_crash.log")

    try:
        log_fd = open(log_path, "a", buffering=1)  # line-buffered so partial writes survive crashes
    except OSError:
        return  # can't open log — silent fallback

    # Native crash handler: writes a C-level traceback to the file even on
    # segfaults and stack overflows that bypass Python's exception machinery.
    faulthandler.enable(file=log_fd, all_threads=True)

    # Python-level logging: captures structured log records from server.py.
    logging.basicConfig(
        stream=log_fd,
        level=logging.DEBUG,
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
        force=True,
    )

    # Unhandled Python exceptions: write full traceback before exit.
    _orig_excepthook = sys.excepthook

    def _excepthook(exc_type, exc_value, exc_tb):
        import traceback
        print("UNHANDLED EXCEPTION:", file=log_fd)
        traceback.print_exception(exc_type, exc_value, exc_tb, file=log_fd)
        log_fd.flush()
        _orig_excepthook(exc_type, exc_value, exc_tb)

    sys.excepthook = _excepthook

    logging.getLogger("renderdoc_mcp").info("MCP server starting — log: %s", log_path)


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
