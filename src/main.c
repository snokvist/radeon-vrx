#include "gui_shell.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_usage(const char *argv0) {
    g_printerr("Usage: %s [--listen-port N] [--payload PT] [--clockrate Hz] [--sync|--no-sync]"
               " [--videorate] [--no-videorate] [--videorate-fps NUM[/DEN]]"
               " [--audio] [--no-audio] [--audio-payload PT] [--audio-clockrate Hz]"
               " [--audio-jitter ms]\n",
               argv0);
}

static gboolean parse_args(int argc, char **argv, UvViewerConfig *cfg) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--listen-port") && i + 1 < argc) {
            cfg->listen_port = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--payload") && i + 1 < argc) {
            cfg->payload_type = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--clockrate") && i + 1 < argc) {
            cfg->clock_rate = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--sync")) {
            cfg->sync_to_clock = TRUE;
        } else if (!strcmp(argv[i], "--no-sync")) {
            cfg->sync_to_clock = FALSE;
        } else if (!strcmp(argv[i], "--videorate")) {
            cfg->videorate_enabled = TRUE;
        } else if (!strcmp(argv[i], "--no-videorate")) {
            cfg->videorate_enabled = FALSE;
        } else if (!strcmp(argv[i], "--videorate-fps")) {
            if (i + 1 >= argc) {
                g_printerr("Missing argument for --videorate-fps\n");
                return FALSE;
            }
            const char *spec = argv[++i];
            char *endptr = NULL;
            guint64 num = g_ascii_strtoull(spec, &endptr, 10);
            if (endptr == spec || num == 0 || num > G_MAXUINT) {
                g_printerr("Invalid videorate numerator: %s\n", spec);
                return FALSE;
            }
            guint64 den = 1;
            if (*endptr == '/') {
                const char *den_str = endptr + 1;
                char *den_end = NULL;
                den = g_ascii_strtoull(den_str, &den_end, 10);
                if (den_end == den_str || *den_end != '\0' || den == 0 || den > G_MAXUINT) {
                    g_printerr("Invalid videorate denominator: %s\n", spec);
                    return FALSE;
                }
            } else if (*endptr != '\0') {
                g_printerr("Invalid videorate value: %s\n", spec);
                return FALSE;
            }
            cfg->videorate_enabled = TRUE;
            cfg->videorate_fps_numerator = (guint)num;
            cfg->videorate_fps_denominator = (guint)den;
        } else if (!strcmp(argv[i], "--audio")) {
            cfg->audio_enabled = TRUE;
        } else if (!strcmp(argv[i], "--no-audio")) {
            cfg->audio_enabled = FALSE;
        } else if (!strcmp(argv[i], "--audio-payload") && i + 1 < argc) {
            int payload = atoi(argv[++i]);
            if (payload < 0) payload = 0;
            if (payload > 127) payload = 127;
            cfg->audio_payload_type = (guint)payload;
        } else if (!strcmp(argv[i], "--audio-clockrate") && i + 1 < argc) {
            int rate = atoi(argv[++i]);
            if (rate > 0) cfg->audio_clock_rate = (guint)rate;
        } else if (!strcmp(argv[i], "--audio-jitter") && i + 1 < argc) {
            int latency = atoi(argv[++i]);
            if (latency < 0) latency = 0;
            cfg->audio_jitter_latency_ms = (guint)latency;
        } else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            print_usage(argv[0]);
            return FALSE;
        } else {
            g_printerr("Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return FALSE;
        }
    }
    return TRUE;
}

int main(int argc, char **argv) {
    UvViewerConfig cfg;
    uv_viewer_config_init(&cfg);

    if (!parse_args(argc, argv, &cfg)) {
        return 1;
    }

    UvViewer *viewer = uv_viewer_new(&cfg);
    if (!viewer) {
        g_printerr("Failed to allocate viewer.\n");
        return 1;
    }

    GError *error = NULL;
    if (!uv_viewer_start(viewer, &error)) {
        g_printerr("Failed to start viewer: %s\n", error ? error->message : "unknown error");
        if (error) g_error_free(error);
        uv_viewer_free(viewer);
        return 1;
    }

    int status = uv_gui_run(&viewer, &cfg, argv ? argv[0] : NULL);

    uv_viewer_stop(viewer);
    uv_viewer_free(viewer);
    return status;
}
