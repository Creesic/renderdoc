#!/usr/bin/env python3
"""Vendor standalone CPython + renderdoc-mcp + PyPI deps into the qrenderdoc runtime directory.

CMake on Linux/macOS invokes this script. Windows MSVC invokes bundle_renderdoc_mcp.ps1
(no host Python required). Both share util/renderdoc_mcp_bundle.json."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import shutil
import struct
import subprocess
import sys
import tarfile
import tempfile
import urllib.request  # noqa: S310 urllib is used only for pinned GitHub URLs
from pathlib import Path


def repo_root_this_file() -> Path:
    return Path(__file__).resolve().parent.parent


def load_manifest(path: Path) -> dict:
    with path.open(encoding="utf-8") as f:
        data = json.load(f)
    for k in list(data.keys()):
        if k.startswith("_"):
            del data[k]
    return data


def load_triplets(cfg: dict) -> dict:
    triplets_obj = cfg.get("triplets")
    if not isinstance(triplets_obj, dict):
        raise ValueError('manifest missing "triplets" object')
    return triplets_obj


def _machine_lower() -> str:
    try:
        return platform.machine().lower()
    except Exception:
        return ""


def infer_triplet(explicit: str | None) -> str:
    if explicit:
        return explicit
    if sys.platform.startswith("win32"):
        bits = struct.calcsize("P") * 8
        is_64 = bits == 64 or os.environ.get("PROCESSOR_ARCHITEW6432") in ("AMD64", "ARM64")
        proc = os.environ.get("PROCESSOR_ARCHITECTURE", "").upper()
        if proc == "ARM64":
            sys.stderr.write(
                "MCP bundle: no windows_arm64 standalone entry defined in renderdoc_mcp_bundle.json\n"
            )
            sys.exit(2)
        return "windows_amd64" if is_64 else "windows_x86"
    if sys.platform == "darwin":
        m = _machine_lower()
        if m in ("arm64", "aarch64"):
            return "macos_arm64"
        return "macos_amd64"
    m = _machine_lower()
    if m in ("aarch64", "arm64"):
        return "linux_aarch64"
    return "linux_amd64"


def artifact_url(cfg: dict, triplet_key: str) -> str:
    tpl = load_triplets(cfg)[triplet_key]
    artifact = tpl["artifact"]
    release = cfg["release_tag"]
    base = cfg["base_url_template"].format(release_tag=release)
    return f"{base}/{artifact}"


def _sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        while True:
            chunk = f.read(1024 * 1024)
            if not chunk:
                break
            h.update(chunk)
    return h.hexdigest()


def _download(url: str, dst: Path) -> None:
    dst.parent.mkdir(parents=True, exist_ok=True)
    tmp = dst.with_suffix(dst.suffix + ".part")
    try:
        with urllib.request.urlopen(url, timeout=300) as r:
            with tmp.open("wb") as f:
                shutil.copyfileobj(r, f, length=1024 * 1024)
    except BaseException:
        if tmp.is_file():
            tmp.unlink(missing_ok=True)
        raise
    tmp.replace(dst)


def _find_python_install_root(stage: Path) -> Path:
    cand = stage / "python"
    if cand.is_dir() and (_is_windows_tree(cand) or _is_posix_tree(cand)):
        return cand
    subs = sorted(p for p in stage.iterdir() if p.is_dir())
    for sub in subs:
        if _is_windows_tree(sub) or _is_posix_tree(sub):
            return sub
    raise RuntimeError(f"could not locate python prefix under extracted tree {stage}")


def _is_windows_tree(root: Path) -> bool:
    return (root / "python.exe").is_file()


def _is_posix_tree(root: Path) -> bool:
    bin_dir = root / "bin"
    if not bin_dir.is_dir():
        return False
    if (bin_dir / "python3").is_file():
        return True
    return any(p.is_file() and p.name.startswith("python3") for p in bin_dir.glob("python3*"))


def _posix_python_cmd(prefix: Path) -> Path:
    p = prefix / "bin" / "python3"
    if p.is_file():
        return p
    for cand in sorted((prefix / "bin").glob("python3*")):
        if cand.is_file():
            return cand
    raise RuntimeError(f"no python3 interpreter under {prefix / 'bin'}")


_SITECUSTOMIZE = '''"""Inserted by RenderDoc util/bundle_renderdoc_mcp (processes mcp_site .pth hooks, e.g. pywin32)."""
import os
import site

_appdir = os.environ.get("RENDERDOC_MCP_APPDIR")
if _appdir:
    try:
        site.addsitedir(os.path.join(_appdir, "mcp_site"))
    except Exception:
        pass
'''


def _write_sitecustomize(py_exe: Path) -> None:
    proc = subprocess.run(
        [
            str(py_exe),
            "-c",
            "import sysconfig; print(sysconfig.get_paths()['purelib'])",
        ],
        capture_output=True,
        text=True,
        check=True,
    )
    purelib = Path(proc.stdout.strip())
    purelib.mkdir(parents=True, exist_ok=True)
    (purelib / "sitecustomize.py").write_text(_SITECUSTOMIZE, encoding="utf-8")


def _make_macos_python_relocatable(prefix: Path) -> None:
    """Give libpython an rpath-based install name suitable for an app bundle."""
    if sys.platform != "darwin":
        return

    dylibs = sorted((prefix / "lib").glob("libpython3*.dylib"))
    if not dylibs:
        raise RuntimeError(f"no libpython dylib under {prefix / 'lib'}")

    for dylib in dylibs:
        subprocess.run(
            ["install_name_tool", "-id", f"@rpath/{dylib.name}", str(dylib)],
            check=True,
        )


def _prune_macos_tcl_payload(prefix: Path) -> None:
    """Remove unused Tcl/Tk data directories that codesign mistakes for nested bundles."""
    if sys.platform != "darwin":
        return

    lib_dir = prefix / "lib"
    for pattern in ("itcl*", "tcl8*", "thread*", "tk8*"):
        for path in lib_dir.glob(pattern):
            if path.is_dir():
                shutil.rmtree(path)

    # Files beneath Contents/MacOS with an executable bit are treated as nested code by codesign.
    # The server only executes the CPython binary; helper scripts such as idle, pip, and pydoc are
    # retained as data so the bundled SDK remains useful without invalidating the outer app seal.
    python_exe = _posix_python_cmd(prefix).resolve()
    for path in (prefix / "bin").iterdir():
        if path.is_symlink() or not path.is_file() or path.resolve() == python_exe:
            continue
        path.chmod(0o644)


def bundle(
    *,
    repo_root: Path,
    runtime_dir: Path,
    triplet_opt: str | None,
    skip_hash: bool,
    offline_archive: Path | None,
    quiet: bool,
) -> None:
    log = (lambda *_a, **_k: None) if quiet else print

    manifest_path = repo_root / "util" / "renderdoc_mcp_bundle.json"
    cfg = load_manifest(manifest_path)
    triplet_key = infer_triplet(triplet_opt)
    triplets_o = load_triplets(cfg)
    tpl = triplets_o.get(triplet_key)
    if not tpl:
        sys.stderr.write(f"unknown triplet {triplet_key!r}; extend renderdoc_mcp_bundle.json\n")
        sys.exit(2)

    cache_dir_env = os.environ.get("RENDERDOC_MCP_CACHE")
    cache_dir = Path(cache_dir_env) if cache_dir_env else repo_root / ".renderdoc_mcp_cache"
    cache_dir.mkdir(parents=True, exist_ok=True)

    archive_path = offline_archive if offline_archive else cache_dir / tpl["artifact"]
    sha_expected = tpl.get("sha256", "") or ""

    if offline_archive:
        log(f"Using offline archive {offline_archive}")
    elif not archive_path.is_file():
        url = artifact_url(cfg, triplet_key)
        log(f"Downloading {url}")
        _download(url, archive_path)

    if skip_hash:
        log("Skipping SHA256 verification.")
    elif sha_expected:
        ha = _sha256_file(archive_path).lower()
        if ha != sha_expected.lower():
            sys.stderr.write(
                f"SHA256 mismatch for {archive_path}\nExpected: {sha_expected}\nGot: {ha}\n"
                f" Remove the file or bump util/renderdoc_mcp_bundle.json\n"
            )
            sys.exit(3)

    mcp_pkg_src = repo_root / "renderdoc-mcp"
    rd_mcp_pkg = mcp_pkg_src / "renderdoc_mcp"
    if not rd_mcp_pkg.is_dir():
        sys.stderr.write(f"missing sources: {rd_mcp_pkg}\n")
        sys.exit(4)

    runtime_dir.mkdir(parents=True, exist_ok=True)
    runtime_python = runtime_dir / "python"
    runtime_mcp = runtime_dir / "mcp" / "renderdoc_mcp"
    runtime_site = runtime_dir / "mcp_site"
    runtime_mcp_outer = runtime_dir / "mcp"
    done = runtime_dir / ".renderdoc_mcp_bundle.stamp.txt"

    # Invalidate the runtime before replacing any of its pieces. A failed pip install must not
    # leave a stale architecture stamp that causes the next configure to accept a partial bundle.
    done.unlink(missing_ok=True)

    for stale in (runtime_python, runtime_mcp_outer, runtime_site):
        if stale.exists():
            shutil.rmtree(stale)

    with tempfile.TemporaryDirectory(prefix="rd_mcp_tar_") as td:
        stage = Path(td)
        with tarfile.open(archive_path, "r:*") as tar:
            try:
                tar.extractall(stage, filter="data")
            except TypeError:
                tar.extractall(stage)
        python_root = _find_python_install_root(stage)
        shutil.copytree(python_root, runtime_python)

    py_exe = runtime_python / "python.exe" if _is_windows_tree(runtime_python) else _posix_python_cmd(runtime_python)

    _prune_macos_tcl_payload(runtime_python)
    _make_macos_python_relocatable(runtime_python)
    _write_sitecustomize(py_exe)

    env = os.environ.copy()
    env["PYTHONUTF8"] = "1"

    subprocess.run(
        [str(py_exe), "-m", "ensurepip", "--default-pip"],
        check=False,
        env=env,
    )
    subprocess.run(
        [
            str(py_exe),
            "-m",
            "pip",
            "install",
            "--disable-pip-version-check",
            "--no-input",
            "--upgrade",
            "pip",
        ],
        check=False,
        env=env,
    )

    runtime_mcp.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(
        rd_mcp_pkg,
        runtime_mcp,
        ignore=shutil.ignore_patterns("__pycache__", "*.pyc", "*.pyo"),
    )

    subprocess.run(
        [
            str(py_exe),
            "-m",
            "pip",
            "install",
            "--disable-pip-version-check",
            "--no-input",
            "--target",
            str(runtime_site),
            str(mcp_pkg_src),
        ],
        check=True,
        env=env,
    )

    done.write_text(f"{triplet_key} {tpl.get('artifact', '')}\n", encoding="utf-8")

    log(f"MCP bundle ready under {runtime_dir}")


def main(argv: list[str] | None) -> int:
    p = argparse.ArgumentParser(description="Bundle embedded Python + MCP for RenderDoc.")
    p.add_argument(
        "--repo-root",
        type=Path,
        default=None,
        help="Repo root (default: parent directory of util/)",
    )
    p.add_argument(
        "--runtime-dir",
        type=Path,
        required=True,
        help="Usually the directory containing qrenderdoc executable",
    )
    p.add_argument("--triplet", default=None)
    p.add_argument(
        "--offline-archive",
        type=Path,
        default=None,
        help="Tarball matching manifest artifact (skip download)",
    )
    p.add_argument("--skip-hash", action="store_true")
    p.add_argument("--quiet", action="store_true")

    ns = p.parse_args(argv)
    repo = (ns.repo_root or repo_root_this_file()).resolve()
    rd = Path(ns.runtime_dir).resolve()

    skip_h = bool(ns.skip_hash or os.environ.get("RENDERDOC_MCP_SKIP_HASH") == "1")
    offline = ns.offline_archive.resolve() if ns.offline_archive else None

    bundle(
        repo_root=repo,
        runtime_dir=rd,
        triplet_opt=ns.triplet,
        skip_hash=skip_h,
        offline_archive=offline,
        quiet=ns.quiet,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
