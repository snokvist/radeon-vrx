# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build & Run

```bash
make                         # build ./udp-h265-viewer
make clean && make           # full rebuild after header/flag changes
sudo make install            # install to $PREFIX/bin (default /usr/local) plus desktop entry
./udp-h265-viewer [OPTIONS]  # see README.md for the full CLI option table
```

There is no automated test harness. Validate changes by pointing the viewer at a `gst-launch-1.0` test stream (recipe in `README.md`) and watching the GUI Monitor tab. Capture decoder FPS / jitter before and after when changes touch QoS-sensitive paths.

`src/cli_shell.c` exists but is **not** wired into the Makefile — `gui_shell.c` is the only shell built today. If you need text-mode telemetry, add `cli_shell.c` to `SRCS` in `Makefile` and call `uv_cli_run` from `main.c`.

## Architecture

The viewer is built around a single `UvViewer` (defined in `src/uv_internal.h`) that owns three long-lived subsystems plus shared stats containers. Public API lives in `include/uv_viewer.h`; everything else is private and lives behind `src/uv_internal.h`.

```
main.c → uv_viewer_new() → relay_controller + pipeline_controller + decoder/qos stats
                            ↓                       ↓
                    own UDP recv thread      own GstBus GMainLoop thread
                            ↓                       ↓
                            └──── GstAppSrc ────────┘
                            (relay pushes RTP buffers into pipeline)

gui_shell.c (GTK 4 main thread) ── polls uv_viewer_get_stats() on a 200 ms timer
```

Key design points:

- **The relay does its own UDP `recvfrom`** (not `udpsrc`). `relay_controller.c` runs a dedicated thread that demultiplexes by source address into `UvRelaySource` slots, tracks RTP sequence/jitter/marker stats, and forwards the *currently selected* source's packets into the pipeline via `GstAppSrc`. Switching sources is instantaneous and per-source counters keep accumulating in the background.
- **The pipeline thread** (`pipeline_builder.c`) owns a `GMainLoop` on its own thread for bus messages and dynamic element rebuilds (`pipeline_controller_update`). The decoder pad has a buffer probe that calls `uv_internal_decoder_stats_push_frame` — that's where decoded-frame timestamps are recorded.
- **Stats are collected from two places.** `relay_controller_snapshot` fills in per-source RTP stats; `pipeline_controller_snapshot` fills in queue/decoder/QoS/audio fields. The GUI/CLI assembles them via `uv_viewer_get_stats()`.

### FPS computation (load-bearing detail)

Both the per-source "input FPS" (RTP marker bits) and the "decoder FPS" (decoded-frame probe) use a sliding window of frame timestamps:

- Each frame event appends `g_get_monotonic_time()` into a circular buffer of size `UV_SOURCE_FRAME_FPS_WINDOW_SAMPLES` (relay) or `UV_DECODER_FPS_WINDOW_SAMPLES` (decoder), both 512.
- At snapshot time the consumer walks the buffer and counts entries that fall within a 1 s window, then reports `(count - 1) / (last - first)`.
- This replaces an older snapshot-delta approach that was sensitive to GTK timer jitter — do not regress to "frames since last snapshot" math, it produces phantom dips.

The relay's marker-frame counting and frame-block byte accumulation **must** be gated on `unique_packet` (i.e. the RTP sequence wasn't already in the dedup window). Counting duplicates inflates both metrics.

### Locking & threading rules

- `RelayController.lock`, `PipelineController` element fields, `DecoderStats.lock`, and `QoSDatabase.lock` are independent — never hold more than one at a time.
- The relay thread is the only writer of `UvRelaySource` fields; readers (snapshots) must hold `RelayController.lock`.
- The decoder pad probe runs on a GStreamer streaming thread — it only touches `DecoderStats` under its own lock.
- GTK calls happen only from the GUI main thread. Stats polling pulls a snapshot copy; do not retain pointers from `UvViewerStats` past the next call.

### Public-API stability

`include/uv_viewer.h` is the contract for any external consumer. When extending `UvSourceStats`, `UvViewerStats`, or `UvViewerConfig`, append fields rather than reordering — downstream binaries link against this layout.

## Conventions (from AGENTS.md)

- C11 with GCC, four-space indent, K&R braces, `snake_case` internals, ALL_CAPS only for macros.
- Keep `-Wall -Wextra` clean.
- Comment only non-obvious or cross-thread behavior.
- Commit subjects: short imperative, ≤72 chars; mention QoS-impacting changes in the body. Rebuild before opening a PR and note manual test coverage.

## Runtime notes

- Relay binds `0.0.0.0` — firewall the host when ingesting real feeds.
- Headless runs auto-fall back to `fakesink` so diagnostics still work; `--video-sink fakesink` forces it.
- `GST_DEBUG=2 ./udp-h265-viewer` is the standard way to inspect pipeline negotiation / QoS messages.
