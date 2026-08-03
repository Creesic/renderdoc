from __future__ import annotations

import os
from types import SimpleNamespace

from renderdoc_mcp.session import CaptureSession, CaptureSessionManager
from renderdoc_mcp.server import _capability_error


class _Controller:
    def __init__(self):
        self.shutdown_called = False

    def GetStructuredFile(self):
        return SimpleNamespace(chunks=[])

    def GetRootActions(self):
        return []

    def GetAPIProperties(self):
        return SimpleNamespace(
            pipelineType="GraphicsAPI::Metal",
            degraded=True,
            features=[
                SimpleNamespace(feature="ReplayFeature::BufferFetch", available=True, reason=""),
                SimpleNamespace(
                    feature="ReplayFeature::PipelineState",
                    available=False,
                    reason="Apple trace pipeline normalization is unavailable",
                ),
            ],
        )

    def Shutdown(self):
        self.shutdown_called = True


class _CaptureAccess:
    def __init__(self, rd, importer: bool, calls: list[tuple]):
        self._rd = rd
        self._importer = importer
        self._calls = calls
        self.DriverName = "Metal"

    def OpenFile(self, path, filetype, progress):
        self._calls.append(("open", path, filetype))
        return self._rd.ResultCode.Succeeded

    def Convert(self, path, filetype, *args):
        self._calls.append(("convert", path, filetype))
        with open(path, "wb") as converted:
            converted.write(b"RDC")
        return self._rd.ResultCode.Succeeded

    def LocalReplaySupport(self):
        return not self._importer

    def OpenCapture(self, opts, progress):
        controller = _Controller()
        self._calls.append(("open_capture", controller))
        return self._rd.ResultCode.Succeeded, controller

    def Shutdown(self):
        self._calls.append(("shutdown", self._importer))


def test_gputrace_open_converts_to_owned_thin_rdc_and_removes_it(monkeypatch, tmp_path):
    calls: list[tuple] = []
    access_count = 0

    class _RD:
        class ResultCode:
            Succeeded = 0

        class ReplayOptions:
            pass

        def OpenCaptureFile(self):
            nonlocal access_count
            access = _CaptureAccess(self, importer=access_count == 0, calls=calls)
            access_count += 1
            return access

    rd = _RD()
    monkeypatch.setattr("renderdoc_mcp.rdutil.get_renderdoc", lambda: rd)

    manager = CaptureSessionManager()
    session = manager.open_capture(str(tmp_path / "fixture.gputrace"))

    assert calls[0][0::2] == ("open", "gputrace")
    assert any(call[0] == "convert" and call[2] == "rdc" for call in calls)
    assert session.api_name == "Metal"
    assert session.degraded is True
    assert session.capability("BufferFetch")["available"] is True
    assert session.capability("PipelineState")["available"] is False
    assert session.owned_temp_dir is not None
    owned_temp_dir = session.owned_temp_dir
    assert os.path.exists(os.path.join(owned_temp_dir, "capture.rdc"))

    assert manager.close_capture(session.capture_id) is True
    assert not os.path.exists(owned_temp_dir)


def test_explicit_unavailable_capability_is_actionable_error():
    session = CaptureSession(
        capture_id="metal-capture",
        path="fixture.gputrace",
        controller=object(),
        structured_file=object(),
        api_name="Metal",
        capabilities=[
            {
                "feature": "TextureFetch",
                "available": False,
                "reason": "Texture normalization is not implemented",
            }
        ],
    )

    error = _capability_error(session, "TextureFetch")

    assert error == {
        "ok": False,
        "error": {
            "code": "unsupported_capability",
            "message": "Texture normalization is not implemented",
            "detail": {
                "capture_id": "metal-capture",
                "api": "Metal",
                "feature": "TextureFetch",
                "available": False,
            },
        },
    }
    assert _capability_error(session, "BufferFetch") is None
