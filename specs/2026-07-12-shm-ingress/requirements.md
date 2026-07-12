# SHM ingress — requirements

Status: DRAFT (not implemented)
Branch: `feature/shm-ingress`
Date: 2026-07-12

## Problem

radeon-vrx can only ingest RTP over UDP. When the ground link is
[waybeam-link](https://github.com/snokvist/waybeam-link) running a `frame-shm`
RX egress, the received video never becomes RTP: waybeam-link reassembles whole
Annex-B access units (FEC-decoded at frame boundaries) and publishes them into a
POSIX shared-memory ring. There is no UDP datagram to listen to, so today that
stream is undisplayable in radeon-vrx.

The waybeam-link RX egress is configured as:

```json
"streams": [
  { "stream_id": 0, "stream_type": "RTP", "dir": "out", "originator": 17,
    "bind": { "kind": "frame-shm", "name": "venc_frame_out" } }
]
```

which publishes the POSIX object `/venc_frame_out` (`/dev/shm/venc_frame_out`).

## Goal

Consume that ring directly, decode it, and present it as a normal entry in the
existing **Sources** list next to the auto-discovered UDP peers — same dropdown,
same selection model, same stats surface where the stats still mean something.

## Wire contract (fixed — do not renegotiate)

The ring is the **VFRM** format, canonically specified in the coordination repo
at `protocols/frame-shm.md`, mirrored in waybeam-link at
`core/include/wblink/frame_shm_format.h` and produced by waybeam_venc's
`include/venc_frame_ring.h`. radeon-vrx is a **pure consumer** and must not
extend it.

- Object: `/<name>`, mmap'd `MAP_SHARED`. Native-endian, same-host only.
- Header: 192 bytes. magic `0x5646524D` ("VFRM") @0, version `1` @4,
  `slot_count` @8 (power of two, default 16), `slot_data_size` @12
  (default 512 KiB, **includes** the 8-byte meta), `total_size` @16,
  `epoch` @20, `init_complete` @24, `write_idx` u64 @64, `futex_seq` u32 @72,
  `read_idx` u64 @128, `consumer_waiting` u32 @136.
- Slot stride: `align8(4 + slot_data_size)` (= 524296 by default);
  slot `i` at `192 + i*stride`, laid out `[u32 length][VencFrameMeta 8B][Annex-B …]`.
  `length` counts the meta **plus** the frame bytes.
- `VencFrameMeta`: `pts` u32 @0 (encoder clock, ms-ish, **wraps**),
  `codec` u8 @4 (`0x01` = H.265, only value emitted), `flags` u8 @5
  (bit 0 = IDR), `reserved` u16 @6 (must be 0).
- Payload: one complete access unit, raw Annex-B with start codes preserved.
  Never a partial frame.
- SPSC: acquire-load `write_idx`, read slot at `read_idx & (slot_count-1)`,
  release-store `read_idx+1`. Indices are free-running u64 counters.
- Wake: producer bumps `futex_seq` and, if `consumer_waiting != 0`, issues a
  **shared** (not `FUTEX_PRIVATE_FLAG`) `FUTEX_WAKE` on `futex_seq`.
- Backpressure is **drop, never block** — a slow consumer loses frames, it never
  stalls the link.

## Functional requirements

**R1 — Attach.** Given a configured ring name, a background reader attaches to
`/<name>`: `shm_open(O_RDWR)` → `fstat` → `mmap` → acquire-load `init_complete`
→ validate magic, version, power-of-two `slot_count`, nonzero `slot_data_size`,
and `192 + slot_count*align8(4+slot_data_size) == st_size == total_size`.
Any failure is a non-fatal "not attached" state that is retried.

**R2 — Appear as a Source.** On successful attach the ring is registered in the
existing source table and emits `UV_VIEWER_EVENT_SOURCE_ADDED`, so it shows up in
the GUI dropdown and the CLI `l`/`s <index>` commands with no change to those
call sites. Its display identity is `shm:/<name>` (occupying the `address` field).
It is **not** listed while unattached.

**R3 — Decode.** Selecting the SHM source plays it: frames are pushed, meta
stripped, into an `appsrc` with caps
`video/x-h265,stream-format=byte-stream,alignment=au`, through `h265parse` into
the same decoder + sink chain the UDP path already uses. The RTP-only elements
(tee, RTP capsfilter, `rtpjitterbuffer`, `rtph265depay`) are **not** in the SHM
chain.

**R4 — Switching.** Selecting a source whose kind differs from the running
ingress mode rebuilds the pipeline with the correct ingress branch. UDP↔UDP
selection stays instant and pipeline-free, exactly as today. A UDP↔SHM switch may
cost a decoder re-init and a black frame; it must not leak, dangle the `appsrc`,
or wedge either reader thread.

**R5 — Settings.** The ring name is user-settable in the Settings tab (text
entry, default `venc_frame_out`), alongside an enable toggle. The name
participates in the "settings unchanged" equality short-circuit so that changing
only the name still rebuilds. A leading `/` is optional and normalised.

**R6 — Stats.** For the SHM source, re-derive what the ring can support and
blank what it cannot:
- *Derived:* frames/s (one AU = one frame), frame size, inbound bitrate, HEVC NAL
  composition and `seconds_since_keyframe` (parse Annex-B start codes directly;
  NAL type = `(byte >> 1) & 0x3F`), time since last frame.
- *Blank (`—`), never zero:* `rtp_lost/duplicate/reordered/expected`,
  `rfc3550_jitter_ms`, `rtp_ap_packets`, `rtp_fu_packets`. Zeros would read as
  "a perfect link", which is a lie.
- *New:* ring health — attached/stale, fill %, `full_drops`, `oversize_drops`,
  `bad_slots`, reattach count.
- *Unchanged:* decoder FPS and QoS (pad probe + bus messages are ingress-agnostic).

**R7 — Producer restart.** waybeam-link `shm_unlink`s and re-creates the object
on restart, so an attached consumer keeps mapping a dead orphan inode. The reader
must detect this — re-`shm_open` the name and compare `(st_dev, st_ino)` against
the values captured at attach, plus `init_complete` — then `munmap` and re-attach.
Detection is polled while starved (no frames for ~500 ms), not per frame.

**R8 — Robustness.** No producer, a stale/garbage ring, a corrupt slot
(`length > slot_data_size`), or a producer that vanishes mid-stream must all
degrade gracefully: the viewer stays alive, the UDP path keeps working, and the
SHM source drops out of "attached". A corrupt slot is skipped by advancing
`read_idx` — never stall on it.

## Non-goals (v1)

- **Restream for SHM sources.** Restream (#24) is a verbatim *datagram* copy taken
  in the relay recv loop; a frame ring has no datagrams. The toggle is greyed out
  while a SHM source is selected. RTP-packetising frames to restream them is a
  separate feature.
- **Sidecar telemetry for SHM sources.** The sidecar controller targets the
  selected source's IP; a ring has none. It goes inert. (A future "sidecar host"
  override could restore it.)
- **Writing to the ring / multiple consumers.** SPSC — one consumer, read-only
  intent. Producing a ring is out of scope.
- **Codecs other than H.265.** `codec != 0x01` is counted as bad meta and dropped.
- **Configurable ring geometry.** The producer owns geometry; we read it from
  the header.
- **Audio.** The frame-shm egress is video-only.

## Constraints

- C11, GTK4/GStreamer, hand-written Makefile. New TU needs `_GNU_SOURCE`
  (`syscall(SYS_futex, …)`); link needs `-lrt` (`shm_open`) and explicit
  `-pthread` if raw pthreads are used — neither is linked today.
- Worker→GTK marshalling is `g_idle_add` only (see `sidecar.c`); do not touch GTK
  from the reader thread.
- The producer is the ring's owner. radeon-vrx must **never** `shm_unlink` it.

## Acceptance

See `validation.md`. Headline: with waybeam-link RX running a `frame-shm` egress,
the ring appears in the Sources dropdown, plays at the producer's frame rate with
no persistent artefacts, survives a producer restart without operator action, and
UDP sources continue to work unchanged in the same session.
