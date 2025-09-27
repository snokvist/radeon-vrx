# Repository Guidelines

## Project Structure & Module Organization
- Public headers live in `include/uv_viewer.h`; keep structs stable for downstream consumers.
- Core logic splits across `src/viewer_core.c`, `src/relay_controller.c`, `src/pipeline_builder.c`, `src/stats.c`, and `src/logging.c`.
- GUI entry flows through `src/main.c` (args) into `src/gui_shell.c`; experimental artifacts stay under `gst-mix/`.
- Build output is the `udp-h265-viewer` binary at the repo root; ignore temporary captures and credentials.

## Build, Test, and Development Commands
- `make`: compile all modules with current artifacts.
- `make clean && make`: force a full rebuild when headers or build flags change.
- `GST_DEBUG=2 ./udp-h265-viewer --listen-port 5600`: run locally with verbose logging and auto `fakesink` fallback.
- `gst-launch-1.0 videotestsrc ... ! udpsink host=127.0.0.1 port=5600`: generate a test RTP/H.265 stream.
- Use the GUI’s Monitor tab for source selection/stats and the Settings tab to tweak listen port, sink sync, jitter latency, jitter drop policy, and queue depth at runtime.

## Coding Style & Naming Conventions
- Target C11 with GCC; keep `-Wall -Wextra` warnings at zero.
- Use four-space indentation, K&R braces, `snake_case` for internals, and ALL_CAPS only for macros.
- Confine module-specific changes to their owning files; document only non-obvious or cross-thread behavior.

## Testing Guidelines
- No automated harness yet; validate by pairing the `gst-launch-1.0` test stream with the GTK control panel (refresh stats, select sources, cycle via Select Next).
- Capture decoder FPS and jitter before/after changes to confirm QoS stability.

## Commit & Pull Request Guidelines
- Write short imperative commit subjects (≤72 chars) with context on behavior or QoS shifts in the body.
- Rebuild (`make`) before review requests; summarize touched modules, new deps, and share any `stats` snapshots if output changes.
- Reference related issues and note manual test coverage in the PR description.

## Security & Runtime Notes
- The relay binds `0.0.0.0`; isolate or firewall hosts when ingesting real feeds.
- Headless runs default to `fakesink`, while desktops auto-select the first available video sink.
- Never commit packet captures, private streams, or credentials.
