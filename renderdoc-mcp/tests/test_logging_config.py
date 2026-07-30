from __future__ import annotations

import logging

from renderdoc_mcp.logging_config import configure_logging


def test_logging_is_rotating_compact_and_suppresses_dependency_debug(tmp_path):
    root = logging.getLogger()
    app_logger = logging.getLogger("renderdoc_mcp.test")
    uvicorn_logger = logging.getLogger("uvicorn.error")
    old_handlers = list(root.handlers)
    old_root_level = root.level
    old_app_level = app_logger.level
    old_uvicorn_level = uvicorn_logger.level

    fault_path = tmp_path / "renderdoc_mcp_crash.log"
    fault_path.write_bytes((b"old fault line\n" * 512))

    state = configure_logging(
        str(tmp_path),
        enable_faulthandler=False,
        service_max_bytes=512,
        service_backup_count=1,
        fault_max_bytes=512,
        record_max_chars=80,
    )
    try:
        logging.getLogger("mcp.server.lowlevel").debug("full structured response payload")
        logging.getLogger("mcp.server.lowlevel").warning("dependency warning retained")
        app_logger.info("application lifecycle retained")
        app_logger.info("X" * 500)
        state.handler.flush()

        initial_logs = list(tmp_path.glob("renderdoc_mcp.log*"))
        initial = "\n".join(
            path.read_text(encoding="utf-8") for path in initial_logs
        )
        assert "full structured response payload" not in initial
        assert "dependency warning retained" in initial
        assert "application lifecycle retained" in initial
        assert "characters omitted" in initial

        for i in range(20):
            app_logger.info("bounded record %d", i)
        state.handler.flush()

        service_logs = list(tmp_path.glob("renderdoc_mcp.log*"))
        assert len(service_logs) <= 2
        assert all(path.stat().st_size < 1024 for path in service_logs)

        fault_data = fault_path.read_bytes()
        assert len(fault_data) <= 512
        assert fault_data.startswith(b"[older native fault log content compacted on startup]\n")
    finally:
        root.removeHandler(state.handler)
        state.handler.close()
        state.fault_file.close()
        root.handlers[:] = old_handlers
        root.setLevel(old_root_level)
        app_logger.setLevel(old_app_level)
        uvicorn_logger.setLevel(old_uvicorn_level)
