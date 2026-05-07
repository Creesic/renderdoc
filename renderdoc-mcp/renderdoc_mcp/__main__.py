"""CLI entry: `python -m renderdoc_mcp` or `renderdoc-mcp`."""

from __future__ import annotations

import argparse
import sys


def main(argv: list[str] | None = None) -> None:
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
