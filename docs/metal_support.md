# Metal support

RenderDoc's Metal work is staged. The current deterministic fixture and Apple-tool contract are
development inputs for the Apple GPU Trace bridge; they do not imply that native RenderDoc Metal
capture or executable replay is complete.

## A0 reference environment

The checked-in `gpudebug` contract was captured on 2026-08-01 with:

| Component | Reference value |
| --- | --- |
| Host | MacBookPro18,2, Apple M1 Max (24 GPU cores) |
| OS | macOS 27.0, build 26A5388g |
| Metal | Metal 4 supported by the device; fixture uses the Metal 3 command-buffer API |
| Compiler | AppleClang 17.0.0.17000603 |
| `gpucapture` | 2027.0.35, `/usr/bin/gpucapture` |
| `gpudebug` | 1.0, `/usr/bin/gpudebug` |
| Xcode | Not selected; validation used the installed command-line tools and OS GPU tools |

The tools are OS-versioned contracts. A bridge implementation must probe executable paths,
versions, JSON support, and replay devices at runtime, and must reject incompatible output rather
than guessing. Do not parse `gpucapture list` as a stable machine protocol.

## Deterministic fixture

`util/test/demos/apple/metal_trace_fixture.mm` renders one labelled command buffer containing:

1. `Fixture Offscreen Encoder`: a 256x256 RGBA8 render target plus Depth32Float attachment,
   sampled 4x4 checkerboard, shared dynamic uniform buffer, inline fragment bytes, and one direct
   triangle draw.
2. `Fixture Present Encoder`: samples the offscreen color into a 640x360 BGRA8
   `CAMetalLayer` drawable with one indexed draw, then presents it.

All important resources, queue, pipeline states, library, command buffer, encoders, and debug
groups have deterministic labels. The fixture updates the shared dynamic buffer before every
frame with the same bytes, making captures independent of the selected frame number.

Build and validate it on Apple silicon:

```sh
cmake -S util/test/demos/apple -B build/metal-trace-fixture -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64
cmake --build build/metal-trace-fixture --verbose
MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 \
  build/metal-trace-fixture/bin/metal_trace_fixture --frames 10
```

## Command-line capture contract

Read the installed `gpucapture(1)` manpage before using these commands. Capture must be enabled at
process launch:

```sh
MTL_CAPTURE_ENABLED=1 MTLCAPTURE_WAIT_FOR_SIGNAL=1 \
  build/metal-trace-fixture/bin/metal_trace_fixture &
fixture_pid=$!
/usr/bin/gpucapture boundaries --pid "$fixture_pid"
/usr/bin/gpucapture start --pid "$fixture_pid" --count 1 \
  --output build/metal-trace-fixture/captures/metal_trace_fixture.gputrace
kill "$fixture_pid"
```

`MTLCAPTURE_WAIT_FOR_SIGNAL=1` pauses the fresh process at device creation. At that point the
documented default boundary is the device, so `gpucapture start` resumes the process and captures
its first command buffer, including the `CAMetalLayer` presentation. This avoids accumulating
historical drawable resources in a long-running process.

The reference capture downloaded 8 Metal resources, produced a 112 KiB `.gputrace` bundle, and
reported these stable `gpudebug` counts in two independent fresh-process captures:

- 1 command buffer, 2 render encoders, and 2 draw calls;
- 59 API calls and 14 resource objects;
- 3 buffers, 4 textures, 2 render pipelines, 1 library, 1 depth/stencil state, 1 sampler,
  1 command queue, and 1 residency set.

The `.gputrace` bundle is a development artifact and is not checked in. It may contain
machine/OS-specific replay data and is not expected to be byte-for-byte stable.

## Non-interactive inspection contract

Read `gpudebug(1)` before invocation. Automation must use `--json`, `-q`, and either repeated
`-c` commands or `--oneshot`; it must not drive the interactive REPL. Browse before fetching so
the background replayer has time to initialize. A representative command is:

```sh
/usr/bin/gpudebug --oneshot --json -q \
  -t build/metal-trace-fixture/captures/metal_trace_fixture.gputrace \
  -c "go commands/cb0/grp0/re0/grp0/grp0/draw0" \
  -c "info --all" -c "info pipeline" -c "fetch color0" -c "status"
```

Normalized metadata is stored in
`util/test/data/metal/apple_trace/gpudebug-1.0/metal_trace_fixture.golden.json`; fetched resource
hashes are stored beside it in `fetch_hashes.json`. Normalization removes trace paths, GPU virtual
addresses, allocation identifiers, operation identifiers, elapsed times, and other host-specific
values. The importer preserves every raw JSON listing in its versioned index while exposing a
stable subset as actions and resources. Unknown required schema fields cause an explicit
compatibility failure.

## Apple GPU Trace bridge

Opening a `.gputrace` now starts an installed, non-interactive `gpudebug` 1.x session, walks its
static command and resource trees, and writes a version-3 Metal index into the in-memory RDC. It
never reads private files inside the trace bundle. Normalized nodes have deterministic FNV-1a
identifiers. The importer requests complete listings when a tree contains more than the default
20 entries and avoids descending into command binding and shader leaves that the current replay
surface cannot represent.

The bridge maps command buffers, debug groups, encoders, draws, and dispatches into RenderDoc's
action tree. It maps buffers, textures, libraries, pipelines, depth/stencil states, samplers,
queues, and residency sets into the resource list. Replayer readiness is probed only after this
static inspection data has been normalized, so an Apple replay/XPC failure does not prevent the
trace from opening. Buffer bytes and texture previews are fetched only when replay is available and
requested, then cached for the session. Buffer data is sliced according to the caller's range;
texture PNGs are decoded to RGBA8 and uploaded into RenderDoc's local OpenGL proxy renderer so the
ordinary Texture Viewer can display them. Temporary fetch artifacts are deleted after being copied
into the cache. When replay is unavailable, the capability panel preserves the specific `gpudebug`
failure reason and reports resource fetch as unavailable.

All tool processes have output limits and bounded timeouts. Cancellation terminates an active
child process, and shutdown terminates the persistent `gpudebug` session exactly once. Missing
tools, incompatible versions, malformed JSON, timeouts, cancellation, and tool failures produce
distinct RenderDoc errors instead of silently returning an empty capture. Transient Apple replay
XPC connection failures restart the persistent session with bounded backoff. A Rosetta-hosted
QRenderDoc explicitly launches the native arm64 `gpudebug` slice. Child stdin is connected to
`/dev/null`, so importing from the GUI cannot accidentally put `gpudebug` into its interactive REPL
when QRenderDoc inherited a terminal.

For a command-line smoke test, normalize a trace into a thin RDC:

```sh
renderdoccmd convert \
  -f build/metal-trace-fixture/captures/metal_trace_fixture.gputrace \
  -i gputrace -c rdc -o build/metal-trace-fixture/metal_trace_fixture.rdc
```

QRenderDoc treats Apple GPU Trace as a directly openable capture format. Use **File -> Open
Capture**, drag a `.gputrace` bundle onto the window, or pass its path on the command line.
QRenderDoc recognizes the registered importer, performs the conversion on its replay worker, and
opens the temporary thin RDC while retaining the original trace path for lazy resource fetches. The
separate **File -> Import From -> Apple GPU Trace** action remains available as an equivalent
explicit route.

Metal captures use a dedicated read-only pipeline panel that shows the current event and resource
counts together with every reported feature and its availability reason. When texture fetch is
available, the Texture Viewer lazily exports the selected resource, creates a local proxy texture,
and supports normal display, channel, range, thumbnail, and pixel-picking operations. Imported
actions explicitly carry no structured RDC chunk so the API Inspector does not present a fabricated
API call. QRenderDoc labels this mode as **read-only inspection** in its title without presenting a
modal degraded-capture notice. Chunkless leaf actions remain ordinary inspection actions rather
than fake markers, so empty Metal encoders can be selected safely from the event browser, timeline,
and marker breadcrumbs.

## Shared replay API contract

Metal now has a first-class public pipeline model (`MetalPipe::State`) alongside D3D11, D3D12,
OpenGL, and Vulkan. It carries command objects, render and compute pipelines, vertex/index input,
vertex/fragment/compute shaders and argument bindings, raster/depth/stencil/blend state, render-pass
attachments, and shader messages. The state is serialized by replay proxies and is available to
Python as `MetalState` through `ReplayController.GetMetalPipelineState()`; the generic `PipeState`
also recognises Metal and exposes the common shader, vertex input, target, raster, depth/stencil,
and blend queries.

The existence of the shared state object is not a claim that a particular source populated every
field. `APIProperties.features` contains one `ReplayFeatureCapability` per explicitly reported
feature, and `HasFeature()` / `FeatureUnavailableReason()` provide the same contract to C++ and
Python callers. The current Apple GPU Trace inspection driver reports normalized buffer and texture
fetch as available only when the Apple replayer became ready, and gives source-specific reasons for
unavailable resource fetch, executable replay, pipeline normalization, shader
inspection, profiling, pixel history, overlays, post-VS, shader debugging/replacement, and custom
shaders. Native Metal containers use the same manifest and public state vocabulary but still
reject creation until executable replay is implemented.

## Current limitations

- These artifacts validate public Apple GPU Trace inspection only. RenderDoc does not yet claim
  native Metal executable replay, shader debugging, overlays, counters, or capture portability.
- GPU Trace replay and resource fetching are device, GPU-family, and OS sensitive. Static tree
  browsing may still work when replay or fetch is unavailable.
- Texture inspection uses Apple's exported base-level PNG preview. Raw Metal texture storage,
  additional mip levels, array/cube slices, MSAA samples, and original depth/stencil precision are
  not yet available through this bridge.
- The reference trace has no embedded profiling session. `profile load` returns a structured
  unsuccessful result with `no profiling sessions in trace`; compute and performance timeline
  nodes are absent.
- The fixture covers render-only Metal 3 primitives required by A0. Compute, mesh/tile shaders,
  ray tracing, MetalFX, Metal 4 command queues, heaps/aliasing, sparse resources, and indirect
  command buffers remain outside this fixture and the initial bridge profile.
