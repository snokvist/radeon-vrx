#include "uv_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <math.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define UV_FRAME_BLOCK_DEFAULT_WIDTH   60u
#define UV_FRAME_BLOCK_DEFAULT_HEIGHT 100u
#define UV_FRAME_BLOCK_COLOR_BUCKETS  4u
#define UV_FRAME_BLOCK_DEFAULT_SIZE_GREEN_KB  64.0
#define UV_FRAME_BLOCK_DEFAULT_SIZE_YELLOW_KB 256.0
#define UV_FRAME_BLOCK_DEFAULT_SIZE_ORANGE_KB 512.0
#define UV_FRAME_BLOCK_MISSING_SENTINEL (-1.0)

typedef struct UvFrameBlockState {
    guint width;
    guint height;
    guint capacity;
    guint cursor;
    guint filled;
    gboolean have_baseline;
    gboolean snapshot_complete;
    gboolean wrap_pending;
    double thresholds_lateness_ms[3];
    double thresholds_size_kb[3];
    double *lateness_ms; // array length == capacity
    double *size_kb;     // array length == capacity
    double sum_lateness_ms;
    double min_lateness_ms;
    double max_lateness_ms;
    double sum_size_kb;
    double min_size_kb;
    double max_size_kb;
    double expected_frame_ms;
    gboolean have_expected_period;
    guint real_samples;
    guint missing_frames;
    guint color_counts_lateness[UV_FRAME_BLOCK_COLOR_BUCKETS];
    guint color_counts_size[UV_FRAME_BLOCK_COLOR_BUCKETS];
    uint32_t last_frame_ts;
    gint64 last_frame_arrival_us;
} UvFrameBlockState;

static void frame_block_state_reset(UvFrameBlockState *state);

static UvFrameBlockState *frame_block_state_new(guint width, guint height) {
    UvFrameBlockState *state = g_new0(UvFrameBlockState, 1);
    state->width = MAX(width, 1u);
    state->height = MAX(height, 1u);
    state->capacity = state->width * state->height;
    state->lateness_ms = g_new(double, state->capacity);
    state->size_kb = g_new(double, state->capacity);
    frame_block_state_reset(state);
    return state;
}

static void frame_block_state_free(UvFrameBlockState *state) {
    if (!state) return;
    g_free(state->lateness_ms);
    state->lateness_ms = NULL;
    g_free(state->size_kb);
    state->size_kb = NULL;
    g_free(state);
}

static void frame_block_state_reset(UvFrameBlockState *state) {
    if (!state) return;
    state->cursor = 0;
    state->filled = 0;
    state->have_baseline = FALSE;
    state->snapshot_complete = FALSE;
    state->wrap_pending = FALSE;
    state->sum_lateness_ms = 0.0;
    state->min_lateness_ms = 0.0;
    state->max_lateness_ms = 0.0;
    state->sum_size_kb = 0.0;
    state->min_size_kb = 0.0;
    state->max_size_kb = 0.0;
    state->expected_frame_ms = 0.0;
    state->have_expected_period = FALSE;
    state->real_samples = 0;
    state->missing_frames = 0;
    memset(state->color_counts_lateness, 0, sizeof(state->color_counts_lateness));
    memset(state->color_counts_size, 0, sizeof(state->color_counts_size));
    state->last_frame_ts = 0;
    state->last_frame_arrival_us = 0;
    for (guint i = 0; i < state->capacity; i++) {
        state->lateness_ms[i] = NAN;
        state->size_kb[i] = NAN;
    }
}

static guint frame_block_classify_value(const double thresholds[3], double value) {
    if (value <= thresholds[0]) return 0;
    if (value <= thresholds[1]) return 1;
    if (value <= thresholds[2]) return 2;
    return 3;
}

static void frame_block_state_reclassify(UvFrameBlockState *state) {
    if (!state) return;
    memset(state->color_counts_lateness, 0, sizeof(state->color_counts_lateness));
    memset(state->color_counts_size, 0, sizeof(state->color_counts_size));
    if (state->filled == 0) return;
    for (guint i = 0; i < state->filled; i++) {
        double lateness = state->lateness_ms[i];
        if (!isnan(lateness) && lateness >= 0.0) {
            guint bucket_l = frame_block_classify_value(state->thresholds_lateness_ms, lateness);
            if (bucket_l < UV_FRAME_BLOCK_COLOR_BUCKETS) {
                state->color_counts_lateness[bucket_l]++;
            }
        }
        double size = state->size_kb[i];
        if (!isnan(size) && size >= 0.0) {
            guint bucket_s = frame_block_classify_value(state->thresholds_size_kb, size);
            if (bucket_s < UV_FRAME_BLOCK_COLOR_BUCKETS) {
                state->color_counts_size[bucket_s]++;
            }
        }
    }
}

static void frame_block_state_apply_lateness_thresholds(UvFrameBlockState *state, const double thresholds_ms[3]) {
    if (!state || !thresholds_ms) return;
    for (guint i = 0; i < 3; i++) {
        state->thresholds_lateness_ms[i] = thresholds_ms[i];
    }
    frame_block_state_reclassify(state);
}

static void frame_block_state_apply_size_thresholds(UvFrameBlockState *state, const double thresholds_kb[3]) {
    if (!state || !thresholds_kb) return;
    for (guint i = 0; i < 3; i++) {
        state->thresholds_size_kb[i] = thresholds_kb[i];
    }
    frame_block_state_reclassify(state);
}

static void frame_block_state_record(UvFrameBlockState *state,
                                     double lateness_ms,
                                     double size_kb,
                                     gboolean snapshot_mode,
                                     gboolean is_missing) {
    if (!state) return;

    if (state->wrap_pending) {
        frame_block_state_reset(state);
    }

    if (state->filled >= state->capacity) {
        if (snapshot_mode) {
            state->snapshot_complete = TRUE;
            return;
        }
        frame_block_state_reset(state);
    }

    guint idx = state->cursor;
    if (idx >= state->capacity) idx = state->capacity - 1;

    if (is_missing) {
        state->lateness_ms[idx] = UV_FRAME_BLOCK_MISSING_SENTINEL;
        state->size_kb[idx] = UV_FRAME_BLOCK_MISSING_SENTINEL;
        state->missing_frames++;
    } else {
        state->lateness_ms[idx] = lateness_ms;
        state->size_kb[idx] = size_kb;

        if (state->real_samples == 0) {
            state->min_lateness_ms = lateness_ms;
            state->max_lateness_ms = lateness_ms;
            state->sum_lateness_ms = lateness_ms;
            state->min_size_kb = size_kb;
            state->max_size_kb = size_kb;
            state->sum_size_kb = size_kb;
        } else {
            state->sum_lateness_ms += lateness_ms;
            if (lateness_ms < state->min_lateness_ms) state->min_lateness_ms = lateness_ms;
            if (lateness_ms > state->max_lateness_ms) state->max_lateness_ms = lateness_ms;

            state->sum_size_kb += size_kb;
            if (size_kb < state->min_size_kb) state->min_size_kb = size_kb;
            if (size_kb > state->max_size_kb) state->max_size_kb = size_kb;
        }

        state->real_samples++;

        guint bucket_l = frame_block_classify_value(state->thresholds_lateness_ms, lateness_ms);
        if (bucket_l < UV_FRAME_BLOCK_COLOR_BUCKETS) {
            state->color_counts_lateness[bucket_l]++;
        }

        guint bucket_s = frame_block_classify_value(state->thresholds_size_kb, size_kb);
        if (bucket_s < UV_FRAME_BLOCK_COLOR_BUCKETS) {
            state->color_counts_size[bucket_s]++;
        }
    }

    if (state->filled < state->capacity) {
        state->filled++;
    }

    if (state->cursor < state->capacity) {
        state->cursor++;
    }

    if (state->filled >= state->capacity) {
        if (snapshot_mode) {
            state->snapshot_complete = TRUE;
        } else {
            state->wrap_pending = TRUE;
        }
    }
}

static void format_source_label(const struct sockaddr_in *sa,
                                uint16_t local_port,
                                char *out,
                                size_t outlen) {
    if (!sa || !out || outlen == 0) return;
    char ip[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof(ip));
    guint remote_port = ntohs(sa->sin_port);
    if (local_port > 0) {
        if (remote_port > 0) {
            g_snprintf(out, outlen, "%s:%u (local %u)", ip, remote_port, (guint)local_port);
        } else {
            g_snprintf(out, outlen, "%s (local %u)", ip, (guint)local_port);
        }
    } else if (remote_port > 0) {
        g_snprintf(out, outlen, "%s:%u", ip, remote_port);
    } else {
        g_strlcpy(out, ip, outlen);
    }
}

static void relay_source_clear_stats(UvRelaySource *src, gboolean reset_totals) {
    if (!src) return;
    if (reset_totals) {
        src->rx_packets = 0;
        src->rx_bytes = 0;
        src->forwarded_packets = 0;
        src->forwarded_bytes = 0;
    }
    src->last_seen_us = 0;
    src->prev_bytes = 0;
    src->prev_timestamp_us = 0;
    src->rtp_initialized = FALSE;
    src->rtp_cycles = 0;
    src->rtp_last_seq = 0;
    src->rtp_first_ext_seq = 0;
    src->rtp_max_ext_seq = 0;
    src->rtp_unique_packets = 0;
    src->rtp_duplicate_packets = 0;
    src->rtp_reordered_packets = 0;
    for (guint i = 0; i < UV_RTP_WIN_SIZE; ++i) {
        src->rtp_seq_slot[i] = UV_RTP_SLOT_EMPTY;
    }
    src->jitter_initialized = FALSE;
    src->jitter_prev_transit = 0;
    src->jitter_value = 0.0;
    if (src->frame_block) {
        frame_block_state_reset(src->frame_block);
    }
    src->frame_block_accum_bytes = 0;
}

static bool relay_add_or_find(RelayController *rc,
                              const struct sockaddr_in *from,
                              socklen_t fromlen,
                              uint16_t local_port,
                              int *out_idx) {
    for (guint i = 0; i < rc->sources_count; i++) {
        UvRelaySource *slot = &rc->sources[i];
        if (!slot->in_use) continue;
        if (slot->addr.sin_family == from->sin_family &&
            slot->addr.sin_addr.s_addr == from->sin_addr.s_addr &&
            slot->local_port == local_port) {
            if (slot->addr.sin_port != from->sin_port) {
                relay_source_clear_stats(slot, TRUE);
            }
            slot->addr = *from;
            slot->addrlen = fromlen;
            slot->local_port = local_port;
            if (out_idx) *out_idx = (int)i;
            return FALSE;
        }
    }
    if (rc->sources_count >= UV_RELAY_MAX_SOURCES) return FALSE;
    UvRelaySource *ns = &rc->sources[rc->sources_count];
    memset(ns, 0, sizeof(*ns));
    ns->addr = *from;
    ns->addrlen = fromlen;
    ns->in_use = TRUE;
    ns->local_port = local_port;
    relay_source_clear_stats(ns, TRUE);
    if (out_idx) *out_idx = (int)rc->sources_count;
    rc->sources_count++;
    return TRUE;
}

static inline uint32_t rtp_ext_seq(UvRelaySource *s, uint16_t seq16) {
    if (s->rtp_initialized) {
        if (seq16 < s->rtp_last_seq && (uint16_t)(s->rtp_last_seq - seq16) > 30000) {
            s->rtp_cycles += 1u << 16;
        }
    }
    s->rtp_last_seq = seq16;
    return s->rtp_cycles + seq16;
}

static inline uint32_t rtp_now_ts_from_us(int clock_rate, gint64 us) {
    long double ts = ((long double)us * (long double)clock_rate) / 1000000.0L;
    if (ts < 0) ts = 0;
    return (uint32_t)((uint64_t)ts);
}

static inline uint32_t rtp_now_ts(int clock_rate) {
    return rtp_now_ts_from_us(clock_rate, g_get_monotonic_time());
}

static void frame_block_process_packet(RelayController *rc,
                                       UvRelaySource *src,
                                       uint32_t ts,
                                       gboolean marker,
                                       gint64 arrival_us,
                                       int clock_rate,
                                       gboolean is_selected,
                                       uint64_t frame_size_bytes) {
    if (!rc || !src) return;
    if (!marker) return;

    if (!rc->frame_block.enabled || !is_selected) {
        if (src->frame_block) {
            src->frame_block->have_baseline = FALSE;
        }
        return;
    }

    UvFrameBlockState *state = src->frame_block;
    if (!state) {
        state = frame_block_state_new(rc->frame_block.width, rc->frame_block.height);
        frame_block_state_apply_lateness_thresholds(state, rc->frame_block.thresholds_ms);
        frame_block_state_apply_size_thresholds(state, rc->frame_block.thresholds_kb);
        src->frame_block = state;
    }

    if (rc->frame_block.reset_requested) {
        frame_block_state_reset(state);
    }
    if (rc->frame_block.thresholds_dirty_ms) {
        frame_block_state_apply_lateness_thresholds(state, rc->frame_block.thresholds_ms);
        rc->frame_block.thresholds_dirty_ms = FALSE;
    }
    if (rc->frame_block.thresholds_dirty_kb) {
        frame_block_state_apply_size_thresholds(state, rc->frame_block.thresholds_kb);
        rc->frame_block.thresholds_dirty_kb = FALSE;
    }

    if (!state->have_baseline) {
        state->last_frame_ts = ts;
        state->last_frame_arrival_us = arrival_us;
        state->have_baseline = TRUE;
        return;
    }

    guint32 ts_delta = ts - state->last_frame_ts;
    double expected_ms = 0.0;
    if (clock_rate > 0) {
        expected_ms = ((double)ts_delta * 1000.0) / (double)clock_rate;
    }

    guint missing = 0;
    double normalized_expected_ms = expected_ms;
    if (state->have_expected_period && state->expected_frame_ms > 0.0 && expected_ms > 0.0) {
        double ratio = expected_ms / state->expected_frame_ms;
        if (ratio > 1.5) {
            double raw_missing = ratio - 1.0;
            if (raw_missing > 0.0) {
                guint estimate = (guint)(ratio + 0.2);
                if (estimate > 0) missing = estimate - 1;
                if (missing > 64) missing = 64;
                if (missing > 0) {
                    normalized_expected_ms = expected_ms / (double)(missing + 1);
                }
            }
        }
    }

    double arrival_delta_ms = 0.0;
    if (arrival_us > state->last_frame_arrival_us) {
        arrival_delta_ms = (double)(arrival_us - state->last_frame_arrival_us) / 1000.0;
    }

    double lateness_ms = arrival_delta_ms - expected_ms;
    if (lateness_ms < 0.0) lateness_ms = 0.0;

    double size_kb = (double)frame_size_bytes / 1024.0;

    state->last_frame_ts = ts;
    state->last_frame_arrival_us = arrival_us;
    state->have_baseline = TRUE;

    if (rc->frame_block.paused) return;

    if (missing > 0) {
        for (guint m = 0; m < missing; m++) {
            frame_block_state_record(state, 0.0, 0.0, rc->frame_block.snapshot_mode, TRUE);
        }
    }

    frame_block_state_record(state, lateness_ms, size_kb, rc->frame_block.snapshot_mode, FALSE);

    if (normalized_expected_ms > 0.0) {
        if (!state->have_expected_period) {
            state->expected_frame_ms = normalized_expected_ms;
            state->have_expected_period = TRUE;
        } else {
            const double alpha = 0.125;
            state->expected_frame_ms = (1.0 - alpha) * state->expected_frame_ms + alpha * normalized_expected_ms;
        }
    }
}

static inline void rtp_update_stats(RelayController *rc,
                                    UvRelaySource *s,
                                    const unsigned char *p,
                                    size_t len,
                                    int clock_rate,
                                    int primary_payload_type,
                                    gboolean is_selected) {
    if (len < 12) return;
    if ((p[0] & 0xC0) != 0x80) return;

    if (primary_payload_type >= 0) {
        int payload_type = p[1] & 0x7F;
        if (payload_type != primary_payload_type) {
            return; // ignore non-primary RTP payload types for stats tracking
        }
    }

    gboolean marker = (p[1] & 0x80) != 0;

    if (rc->frame_block.enabled && is_selected) {
        s->frame_block_accum_bytes += (uint64_t)len;
    }

    uint16_t seq = (uint16_t)((p[2] << 8) | p[3]);
    uint32_t ts  = (uint32_t)((p[4] << 24) | (p[5] << 16) | (p[6] << 8) | p[7]);

    uint32_t ext = rtp_ext_seq(s, seq);
    if (!s->rtp_initialized) {
        s->rtp_initialized = TRUE;
        s->rtp_first_ext_seq = ext;
        s->rtp_max_ext_seq   = ext;
    }

    guint idx = ext % UV_RTP_WIN_SIZE;
    if (s->rtp_seq_slot[idx] == ext) {
        s->rtp_duplicate_packets++;
    } else {
        if (ext < s->rtp_max_ext_seq) s->rtp_reordered_packets++;
        s->rtp_seq_slot[idx] = ext;
        s->rtp_unique_packets++;
        if (ext > s->rtp_max_ext_seq) s->rtp_max_ext_seq = ext;
    }

    gint64 arrival_us = g_get_monotonic_time();
    uint32_t arrival_ts = rtp_now_ts_from_us(clock_rate, arrival_us);
    uint32_t transit = arrival_ts - ts;
    if (!s->jitter_initialized) {
        s->jitter_initialized = TRUE;
        s->jitter_prev_transit = transit;
    } else {
        int32_t d = (int32_t)(transit - s->jitter_prev_transit);
        if (d < 0) d = -d;
        s->jitter_value += ((double)d - s->jitter_value) / 16.0;
        s->jitter_prev_transit = transit;
    }

    if (marker) {
        uint64_t frame_size_bytes = s->frame_block_accum_bytes;
        frame_block_process_packet(rc, s, ts, marker, arrival_us, clock_rate, is_selected, frame_size_bytes);
        s->frame_block_accum_bytes = 0;
    }
}

static GstFlowReturn relay_push_buffer(RelayController *rc, const unsigned char *buf, size_t len) {
    if (!rc->appsrc) return GST_FLOW_ERROR;
    GstBuffer *gbuf = gst_buffer_new_allocate(NULL, (gsize)len, NULL);
    if (!gbuf) return GST_FLOW_ERROR;
    GstMapInfo map;
    if (gst_buffer_map(gbuf, &map, GST_MAP_WRITE)) {
        memcpy(map.data, buf, len);
        gst_buffer_unmap(gbuf, &map);
    }
    GST_BUFFER_FLAG_SET(gbuf, GST_BUFFER_FLAG_LIVE);
    return gst_app_src_push_buffer(rc->appsrc, gbuf);
}

static gpointer relay_thread_run(gpointer data) {
    RelayController *rc = (RelayController *)data;
    UvViewer *viewer = rc->viewer;

    int ports[1 + UV_VIEWER_MAX_EXTRA_LISTEN_PORTS] = {0};
    guint port_count = 0;
    if (rc->listen_port > 0) {
        ports[port_count++] = rc->listen_port;
    }
    for (guint i = 0; i < rc->extra_listen_port_count && port_count < G_N_ELEMENTS(ports); i++) {
        int port = rc->extra_listen_ports[i];
        if (port <= 0) continue;
        gboolean duplicate = FALSE;
        for (guint j = 0; j < port_count; j++) {
            if (ports[j] == port) {
                duplicate = TRUE;
                break;
            }
        }
        if (!duplicate) {
            ports[port_count++] = port;
        }
    }

    if (port_count == 0) {
        uv_log_error("Relay: no valid UDP listen ports configured");
        return NULL;
    }

    struct pollfd *fds = g_new0(struct pollfd, port_count);
    uint16_t *local_ports = g_new0(uint16_t, port_count);
    if (!fds || !local_ports) {
        uv_log_error("Relay: failed to allocate poll structures");
        goto cleanup;
    }
    unsigned char *buf = NULL;
    guint active_count = 0;

    for (guint i = 0; i < port_count; i++) {
        int port = ports[i];
        gboolean primary = (port == rc->listen_port);

        int fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0) {
            uv_log_error("Relay: socket() failed for port %d: %s", port, g_strerror(errno));
            if (primary) goto cleanup;
            else continue;
        }

        int flags = fcntl(fd, F_GETFL, 0);
        if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        int reuse = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        int rcvbuf = 4 * 1024 * 1024;
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

        struct sockaddr_in bind_addr = {
            .sin_family = AF_INET,
            .sin_port = htons((uint16_t)port),
            .sin_addr.s_addr = htonl(INADDR_ANY)
        };
        if (bind(fd, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
            uv_log_error("Relay: bind() failed on port %d: %s", port, g_strerror(errno));
            close(fd);
            if (primary) goto cleanup;
            else continue;
        }

        uv_log_info("Relay: listening on UDP port %d", port);

        fds[active_count].fd = fd;
        fds[active_count].events = POLLIN;
        local_ports[active_count] = (uint16_t)port;
        active_count++;
    }

    if (active_count == 0) {
        uv_log_error("Relay: failed to bind any UDP ports");
        goto cleanup;
    }

    buf = g_malloc0(UV_RELAY_BUF_SIZE);
    if (!buf) goto cleanup;

    while (rc->running) {
        int pr = poll(fds, active_count, 200);
        if (pr < 0) {
            if (errno == EINTR) continue;
            uv_log_warn("Relay: poll() error: %s", g_strerror(errno));
            break;
        }
        if (pr == 0) continue;

        for (guint i = 0; i < active_count; i++) {
            if (!(fds[i].revents & (POLLIN | POLLERR | POLLHUP | POLLNVAL))) continue;
            if (fds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                uv_log_warn("Relay: poll() socket event 0x%x on port %u", fds[i].revents, (unsigned)local_ports[i]);
            }
            if (!(fds[i].revents & POLLIN)) continue;

            struct sockaddr_in from = {0};
            socklen_t fromlen = sizeof(from);
            ssize_t r = recvfrom(fds[i].fd,
                                 buf,
                                 UV_RELAY_BUF_SIZE,
                                 0,
                                 (struct sockaddr *)&from,
                                 &fromlen);
            if (r < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
                uv_log_warn("Relay: recvfrom() error on port %u: %s",
                            (unsigned)local_ports[i],
                            g_strerror(errno));
                continue;
            }

            gboolean emit_added = FALSE;
            gboolean emit_selected = FALSE;
            int emit_index = -1;
            UvRelaySource snapshot = {0};

            g_mutex_lock(&rc->lock);
            int idx = -1;
            bool is_new = relay_add_or_find(rc, &from, fromlen, local_ports[i], &idx);
            UvRelaySource *src = NULL;
            if (idx >= 0 && (guint)idx < rc->sources_count) src = &rc->sources[idx];
            if (src) {
                src->rx_packets++;
                src->rx_bytes += (uint64_t)r;
                src->last_seen_us = g_get_monotonic_time();
                gboolean is_selected = (idx == rc->selected_index);
                rtp_update_stats(rc,
                                 src,
                                 buf,
                                 (size_t)r,
                                 viewer->config.clock_rate,
                                 viewer->config.payload_type,
                                 is_selected);
            }
            if (is_new && src) {
                char addr[64];
                format_source_label(&src->addr, src->local_port, addr, sizeof(addr));
                uv_log_info("Relay: discovered source [%d] %s", idx, addr);
                emit_added = TRUE;
                emit_index = idx;
                snapshot = *src;
                if (rc->selected_index < 0) {
                    rc->selected_index = idx;
                    emit_selected = TRUE;
                }
            }

            gboolean push_now = rc->push_enabled && rc->selected_index >= 0 && idx == rc->selected_index;
            int push_index = push_now ? idx : -1;
            g_mutex_unlock(&rc->lock);

            if (emit_added) {
                uv_internal_emit_event(viewer, UV_VIEWER_EVENT_SOURCE_ADDED, emit_index, &snapshot, NULL);
            }
            if (emit_selected) {
                uv_internal_emit_event(viewer, UV_VIEWER_EVENT_SOURCE_SELECTED, rc->selected_index, &snapshot, NULL);
            }

            if (push_index >= 0) {
                GstFlowReturn push_ret = relay_push_buffer(rc, buf, (size_t)r);
                if (push_ret != GST_FLOW_OK) {
                    uv_log_warn("Relay: appsrc push returned %s", gst_flow_get_name(push_ret));
                } else {
                    g_mutex_lock(&rc->lock);
                    if (push_index >= 0 && (guint)push_index < rc->sources_count) {
                        UvRelaySource *forward_src = &rc->sources[push_index];
                        if (forward_src->in_use) {
                            forward_src->forwarded_packets++;
                            forward_src->forwarded_bytes += (uint64_t)r;
                        }
                    }
                    g_mutex_unlock(&rc->lock);
                }
            }
        }
    }

cleanup:
    if (buf) g_free(buf);
    if (fds) {
        for (guint i = 0; i < port_count; i++) {
            if (i < active_count && fds[i].fd >= 0) {
                close(fds[i].fd);
                fds[i].fd = -1;
            }
        }
        g_free(fds);
    }
    g_free(local_ports);
    rc->running = 0;
    return NULL;
}

gboolean relay_controller_init(RelayController *rc, struct _UvViewer *viewer) {
    g_return_val_if_fail(rc != NULL, FALSE);
    g_return_val_if_fail(viewer != NULL, FALSE);

    memset(rc, 0, sizeof(*rc));
    g_mutex_init(&rc->lock);
    rc->listen_port = viewer->config.listen_port;
    rc->extra_listen_port_count = 0;
    for (guint i = 0; i < viewer->config.extra_listen_port_count &&
                    rc->extra_listen_port_count < UV_VIEWER_MAX_EXTRA_LISTEN_PORTS; i++) {
        int port = viewer->config.extra_listen_ports[i];
        if (port <= 0 || port > 65535) continue;
        if (port == rc->listen_port) continue;
        gboolean duplicate = FALSE;
        for (guint j = 0; j < rc->extra_listen_port_count; j++) {
            if (rc->extra_listen_ports[j] == port) {
                duplicate = TRUE;
                break;
            }
        }
        if (duplicate) continue;
        rc->extra_listen_ports[rc->extra_listen_port_count++] = port;
    }
    rc->selected_index = -1;
    rc->viewer = viewer;
    rc->frame_block.enabled = FALSE;
    rc->frame_block.paused = FALSE;
    rc->frame_block.snapshot_mode = FALSE;
    rc->frame_block.width = UV_FRAME_BLOCK_DEFAULT_WIDTH;
    rc->frame_block.height = UV_FRAME_BLOCK_DEFAULT_HEIGHT;
    rc->frame_block.thresholds_ms[0] = 5.0;
    rc->frame_block.thresholds_ms[1] = 15.0;
    rc->frame_block.thresholds_ms[2] = 30.0;
    rc->frame_block.thresholds_kb[0] = UV_FRAME_BLOCK_DEFAULT_SIZE_GREEN_KB;
    rc->frame_block.thresholds_kb[1] = UV_FRAME_BLOCK_DEFAULT_SIZE_YELLOW_KB;
    rc->frame_block.thresholds_kb[2] = UV_FRAME_BLOCK_DEFAULT_SIZE_ORANGE_KB;
    rc->frame_block.reset_requested = TRUE;
    rc->frame_block.thresholds_dirty_ms = TRUE;
    rc->frame_block.thresholds_dirty_kb = TRUE;
    return TRUE;
}

void relay_controller_deinit(RelayController *rc) {
    if (!rc) return;
    relay_controller_stop(rc);
    g_mutex_lock(&rc->lock);
    rc->appsrc = NULL;
    for (guint i = 0; i < rc->sources_count; i++) {
        frame_block_state_free(rc->sources[i].frame_block);
        rc->sources[i].frame_block = NULL;
    }
    rc->sources_count = 0;
    rc->selected_index = -1;
    g_mutex_unlock(&rc->lock);
    g_mutex_clear(&rc->lock);
}

gboolean relay_controller_start(RelayController *rc) {
    g_return_val_if_fail(rc != NULL, FALSE);
    if (rc->thread) return TRUE;
    rc->running = 1;
    rc->thread = g_thread_new("uv-relay", relay_thread_run, rc);
    if (!rc->thread) {
        rc->running = 0;
        return FALSE;
    }
    return TRUE;
}

void relay_controller_stop(RelayController *rc) {
    if (!rc) return;
    rc->running = 0;
    if (rc->thread) {
        g_thread_join(rc->thread);
        rc->thread = NULL;
    }
}

gboolean relay_controller_select(RelayController *rc, int index, GError **error) {
    g_return_val_if_fail(rc != NULL, FALSE);
    gboolean valid = FALSE;
    UvRelaySource snapshot = {0};
    g_mutex_lock(&rc->lock);
    if (index >= 0 && (guint)index < rc->sources_count && rc->sources[index].in_use) {
        rc->selected_index = index;
        UvRelaySource *selected_src = &rc->sources[index];
        snapshot = *selected_src;
        if (rc->frame_block.enabled) {
            if (!selected_src->frame_block) {
                selected_src->frame_block = frame_block_state_new(rc->frame_block.width, rc->frame_block.height);
                frame_block_state_apply_lateness_thresholds(selected_src->frame_block, rc->frame_block.thresholds_ms);
                frame_block_state_apply_size_thresholds(selected_src->frame_block, rc->frame_block.thresholds_kb);
            }
            frame_block_state_reset(selected_src->frame_block);
        }
        selected_src->frame_block_accum_bytes = 0;
        valid = TRUE;
    }
    g_mutex_unlock(&rc->lock);
    if (!valid) {
        g_set_error(error, g_quark_from_static_string("uv-viewer"), 1, "Invalid source index %d", index);
        return FALSE;
    }
    uv_internal_emit_event(rc->viewer, UV_VIEWER_EVENT_SOURCE_SELECTED, index, &snapshot, NULL);
    return TRUE;
}

gboolean relay_controller_select_next(RelayController *rc, GError **error) {
    g_return_val_if_fail(rc != NULL, FALSE);
    gboolean success = FALSE;
    int next_index = -1;
    UvRelaySource snapshot = {0};
    g_mutex_lock(&rc->lock);
    if (rc->sources_count > 0) {
        if (rc->selected_index < 0) {
            rc->selected_index = 0;
        } else {
            rc->selected_index = (rc->selected_index + 1) % (int)rc->sources_count;
        }
        next_index = rc->selected_index;
        UvRelaySource *selected_src = &rc->sources[next_index];
        snapshot = *selected_src;
        if (rc->frame_block.enabled) {
            if (!selected_src->frame_block) {
                selected_src->frame_block = frame_block_state_new(rc->frame_block.width, rc->frame_block.height);
                frame_block_state_apply_lateness_thresholds(selected_src->frame_block, rc->frame_block.thresholds_ms);
                frame_block_state_apply_size_thresholds(selected_src->frame_block, rc->frame_block.thresholds_kb);
            }
            frame_block_state_reset(selected_src->frame_block);
        }
        selected_src->frame_block_accum_bytes = 0;
        success = TRUE;
    }
    g_mutex_unlock(&rc->lock);
    if (!success) {
        g_set_error(error, g_quark_from_static_string("uv-viewer"), 2, "No sources available");
        return FALSE;
    }
    uv_internal_emit_event(rc->viewer, UV_VIEWER_EVENT_SOURCE_SELECTED, next_index, &snapshot, NULL);
    return TRUE;
}

int relay_controller_selected(const RelayController *rc) {
    if (!rc) return -1;
    return rc->selected_index;
}

void relay_controller_snapshot(RelayController *rc, UvViewerStats *stats, int clock_rate) {
    if (!rc || !stats) return;
    (void)clock_rate;
    gint64 now_us = g_get_monotonic_time();

    GArray *fb_lateness = stats->frame_block.lateness_ms;
    GArray *fb_sizes = stats->frame_block.frame_size_kb;
    memset(&stats->frame_block, 0, sizeof(stats->frame_block));
    stats->frame_block.lateness_ms = fb_lateness;
    stats->frame_block.frame_size_kb = fb_sizes;
    if (fb_lateness) g_array_set_size(fb_lateness, 0);
    if (fb_sizes) g_array_set_size(fb_sizes, 0);
    stats->frame_block_valid = FALSE;

    g_mutex_lock(&rc->lock);
    for (guint i = 0; i < rc->sources_count; i++) {
        UvRelaySource *src = &rc->sources[i];
        if (!src->in_use) continue;
        UvSourceStats s = {0};
        format_source_label(&src->addr, src->local_port, s.address, sizeof(s.address));
        s.selected = (rc->selected_index == (int)i);
        s.rx_packets = src->rx_packets;
        s.rx_bytes = src->rx_bytes;
        s.forwarded_packets = src->forwarded_packets;
        s.forwarded_bytes = src->forwarded_bytes;
        s.rtp_unique_packets = src->rtp_unique_packets;
        if (src->rtp_initialized) {
            s.rtp_expected_packets = (uint64_t)(src->rtp_max_ext_seq - src->rtp_first_ext_seq + 1);
            if (s.rtp_expected_packets > s.rtp_unique_packets) {
                s.rtp_lost_packets = s.rtp_expected_packets - s.rtp_unique_packets;
            }
        }
        s.rtp_duplicate_packets = src->rtp_duplicate_packets;
        s.rtp_reordered_packets = src->rtp_reordered_packets;
        if (src->jitter_value > 0.0) {
            s.rfc3550_jitter_ms = (src->jitter_value * 1000.0) / (double)MAX(clock_rate, 1);
        }
        if (src->last_seen_us > 0) {
            s.seconds_since_last_seen = (double)(now_us - src->last_seen_us) / 1e6;
        } else {
            s.seconds_since_last_seen = -1.0;
        }

        if (src->prev_timestamp_us != 0 && now_us > src->prev_timestamp_us && src->rx_bytes >= src->prev_bytes) {
            uint64_t dbytes = src->rx_bytes - src->prev_bytes;
            double dt = (now_us - src->prev_timestamp_us) / 1e6;
            if (dt > 0.0) s.inbound_bitrate_bps = (double)dbytes * 8.0 / dt;
        }
        src->prev_bytes = src->rx_bytes;
        src->prev_timestamp_us = now_us;

        g_array_append_val(stats->sources, s);

        if (s.selected) {
            stats->frame_block_valid = TRUE;
            UvFrameBlockStats *fb = &stats->frame_block;
            UvFrameBlockState *state = src->frame_block;

            fb->active = rc->frame_block.enabled;
            fb->paused = rc->frame_block.paused;
            fb->snapshot_mode = rc->frame_block.snapshot_mode;
            fb->snapshot_complete = state ? state->snapshot_complete : FALSE;

            fb->width = state ? state->width : rc->frame_block.width;
            fb->height = state ? state->height : rc->frame_block.height;
            if (fb->width == 0) fb->width = UV_FRAME_BLOCK_DEFAULT_WIDTH;
            if (fb->height == 0) fb->height = UV_FRAME_BLOCK_DEFAULT_HEIGHT;
            guint capacity = fb->width * fb->height;

            if (!fb->lateness_ms) {
                fb->lateness_ms = g_array_new(FALSE, TRUE, sizeof(double));
            }
            if (!fb->frame_size_kb) {
                fb->frame_size_kb = g_array_new(FALSE, TRUE, sizeof(double));
            }
            g_array_set_size(fb->lateness_ms, capacity);
            g_array_set_size(fb->frame_size_kb, capacity);

            if (state && state->lateness_ms && state->size_kb) {
                for (guint k = 0; k < capacity && k < state->capacity; k++) {
                    g_array_index(fb->lateness_ms, double, k) = state->lateness_ms[k];
                    g_array_index(fb->frame_size_kb, double, k) = state->size_kb[k];
                }
                for (guint k = state->capacity; k < capacity; k++) {
                    g_array_index(fb->lateness_ms, double, k) = NAN;
                    g_array_index(fb->frame_size_kb, double, k) = NAN;
                }
            } else {
                for (guint k = 0; k < capacity; k++) {
                    g_array_index(fb->lateness_ms, double, k) = NAN;
                    g_array_index(fb->frame_size_kb, double, k) = NAN;
                }
            }

            memcpy(fb->thresholds_lateness_ms, rc->frame_block.thresholds_ms, sizeof(fb->thresholds_lateness_ms));
            memcpy(fb->thresholds_size_kb, rc->frame_block.thresholds_kb, sizeof(fb->thresholds_size_kb));

            fb->real_frames = state ? state->real_samples : 0;
            fb->missing_frames = state ? state->missing_frames : 0;

            if (state) {
                fb->filled = state->filled;
                fb->next_index = MIN(state->cursor, state->capacity);
                if (state->real_samples > 0) {
                    fb->min_lateness_ms = state->min_lateness_ms;
                    fb->max_lateness_ms = state->max_lateness_ms;
                    fb->avg_lateness_ms = state->sum_lateness_ms / (double)state->real_samples;
                    fb->min_size_kb = state->min_size_kb;
                    fb->max_size_kb = state->max_size_kb;
                    fb->avg_size_kb = state->sum_size_kb / (double)state->real_samples;
                }
            } else {
                fb->filled = 0;
                fb->next_index = 0;
            }

            if (state && state->real_samples > 0) {
                for (guint c = 0; c < UV_FRAME_BLOCK_COLOR_BUCKETS; c++) {
                    fb->color_counts_lateness[c] = state->color_counts_lateness[c];
                    fb->color_counts_size[c] = state->color_counts_size[c];
                }
            } else {
                fb->min_lateness_ms = 0.0;
                fb->max_lateness_ms = 0.0;
                fb->avg_lateness_ms = 0.0;
                fb->min_size_kb = 0.0;
                fb->max_size_kb = 0.0;
                fb->avg_size_kb = 0.0;
                memset(fb->color_counts_lateness, 0, sizeof(fb->color_counts_lateness));
                memset(fb->color_counts_size, 0, sizeof(fb->color_counts_size));
            }
        }
    }
    g_mutex_unlock(&rc->lock);
}

void relay_controller_set_appsrc(RelayController *rc, GstAppSrc *appsrc) {
    if (!rc) return;
    g_mutex_lock(&rc->lock);
    rc->appsrc = appsrc;
    g_mutex_unlock(&rc->lock);
}

void relay_controller_set_push_enabled(RelayController *rc, gboolean enabled) {
    if (!rc) return;
    g_mutex_lock(&rc->lock);
    rc->push_enabled = enabled;
    g_mutex_unlock(&rc->lock);
}

void relay_controller_frame_block_configure(RelayController *rc, gboolean enabled, gboolean snapshot_mode) {
    if (!rc) return;
    g_mutex_lock(&rc->lock);
    rc->frame_block.enabled = enabled;
    rc->frame_block.snapshot_mode = snapshot_mode;
    if (!enabled) {
        rc->frame_block.paused = FALSE;
    }
    rc->frame_block.reset_requested = TRUE;
    for (guint i = 0; i < rc->sources_count; i++) {
        UvRelaySource *src = &rc->sources[i];
        if (src->frame_block) {
            frame_block_state_reset(src->frame_block);
        }
        src->frame_block_accum_bytes = 0;
    }
    rc->frame_block.reset_requested = FALSE;
    g_mutex_unlock(&rc->lock);
}

void relay_controller_frame_block_set_width(RelayController *rc, guint width) {
    if (!rc) return;
    guint clamped = MAX(width, 1u);

    g_mutex_lock(&rc->lock);
    if (rc->frame_block.width == clamped) {
        g_mutex_unlock(&rc->lock);
        return;
    }

    rc->frame_block.width = clamped;
    guint height = rc->frame_block.height ? rc->frame_block.height : UV_FRAME_BLOCK_DEFAULT_HEIGHT;

    for (guint i = 0; i < rc->sources_count; i++) {
        UvRelaySource *src = &rc->sources[i];
        if (!src->frame_block) continue;
        UvFrameBlockState *old_state = src->frame_block;
        src->frame_block = frame_block_state_new(rc->frame_block.width, height);
        frame_block_state_apply_lateness_thresholds(src->frame_block, rc->frame_block.thresholds_ms);
        frame_block_state_apply_size_thresholds(src->frame_block, rc->frame_block.thresholds_kb);
        frame_block_state_free(old_state);
    }

    g_mutex_unlock(&rc->lock);
}

void relay_controller_frame_block_pause(RelayController *rc, gboolean paused) {
    if (!rc) return;
    g_mutex_lock(&rc->lock);
    rc->frame_block.paused = paused;
    g_mutex_unlock(&rc->lock);
}

void relay_controller_frame_block_reset(RelayController *rc) {
    if (!rc) return;
    g_mutex_lock(&rc->lock);
    rc->frame_block.reset_requested = TRUE;
    for (guint i = 0; i < rc->sources_count; i++) {
        UvRelaySource *src = &rc->sources[i];
        if (src->frame_block) {
            frame_block_state_reset(src->frame_block);
        }
        src->frame_block_accum_bytes = 0;
    }
    rc->frame_block.reset_requested = FALSE;
    g_mutex_unlock(&rc->lock);
}

void relay_controller_frame_block_set_thresholds(RelayController *rc,
                                                 double green_ms,
                                                 double yellow_ms,
                                                 double orange_ms) {
    if (!rc) return;
    if (green_ms < 0.0) green_ms = 0.0;
    if (yellow_ms < 0.0) yellow_ms = 0.0;
    if (orange_ms < 0.0) orange_ms = 0.0;

    if (green_ms > yellow_ms) {
        double tmp = green_ms;
        green_ms = yellow_ms;
        yellow_ms = tmp;
    }
    if (yellow_ms > orange_ms) {
        double tmp = yellow_ms;
        yellow_ms = orange_ms;
        orange_ms = tmp;
    }
    if (green_ms > yellow_ms) {
        double tmp = green_ms;
        green_ms = yellow_ms;
        yellow_ms = tmp;
    }

    g_mutex_lock(&rc->lock);
    rc->frame_block.thresholds_ms[0] = green_ms;
    rc->frame_block.thresholds_ms[1] = yellow_ms;
    rc->frame_block.thresholds_ms[2] = orange_ms;
    rc->frame_block.thresholds_dirty_ms = TRUE;
    for (guint i = 0; i < rc->sources_count; i++) {
        UvRelaySource *src = &rc->sources[i];
        if (src->frame_block) {
            frame_block_state_apply_lateness_thresholds(src->frame_block, rc->frame_block.thresholds_ms);
        }
    }
    rc->frame_block.thresholds_dirty_ms = FALSE;
    g_mutex_unlock(&rc->lock);
}

void relay_controller_frame_block_set_size_thresholds(RelayController *rc,
                                                      double green_kb,
                                                      double yellow_kb,
                                                      double orange_kb) {
    if (!rc) return;
    if (green_kb < 0.0) green_kb = 0.0;
    if (yellow_kb < 0.0) yellow_kb = 0.0;
    if (orange_kb < 0.0) orange_kb = 0.0;

    if (green_kb > yellow_kb) {
        double tmp = green_kb;
        green_kb = yellow_kb;
        yellow_kb = tmp;
    }
    if (yellow_kb > orange_kb) {
        double tmp = yellow_kb;
        yellow_kb = orange_kb;
        orange_kb = tmp;
    }
    if (green_kb > yellow_kb) {
        double tmp = green_kb;
        green_kb = yellow_kb;
        yellow_kb = tmp;
    }

    g_mutex_lock(&rc->lock);
    rc->frame_block.thresholds_kb[0] = green_kb;
    rc->frame_block.thresholds_kb[1] = yellow_kb;
    rc->frame_block.thresholds_kb[2] = orange_kb;
    rc->frame_block.thresholds_dirty_kb = TRUE;
    for (guint i = 0; i < rc->sources_count; i++) {
        UvRelaySource *src = &rc->sources[i];
        if (src->frame_block) {
            frame_block_state_apply_size_thresholds(src->frame_block, rc->frame_block.thresholds_kb);
        }
    }
    rc->frame_block.thresholds_dirty_kb = FALSE;
    g_mutex_unlock(&rc->lock);
}
