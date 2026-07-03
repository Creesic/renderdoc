"""Tests for MCP server lifespan replay ownership."""

from __future__ import annotations

import pytest

import renderdoc_mcp.server as server


@pytest.mark.asyncio
async def test_lifespan_keeps_replay_alive_when_streamable_http_session_ends(monkeypatch):
    """Given an initialized replay, session teardown must not shut down RenderDoc replay."""

    # Given
    cleanup_calls: list[str] = []

    def close_all() -> None:
        cleanup_calls.append("close_all")

    monkeypatch.setattr(server.sessions, "close_all", close_all)
    monkeypatch.setattr(server, "_replay_initialized", True)

    # When
    async with server._lifespan(None):
        pass

    # Then
    assert cleanup_calls == []
    assert server._replay_initialized is True
