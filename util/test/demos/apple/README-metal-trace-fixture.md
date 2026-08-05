# Metal trace fixture

`metal_trace_fixture` is a deterministic Metal 3 workload for validating native RenderDoc capture
and Apple GPU Trace inspection. It labels every important object and contains a compute encoder,
a descriptor-created blit encoder, and two render encoders in one presented command buffer:

- an `MTLEvent` signal command buffer followed by a wait on the frame command buffer;
- a formatted texture view that aliases a captured buffer allocation;
- a compute dispatch that generates the only checkerboard source data;
- a texture-to-texture copy into the parent of a sampled texture view;
- a pre-capture private BC1 texture with a complete mip chain, used as immutable shader input;
- an offscreen color/depth pass with the copied checkerboard, dynamic uniform-buffer update,
  inline bytes, and a direct draw;
- a presentation pass that samples the offscreen texture and performs an indexed draw into a
  `CAMetalLayer` drawable, presented from a scheduled handler like Plume.

Build it independently from the cross-API demo runner:

```sh
cmake -S util/test/demos/apple -B build/metal-trace-fixture -G Ninja \
  -DCMAKE_OSX_ARCHITECTURES=arm64
cmake --build build/metal-trace-fixture
```

Run a bounded smoke test with `--frames N`, or omit the option to render until the window closes:

```sh
build/metal-trace-fixture/bin/metal_trace_fixture --frames 10
```

When launched through RenderDoc injection, pass `--renderdoc-capture` to request one native
RenderDoc capture through the in-process API. The first presentation arms capture and the following
frame is written, so use a limit of at least three frames:

```sh
renderdoccmd capture --wait-for-exit --capture-file /tmp/metal-native \
  build/metal-trace-fixture/bin/metal_trace_fixture --frames 3 --renderdoc-capture
```

Validate the native RDC without creating a replay window. A complete fixture capture reports
sixteen actions (one event submission, one compute dispatch, two draws, and present), twenty-nine
resources, seven buffers, nine textures, and 2,484 fetchable buffer bytes. Strict validation also executes the
whole captured Metal
stream, verifies that a captured buffer GPU address plus texture-view and sampler resource IDs are
rebased for replay, retains a sampler referenced only by its argument-buffer identity, retains an
otherwise-unused buffer solely through queue-level residency-set
membership, preserves private compressed mipmapped initial contents, and requires fetchable,
non-uniform compute, blit, and render textures:

```sh
renderdoccmd replay --validate-metal-fixture /tmp/metal-native_frame1.rdc
```

RenderDoc rebuilds the native command stream through the selected event. The Texture Viewer can
therefore fetch the output produced at each draw instead of reusing the final frame for every event.

For a deterministic command-line capture, launch a fresh process with `MTL_CAPTURE_ENABLED=1`
and `MTLCAPTURE_WAIT_FOR_SIGNAL=1`, then capture its default device boundary. This pauses before
resource creation and captures the first presented command buffer without attach-timing variance.
The exact supported commands and checked-in inspection artifacts are documented in
`docs/metal_support.md`.
