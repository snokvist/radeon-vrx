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
- Request a fresh IDR keyframe from the currently locked source with a single click (or `Ctrl+I`) — useful to recover after a freeze or after joining mid-stream. Targets the encoder's `/request/idr` HTTP endpoint (compatible with [OpenIPC waybeam_venc](https://github.com/OpenIPC/waybeam_venc)).
- **HEVC stream composition counters** parsed live from the RTP payload (RFC 7798): IDR/CRA/trailing-slice/VPS/SPS/PPS/AUD/SEI counts, RFC 7798 aggregation (AP) and fragmentation (FU) packet counts, fragmentation percentage, time since the most recent keyframe, and the gap between the two most recent keyframes. Surfaces intra-refresh / GDR streams as "long time since keyframe" with steady bitrate.
- **Optional encoder-side telemetry** via the waybeam_venc RTP sidecar protocol (`--sidecar`, default UDP 5602). Subscribes to the locked source's sidecar channel and surfaces per-frame ground-truth metrics that the receiver can't infer from the RTP stream alone: frame type (P/I/IDR), QP, scene-complexity (0-255), scene-change flag, GOP state, IDR-insertion events, frames-since-IDR, plus the encoder-side transport queue fill / backpressure flag / drop counters when the encoder also emits the transport trailer.
- Keyboard shortcuts for the most common actions: `Ctrl+I` request IDR, `Ctrl+R` restart pipeline, `Ctrl+N` select next source.
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
| `--idr-port N` | `80` | TCP port used by the GUI's "Request IDR" button (and `Ctrl+I`) to reach the encoder's `/request/idr` endpoint on the currently locked source's IP. |
| `--sidecar` / `--no-sidecar` | `--no-sidecar` | Subscribe to the encoder's RTP sidecar telemetry channel for per-frame QP, complexity, scene-change, and IDR-insertion data. |
| `--sidecar-port N` | `5602` | UDP port on the encoder side that hosts the sidecar listener. |
| `--help` / `-h` | — | Print usage information and exit. |

## Using the GUI
1. **Monitor Tab:** Displays discovered sources, inbound/outbound packet counts, bitrate, jitter, and loss. Select a source to view its video feed. Use the toolbar buttons to advance to the next source, restart the pipeline, or request a fresh IDR keyframe from the locked encoder. The "Request IDR" button (highlighted) fires a non-blocking `GET /request/idr` to `http://<source-ip>:<idr-port>/` — handy when the picture freezes or you join a stream that hasn't sent an IDR yet. Set the port from the Settings tab or `--idr-port`.
2. **Settings Tab:** Adjust listen port, toggle sink synchronization, enable/disable videorate, configure the audio branch, and switch decoder preferences while the pipeline is live. Updates take effect immediately when supported by GStreamer.
3. **Stats Tab:** Time-series charts for inbound bitrate, RTP lost/duplicate/reordered (charted as **packets per second** so spikes are visible, instead of monotonically increasing totals), jitter, input FPS, and decoder FPS. The banner along the top shows the currently locked source and live numbers. Each chart now uses a stable axis (rounded to a "nice" boundary so the Y-axis stops jittering on every redraw), displays time tick labels on the X-axis (`-30s`, `-1m`, `now`), and overlays the window's mean as a dashed line. Time-range options: 30 s / 1 m / 5 m / 10 m. The **Pause** button freezes the charts and labels so you can examine a glitch; **Reset** drops the recorded history.
4. **QoS & Decoder Panels / Frame Blocks Tab:** Track QoS events, jitter measurements, and decoder FPS to diagnose pipeline bottlenecks. The frame block grid helps visualize frame lateness and size distribution during high-load testing. Live/Max readouts use tabular-figure typography with fixed widths so the layout doesn't shimmer as values change. The metric selector now offers four per-frame views: **Lateness (ms)**, **Size (KB)**, **Span (ms)** — the wall-clock time from a frame's first RTP packet to its marker packet, i.e. how long all of the frame's packets took to land — and **Chunks/frame**, the number of distinct release bursts the frame's packets were spread across (1 = a single clean burst; 2+ means the frame straddled FEC blocks). Span and Chunks expose whether a frame was fully delivered inside the FPS budget or dribbled in late.
5. **Frame Release Tab:** Visualizes how the link releases RTP packets in Reed-Solomon FEC-block bursts ("chunks"). Each burst is drawn as a band on a left→right timeline, width proportional to packet count; single-frame bursts use alternating calm shades so you can tell consecutive frames apart, while a **cross-frame burst** (one release carrying packets from two or more frames — the condition that forces a downstream frame drop) is drawn **red and split into per-frame sub-stripes**. A frames-touched histogram (1 / 2 / 3 / 4+) and a summary line (chunk count, overlap count and rate, average packets- and frames-per-chunk) quantify how often bursts straddle frame boundaries. The **Gap (µs)** control sets the idle-gap threshold that separates one burst from the next — tune it to match your link's FEC block cadence. Enable, Pause, and Reset mirror the Frame Blocks controls.

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
