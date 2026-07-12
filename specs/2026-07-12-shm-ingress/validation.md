# SHM ingress — validation

Each check maps to a requirement in `requirements.md`. A step is done only when
its check passes.

## Rigs

**A — Bench, no radio (fast loop).** A local producer writes a VFRM ring; only
radeon-vrx is under test.

```bash
# waybeam-link's bench producer (also the reference consumer for the format)
cd ~/dev/waybeam-link && cmake --build build --target frame_shm_gst_bench
./build/frame_shm_gst_bench --produce --name venc_frame_out    # or tools/frame_shm_udp_bench.sh
ls -l /dev/shm/venc_frame_out                                  # ~8 MB, 16 x 512 KiB
```

**B — Real link.** waybeam_venc (`outgoing.server = frame-shm://venc_frames`) →
`waybeam-link tx` on the vehicle → `waybeam-link rx` on this host with
`"bind": {"kind":"frame-shm","name":"venc_frame_out"}` (see
`waybeam-link/examples/config.frame-shm-rx.sample.json`). Cross-check the link's
own view via `GET :8091/api/v1/stats` (`frames_fast`, `shm_full_drops`,
`dropped_deadline`).

## Checks

| # | Req | Check | Pass |
|---|---|---|---|
| V1 | R1 | Start viewer with no producer, then start one | No error, no crash; source appears within ~1 s of the producer starting |
| V2 | R1/R8 | Point the ring name at a bogus object, then at a truncated/garbage file in `/dev/shm` | Stays unattached, logs a validation failure, UDP path unaffected, no crash |
| V3 | R2 | Rig A running | `shm:/venc_frame_out` is in the GUI dropdown and CLI `l`, at a stable index alongside UDP peers |
| V4 | R3 | Select the SHM source | Video plays; decoder pad probe reports FPS within ±1 of the producer's rate; no persistent artefacts after the first IDR |
| V5 | R4 | Loop select SHM → UDP → SHM 10× with both live | Video returns every time; thread count stable (`ls /proc/$(pidof udp-h265-viewer)/task \| wc -l`); zero GStreamer criticals; UDP↔UDP switching still causes no pipeline restart |
| V6 | R5 | Change only the ring name in Settings → Apply | Viewer rebuilds and attaches to the new object (guards against the "settings unchanged" short-circuit trap) |
| V7 | R6 | SHM source selected, Stats tab | frames/s, frame size, bitrate, NAL counts, `seconds_since_keyframe` are live and plausible; RTP loss/jitter/FU/AP show `—`, **not** `0`; ring health (fill %, `full_drops`, `bad_slots`) is shown |
| V8 | R6 | Force IDRs at the producer (venc `GET /request/idr` ×3) | `hevc_idr_count` rises by exactly 3; `seconds_since_keyframe` resets |
| V9 | R7 | `SIGTERM` the producer, restart it, leave the viewer running | Source goes stale, then re-attaches on its own; `reattaches` increments; video resumes with no operator action |
| V10 | R8 | `SIGKILL` the producer mid-stream | Viewer survives, drops to unattached, no busy-spin (CPU of the reader thread ~0), UDP sources keep playing |
| V11 | R8 | Stall the consumer (pause the viewer process ~2 s with `SIGSTOP`, then `SIGCONT`) | Producer's `full_drops` rises; viewer recovers on the next IDR; the link is never blocked |
| V12 | Non-goal | SHM source selected | Restream toggle and sidecar panel are greyed out (not silently no-op) |
| V13 | R3 | Rig B, 5-minute run | Frame count delivered to the decoder matches waybeam-link's `frames_fast + frames_recovered` minus its own `shm_full_drops`, within the drop counters we report |
| V14 | — | Build | `make clean && make` emits no warnings (`-Wall -Wextra` already on) |
| V15 | — | Leaks | `valgrind --leak-check=full` (or ASan) over a start → SHM select → UDP select → quit cycle: no leaks from the reader, no use-after-free on the appsrc across the restart |

## Regression guard

The UDP path must be untouched. Re-run the pre-existing manual smoke with a plain
venc RTP sender: sources discover, select, stats populate, restream forwards,
sidecar telemetry arrives — all identical to `main`. Any behaviour change on the
UDP side is a bug in this branch, not a trade-off.
