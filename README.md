# UDP H.265 Viewer

## Overview
The UDP H.265 Viewer (`udp-h265-viewer`) is a GTK 4 desktop application built around GStreamer. It listens for RTP/H.265 (HEVC) video streams over UDP, optionally mixes in Opus audio, and exposes realtime telemetry so you can monitor network health, decoder throughput, and queue behavior while inspecting the live video feed. The program was designed for rapid validation of airborne or robotics camera links but works with any unicast RTP source.

> **Insert Screenshot 1:** _Overall GUI layout showing live video pane and side panels._

## Key Features
- Auto-discovers incoming RTP sources on the configured UDP port and lets you switch between them from the GUI.
- Flexible RTP pipeline with optional videorate element to normalize frame cadence.
- Audio branch (Opus over RTP) with configurable jitter buffer latency.
- Multiple decoder backends (auto, Intel VA-API, NVIDIA NVDEC, generic VA-API, software) that can be forced via CLI.
- Detailed stats panes: per-source counters, pipeline QoS, decoder FPS, queue depth, and frame block analysis snapshots.
- Headless resilience: if no display sink is available the pipeline falls back to `fakesink` so you can still capture diagnostics.

> **Insert Screenshot 2:** _Monitor tab with source list and network statistics._

## Prerequisites
Install the build toolchain plus the GTK/GStreamer development headers and runtime plugins. On Ubuntu/Debian:

```bash
sudo apt update
sudo apt install build-essential pkg-config libgtk-4-dev \
    libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
    libglib2.0-dev gstreamer1.0-plugins-base gstreamer1.0-plugins-good \
    gstreamer1.0-plugins-bad gstreamer1.0-libav gstreamer1.0-vaapi \
    gstreamer1.0-gtk4 gstreamer1.0-tools
```

If you plan to exercise hardware decoders, ensure the corresponding VA-API/NVDEC drivers are installed and the user has access to `/dev/dri` or the NVIDIA device nodes.

## Building the Application
The project uses a simple Makefile that drives GCC via `pkg-config`.

```bash
make
```

The resulting `udp-h265-viewer` binary is placed at the repository root. Re-run `make` after pulling updates or modifying the source. Use `make clean && make` when headers or compiler flags change to guarantee a full rebuild.

To integrate the viewer into your desktop environment, install the binary and the accompanying GNOME application shortcut:

```bash
sudo make install
```

By default the binary is installed to `/usr/local/bin` and the desktop entry is registered under `/usr/local/share/applications/udp-h265-viewer.desktop`. Override `PREFIX` (and/or use `DESTDIR`) if you want to install into a different prefix such as a staging directory or your user-local tree.

> **Insert Screenshot 3:** _Terminal capture of a successful build in progress._

## Running the Viewer
Start the program with optional flags to tune the receive pipeline:

```bash
./udp-h265-viewer [OPTIONS]
```

Common examples:

- Listen on the default port (5600) and auto-select a decoder:
  ```bash
  ./udp-h265-viewer
  ```
- Force the NVIDIA decoder on port 5700, disable videorate, and enable audio:
  ```bash
  ./udp-h265-viewer --listen-port 5700 --decoder nvidia --no-videorate --audio
  ```
- Headless diagnostics with clock sync disabled:
  ```bash
  GST_DEBUG=2 ./udp-h265-viewer --no-sync
  ```

To generate a local test stream, run GStreamer in another terminal:

```bash
gst-launch-1.0 videotestsrc ! video/x-raw,framerate=30/1 ! x265enc tune=zerolatency \
    ! rtph265pay pt=97 config-interval=-1 ! udpsink host=127.0.0.1 port=5600
```

> **Insert Screenshot 4:** _Example test pattern as rendered by the viewer._

## Command-Line Options
| Option | Default | Description |
| --- | --- | --- |
| `--listen-port N` | `5600` | UDP port to bind for incoming RTP packets. |
| `--payload PT` | `97` | RTP payload type for the video stream. |
| `--clockrate Hz` | `90000` | RTP clock rate used by the sender. |
| `--sync` / `--no-sync` | `--sync` | Enable or disable sink clock synchronization. |
| `--videorate` / `--no-videorate` | Enabled | Toggle the `videorate` element that enforces a fixed FPS. |
| `--videorate-fps NUM[/DEN]` | `60/1` | Target frame rate when videorate is active. |
| `--audio` / `--no-audio` | Disabled | Toggle the Opus audio receive path. |
| `--audio-payload PT` | `98` | RTP payload type for the Opus stream. |
| `--audio-clockrate Hz` | `48000` | RTP clock rate for audio packets. |
| `--audio-jitter ms` | `8` | Latency window (milliseconds) for the audio jitter buffer. |
| `--decoder auto|intel|nvidia|vaapi|software` | `auto` | Choose the preferred decoder backend. |
| `--help` / `-h` | — | Print usage information and exit. |

## Using the GUI
1. **Monitor Tab:** Displays discovered sources, inbound/outbound packet counts, bitrate, jitter, and loss. Select a source to view its video feed. Use the toolbar buttons to advance to the next source or refresh stats.
2. **Settings Tab:** Adjust listen port, toggle sink synchronization, enable/disable videorate, configure the audio branch, and switch decoder preferences while the pipeline is live. Updates take effect immediately when supported by GStreamer.
3. **QoS & Decoder Panels:** Track QoS events, jitter measurements, and decoder FPS to diagnose pipeline bottlenecks. The frame block grid helps visualize frame lateness and size distribution during high-load testing.

## Development Tips
- Run with `GST_DEBUG=2` (or higher) to inspect pipeline negotiation and QoS messages. Messages are routed to stderr.
- Collect stats snapshots before and after tuning settings to quantify improvements in jitter or frame rate stability.
- When testing over lossy links, experiment with the jitter buffer latency, queue depth, and decoder selection to balance latency against resilience.

## Troubleshooting
- **No video shown:** Ensure the sender is targeting the correct port and payload type, and confirm firewall rules allow UDP ingress. The Monitor tab should list each source as it is detected.
- **Decoder errors:** For hardware backends, verify the necessary drivers and GStreamer VA-API/NVDEC plugins are available. Falling back to `--decoder software` isolates issues to the GPU stack.
- **Choppy playback:** Enable `--videorate` with a conservative FPS, increase the jitter buffer latency, and monitor the queue statistics for drops.

## License
This repository does not currently publish a license file. Contact the maintainers for redistribution and usage terms.
