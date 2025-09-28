#define _GNU_SOURCE
#include "cli_shell.h"

#include <ctype.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

typedef struct {
    UvViewer *viewer;
    const UvViewerConfig *cfg;
    gint running;
} CliContext;

static CliContext *g_cli_context = NULL;

static void format_bitrate(double bps, char *out, size_t outlen) {
    if (bps < 1000.0) {
        g_snprintf(out, outlen, "%.0f bps", bps);
    } else if (bps < 1e6) {
        g_snprintf(out, outlen, "%.2f kbps", bps / 1e3);
    } else if (bps < 1e9) {
        g_snprintf(out, outlen, "%.2f Mbps", bps / 1e6);
    } else {
        g_snprintf(out, outlen, "%.2f Gbps", bps / 1e9);
    }
}

static void print_sources(UvViewer *viewer) {
    UvViewerStats stats = {0};
    uv_viewer_stats_init(&stats);
    if (!uv_viewer_get_stats(viewer, &stats)) {
        g_printerr("Failed to fetch stats.\n");
        uv_viewer_stats_clear(&stats);
        return;
    }

    if (stats.sources->len == 0) {
        g_print("No sources discovered yet.\n");
    } else {
        g_print("Known sources:\n");
        for (guint i = 0; i < stats.sources->len; i++) {
            UvSourceStats *s = &g_array_index(stats.sources, UvSourceStats, i);
            const char *mark = s->selected ? "*" : "";
            g_print("  [%u]%s %s\n", i, mark, s->address);
        }
    }
    uv_viewer_stats_clear(&stats);
}

static void print_qos(const UvViewerStats *stats) {
    if (!stats->qos_entries || stats->qos_entries->len == 0) {
        g_print("QoS: (no messages yet)\n");
        return;
    }
    g_print("---- QoS (per element) ----\n");
    for (guint i = 0; i < stats->qos_entries->len; i++) {
        const UvNamedQoSStats *entry = &g_array_index(stats->qos_entries, UvNamedQoSStats, i);
        double last_ms = entry->stats.last_jitter_ns / 1e6;
        double avg_ns = entry->stats.average_abs_jitter_ns;
        double avg_ms = avg_ns / 1e6;
        double min_ms = (entry->stats.min_jitter_ns == G_MAXINT64) ? 0.0 : entry->stats.min_jitter_ns / 1e6;
        double max_ms = (entry->stats.max_jitter_ns == G_MININT64) ? 0.0 : entry->stats.max_jitter_ns / 1e6;
        g_print("%s proc=%" G_GUINT64_FORMAT " drop=%" G_GUINT64_FORMAT
                " jitter(ms): last=%.2f avg=%.2f min=%.2f max=%.2f"
                " proportion=%.3f quality=%d live=%d events=%" G_GUINT64_FORMAT "\n",
                entry->element_path,
                entry->stats.processed,
                entry->stats.dropped,
                last_ms, avg_ms, min_ms, max_ms,
                entry->stats.last_proportion,
                entry->stats.last_quality,
                entry->stats.live ? 1 : 0,
                entry->stats.events);
    }
}

static void print_stats(UvViewer *viewer, int clock_rate) {
    UvViewerStats stats = {0};
    uv_viewer_stats_init(&stats);
    if (!uv_viewer_get_stats(viewer, &stats)) {
        g_printerr("Failed to fetch stats.\n");
        uv_viewer_stats_clear(&stats);
        return;
    }

    if (stats.sources->len == 0) {
        g_print("No sources discovered yet.\n");
    } else {
        g_print("---- Sources ----\n");
        for (guint i = 0; i < stats.sources->len; i++) {
            UvSourceStats *s = &g_array_index(stats.sources, UvSourceStats, i);
            char rate_buf[64];
            format_bitrate(s->inbound_bitrate_bps, rate_buf, sizeof(rate_buf));
            double jitter_ms = s->rfc3550_jitter_ms;
            double last_seen = s->seconds_since_last_seen;
            g_print("[%u]%s %s rx_pkts=%" G_GUINT64_FORMAT " rx_bytes=%" G_GUINT64_FORMAT
                    " fwd_pkts=%" G_GUINT64_FORMAT " fwd_bytes=%" G_GUINT64_FORMAT
                    " rate=%s last_seen=%.1fs"
                    " | rtp_unique=%" G_GUINT64_FORMAT " expected=%" G_GUINT64_FORMAT
                    " lost=%" G_GUINT64_FORMAT " dup=%" G_GUINT64_FORMAT
                    " reorder=%" G_GUINT64_FORMAT " jitter=%.2fms\n",
                    i,
                    s->selected ? "*" : "",
                    s->address,
                    s->rx_packets,
                    s->rx_bytes,
                    s->forwarded_packets,
                    s->forwarded_bytes,
                    rate_buf,
                    (last_seen >= 0.0 ? last_seen : 0.0),
                    s->rtp_unique_packets,
                    s->rtp_expected_packets,
                    s->rtp_lost_packets,
                    s->rtp_duplicate_packets,
                    s->rtp_reordered_packets,
                    jitter_ms);
        }
    }

    g_print("---- Pipeline ----\n");
    if (stats.queue0_valid) {
        g_print("queue0: level buffers=%d bytes=%u time=%.1fms\n",
                stats.queue0.current_level_buffers,
                stats.queue0.current_level_bytes,
                stats.queue0.current_level_time_ms);
    } else {
        g_print("queue0: (not available)\n");
    }

    const char *caps_str = stats.decoder.caps_str[0] ? stats.decoder.caps_str : "(caps not negotiated yet)";
    g_print("decoder: fps(inst)=%.2f fps(avg)=%.2f frames=%" G_GUINT64_FORMAT " caps=%s\n",
            stats.decoder.instantaneous_fps,
            stats.decoder.average_fps,
            stats.decoder.frames_total,
            caps_str);

    g_print("audio: %s", stats.audio_enabled ? "enabled" : "disabled");
    if (stats.audio_enabled) {
        g_print(" (%s)\n", stats.audio_active ? "active" : "waiting");
    } else {
        g_print("\n");
    }

    print_qos(&stats);
    uv_viewer_stats_clear(&stats);
    (void)clock_rate;
}

static void print_help(void) {
    g_print("Commands: l, n, s <index>, stats, q\n");
}

static gboolean process_command(CliContext *ctx, const char *line) {
    if (!line) return TRUE;
    while (*line && g_ascii_isspace(*line)) line++;
    if (*line == '\0') return TRUE;

    if (!g_ascii_strcasecmp(line, "q")) {
        g_atomic_int_set(&ctx->running, 0);
        return TRUE;
    }
    if (!g_ascii_strcasecmp(line, "l")) {
        print_sources(ctx->viewer);
        return TRUE;
    }
    if (!g_ascii_strcasecmp(line, "n")) {
        GError *err = NULL;
        if (!uv_viewer_select_next_source(ctx->viewer, &err)) {
            g_printerr("%s\n", err ? err->message : "Failed to select next source");
            if (err) g_error_free(err);
        }
        return TRUE;
    }
    if (!g_ascii_strncasecmp(line, "s ", 2)) {
        line += 2;
        while (*line && g_ascii_isspace(*line)) line++;
        if (*line == '\0') {
            g_print("Usage: s <index>\n");
            return TRUE;
        }
        char *end = NULL;
        long idx = strtol(line, &end, 10);
        if (end == line) {
            g_print("Usage: s <index>\n");
            return TRUE;
        }
        GError *err = NULL;
        if (!uv_viewer_select_source(ctx->viewer, (int)idx, &err)) {
            g_printerr("%s\n", err ? err->message : "Failed to select source");
            if (err) g_error_free(err);
        }
        return TRUE;
    }
    if (!g_ascii_strcasecmp(line, "stats")) {
        print_stats(ctx->viewer, ctx->cfg->clock_rate);
        return TRUE;
    }

    print_help();
    return TRUE;
}

static void cli_event_callback(const UvViewerEvent *event, gpointer user_data) {
    CliContext *ctx = (CliContext *)user_data;
    switch (event->kind) {
        case UV_VIEWER_EVENT_SOURCE_ADDED:
            g_print("Relay: discovered source [%d] %s\n",
                    event->source_index,
                    event->source_snapshot.address);
            break;
        case UV_VIEWER_EVENT_SOURCE_SELECTED:
            g_print("Relay: selected [%d] %s\n",
                    event->source_index,
                    event->source_snapshot.address);
            break;
        case UV_VIEWER_EVENT_PIPELINE_ERROR:
            g_printerr("Pipeline error: %s\n",
                       event->error ? event->error->message : "unknown");
            g_atomic_int_set(&ctx->running, 0);
            break;
        case UV_VIEWER_EVENT_SHUTDOWN:
            g_print("Pipeline shutdown signalled.\n");
            g_atomic_int_set(&ctx->running, 0);
            break;
        case UV_VIEWER_EVENT_SOURCE_REMOVED:
            g_print("Relay: source removed [%d]\n", event->source_index);
            break;
        default:
            break;
    }
}

static void sigint_handler(int signum) {
    (void)signum;
    if (g_cli_context) {
        g_atomic_int_set(&g_cli_context->running, 0);
    }
}

static void install_signal_handlers(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigint_handler;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

int uv_cli_run(UvViewer *viewer, const UvViewerConfig *cfg) {
    CliContext ctx = {
        .viewer = viewer,
        .cfg = cfg,
        .running = 1
    };
    g_cli_context = &ctx;
    install_signal_handlers();

    uv_viewer_set_event_callback(viewer, cli_event_callback, &ctx);

    g_print("Viewer: waiting for UDP on %d. Commands: l, n, s <i>, stats, q\n",
            cfg->listen_port);

    char *line = NULL;
    size_t n = 0;
    while (g_atomic_int_get(&ctx.running)) {
        ssize_t r = getline(&line, &n, stdin);
        if (r < 0) {
            if (feof(stdin)) break;
            if (errno == EINTR) continue;
            g_printerr("stdin read error\n");
            break;
        }
        if (r > 0 && (line[r-1] == '\n' || line[r-1] == '\r')) line[r-1] = '\0';
        process_command(&ctx, line);
    }
    g_free(line);

    uv_viewer_set_event_callback(viewer, NULL, NULL);
    g_cli_context = NULL;
    return 0;
}
