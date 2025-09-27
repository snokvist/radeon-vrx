#include "uv_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

static void addr_to_str(const struct sockaddr_in *sa, char *out, size_t outlen) {
    char ip[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof(ip));
    g_strlcpy(out, ip, outlen);
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
}

static bool relay_add_or_find(RelayController *rc, const struct sockaddr_in *from, socklen_t fromlen, int *out_idx) {
    for (guint i = 0; i < rc->sources_count; i++) {
        UvRelaySource *slot = &rc->sources[i];
        if (!slot->in_use) continue;
        if (slot->addr.sin_family == from->sin_family &&
            slot->addr.sin_addr.s_addr == from->sin_addr.s_addr) {
            if (slot->addr.sin_port != from->sin_port) {
                relay_source_clear_stats(slot, TRUE);
            }
            slot->addr = *from;
            slot->addrlen = fromlen;
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

static inline uint32_t rtp_now_ts(int clock_rate) {
    gint64 us = g_get_monotonic_time();
    long double ts = ((long double)us * (long double)clock_rate) / 1000000.0L;
    if (ts < 0) ts = 0;
    return (uint32_t)((uint64_t)ts);
}

static inline void rtp_update_stats(UvRelaySource *s, const unsigned char *p, size_t len, int clock_rate, int primary_payload_type) {
    if (len < 12) return;
    if ((p[0] & 0xC0) != 0x80) return;

    if (primary_payload_type >= 0) {
        int payload_type = p[1] & 0x7F;
        if (payload_type != primary_payload_type) {
            return; // ignore non-primary RTP payload types for stats tracking
        }
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

    uint32_t arrival_ts = rtp_now_ts(clock_rate);
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

    int in_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (in_fd < 0) {
        uv_log_error("Relay: socket() failed: %s", g_strerror(errno));
        return NULL;
    }

    int flags = fcntl(in_fd, F_GETFL, 0);
    if (flags >= 0) fcntl(in_fd, F_SETFL, flags | O_NONBLOCK);

    int reuse = 1;
    setsockopt(in_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    int rcvbuf = 4 * 1024 * 1024; // allow bursty sources before poll loop catches up
    setsockopt(in_fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    struct sockaddr_in bind_addr = {
        .sin_family = AF_INET,
        .sin_port = htons((uint16_t)rc->listen_port),
        .sin_addr.s_addr = htonl(INADDR_ANY)
    };
    if (bind(in_fd, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        uv_log_error("Relay: bind() failed on port %d: %s", rc->listen_port, g_strerror(errno));
        close(in_fd);
        return NULL;
    }

    uv_log_info("Relay: listening on UDP port %d", rc->listen_port);

    unsigned char *buf = g_malloc0(UV_RELAY_BUF_SIZE);
    if (!buf) {
        close(in_fd);
        return NULL;
    }

    struct pollfd fds[1];
    fds[0].fd = in_fd;
    fds[0].events = POLLIN;

    while (rc->running) {
        int pr = poll(fds, 1, 200);
        if (pr < 0) {
            if (errno == EINTR) continue;
            uv_log_warn("Relay: poll() error: %s", g_strerror(errno));
            break;
        }
        if (!(pr > 0 && (fds[0].revents & POLLIN))) continue;

        struct sockaddr_in from = {0};
        socklen_t fromlen = sizeof(from);
        ssize_t r = recvfrom(in_fd, buf, UV_RELAY_BUF_SIZE, 0, (struct sockaddr *)&from, &fromlen);
        if (r < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            uv_log_warn("Relay: recvfrom() error: %s", g_strerror(errno));
            continue;
        }

        gboolean emit_added = FALSE;
        gboolean emit_selected = FALSE;
        int emit_index = -1;
        UvRelaySource snapshot = {0};

        g_mutex_lock(&rc->lock);
        int idx = -1;
        bool is_new = relay_add_or_find(rc, &from, fromlen, &idx);
        UvRelaySource *src = NULL;
        if (idx >= 0 && (guint)idx < rc->sources_count) src = &rc->sources[idx];
        if (src) {
            src->rx_packets++;
            src->rx_bytes += (uint64_t)r;
            src->last_seen_us = g_get_monotonic_time();
            rtp_update_stats(src, buf, (size_t)r, viewer->config.clock_rate, viewer->config.payload_type);
        }
        if (is_new && src) {
            char addr[64];
            addr_to_str(&src->addr, addr, sizeof(addr));
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

    close(in_fd);
    g_free(buf);
    rc->running = 0;
    return NULL;
}

gboolean relay_controller_init(RelayController *rc, struct _UvViewer *viewer) {
    g_return_val_if_fail(rc != NULL, FALSE);
    g_return_val_if_fail(viewer != NULL, FALSE);

    memset(rc, 0, sizeof(*rc));
    g_mutex_init(&rc->lock);
    rc->listen_port = viewer->config.listen_port;
    rc->selected_index = -1;
    rc->viewer = viewer;
    return TRUE;
}

void relay_controller_deinit(RelayController *rc) {
    if (!rc) return;
    relay_controller_stop(rc);
    g_mutex_lock(&rc->lock);
    rc->appsrc = NULL;
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
        snapshot = rc->sources[index];
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
        snapshot = rc->sources[next_index];
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
    gint64 now_us = g_get_monotonic_time();

    g_mutex_lock(&rc->lock);
    for (guint i = 0; i < rc->sources_count; i++) {
        UvRelaySource *src = &rc->sources[i];
        if (!src->in_use) continue;
        UvSourceStats s = {0};
        addr_to_str(&src->addr, s.address, sizeof(s.address));
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
