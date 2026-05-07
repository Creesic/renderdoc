"""Standard JSON envelopes for MCP tools."""

from __future__ import annotations

from typing import Any


def ok(data: Any | None = None, evidence: list[dict[str, Any]] | None = None) -> dict[str, Any]:
    out: dict[str, Any] = {"ok": True}
    if data is not None:
        out["data"] = data
    if evidence:
        out["evidence"] = evidence
    return out


def err(code: str, message: str, detail: Any | None = None) -> dict[str, Any]:
    e: dict[str, Any] = {"code": code, "message": message}
    if detail is not None:
        e["detail"] = detail
    return {"ok": False, "error": e}
