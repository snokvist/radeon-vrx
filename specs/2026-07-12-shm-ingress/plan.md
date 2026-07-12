# SHM ingress — implementation plan

Companion to `requirements.md`. Anchors are against `772385c` (post-#24 restream).

## Design decisions (settled)

| Decision | Choice | Why |
|---|---|---|
| Source switching | **Rebuild the pipeline when the ingress mode changes** | Selecting a source never touches the pipeline today (`relay_controller_select` just moves an index). UDP↔SHM changes which element chain must run, so it needs a rebuild. Reuses `uv_viewer_restart_pipeline` (`viewer_core.c:136`). Rejected: dual branch + `input-selector` — seamless, but carries a permanently idle branch and switch-time caps/PTS discontinuities. |
| Listing | **Appear on successful attach** | Mirrors UDP peer auto-discovery. A source you cannot play should not be in the list. |
| Stats | **Re-derive what the ring supports, blank the rest** | Zeroed RTP loss/jitter would read as a perfect link. |
| Source identity | **A normal slot in `RelayController.sources[]`, tagged with a kind** | The table is append-only (`sources_count` + `in_use`, no removal/compaction — `relay_controller.c:400`), so indices are stable. The dropdown (`gui_shell.c`), CLI (`cli_shell.c`), and `uv_viewer_select_source` (`viewer_core.c:204`) then need **zero** index-space changes. |
| ABI header | **A C11 mirror of the VFRM layout, checked with `_Static_assert`** | waybeam-link's `frame_shm_format.h` is C++ (`constexpr`, default member initialisers) and cannot be included from C11. |

`RelayController` becomes the *source registry* (it already is, in practice) while
staying the *UDP* reader; the SHM reader is a separate peer module.

## Step 1 — VFRM ABI header

**New:** `include/frame_shm_format.h`.

Plain-C mirror of `protocols/frame-shm.md`: magic/version, header field offsets,
`slot_count`/`slot_data_size` defaults, `align8` stride helper, and

```c
typedef struct { uint32_t pts; uint8_t codec; uint8_t flags; uint16_t reserved; } VencFrameMeta;
_Static_assert(sizeof(VencFrameMeta) == 8, "VencFrameMeta must be 8 bytes");
```

plus `UV_FRAME_CODEC_H265 (0x01)` and `UV_FRAME_FLAG_IDR (0x01)`. Offsets stay
byte-literal constants (not a struct) so no packing assumptions can drift.

*Verify:* `make` — header compiles standalone in C11; offsets match
`protocols/frame-shm.md` §Header by inspection.

## Step 2 — SHM reader module

**New:** `src/shm_ingress.c` (+ declarations in `src/uv_internal.h`).
Model it 1:1 on `SidecarController` (`src/sidecar.c`): own thread, own `GMutex`,
`_init` / `_start` / `_stop` / `_deinit` / `_snapshot`.

```c
typedef struct {
    char      name[UV_SHM_NAME_MAX];   /* normalised to a leading '/' */
    gboolean  enabled;
    /* mapping */
    void     *base; size_t map_size;
    uint32_t  slot_count, slot_data_size; size_t stride;
    dev_t     st_dev; ino_t st_ino;    /* captured at attach, for restart detect */
    gboolean  attached;
    /* thread */
    GThread  *thread; volatile gboolean stop;
    GMutex    lock;
    /* sink */
    GstAppSrc *appsrc;                 /* set/cleared like relay_controller_set_appsrc */
    RelayController *registry;         /* to register the source + push stats */
    /* counters */
    uint64_t frames, bytes, full_drops_seen, oversize_drops, bad_slots, reattaches;
} ShmIngress;
```

Thread loop:

1. **Not attached** → try attach (R1) every 500 ms. On success: capture
   `(st_dev, st_ino)`, register the source (Step 3), emit `SOURCE_ADDED`.
2. **Attached** → drain: acquire-load `write_idx`; while `read_idx != write_idx`,
   copy `length`, reject `length > slot_data_size` (`bad_slots++`, advance
   `read_idx`, continue), copy `[meta][annex-b]`, release-store `read_idx+1`.
3. **Empty** → park on the futex: store `consumer_waiting = 1` (SEQ_CST), load
   `futex_seq`, `FUTEX_WAIT` (**shared**, no `PRIVATE` flag) with a 100 ms
   timeout, store `consumer_waiting = 0`. The timeout tick both closes the
   store/load race and gives us a place to run the restart check.
4. **Starved ≥ 500 ms** → re-`shm_open` + `fstat`; if `(st_dev, st_ino)` differs
   or `init_complete != 1` → `munmap`, mark detached, `reattaches++`, go to 1.
   Never `shm_unlink` (R7/Constraints).

Per frame: validate `meta.codec == UV_FRAME_CODEC_H265` (else drop, count),
strip the 8 bytes, `gst_buffer_fill` a new buffer with the Annex-B body,
`gst_app_src_push_buffer`. Do **not** set `GST_BUFFER_PTS` from `meta.pts` — it is
a wrapping encoder clock and survives neither a wrap nor a producer restart;
let `appsrc do-timestamp=TRUE is-live=TRUE format=TIME` stamp on arrival, and keep
`meta.pts`/`meta.flags` for stats only.

Reference consumer to crib from: `waybeam-link/tools/frame_shm_gst_bench.cpp:134-224`
(attach → `read_frame` → strip meta → `gst_app_src_push_buffer`) and
`waybeam-link/io/src/frame_shm.cpp:150-345` (attach validation, read loop, futex
protocol, `backing_object_current()`).

*Verify:* build clean; with `frame_shm_gst_bench` (or venc's
`tools/frame_shm_consumer_test`) producing into a scratch ring, a debug log line
per frame shows monotonic frames with plausible sizes and IDR flags.

## Step 3 — Source registry: a kind tag

**Edit:** `src/uv_internal.h` (`UvRelaySource`, ~line 28) — add
`UvSourceKind kind;` (`UV_SOURCE_UDP` / `UV_SOURCE_SHM`) and a
`char label[UV_VIEWER_ADDR_MAX];` used verbatim for SHM (`shm:/venc_frame_out`);
UDP keeps formatting from `addr`.

**Edit:** `src/relay_controller.c` — new
`int relay_controller_register_shm(RelayController *rc, const char *label)`:
takes `rc->lock`, appends an `in_use` slot with `kind = UV_SOURCE_SHM`, emits
`UV_VIEWER_EVENT_SOURCE_ADDED`, returns the index. Plus
`void relay_controller_shm_frame(RelayController *rc, int idx, const uint8_t *au, size_t len, const VencFrameMeta *meta)`
which updates that slot's `rx_bytes`, bitrate EMA, `last_seen_us`, marker-frame
counter (one AU = one marker frame — reuse the existing
`source_record_marker_frame()` path so input-FPS lights up unchanged) and walks
Annex-B start codes for the HEVC NAL counters (`(byte >> 1) & 0x3F` — same
extraction the RFC 7798 walker already does at `relay_controller.c:620`).

**Edit:** `uv_internal_populate_source_stats` (`src/relay_controller.c`, declared
`uv_internal.h:281`) — for `UV_SOURCE_SHM`, leave the RTP-only fields at a
sentinel (`-1` / `UINT64_MAX`) so the GUI can print `—` instead of `0`.

**Edit:** `include/uv_viewer.h` (`UvSourceStats`, line 67) — add
`UvSourceKind kind;` and the ring-health block (`shm_attached`, `shm_fill_pct`,
`shm_full_drops`, `shm_oversize_drops`, `shm_bad_slots`, `shm_reattaches`).
`shm_ingress` fills these via a snapshot under its own lock.

*Verify:* run with no producer → source list unchanged from today. Start a
producer → an `N: shm:/venc_frame_out` row appears in the dropdown and CLI `l`.

## Step 4 — Pipeline: the Annex-B ingress branch

**Edit:** `src/pipeline_builder.c`, `build_pipeline` (line 394) and the
`PipelineController` struct.

Add `UvIngressMode ingress_mode` to `PipelineController`. Branch element creation
and linking:

- `UV_INGRESS_UDP` (today, unchanged):
  `appsrc(application/x-rtp) → queue_ingress → tee → cf_rtp_video → jbuf_video → depay → h265parse → h265caps → decoder → …`
- `UV_INGRESS_SHM`:
  `appsrc(video/x-h265,stream-format=byte-stream,alignment=au) → queue_ingress → h265parse → h265caps → decoder → …`

Keep `queue_ingress` in both (its stats feed `stats->queue0`) and keep everything
from `h265caps` downstream **byte-identical**, so the decoder src pad probe
(`pipeline_builder.c:218`, installed ~:863) and the caps-change/sink-bounce
recovery keep working untouched. The caps string already exists at
`pipeline_builder.c:571`. SHM `appsrc`: `is-live=TRUE`, `format=TIME`,
`do-timestamp=TRUE`.

`on_need_data`/`on_enough_data` (`:262`) currently call
`relay_controller_set_push_enabled()` — in SHM mode they must gate the SHM feeder
instead. Route both through a mode-aware `pipeline_set_push_enabled()`.

*Verify:* `GST_DEBUG=3` shows no "not-linked"/"not-negotiated"; the SHM branch
reaches PLAYING and the decoder pad probe reports FPS.

## Step 5 — Wiring and mode switching

**Edit:** `src/viewer_core.c`.

- `uv_viewer_new` (:44) — `shm_ingress_init` after `sidecar_controller_init`;
  hand it `&viewer->relay` as the registry.
- `uv_viewer_start` (:82) — instead of unconditionally
  `relay_controller_set_appsrc(pipeline_controller_get_appsrc(...))` at **:95**,
  wire the appsrc to the reader that matches the current ingress mode and pass
  `NULL` to the other. Start the SHM reader whenever `shm_enabled` (it must run
  even when a UDP source is selected — otherwise the ring never attaches and
  never appears in the list).
- `uv_viewer_stop` (:110) — stop the SHM reader in the sidecar/relay order.
- `uv_viewer_restart_pipeline` (:136) — same use-after-free discipline the relay
  already gets: `shm_ingress_set_appsrc(NULL)` **before** `pipeline_controller_deinit`
  (the comment at :152 explains why), re-wire after re-init at :181.
- `uv_viewer_select_source` (:204) — new logic:

  ```c
  UvSourceKind k = relay_controller_source_kind(&viewer->relay, index);
  UvIngressMode want = (k == UV_SOURCE_SHM) ? UV_INGRESS_SHM : UV_INGRESS_UDP;
  if (want != pipeline_controller_ingress_mode(&viewer->pipeline)) {
      pipeline_controller_set_ingress_mode(&viewer->pipeline, want);
      if (!uv_viewer_restart_pipeline(viewer, error)) return FALSE;   /* rewires appsrc */
  }
  return relay_controller_select(&viewer->relay, index, error);       /* unchanged */
  ```

  Failure to restart leaves the previous source selected and surfaces
  `UV_VIEWER_EVENT_PIPELINE_ERROR` — do not leave a half-torn pipeline selected.

*Verify:* select SHM → UDP → SHM in a loop 10×; video returns each time, no
leaked threads (`ls /proc/$(pidof udp-h265-viewer)/task | wc -l` stable), no
GStreamer criticals.

## Step 6 — Config and Settings UI

**Edit:** `include/uv_viewer.h` `UvViewerConfig` (line 33) — append
`gboolean shm_enabled;` and `char shm_name[UV_SHM_NAME_MAX];`
(default: disabled, `venc_frame_out`). **Edit:** `src/main.c` argv parsing —
`--shm-name <name>` / `--shm` to match the CLI-first config pattern.

**Edit:** `src/gui_shell.c` — a "SHM Ingress" section in `build_settings_page`
(:3252), following the Restream block verbatim: a `.uv-info` header label with
`margin-top 6` spanning both columns, then `GtkCheckButton *shm_toggle` and
`GtkEntry *shm_name_entry` (placeholder `venc_frame_out`, tooltip naming
`/dev/shm/<name>`) at grid col 1, on the rows after Restream. Then:

1. widgets on `GuiContext` (near the settings widgets at :53-71);
2. cfg → widgets in `sync_settings_controls`;
3. widgets → `new_cfg` in `on_settings_apply_clicked` (:2870);
4. **add both fields to the "settings unchanged" equality short-circuit**
   (~:2780-2802) — otherwise changing only the ring name silently does nothing.
   This is the step that gets forgotten;
5. NULL the pointers in `on_app_shutdown`.

Grey out the Restream controls and the sidecar panel while the selected source is
`UV_SOURCE_SHM` (non-goals), and render RTP-only stat cells as `—` on the
sentinel values from Step 3. Add the ring-health readout to the Stats tab.

*Verify:* change only the ring name → Apply → the viewer rebuilds and attaches to
the new object. Restream toggle is insensitive while SHM is selected.

## Step 7 — Build

**Edit:** `Makefile` — add `src/shm_ingress.c` to `SRCS` (:18-26); add `-lrt` and
`-pthread` to the link line (:34) and `-pthread` to `CFLAGS`. `_GNU_SOURCE` is set
in `shm_ingress.c` only (the pattern `cli_shell.c:1` already uses), not globally.

*Verify:* `make clean && make 2>&1 | grep -E '(error|warning)'` is empty
(`-Wall -Wextra` is already on).

## Risks

- **Latency.** waybeam-link emits a frame the instant its FEC block decodes; it
  does no pacing. With `do-timestamp` + a live appsrc and `sync_to_clock=false`,
  frames should render immediately. If the sink instead builds a growing latency
  queue, revisit `is-live`/`sync` before adding a jitter buffer — a frame ring has
  nothing to jitter-buffer.
- **Slot size.** 512 KiB per slot; a 4K IDR at high bitrate can exceed it and the
  *producer* drops that frame (`oversize_drops`). We can only report it.
- **Futex is shared, not private.** Using `FUTEX_PRIVATE_FLAG` would silently
  never wake (different keying). Polling with a short sleep is an acceptable
  fallback if the futex path misbehaves — the bench does exactly that.
- **`sources_count` is monotonic.** A ring that attaches, drops, and re-attaches
  must reuse its slot, not append a second one. Key the reuse on the label.

## Follow-ups (outside this branch)

- Coordination repo: add radeon-vrx to the consumer table in
  `protocols/frame-shm.md` and to `roadmaps/radeon-vrx.md`.
- Sidecar-host override so venc telemetry works for a SHM source.
- RTP-packetised restream for SHM sources.
