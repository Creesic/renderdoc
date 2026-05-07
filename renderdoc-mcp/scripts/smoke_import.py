#!/usr/bin/env python3
"""Minimal verification with pymodules on PYTHONPATH: imports server without starting replay."""

from __future__ import annotations

import sys


def main() -> int:
    try:
        from renderdoc_mcp.server import build_mcp

        build_mcp()
    except Exception as e:
        print("FAIL:", e, file=sys.stderr)
        return 1
    print("OK: renderdoc_mcp.server.build_mcp")
    return 0


if __name__ == "__main__":
    sys.exit(main())
