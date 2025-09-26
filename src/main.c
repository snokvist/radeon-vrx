#include "cli_shell.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_usage(const char *argv0) {
    g_printerr("Usage: %s [--listen-port N] [--payload PT] [--clockrate Hz] [--sync|--no-sync]\n",
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

    uv_cli_run(viewer, &cfg);

    uv_viewer_stop(viewer);
    uv_viewer_free(viewer);
    return 0;
}
