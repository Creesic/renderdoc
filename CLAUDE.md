# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project overview

This is **RenderDoc**, a frame-capture graphics debugger supporting Vulkan, D3D11, D3D12, OpenGL/GLES, and (partial) Metal, on Windows/Linux/Android. Upstream: https://github.com/baldurk/renderdoc, branch `v1.x`.

This checkout is a personal fork that additionally bundles **`renderdoc-mcp`**, a Python MCP server exposing replay introspection (pipeline state, resources, textures, buffers, pixel history, shader reflection, etc.) over Streamable HTTP or stdio. It wraps the same `renderdoc` Python module (`pymodules`) that ships next to any RenderDoc build; it does not automate the GUI. See `renderdoc-mcp/README.md` for tool list and transport details, and `AGENTS.md` for accumulated gotchas specific to this integration (PYTHONPATH ordering, bundled-Python DLL shadowing, SWIG `ResourceId` construction, replay API quirks across builds). The `mcp__renderdoc__*` tools available in this environment are backed by that server.

## Build

**Windows** (primary dev platform for this checkout): open `renderdoc.sln` in Visual Studio (VS2015 compilers or later with retarget) or build headlessly:
```
msbuild.exe renderdoc.sln /m /p:Configuration=Development /p:Platform=x64
```
Use `Configuration=Development` for day-to-day work (debuggable, not too slow); use `Release` for anything performance-sensitive or distributed. Local builds in this checkout typically target `Release|x64`.

**Linux/Mac**:
```
cmake -DCMAKE_BUILD_TYPE=Debug -Bbuild -H.
make -C build
```
Toggle drivers with `-DENABLE_GL=OFF`, `-DENABLE_VULKAN=OFF`, `-DENABLE_METAL=On`, etc. (see `CMakeLists.txt` for the full `ENABLE_*` option list). Mac requires a C++17 compiler (Xcode clang); Linux only requires C++14 (gcc 5+ / clang 3.4+).

**Android**: build from a bash shell (msys2/WSL/cygwin, not cmd) with `-DBUILD_ANDROID=On -DANDROID_ABI=<abi>`.

The MCP add-on lives outside the C++ build: `cd renderdoc-mcp && pip install -e .`. `RENDERDOC_BUNDLE_MCP_PYTHON_SITE` (CMake option) vendors its Python deps into `bin/mcp_site/` during packaging.

## Tests

**C++ unit tests** (compiled into the binaries, not a separate target):
```
renderdoccmd.exe test unit -o test.log      # core
qrenderdoc.exe --unittest log=test.log      # UI
```

**Integration/replay tests** (`util/test/rdtest`, Python-driven, requires the `demos` program built from `demos.sln` or `cmake -Bbuild -Hdemos` first):
```
python util/test/run_tests.py --renderdoc <path>/x64/Development --pyrenderdoc <path>/x64/Development/pymodules
```
Key flags: `-t`/`-x` (regex include/exclude), `-l` (list tests), `--slow-tests` (opt in), `--in-process` (debugging, no per-test child process). The python version/bitness must match the RenderDoc build under test. See `util/test/README.md`.

**Formatting check** (CI-enforced, run before committing C++ changes):
```
bash ./util/clang_format_all.sh
```

**MCP smoke check**:
```
python -c "from renderdoc_mcp.server import build_mcp; build_mcp(); print('ok')"
```

## Commit conventions

CI rejects commit summary (first) lines of 73+ characters — keep them short. See `docs/CONTRIBUTING/Preparing-Commits.md`.

## Code style (C++, enforced beyond clang-format)

- `auto` only for STL iterators and lambdas; explicit types elsewhere.
- `NULL`, not `nullptr`.
- Brace all arms of an `if`/`else` chain if any arm needs braces.
- Use `rdcarray`/`rdcstr` instead of `std::vector`/`std::string`. STL is otherwise limited to `std::map`, `std::set`, `std::function`, standard algorithms, and type-traits checks.
- No hungarian notation except `m_` for members (exception: parameter names in hooked functions must match the official signature).
- Strings are UTF-8 `char*` everywhere except OS-specific wide-string code.
- Match surrounding code style over personal preference — the codebase isn't fully consistent.

## Architecture

**Driver abstraction is the core organizing principle.** `renderdoc/core/core.h` defines the `RenderDoc` singleton, which each API backend registers itself with via `RenderDoc::Inst().RegisterReplayProvider(RDCDriver, provider)`. Backends live under `renderdoc/driver/{d3d11,d3d12,gl,vulkan,metal,dx,dxgi,ihv}/` and implement the `IReplayDriver`/`IRemoteDriver` interfaces from `renderdoc/replay/replay_driver.h`. Adding a feature to only one API and porting later is an accepted, common workflow here — don't assume every driver needs simultaneous changes.

**Capture and replay are separate concerns.** Capture-side hooking lives in `renderdoc/hooks/` (link-time/runtime API interception) plus `renderdocshim/` (a minimal kernel32-only DLL used for global hook injection on Windows). Replay/analysis is driven through `renderdoc/replay/` (`ReplayController`, `CaptureFile`, `ReplayOutput`) and `renderdoc/core/remote_server.h` + `replay_proxy.h` for remote/cross-machine replay.

**Public API surface**: `renderdoc/api/replay/*.h` (notably `renderdoc_replay.h`, `apidefs.h`, pipestate headers) is the stable C/C++ interface. It is what both `qrenderdoc` and the SWIG-generated Python bindings (`qrenderdoc/Code/pyrenderdoc/`) consume — changes here ripple into both the UI and Python/MCP layers, so check `pyrenderdoc/cosmetics.i` when adding anything Python needs to touch specially (e.g. `ResourceId.FromUInt64`).

**qrenderdoc** (`qrenderdoc/Code/`) is the Qt desktop UI, built entirely on top of the replay API above (`CaptureContext`, `ReplayManager`) — it has no privileged access to internals that isn't otherwise exposed through `renderdoc/api/`.

**renderdoccmd** is a thin CLI wrapping the same core for scripting/automation/unit-test entry points, and hosts the global-hook injection logic used to attach RenderDoc to a target process.

**renderdoc-mcp** (this fork's addition) is a separate Python package layered on top of the Python replay bindings (`pymodules`); it never touches C++ directly. It's launched as a subprocess by qrenderdoc's Settings → MCP toggle, or run standalone. Replay calls are serialized behind a single lock since the underlying replay APIs aren't safe for concurrent access — keep that in mind before assuming tools can run in parallel.

Don't read every driver directory to get oriented — `docs/CONTRIBUTING/Code-Explanation.md` has the canonical top-level map, and this section covers the parts that require cross-file understanding.
