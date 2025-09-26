# Repository Guidelines

## Project Structure & Module Organization
Public APIs live in `include/uv_viewer.h`, exposing the reusable viewing library. Implementation is split across `src/viewer_core.c` (orchestration), `src/relay_controller.c` (UDP discovery + forwarding), `src/pipeline_builder.c` (GStreamer graph lifecycle), `src/stats.c` (decoder/QoS aggregation), and `src/logging.c`. The CLI harness sits in `src/cli_shell.c` with `src/main.c` limited to argument parsing and wiring. Build artefacts land at the repo root as `udp-h265-viewer`; legacy experiments stay in `gst-mix/`.

## Build, Test, and Development Commands
Install prerequisites on Debian/Ubuntu with `sudo apt install build-essential pkg-config libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev gstreamer1.0-vaapi`. Build everything via `make` (or `make clean && make` to force a rebuild); the command links all modules automatically. Run locally with `GST_DEBUG=2 ./udp-h265-viewer --listen-port 5600` for verbose logging. The binary falls back to `fakesink` when no display is available, and honours `--payload`, `--clockrate`, `--sync`, and `--no-sync` flags to match stream metadata.

## Coding Style & Naming Conventions
Write C11 code, keep `-Wall -Wextra` clean, and prefer four-space indentation with K&R braces. Use `snake_case` for internal symbols, reserve ALL_CAPS for macros, and keep public structs in `include/uv_viewer.h` stable. Module boundaries matter: touch `src/relay_controller.c` only for UDP/source changes, `src/pipeline_builder.c` for pipeline tweaks, and `src/viewer_core.c` for cross-module orchestration. Add brief comments only when behaviour is non-obvious or cross-thread synchronization requires explanation.

## Testing Guidelines
No automated tests yet. Validate by piping a known stream into the relay and driving the CLI: `gst-launch-1.0 videotestsrc is-live=true ! video/x-raw,framerate=30/1 ! x265enc tune=zerolatency bitrate=4000 ! rtph265pay pt=97 config-interval=-1 ! udpsink host=127.0.0.1 port=5600`. Exercise `l`, `n`, `s <i>`, `stats`, and `q` to ensure source cycling and stats aggregation behave. Capture QoS output before/after changes and confirm decoder FPS and jitter numbers remain sane.

## Commit & Pull Request Guidelines
Adopt short imperative commit subjects (≤72 chars) with body context when behaviour changes, noting before/after outcomes and any QoS or decoder metrics affected. For pull requests, summarise architectural impact (e.g., touched modules), call out new dependencies, list the build/test commands executed (`make`, `gst-launch` pipelines), and attach relevant `stats` snapshots when output shifts. Rebuild before requesting review to ensure the aggregated library still links cleanly.

## Security & Runtime Notes
The relay binds 0.0.0.0 on the configured port; firewall or isolate the host when ingesting real camera feeds. Headless runs default to `fakesink`, whereas desktops pick the first available video sink. Never commit packet captures or credentials; stash samples under ignored paths while debugging.
