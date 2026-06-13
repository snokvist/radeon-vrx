#include "uv_internal.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static void qos_stats_free(gpointer data) {
    g_free(data);
}

void uv_internal_decoder_stats_reset(DecoderStats *stats) {
    if (!stats) return;
    g_mutex_lock(&stats->lock);
    stats->frames_total = 0;
    stats->first_frame_us = 0;
    stats->prev_timestamp_us = 0;
    stats->last_snapshot_fps = 0.0;
    memset(stats->frame_times_us, 0, sizeof(stats->frame_times_us));
    stats->frame_times_head = 0;
    stats->frame_times_count = 0;
    g_mutex_unlock(&stats->lock);
}

void uv_internal_decoder_stats_push_frame(DecoderStats *stats, gint64 now_us) {
    if (!stats) return;
    g_mutex_lock(&stats->lock);
    stats->frames_total++;
    if (stats->first_frame_us == 0) stats->first_frame_us = now_us;
    stats->prev_timestamp_us = now_us;
    stats->frame_times_us[stats->frame_times_head] = now_us;
    stats->frame_times_head = (stats->frame_times_head + 1u) % UV_DECODER_FPS_WINDOW_SAMPLES;
    if (stats->frame_times_count < UV_DECODER_FPS_WINDOW_SAMPLES) {
        stats->frame_times_count++;
    }
    g_mutex_unlock(&stats->lock);
}

void uv_internal_qos_db_init(QoSDatabase *db) {
    if (!db) return;
    g_mutex_init(&db->lock);
    db->table = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, qos_stats_free);
}

void uv_internal_qos_db_clear(QoSDatabase *db) {
    if (!db) return;
    g_mutex_lock(&db->lock);
    if (db->table) g_hash_table_remove_all(db->table);
    g_mutex_unlock(&db->lock);
}

void uv_internal_qos_db_update(QoSDatabase *db, GstMessage *msg) {
    if (!db || !msg) return;
    GstObject *src = GST_MESSAGE_SRC(msg);
    if (!src) return;
    gchar *path = gst_object_get_path_string(src);
    if (!path) return;

    gboolean live = FALSE;
    guint64 running_time = 0, stream_time = 0, timestamp = 0, duration = 0;
    gst_message_parse_qos(msg, &live, &running_time, &stream_time, &timestamp, &duration);

    GstFormat fmt = GST_FORMAT_UNDEFINED;
    guint64 processed = 0, dropped = 0;
    gst_message_parse_qos_stats(msg, &fmt, &processed, &dropped);

    gint64 jitter_ns = 0;
    gdouble proportion = 0.0;
    gint quality = 0;
    gst_message_parse_qos_values(msg, &jitter_ns, &proportion, &quality);

    g_mutex_lock(&db->lock);
    QoSStatsImpl *qs = NULL;
    if (db->table) qs = g_hash_table_lookup(db->table, path);
    if (!qs) {
        qs = g_new0(QoSStatsImpl, 1);
        qs->min_jitter_ns = G_MAXINT64;
        qs->max_jitter_ns = G_MININT64;
        if (db->table) g_hash_table_insert(db->table, path, qs);
        else g_free(path);
    } else {
        g_free(path);
    }

    qs->events++;
    qs->processed = processed;
    qs->dropped = dropped;
    qs->last_jitter_ns = jitter_ns;
    if (jitter_ns < qs->min_jitter_ns) qs->min_jitter_ns = jitter_ns;
    if (jitter_ns > qs->max_jitter_ns) qs->max_jitter_ns = jitter_ns;
    qs->sum_abs_jitter_ns += (long double)((jitter_ns < 0) ? -jitter_ns : jitter_ns);
    qs->last_proportion = proportion;
    qs->last_quality = quality;
    qs->live = live;
    g_mutex_unlock(&db->lock);
}

void uv_internal_qos_db_snapshot(QoSDatabase *db, UvViewerStats *stats) {
    if (!db || !stats) return;
    g_mutex_lock(&db->lock);
    if (!db->table) {
        g_mutex_unlock(&db->lock);
        return;
    }

    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, db->table);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        const char *path = (const char *)key;
        QoSStatsImpl *qs = (QoSStatsImpl *)value;
        UvNamedQoSStats entry = {0};
        g_strlcpy(entry.element_path, path, sizeof(entry.element_path));
        entry.stats.processed = qs->processed;
        entry.stats.dropped = qs->dropped;
        entry.stats.events = qs->events;
        entry.stats.last_jitter_ns = qs->last_jitter_ns;
        entry.stats.min_jitter_ns = qs->min_jitter_ns;
        entry.stats.max_jitter_ns = qs->max_jitter_ns;
        entry.stats.average_abs_jitter_ns = (qs->events > 0)
            ? (double)(qs->sum_abs_jitter_ns / (long double)qs->events)
            : 0.0;
        entry.stats.last_proportion = qs->last_proportion;
        entry.stats.last_quality = qs->last_quality;
        entry.stats.live = qs->live;
        g_array_append_val(stats->qos_entries, entry);
    }
    g_mutex_unlock(&db->lock);
}

void uv_internal_emit_event(struct _UvViewer *viewer, UvViewerEventKind kind, int source_index, const UvRelaySource *source, GError *error) {
    if (!viewer || !viewer->event_cb) return;
    UvViewerEvent event = {
        .kind = kind,
        .source_index = source_index,
        .error = error
    };
    if (source) {
        uv_internal_populate_source_stats(source, viewer->config.clock_rate,
                                          g_get_monotonic_time(), &event.source_snapshot);
        int current = relay_controller_selected(&viewer->relay);
        event.source_snapshot.selected = (source_index >= 0 && current == source_index);
    }
    viewer->event_cb(&event, viewer->event_cb_data);
}
