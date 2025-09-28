#include "uv_internal.h"
#include <string.h>

static void uv_viewer_init_struct(UvViewer *viewer, const UvViewerConfig *cfg) {
    viewer->config = *cfg;
    g_mutex_init(&viewer->state_lock);
    viewer->started = FALSE;
    viewer->shutting_down = FALSE;
    viewer->event_cb = NULL;
    viewer->event_cb_data = NULL;
    g_mutex_init(&viewer->decoder.lock);
    uv_internal_decoder_stats_reset(&viewer->decoder);
    uv_internal_qos_db_init(&viewer->qos);
}

void uv_viewer_config_init(UvViewerConfig *cfg) {
    if (!cfg) return;
    cfg->listen_port = 5600;
    cfg->payload_type = 97;
    cfg->clock_rate = 90000;
    cfg->sync_to_clock = FALSE;
    cfg->appsrc_queue_size = 0;
    cfg->jitter_latency_ms = 4;
    cfg->queue_max_buffers = 96;
    cfg->jitter_drop_on_latency = TRUE;
    cfg->jitter_do_lost = TRUE;
    cfg->jitter_post_drop_messages = TRUE;
    cfg->videorate_enabled = FALSE;
    cfg->videorate_fps_numerator = 60;
    cfg->videorate_fps_denominator = 1;
    cfg->audio_enabled = FALSE;
    cfg->audio_payload_type = 98;
    cfg->audio_clock_rate = 48000;
    cfg->audio_jitter_latency_ms = 8;
}

UvViewer *uv_viewer_new(const UvViewerConfig *cfg) {
    UvViewerConfig local;
    if (!cfg) {
        uv_viewer_config_init(&local);
        cfg = &local;
    }
    UvViewer *viewer = g_new0(UvViewer, 1);
    uv_viewer_init_struct(viewer, cfg);
    if (!relay_controller_init(&viewer->relay, viewer)) {
        g_free(viewer);
        return NULL;
    }
    if (!pipeline_controller_init(&viewer->pipeline, viewer, NULL)) {
        relay_controller_deinit(&viewer->relay);
        g_free(viewer);
        return NULL;
    }
    return viewer;
}

void uv_viewer_free(UvViewer *viewer) {
    if (!viewer) return;
    uv_viewer_stop(viewer);
    relay_controller_deinit(&viewer->relay);
    pipeline_controller_deinit(&viewer->pipeline);
    uv_internal_qos_db_clear(&viewer->qos);
    if (viewer->qos.table) {
        g_hash_table_destroy(viewer->qos.table);
        viewer->qos.table = NULL;
    }
    g_mutex_clear(&viewer->qos.lock);
    g_mutex_clear(&viewer->state_lock);
    g_mutex_clear(&viewer->decoder.lock);
    g_free(viewer);
}

bool uv_viewer_start(UvViewer *viewer, GError **error) {
    if (!viewer) return FALSE;
    g_mutex_lock(&viewer->state_lock);
    if (viewer->started) {
        g_mutex_unlock(&viewer->state_lock);
        return TRUE;
    }
    g_mutex_unlock(&viewer->state_lock);

    if (!pipeline_controller_start(&viewer->pipeline, error)) return FALSE;
    relay_controller_set_appsrc(&viewer->relay, pipeline_controller_get_appsrc(&viewer->pipeline));
    if (!relay_controller_start(&viewer->relay)) {
        pipeline_controller_stop(&viewer->pipeline);
        g_set_error(error, g_quark_from_static_string("uv-viewer"), 100,
                    "Failed to start relay thread");
        return FALSE;
    }

    g_mutex_lock(&viewer->state_lock);
    viewer->started = TRUE;
    g_mutex_unlock(&viewer->state_lock);
    return TRUE;
}

void uv_viewer_stop(UvViewer *viewer) {
    if (!viewer) return;
    g_mutex_lock(&viewer->state_lock);
    if (!viewer->started) {
        g_mutex_unlock(&viewer->state_lock);
        return;
    }
    viewer->started = FALSE;
    g_mutex_unlock(&viewer->state_lock);

    relay_controller_stop(&viewer->relay);
    pipeline_controller_stop(&viewer->pipeline);
}

void uv_viewer_set_event_callback(UvViewer *viewer, UvViewerEventCallback cb, gpointer user_data) {
    if (!viewer) return;
    viewer->event_cb = cb;
    viewer->event_cb_data = user_data;
}

bool uv_viewer_select_source(UvViewer *viewer, int index, GError **error) {
    if (!viewer) return FALSE;
    return relay_controller_select(&viewer->relay, index, error);
}

bool uv_viewer_select_next_source(UvViewer *viewer, GError **error) {
    if (!viewer) return FALSE;
    return relay_controller_select_next(&viewer->relay, error);
}

int uv_viewer_get_selected_source(const UvViewer *viewer) {
    if (!viewer) return -1;
    return relay_controller_selected(&viewer->relay);
}

bool uv_viewer_update_pipeline(UvViewer *viewer, const UvPipelineOverrides *overrides, GError **error) {
    if (!viewer) return FALSE;
    return pipeline_controller_update(&viewer->pipeline, overrides, error);
}

void uv_viewer_frame_block_configure(UvViewer *viewer, gboolean enabled, gboolean snapshot_mode) {
    if (!viewer) return;
    relay_controller_frame_block_configure(&viewer->relay, enabled, snapshot_mode);
}

void uv_viewer_frame_block_pause(UvViewer *viewer, gboolean paused) {
    if (!viewer) return;
    relay_controller_frame_block_pause(&viewer->relay, paused);
}

void uv_viewer_frame_block_reset(UvViewer *viewer) {
    if (!viewer) return;
    relay_controller_frame_block_reset(&viewer->relay);
}

void uv_viewer_frame_block_set_thresholds(UvViewer *viewer,
                                          double green_ms,
                                          double yellow_ms,
                                          double orange_ms) {
    if (!viewer) return;
    relay_controller_frame_block_set_thresholds(&viewer->relay, green_ms, yellow_ms, orange_ms);
}

void uv_viewer_frame_block_set_size_thresholds(UvViewer *viewer,
                                               double green_kb,
                                               double yellow_kb,
                                               double orange_kb) {
    if (!viewer) return;
    relay_controller_frame_block_set_size_thresholds(&viewer->relay, green_kb, yellow_kb, orange_kb);
}

void uv_viewer_stats_init(UvViewerStats *stats) {
    if (!stats) return;
    stats->sources = g_array_new(FALSE, TRUE, sizeof(UvSourceStats));
    stats->qos_entries = g_array_new(FALSE, TRUE, sizeof(UvNamedQoSStats));
    memset(&stats->decoder, 0, sizeof(stats->decoder));
    stats->audio_enabled = FALSE;
    stats->audio_active = FALSE;
    stats->queue0_valid = FALSE;
    memset(&stats->queue0, 0, sizeof(stats->queue0));
    stats->frame_block_valid = FALSE;
    memset(&stats->frame_block, 0, sizeof(stats->frame_block));
    stats->frame_block.lateness_ms = g_array_new(FALSE, TRUE, sizeof(double));
    stats->frame_block.frame_size_kb = g_array_new(FALSE, TRUE, sizeof(double));
    stats->frame_block.real_frames = 0;
    stats->frame_block.missing_frames = 0;
}

void uv_viewer_stats_clear(UvViewerStats *stats) {
    if (!stats) return;
    if (stats->sources) {
        g_array_unref(stats->sources);
        stats->sources = NULL;
    }
    if (stats->qos_entries) {
        g_array_unref(stats->qos_entries);
        stats->qos_entries = NULL;
    }
    stats->queue0_valid = FALSE;
    memset(&stats->queue0, 0, sizeof(stats->queue0));
    memset(&stats->decoder, 0, sizeof(stats->decoder));
    stats->audio_enabled = FALSE;
    stats->audio_active = FALSE;
    if (stats->frame_block.lateness_ms) {
        g_array_unref(stats->frame_block.lateness_ms);
        stats->frame_block.lateness_ms = NULL;
    }
    if (stats->frame_block.frame_size_kb) {
        g_array_unref(stats->frame_block.frame_size_kb);
        stats->frame_block.frame_size_kb = NULL;
    }
    stats->frame_block_valid = FALSE;
    memset(&stats->frame_block, 0, sizeof(stats->frame_block));
}

bool uv_viewer_get_stats(UvViewer *viewer, UvViewerStats *stats) {
    if (!viewer || !stats) return FALSE;
    if (!stats->sources) uv_viewer_stats_init(stats);
    g_array_set_size(stats->sources, 0);
    g_array_set_size(stats->qos_entries, 0);
    relay_controller_snapshot(&viewer->relay, stats, viewer->config.clock_rate);
    pipeline_controller_snapshot(&viewer->pipeline, stats);
    uv_internal_qos_db_snapshot(&viewer->qos, stats);
    return TRUE;
}

GstElement *uv_internal_viewer_get_sink(UvViewer *viewer) {
    if (!viewer) return NULL;
    return pipeline_controller_get_sink(&viewer->pipeline);
}
