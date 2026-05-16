/* RTP sidecar probe — subscribes to the encoder's per-frame telemetry
 * channel defined by waybeam_venc's rtp_sidecar.h.  Owns its own UDP
 * socket and worker thread; per-frame metadata is folded into
 * SidecarController under sc->lock and surfaced to the GUI through
 * sidecar_controller_snapshot(). */

#include "uv_internal.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define SIDECAR_MAGIC          0x52545053u    /* "RTPS" */
#define SIDECAR_VERSION        1
#define SIDECAR_MSG_SUBSCRIBE  1
#define SIDECAR_MSG_FRAME      2

#define SIDECAR_FLAG_KEYFRAME       0x01
#define SIDECAR_FLAG_ENC_INFO       0x02
#define SIDECAR_FLAG_TRANSPORT_INFO 0x04

#define SIDECAR_FRAME_WIRE_SIZE        52u
#define SIDECAR_ENC_INFO_WIRE_SIZE     12u
#define SIDECAR_TRANSPORT_WIRE_SIZE    16u
#define SIDECAR_SUBSCRIBE_INTERVAL_US  (2 * 1000000LL)
#define SIDECAR_STALE_AFTER_US         (5 * 1000000LL)
#define SIDECAR_POLL_TIMEOUT_MS        500

static uint64_t read_be64(const uint8_t *p) {
    return ((uint64_t)p[0] << 56) | ((uint64_t)p[1] << 48) |
           ((uint64_t)p[2] << 40) | ((uint64_t)p[3] << 32) |
           ((uint64_t)p[4] << 24) | ((uint64_t)p[5] << 16) |
           ((uint64_t)p[6] << 8)  |  (uint64_t)p[7];
}

static uint32_t read_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

static uint16_t read_be16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

gboolean sidecar_controller_init(SidecarController *sc, struct _UvViewer *viewer) {
    if (!sc) return FALSE;
    memset(sc, 0, sizeof(*sc));
    sc->fd = -1;
    sc->viewer = viewer;
    g_mutex_init(&sc->lock);
    return TRUE;
}

void sidecar_controller_deinit(SidecarController *sc) {
    if (!sc) return;
    sidecar_controller_stop(sc);
    g_mutex_clear(&sc->lock);
}

void sidecar_controller_set_target(SidecarController *sc, const char *address) {
    if (!sc) return;
    g_mutex_lock(&sc->lock);
    if (!address || !*address) {
        if (sc->target_valid) {
            sc->target_valid = FALSE;
            sc->target_addr[0] = '\0';
            sc->frames_received = 0;
            sc->idr_inserted_count = 0;
            sc->scene_change_count = 0;
            sc->keyframes_count = 0;
            sc->last_frame_us = 0;
            sc->last_subscribe_us = 0;
            sc->window_count = 0;
            sc->window_head = 0;
            sc->transport_info_seen = FALSE;
        }
        g_mutex_unlock(&sc->lock);
        return;
    }
    if (g_strcmp0(sc->target_addr, address) != 0) {
        g_strlcpy(sc->target_addr, address, sizeof(sc->target_addr));
        sc->target_valid = TRUE;
        /* Force an immediate SUBSCRIBE on next poll iteration so we
         * don't wait 2 s after a source switch. */
        sc->last_subscribe_us = 0;
        /* Reset accumulators tied to the previous encoder. */
        sc->frames_received = 0;
        sc->idr_inserted_count = 0;
        sc->scene_change_count = 0;
        sc->keyframes_count = 0;
        sc->last_frame_us = 0;
        sc->window_count = 0;
        sc->window_head = 0;
        sc->transport_info_seen = FALSE;
    }
    g_mutex_unlock(&sc->lock);
}

static void sidecar_send_subscribe(SidecarController *sc) {
    char target[UV_VIEWER_ADDR_MAX];
    uint16_t port;
    gboolean valid;
    g_mutex_lock(&sc->lock);
    valid = sc->target_valid;
    g_strlcpy(target, sc->target_addr, sizeof(target));
    port = sc->encoder_port;
    g_mutex_unlock(&sc->lock);
    if (!valid || port == 0 || sc->fd < 0) return;

    struct sockaddr_in dest = {0};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(port);
    if (inet_pton(AF_INET, target, &dest.sin_addr) != 1) return;

    uint8_t msg[8] = {0};
    uint32_t magic = htonl(SIDECAR_MAGIC);
    memcpy(msg, &magic, 4);
    msg[4] = SIDECAR_VERSION;
    msg[5] = SIDECAR_MSG_SUBSCRIBE;
    /* msg[6..7] = padding (zero) */
    sendto(sc->fd, msg, sizeof(msg), MSG_DONTWAIT,
           (struct sockaddr *)&dest, sizeof(dest));
}

static void sidecar_consume_frame(SidecarController *sc, const uint8_t *buf, size_t len, gint64 arrival_us) {
    if (len < SIDECAR_FRAME_WIRE_SIZE) return;

    uint8_t flags = buf[7];
    uint32_t ssrc           = read_be32(buf + 8);
    uint32_t rtp_timestamp  = read_be32(buf + 12);
    uint64_t frame_id       = read_be64(buf + 16);
    uint16_t seq_count      = read_be16(buf + 34);

    /* Optional trailers follow the 52-byte header. */
    const uint8_t *trailer = buf + SIDECAR_FRAME_WIRE_SIZE;
    size_t trailer_remaining = len - SIDECAR_FRAME_WIRE_SIZE;

    gboolean have_enc = FALSE;
    uint32_t frame_size_bytes = 0;
    uint8_t  frame_type = 0;
    uint8_t  qp = 0;
    uint8_t  complexity = 0;
    uint8_t  scene_change = 0;
    uint8_t  gop_state = 0;
    uint8_t  idr_inserted = 0;
    uint16_t frames_since_idr = 0;

    if ((flags & SIDECAR_FLAG_ENC_INFO) && trailer_remaining >= SIDECAR_ENC_INFO_WIRE_SIZE) {
        frame_size_bytes = read_be32(trailer + 0);
        frame_type       = trailer[4];
        qp               = trailer[5];
        complexity       = trailer[6];
        scene_change     = trailer[7];
        gop_state        = trailer[8];
        idr_inserted     = trailer[9];
        frames_since_idr = read_be16(trailer + 10);
        trailer += SIDECAR_ENC_INFO_WIRE_SIZE;
        trailer_remaining -= SIDECAR_ENC_INFO_WIRE_SIZE;
        have_enc = TRUE;
    }

    gboolean have_transport = FALSE;
    uint8_t  fill_pct = 0;
    uint8_t  in_pressure = 0;
    uint32_t transport_drops = 0;
    uint32_t pressure_drops = 0;
    uint32_t packets_sent = 0;
    if ((flags & SIDECAR_FLAG_TRANSPORT_INFO) && trailer_remaining >= SIDECAR_TRANSPORT_WIRE_SIZE) {
        fill_pct        = trailer[0];
        in_pressure     = trailer[1];
        /* trailer[2..3] reserved */
        transport_drops = read_be32(trailer + 4);
        pressure_drops  = read_be32(trailer + 8);
        packets_sent    = read_be32(trailer + 12);
        have_transport = TRUE;
    }

    g_mutex_lock(&sc->lock);
    sc->frames_received++;
    sc->last_frame_us = arrival_us;
    sc->last_ssrc = ssrc;
    sc->last_frame_id = frame_id;
    sc->last_rtp_timestamp = rtp_timestamp;
    sc->last_seq_count = seq_count;
    if (have_enc) {
        sc->last_frame_size_bytes = frame_size_bytes;
        sc->last_frame_type = frame_type;
        sc->last_qp = qp;
        sc->last_complexity = complexity;
        sc->last_scene_change = scene_change;
        sc->last_gop_state = gop_state;
        sc->last_idr_inserted = idr_inserted;
        sc->last_frames_since_idr = frames_since_idr;
        if (idr_inserted) sc->idr_inserted_count++;
        if (scene_change) sc->scene_change_count++;
        if (frame_type == UV_SIDECAR_FRAME_I || frame_type == UV_SIDECAR_FRAME_IDR) {
            sc->keyframes_count++;
        }
        /* Rolling window for averages. */
        guint slot = sc->window_head;
        sc->qp_window[slot] = qp;
        sc->cx_window[slot] = complexity;
        sc->window_head = (sc->window_head + 1) % UV_SIDECAR_AVG_WINDOW;
        if (sc->window_count < UV_SIDECAR_AVG_WINDOW) sc->window_count++;
    }
    if (have_transport) {
        sc->transport_info_seen = TRUE;
        sc->encoder_fill_pct = fill_pct;
        sc->encoder_in_pressure = in_pressure;
        sc->encoder_transport_drops = transport_drops;
        sc->encoder_pressure_drops = pressure_drops;
        sc->encoder_packets_sent = packets_sent;
    }
    g_mutex_unlock(&sc->lock);
}

static void sidecar_process_packet(SidecarController *sc,
                                   const uint8_t *buf, size_t len,
                                   gint64 arrival_us) {
    if (len < 6) return;
    if (read_be32(buf) != SIDECAR_MAGIC) return;
    if (buf[4] != SIDECAR_VERSION) return;
    uint8_t msg_type = buf[5];
    if (msg_type == SIDECAR_MSG_FRAME) {
        sidecar_consume_frame(sc, buf, len, arrival_us);
    }
    /* SYNC_RESP not implemented in this build. */
}

static gpointer sidecar_thread_run(gpointer data) {
    SidecarController *sc = data;
    while (sc->running) {
        struct pollfd pfd = { .fd = sc->fd, .events = POLLIN };
        int pr = poll(&pfd, 1, SIDECAR_POLL_TIMEOUT_MS);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }

        gint64 now = g_get_monotonic_time();

        g_mutex_lock(&sc->lock);
        gboolean send_sub = sc->target_valid &&
                            (sc->last_subscribe_us == 0 ||
                             (now - sc->last_subscribe_us) >= SIDECAR_SUBSCRIBE_INTERVAL_US);
        if (send_sub) sc->last_subscribe_us = now;
        g_mutex_unlock(&sc->lock);
        if (send_sub) sidecar_send_subscribe(sc);

        if (pr > 0 && (pfd.revents & POLLIN)) {
            for (int i = 0; i < 16; i++) {
                uint8_t buf[128];
                struct sockaddr_in src;
                socklen_t srclen = sizeof(src);
                ssize_t n = recvfrom(sc->fd, buf, sizeof(buf), MSG_DONTWAIT,
                                     (struct sockaddr *)&src, &srclen);
                if (n < 0) break;
                sidecar_process_packet(sc, buf, (size_t)n, g_get_monotonic_time());
            }
        }
    }
    return NULL;
}

gboolean sidecar_controller_start(SidecarController *sc) {
    if (!sc || !sc->viewer) return FALSE;
    if (!sc->viewer->config.sidecar_enabled) return TRUE; /* not an error; just disabled */
    if (sc->running) return TRUE;

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        uv_log_error("Sidecar: socket() failed: %s", g_strerror(errno));
        return FALSE;
    }
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in bind_addr = {0};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind_addr.sin_port = 0; /* ephemeral */
    if (bind(fd, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        uv_log_error("Sidecar: bind() failed: %s", g_strerror(errno));
        close(fd);
        return FALSE;
    }
    struct sockaddr_in bound = {0};
    socklen_t bound_len = sizeof(bound);
    uint16_t local_port = 0;
    if (getsockname(fd, (struct sockaddr *)&bound, &bound_len) == 0) {
        local_port = ntohs(bound.sin_port);
    }

    g_mutex_lock(&sc->lock);
    sc->fd = fd;
    sc->local_port = local_port;
    sc->enabled = TRUE;
    sc->encoder_port = (uint16_t)sc->viewer->config.sidecar_port;
    if (sc->encoder_port == 0) sc->encoder_port = 5602;
    sc->running = 1;
    g_mutex_unlock(&sc->lock);

    sc->thread = g_thread_new("uv-sidecar", sidecar_thread_run, sc);
    if (!sc->thread) {
        g_mutex_lock(&sc->lock);
        sc->running = 0;
        sc->enabled = FALSE;
        sc->fd = -1;
        g_mutex_unlock(&sc->lock);
        close(fd);
        return FALSE;
    }
    uv_log_info("Sidecar: bound to UDP port %u, encoder port %u",
                (unsigned)local_port, (unsigned)sc->encoder_port);
    return TRUE;
}

void sidecar_controller_stop(SidecarController *sc) {
    if (!sc) return;
    if (!sc->running && sc->fd < 0) return;
    sc->running = 0;
    if (sc->thread) {
        g_thread_join(sc->thread);
        sc->thread = NULL;
    }
    g_mutex_lock(&sc->lock);
    if (sc->fd >= 0) {
        close(sc->fd);
        sc->fd = -1;
    }
    sc->enabled = FALSE;
    sc->local_port = 0;
    /* Clear all per-session state so a re-enable starts fresh and the GUI
     * doesn't show stale frame counts / averages while the probe is off. */
    sc->target_valid = FALSE;
    sc->target_addr[0] = '\0';
    sc->last_frame_us = 0;
    sc->last_subscribe_us = 0;
    sc->frames_received = 0;
    sc->idr_inserted_count = 0;
    sc->scene_change_count = 0;
    sc->keyframes_count = 0;
    sc->last_ssrc = 0;
    sc->last_frame_id = 0;
    sc->last_rtp_timestamp = 0;
    sc->last_seq_count = 0;
    sc->last_frame_size_bytes = 0;
    sc->last_frame_type = 0;
    sc->last_qp = 0;
    sc->last_complexity = 0;
    sc->last_scene_change = 0;
    sc->last_gop_state = 0;
    sc->last_idr_inserted = 0;
    sc->last_frames_since_idr = 0;
    sc->window_head = 0;
    sc->window_count = 0;
    sc->transport_info_seen = FALSE;
    sc->encoder_fill_pct = 0;
    sc->encoder_in_pressure = 0;
    sc->encoder_transport_drops = 0;
    sc->encoder_pressure_drops = 0;
    sc->encoder_packets_sent = 0;
    g_mutex_unlock(&sc->lock);
}

void sidecar_controller_snapshot(SidecarController *sc, UvViewerStats *stats) {
    if (!sc || !stats) return;
    UvSidecarStats *out = &stats->sidecar;
    memset(out, 0, sizeof(*out));

    g_mutex_lock(&sc->lock);
    out->enabled = sc->enabled;
    out->socket_bound = (sc->fd >= 0);
    out->target_port = sc->encoder_port;
    out->local_port = sc->local_port;
    g_strlcpy(out->target_address, sc->target_addr, sizeof(out->target_address));

    gint64 now = g_get_monotonic_time();
    if (sc->last_frame_us > 0) {
        out->seconds_since_last_frame = (double)(now - sc->last_frame_us) / 1e6;
        out->subscribed = (now - sc->last_frame_us) < SIDECAR_STALE_AFTER_US;
    } else {
        out->seconds_since_last_frame = -1.0;
        out->subscribed = FALSE;
    }

    out->frames_received = sc->frames_received;
    out->idr_inserted_count = sc->idr_inserted_count;
    out->scene_change_count = sc->scene_change_count;
    out->keyframes_count = sc->keyframes_count;

    out->last_ssrc = sc->last_ssrc;
    out->last_frame_id = sc->last_frame_id;
    out->last_rtp_timestamp = sc->last_rtp_timestamp;
    out->last_seq_count = sc->last_seq_count;
    out->last_frame_size_bytes = sc->last_frame_size_bytes;
    out->last_frame_type = sc->last_frame_type;
    out->last_qp = sc->last_qp;
    out->last_complexity = sc->last_complexity;
    out->last_scene_change = sc->last_scene_change;
    out->last_gop_state = sc->last_gop_state;
    out->last_idr_inserted = sc->last_idr_inserted;
    out->last_frames_since_idr = sc->last_frames_since_idr;

    if (sc->window_count > 0) {
        uint32_t sum_qp = 0, sum_cx = 0;
        for (guint i = 0; i < sc->window_count; i++) {
            sum_qp += sc->qp_window[i];
            sum_cx += sc->cx_window[i];
        }
        out->avg_qp = (double)sum_qp / (double)sc->window_count;
        out->avg_complexity = (double)sum_cx / (double)sc->window_count;
    }

    out->transport_info_seen = sc->transport_info_seen;
    out->encoder_fill_pct = sc->encoder_fill_pct;
    out->encoder_in_pressure = sc->encoder_in_pressure;
    out->encoder_transport_drops = sc->encoder_transport_drops;
    out->encoder_pressure_drops = sc->encoder_pressure_drops;
    out->encoder_packets_sent = sc->encoder_packets_sent;

    g_mutex_unlock(&sc->lock);
}
