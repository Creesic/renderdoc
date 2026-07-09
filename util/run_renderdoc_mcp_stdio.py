"""Launch the bundled RenderDoc MCP server over stdio.

This wrapper keeps Claude config simple and avoids relying on PYTHONPATH,
.pth processing, or inline Python snippets. It must be run with the bundled
python.exe from x64/<config>/python.
"""

from __future__ import annotations

import os
import sys
from pathlib import Path


def main() -> None:
    repo_root = Path(__file__).resolve().parents[1]
    runtime_dir = repo_root / "x64" / "Development"
    mcp_site = runtime_dir / "mcp_site"

    paths = (
        runtime_dir / "pymodules",
        runtime_dir / "mcp",
        mcp_site,
        mcp_site / "win32",
        mcp_site / "win32" / "lib",
        mcp_site / "pythonwin",
    )
    sys.path[:0] = [str(path) for path in paths]

    os.add_dll_directory(str(runtime_dir))
    os.add_dll_directory(str(mcp_site / "pywin32_system32"))

    from renderdoc_mcp.__main__ import main as mcp_main

    mcp_main(["--transport", "stdio"])


if __name__ == "__main__":
    main()
