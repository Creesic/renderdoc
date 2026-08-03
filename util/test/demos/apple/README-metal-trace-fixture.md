# Metal trace fixture

`metal_trace_fixture` is a deterministic Metal 3 workload for validating the Apple GPU Trace
inspection contract. It labels every important object and contains two render encoders in one
presented command buffer:

- an offscreen color/depth pass with a sampled checkerboard, dynamic uniform-buffer update,
  inline bytes, and a direct draw;
- a presentation pass that samples the offscreen texture and performs an indexed draw into a
  `CAMetalLayer` drawable.

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

For a deterministic command-line capture, launch a fresh process with `MTL_CAPTURE_ENABLED=1`
and `MTLCAPTURE_WAIT_FOR_SIGNAL=1`, then capture its default device boundary. This pauses before
resource creation and captures the first presented command buffer without attach-timing variance.
The exact supported commands and checked-in inspection artifacts are documented in
`docs/metal_support.md`.
