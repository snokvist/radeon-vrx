#include "gui_shell.h"
#include "uv_internal.h"

#include <gtk/gtk.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef struct {
    UvViewer *viewer;
    UvViewerConfig current_cfg;
    GtkApplication *app;
    GtkWindow *window;
    GtkLabel *status_label;
    GtkLabel *info_label;
    GtkListBox *source_list;
    GtkPicture *video_picture;
    GtkScrolledWindow *source_scroller;
    GtkWidget *sources_frame;
    GtkToggleButton *sources_toggle;
    GtkSpinButton *listen_port_spin;
    GtkSpinButton *jitter_latency_spin;
    GtkCheckButton *sync_toggle_settings;
    GtkSpinButton *queue_max_buffers_spin;
    GtkCheckButton *jitter_drop_toggle;
    GtkCheckButton *jitter_do_lost_toggle;
    GtkCheckButton *jitter_post_drop_toggle;
    GtkNotebook *notebook;
    GtkDropDown *stats_range_dropdown;
    GtkDrawingArea *stats_charts[6];
    double stats_range_seconds;
    GArray *stats_history;
    guint stats_timeout_id;
    GstElement *bound_sink;
    gulong sink_paintable_handler;
    gboolean paintable_bound;
} GuiContext;

typedef struct {
    GuiContext *ctx;
    UvViewerEventKind kind;
    int source_index;
    char *address;
    char *error_message;
} UiEvent;

typedef enum {
    STATS_METRIC_RATE = 0,
    STATS_METRIC_LOST,
    STATS_METRIC_DUP,
    STATS_METRIC_REORDER,
    STATS_METRIC_JITTER,
    STATS_METRIC_FPS,
    STATS_METRIC_COUNT
} StatsMetric;

typedef struct {
    double timestamp;
    double rate_bps;
    double lost_packets;
    double dup_packets;
    double reorder_packets;
    double jitter_ms;
    double fps_avg;
} StatsSample;

static GtkWidget *build_monitor_page(GuiContext *ctx);
static GtkWidget *build_settings_page(GuiContext *ctx);
static GtkWidget *build_stats_page(GuiContext *ctx);
static void viewer_event_callback(const UvViewerEvent *event, gpointer user_data);
static void on_notebook_switch_page(GtkNotebook *notebook, GtkWidget *page, guint page_num, gpointer user_data);
static void detach_bound_sink(GuiContext *ctx);
static gboolean ensure_video_paintable(GuiContext *ctx);
static void stats_range_changed(GObject *dropdown, GParamSpec *pspec, gpointer user_data);
static void stats_chart_draw(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer user_data);

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

static gboolean check_get(GtkCheckButton *button) {
    return button ? gtk_check_button_get_active(button) : FALSE;
}

static void check_set(GtkCheckButton *button, gboolean state) {
    if (!button) return;
    gtk_check_button_set_active(button, state);
}

static void update_status(GuiContext *ctx, const char *message) {
    if (!ctx || !ctx->status_label) return;
    gtk_label_set_text(ctx->status_label, message ? message : "");
}

static void update_info_label(GuiContext *ctx) {
    if (!ctx || !ctx->info_label) return;
    char info[160];
    UvViewerConfig *cfg = &ctx->current_cfg;
    g_snprintf(info, sizeof(info),
               "Listening on %d | PT %d | Clock %d | %s | Jitter %ums | Queue buffers %u"
               " | drop=%s | lost=%s | bus-msg=%s",
               cfg->listen_port,
               cfg->payload_type,
               cfg->clock_rate,
               cfg->sync_to_clock ? "sync" : "no-sync",
               cfg->jitter_latency_ms,
               cfg->queue_max_buffers,
               cfg->jitter_drop_on_latency ? "on" : "off",
               cfg->jitter_do_lost ? "on" : "off",
               cfg->jitter_post_drop_messages ? "on" : "off");
    gtk_label_set_text(ctx->info_label, info);
}

static void sync_settings_controls(GuiContext *ctx) {
    if (!ctx) return;
    if (ctx->listen_port_spin) {
        gtk_spin_button_set_value(ctx->listen_port_spin, ctx->current_cfg.listen_port);
    }
    if (ctx->jitter_latency_spin) {
        gtk_spin_button_set_value(ctx->jitter_latency_spin, ctx->current_cfg.jitter_latency_ms);
    }
    if (ctx->queue_max_buffers_spin) {
        gtk_spin_button_set_value(ctx->queue_max_buffers_spin, ctx->current_cfg.queue_max_buffers);
    }
    check_set(ctx->sync_toggle_settings, ctx->current_cfg.sync_to_clock ? TRUE : FALSE);
    check_set(ctx->jitter_drop_toggle, ctx->current_cfg.jitter_drop_on_latency ? TRUE : FALSE);
    check_set(ctx->jitter_do_lost_toggle, ctx->current_cfg.jitter_do_lost ? TRUE : FALSE);
    check_set(ctx->jitter_post_drop_toggle, ctx->current_cfg.jitter_post_drop_messages ? TRUE : FALSE);
    update_info_label(ctx);
}

static void clear_source_list(GuiContext *ctx) {
    if (!ctx || !ctx->source_list) return;
    GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(ctx->source_list));
    while (child) {
        GtkWidget *next = gtk_widget_get_next_sibling(child);
        gtk_list_box_remove(ctx->source_list, child);
        child = next;
    }
}

static void restore_adjustment(GtkAdjustment *adj, double previous_value) {
    if (!adj) return;
    double lower = gtk_adjustment_get_lower(adj);
    double upper = gtk_adjustment_get_upper(adj);
    double page = gtk_adjustment_get_page_size(adj);
    double max_value = MAX(lower, upper - page);
    double clamped = CLAMP(previous_value, lower, max_value);
    gtk_adjustment_set_value(adj, clamped);
}

static void append_source_row(GuiContext *ctx, const UvSourceStats *src, guint index) {
    if (!ctx || !ctx->source_list || !src) return;
    GtkListBoxRow *row = GTK_LIST_BOX_ROW(gtk_list_box_row_new());
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_list_box_row_set_child(row, box);

    char header[128];
    g_snprintf(header, sizeof(header), "[%u]%s %s",
               index,
               src->selected ? "*" : "",
               src->address);
    GtkWidget *header_label = gtk_label_new(header);
    gtk_label_set_xalign(GTK_LABEL(header_label), 0.0);
    gtk_box_append(GTK_BOX(box), header_label);

    char bitrate[64];
    format_bitrate(src->inbound_bitrate_bps, bitrate, sizeof(bitrate));

    char detail[256];
    g_snprintf(detail, sizeof(detail),
               "rx=%" G_GUINT64_FORMAT "/%" G_GUINT64_FORMAT
               " fwd=%" G_GUINT64_FORMAT "/%" G_GUINT64_FORMAT
               " rate=%s jitter=%.2fms last_seen=%.1fs",
               src->rx_packets,
               src->rx_bytes,
               src->forwarded_packets,
               src->forwarded_bytes,
               bitrate,
               src->rfc3550_jitter_ms,
               src->seconds_since_last_seen >= 0.0 ? src->seconds_since_last_seen : 0.0);
    GtkWidget *detail_label = gtk_label_new(detail);
    gtk_label_set_xalign(GTK_LABEL(detail_label), 0.0);
    gtk_label_set_wrap(GTK_LABEL(detail_label), TRUE);
    gtk_box_append(GTK_BOX(box), detail_label);

    gtk_list_box_append(ctx->source_list, GTK_WIDGET(row));
    g_object_set_data(G_OBJECT(row), "source-index", GINT_TO_POINTER(index));
    if (src->selected) {
        gtk_list_box_select_row(ctx->source_list, row);
    }
}

static gboolean bind_sink_paintable(GuiContext *ctx, GstElement *sink) {
    if (!ctx || !ctx->video_picture || !sink) return FALSE;

    GParamSpec *prop = g_object_class_find_property(G_OBJECT_GET_CLASS(sink), "paintable");
    if (!prop) return FALSE;

    GdkPaintable *paintable = NULL;
    g_object_get(sink, "paintable", &paintable, NULL);
    if (!paintable) return FALSE;

    gtk_picture_set_paintable(ctx->video_picture, paintable);
    gtk_widget_queue_draw(GTK_WIDGET(ctx->video_picture));
    g_object_unref(paintable);
    ctx->paintable_bound = TRUE;
    return TRUE;
}

static void on_sink_paintable_notify(GObject *object, GParamSpec *pspec, gpointer user_data) {
    (void)pspec;
    GuiContext *ctx = user_data;
    if (!ctx || !ctx->viewer) return;
    bind_sink_paintable(ctx, GST_ELEMENT(object));
}

static void detach_bound_sink(GuiContext *ctx) {
    if (!ctx) return;
    if (ctx->bound_sink && ctx->sink_paintable_handler) {
        g_signal_handler_disconnect(ctx->bound_sink, ctx->sink_paintable_handler);
        ctx->sink_paintable_handler = 0;
    }
    if (ctx->bound_sink) {
        gst_object_unref(ctx->bound_sink);
        ctx->bound_sink = NULL;
    }
    ctx->paintable_bound = FALSE;
    if (ctx->video_picture && GTK_IS_PICTURE(ctx->video_picture)) {
        gtk_picture_set_paintable(ctx->video_picture, NULL);
    }
}

static gboolean ensure_video_paintable(GuiContext *ctx) {
    if (!ctx || !ctx->video_picture || !ctx->viewer) return FALSE;

    GstElement *sink = uv_internal_viewer_get_sink(ctx->viewer);
    if (!sink) return FALSE;

    GParamSpec *prop = g_object_class_find_property(G_OBJECT_GET_CLASS(sink), "paintable");
    if (!prop) {
        detach_bound_sink(ctx);
        return FALSE;
    }

    if (ctx->bound_sink != sink) {
        detach_bound_sink(ctx);
        ctx->bound_sink = GST_ELEMENT(gst_object_ref(sink));
        ctx->sink_paintable_handler = g_signal_connect(ctx->bound_sink,
                                                       "notify::paintable",
                                                       G_CALLBACK(on_sink_paintable_notify),
                                                       ctx);
    }

    if (ctx->paintable_bound) {
        return TRUE;
    }

    return bind_sink_paintable(ctx, sink);
}

static double stats_metric_value(const StatsSample *sample, StatsMetric metric) {
    switch (metric) {
        case STATS_METRIC_RATE:    return sample->rate_bps;
        case STATS_METRIC_LOST:    return sample->lost_packets;
        case STATS_METRIC_DUP:     return sample->dup_packets;
        case STATS_METRIC_REORDER: return sample->reorder_packets;
        case STATS_METRIC_JITTER:  return sample->jitter_ms;
        case STATS_METRIC_FPS:     return sample->fps_avg;
        default:                   return 0.0;
    }
}

static void stats_history_push(GuiContext *ctx, const StatsSample *sample) {
    if (!ctx) return;
    if (!ctx->stats_history) {
        ctx->stats_history = g_array_new(FALSE, FALSE, sizeof(StatsSample));
    }
    g_array_append_val(ctx->stats_history, *sample);

    double window = 600.0; // keep 10 minutes of data
    double cutoff = sample->timestamp - window;
    guint remove_count = 0;
    StatsSample *data = (StatsSample *)ctx->stats_history->data;
    for (guint i = 0; i < ctx->stats_history->len; i++) {
        if (data[i].timestamp >= cutoff) {
            remove_count = i;
            break;
        }
        if (i + 1 == ctx->stats_history->len) {
            remove_count = ctx->stats_history->len;
        }
    }
    if (remove_count > 0 && remove_count < ctx->stats_history->len) {
        g_array_remove_range(ctx->stats_history, 0, remove_count);
    } else if (remove_count == ctx->stats_history->len) {
        g_array_set_size(ctx->stats_history, 0);
    }
}

static void refresh_stats(GuiContext *ctx) {
    if (!ctx || !ctx->viewer) return;

    if (!ctx->paintable_bound) {
        ensure_video_paintable(ctx);
    }

    GtkAdjustment *source_adj = NULL;
    double source_value = 0.0;
    if (ctx->source_scroller) {
        source_adj = gtk_scrolled_window_get_vadjustment(ctx->source_scroller);
        source_value = gtk_adjustment_get_value(source_adj);
    }

    UvViewerStats stats = {0};
    uv_viewer_stats_init(&stats);

    if (!uv_viewer_get_stats(ctx->viewer, &stats)) {
        update_status(ctx, "Failed to fetch stats");
        uv_viewer_stats_clear(&stats);
        return;
    }

    if (ctx->source_list) {
        clear_source_list(ctx);
    }

    UvSourceStats *selected_source = NULL;

    if (!stats.sources || stats.sources->len == 0) {
        update_status(ctx, "Listening for sources...");
    } else {
        for (guint i = 0; i < stats.sources->len; i++) {
            UvSourceStats *src = &g_array_index(stats.sources, UvSourceStats, i);
            if (ctx->source_list) {
                append_source_row(ctx, src, i);
            }
            if (src->selected) {
                selected_source = src;
            }
        }
        update_status(ctx, "");
    }

    if (selected_source) {
        StatsSample sample = {0};
        sample.timestamp = g_get_monotonic_time() / 1e6;
        sample.rate_bps = selected_source->inbound_bitrate_bps;
        sample.lost_packets = (double)selected_source->rtp_lost_packets;
        sample.dup_packets = (double)selected_source->rtp_duplicate_packets;
        sample.reorder_packets = (double)selected_source->rtp_reordered_packets;
        sample.jitter_ms = selected_source->rfc3550_jitter_ms;
        sample.fps_avg = stats.decoder.average_fps;
        stats_history_push(ctx, &sample);

        for (int i = 0; i < STATS_METRIC_COUNT; i++) {
            if (ctx->stats_charts[i]) {
                gtk_widget_queue_draw(GTK_WIDGET(ctx->stats_charts[i]));
            }
        }
    } else {
        for (int i = 0; i < STATS_METRIC_COUNT; i++) {
            if (ctx->stats_charts[i]) {
                gtk_widget_queue_draw(GTK_WIDGET(ctx->stats_charts[i]));
            }
        }
    }

    if (source_adj) restore_adjustment(source_adj, source_value);

    uv_viewer_stats_clear(&stats);
}

static gboolean stats_timeout_cb(gpointer user_data) {
    GuiContext *ctx = user_data;
    if (!ctx || !ctx->window) return G_SOURCE_REMOVE;
    refresh_stats(ctx);
    return G_SOURCE_CONTINUE;
}

static void update_sources_toggle_label(GuiContext *ctx, gboolean hidden) {
    if (!ctx || !ctx->sources_toggle || !GTK_IS_TOGGLE_BUTTON(ctx->sources_toggle)) return;
    gtk_button_set_label(GTK_BUTTON(ctx->sources_toggle), hidden ? "Show Sources" : "Hide Sources");
}

static void on_refresh_button_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    GuiContext *ctx = user_data;
    refresh_stats(ctx);
}

static void on_next_button_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    GuiContext *ctx = user_data;
    if (!ctx || !ctx->viewer) return;
    GError *error = NULL;
    if (!uv_viewer_select_next_source(ctx->viewer, &error)) {
        update_status(ctx, error ? error->message : "Failed to select next source");
    } else {
        update_status(ctx, "Selected next source");
        refresh_stats(ctx);
    }
    if (error) {
        g_error_free(error);
    }
}

static void on_quit_button_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    GuiContext *ctx = user_data;
    if (!ctx || !ctx->app) return;
    g_application_quit(G_APPLICATION(ctx->app));
}

static void on_sources_toggle_toggled(GtkToggleButton *button, gpointer user_data) {
    GuiContext *ctx = user_data;
    if (!ctx) return;
    gboolean hidden = gtk_toggle_button_get_active(button);
    if (ctx->sources_frame) gtk_widget_set_visible(ctx->sources_frame, !hidden);
    update_sources_toggle_label(ctx, hidden);
}

static gboolean gui_restart_with_config(GuiContext *ctx, const UvViewerConfig *cfg) {
    if (!ctx || !ctx->viewer || !cfg) return FALSE;

    if (cfg->listen_port == ctx->current_cfg.listen_port &&
        cfg->sync_to_clock == ctx->current_cfg.sync_to_clock &&
        cfg->jitter_latency_ms == ctx->current_cfg.jitter_latency_ms &&
        cfg->queue_max_buffers == ctx->current_cfg.queue_max_buffers &&
        cfg->jitter_drop_on_latency == ctx->current_cfg.jitter_drop_on_latency &&
        cfg->jitter_do_lost == ctx->current_cfg.jitter_do_lost &&
        cfg->jitter_post_drop_messages == ctx->current_cfg.jitter_post_drop_messages) {
        update_status(ctx, "Settings unchanged");
        return TRUE;
    }

    UvViewer *old_viewer = ctx->viewer;
    uv_viewer_set_event_callback(old_viewer, NULL, NULL);
    uv_viewer_stop(old_viewer);

    UvViewer *new_viewer = uv_viewer_new(cfg);
    if (!new_viewer) {
        update_status(ctx, "Failed to create viewer for new settings");
        GError *restart_error = NULL;
        if (!uv_viewer_start(old_viewer, &restart_error)) {
            update_status(ctx, restart_error ? restart_error->message : "Failed to restart viewer");
            if (restart_error) g_error_free(restart_error);
            return FALSE;
        }
        uv_viewer_set_event_callback(old_viewer, viewer_event_callback, ctx);
        return FALSE;
    }

    uv_viewer_set_event_callback(new_viewer, viewer_event_callback, ctx);
    GError *error = NULL;
    if (!uv_viewer_start(new_viewer, &error)) {
        update_status(ctx, error ? error->message : "Failed to start viewer");
        if (error) g_error_free(error);
        uv_viewer_set_event_callback(new_viewer, NULL, NULL);
        uv_viewer_free(new_viewer);

        GError *restart_error = NULL;
        if (!uv_viewer_start(old_viewer, &restart_error)) {
            update_status(ctx, restart_error ? restart_error->message : "Failed to restart viewer");
            if (restart_error) g_error_free(restart_error);
        } else {
            uv_viewer_set_event_callback(old_viewer, viewer_event_callback, ctx);
        }
        return FALSE;
    }

    uv_viewer_free(old_viewer);
    detach_bound_sink(ctx);
    ctx->viewer = new_viewer;
    ctx->current_cfg = *cfg;
    if (ctx->stats_history) {
        g_array_set_size(ctx->stats_history, 0);
    }
    update_info_label(ctx);
    sync_settings_controls(ctx);
    refresh_stats(ctx);
    update_status(ctx, "Settings applied");
    return TRUE;
}

static void on_settings_apply_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    GuiContext *ctx = user_data;
    if (!ctx) return;

    UvViewerConfig new_cfg = ctx->current_cfg;
    if (ctx->listen_port_spin) {
        new_cfg.listen_port = gtk_spin_button_get_value_as_int(ctx->listen_port_spin);
        if (new_cfg.listen_port < 1) new_cfg.listen_port = 1;
    }
    if (ctx->jitter_latency_spin) {
        new_cfg.jitter_latency_ms = gtk_spin_button_get_value_as_int(ctx->jitter_latency_spin);
        if (new_cfg.jitter_latency_ms == 0) new_cfg.jitter_latency_ms = 1;
    }
    if (ctx->queue_max_buffers_spin) {
        new_cfg.queue_max_buffers = gtk_spin_button_get_value_as_int(ctx->queue_max_buffers_spin);
    }
    new_cfg.sync_to_clock = check_get(ctx->sync_toggle_settings);
    new_cfg.jitter_drop_on_latency = check_get(ctx->jitter_drop_toggle);
    new_cfg.jitter_do_lost = check_get(ctx->jitter_do_lost_toggle);
    new_cfg.jitter_post_drop_messages = check_get(ctx->jitter_post_drop_toggle);

    if (!gui_restart_with_config(ctx, &new_cfg)) {
        sync_settings_controls(ctx);
    }
}

static void on_source_row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer user_data) {
    (void)box;
    GuiContext *ctx = user_data;
    if (!ctx || !ctx->viewer || !row) return;
    int index = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "source-index"));
    GError *error = NULL;
    if (!uv_viewer_select_source(ctx->viewer, index, &error)) {
        update_status(ctx, error ? error->message : "Failed to select source");
    } else {
        char msg[128];
        g_snprintf(msg, sizeof(msg), "Selected source %d", index);
        update_status(ctx, msg);
        refresh_stats(ctx);
    }
    if (error) {
        g_error_free(error);
    }
}

static gboolean on_window_close_request(GtkWindow *window, gpointer user_data) {
    (void)window;
    GuiContext *ctx = user_data;
    if (!ctx || !ctx->viewer) return FALSE;
    uv_viewer_set_event_callback(ctx->viewer, NULL, NULL);
    if (ctx->stats_timeout_id) {
        GSource *source = g_main_context_find_source_by_id(NULL, ctx->stats_timeout_id);
        if (source) {
            g_source_destroy(source);
        }
        ctx->stats_timeout_id = 0;
    }
    detach_bound_sink(ctx);
    return FALSE;
}

static gboolean dispatch_ui_event(gpointer user_data) {
    UiEvent *event = user_data;
    GuiContext *ctx = event->ctx;
    if (!ctx) {
        g_free(event->address);
        g_free(event->error_message);
        g_free(event);
        return G_SOURCE_REMOVE;
    }

    switch (event->kind) {
        case UV_VIEWER_EVENT_SOURCE_ADDED: {
            char msg[256];
            g_snprintf(msg, sizeof(msg), "Discovered source [%d] %s",
                       event->source_index, event->address ? event->address : "");
            update_status(ctx, msg);
            refresh_stats(ctx);
            break;
        }
        case UV_VIEWER_EVENT_SOURCE_SELECTED: {
            char msg[256];
            g_snprintf(msg, sizeof(msg), "Selected [%d] %s",
                       event->source_index, event->address ? event->address : "");
            update_status(ctx, msg);
            refresh_stats(ctx);
            break;
        }
        case UV_VIEWER_EVENT_SOURCE_REMOVED: {
            char msg[256];
            g_snprintf(msg, sizeof(msg), "Source removed [%d]",
                       event->source_index);
            update_status(ctx, msg);
            refresh_stats(ctx);
            break;
        }
        case UV_VIEWER_EVENT_PIPELINE_ERROR: {
            const char *err = event->error_message ? event->error_message : "unknown";
            char msg[256];
            g_snprintf(msg, sizeof(msg), "Pipeline error: %s", err);
            update_status(ctx, msg);
            if (ctx->app) {
                g_application_quit(G_APPLICATION(ctx->app));
            }
            break;
        }
        case UV_VIEWER_EVENT_SHUTDOWN:
            update_status(ctx, "Pipeline shutdown requested");
            if (ctx->app) {
                g_application_quit(G_APPLICATION(ctx->app));
            }
            break;
        default:
            break;
    }

    g_free(event->address);
    g_free(event->error_message);
    g_free(event);
    return G_SOURCE_REMOVE;
}

static void viewer_event_callback(const UvViewerEvent *event, gpointer user_data) {
    GuiContext *ctx = user_data;
    UiEvent *copy = g_new0(UiEvent, 1);
    copy->ctx = ctx;
    copy->kind = event->kind;
    copy->source_index = event->source_index;
    copy->address = g_strdup(event->source_snapshot.address);
    if (event->error) {
        copy->error_message = g_strdup(event->error->message);
    }
    g_idle_add(dispatch_ui_event, copy);
}

static GtkWidget *build_monitor_page(GuiContext *ctx) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_top(page, 12);
    gtk_widget_set_margin_bottom(page, 12);
    gtk_widget_set_margin_start(page, 12);
    gtk_widget_set_margin_end(page, 12);

    ctx->status_label = GTK_LABEL(gtk_label_new("Waiting for sources..."));
    gtk_label_set_xalign(ctx->status_label, 0.0);
    gtk_box_append(GTK_BOX(page), GTK_WIDGET(ctx->status_label));

    GtkWidget *video_frame = gtk_frame_new("Video Preview");
    gtk_widget_set_hexpand(video_frame, TRUE);
    gtk_widget_set_vexpand(video_frame, TRUE);
    ctx->video_picture = GTK_PICTURE(gtk_picture_new());
    gtk_picture_set_content_fit(ctx->video_picture, GTK_CONTENT_FIT_CONTAIN);
    gtk_picture_set_can_shrink(ctx->video_picture, TRUE);
    GtkWidget *video_widget = GTK_WIDGET(ctx->video_picture);
    gtk_widget_set_hexpand(video_widget, TRUE);
    gtk_widget_set_vexpand(video_widget, TRUE);
    gtk_frame_set_child(GTK_FRAME(video_frame), video_widget);
    gtk_box_append(GTK_BOX(page), video_frame);

    ctx->sources_frame = gtk_frame_new("Sources");
    gtk_box_append(GTK_BOX(page), ctx->sources_frame);

    ctx->source_scroller = GTK_SCROLLED_WINDOW(gtk_scrolled_window_new());
    gtk_widget_set_hexpand(GTK_WIDGET(ctx->source_scroller), TRUE);
    gtk_widget_set_vexpand(GTK_WIDGET(ctx->source_scroller), TRUE);
    gtk_scrolled_window_set_policy(ctx->source_scroller, GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(GTK_WIDGET(ctx->source_scroller), -1, 140);

    ctx->source_list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(ctx->source_list, GTK_SELECTION_SINGLE);
    gtk_list_box_set_activate_on_single_click(ctx->source_list, TRUE);
    g_signal_connect(ctx->source_list, "row-activated", G_CALLBACK(on_source_row_activated), ctx);

    gtk_scrolled_window_set_child(ctx->source_scroller, GTK_WIDGET(ctx->source_list));
    gtk_frame_set_child(GTK_FRAME(ctx->sources_frame), GTK_WIDGET(ctx->source_scroller));

    GtkWidget *button_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append(GTK_BOX(page), button_box);

    GtkWidget *refresh_button = gtk_button_new_with_label("Refresh");
    g_signal_connect(refresh_button, "clicked", G_CALLBACK(on_refresh_button_clicked), ctx);
    gtk_box_append(GTK_BOX(button_box), refresh_button);

    GtkWidget *next_button = gtk_button_new_with_label("Select Next");
    g_signal_connect(next_button, "clicked", G_CALLBACK(on_next_button_clicked), ctx);
    gtk_box_append(GTK_BOX(button_box), next_button);

    ctx->sources_toggle = GTK_TOGGLE_BUTTON(gtk_toggle_button_new_with_label("Hide Sources"));
    g_signal_connect(ctx->sources_toggle, "toggled", G_CALLBACK(on_sources_toggle_toggled), ctx);
    gtk_box_append(GTK_BOX(button_box), GTK_WIDGET(ctx->sources_toggle));

    GtkWidget *quit_button = gtk_button_new_with_label("Quit");
    g_signal_connect(quit_button, "clicked", G_CALLBACK(on_quit_button_clicked), ctx);
    gtk_box_append(GTK_BOX(button_box), quit_button);

    gtk_toggle_button_set_active(ctx->sources_toggle, TRUE);

    return page;
}

static GtkWidget *build_settings_page(GuiContext *ctx) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(page, 12);
    gtk_widget_set_margin_bottom(page, 12);
    gtk_widget_set_margin_start(page, 12);
    gtk_widget_set_margin_end(page, 12);

    ctx->info_label = GTK_LABEL(gtk_label_new(""));
    gtk_label_set_xalign(ctx->info_label, 0.0);
    gtk_box_append(GTK_BOX(page), GTK_WIDGET(ctx->info_label));
    update_info_label(ctx);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_box_append(GTK_BOX(page), grid);

    GtkWidget *listen_label = gtk_label_new("Listen Port:");
    gtk_label_set_xalign(GTK_LABEL(listen_label), 0.0);
    gtk_grid_attach(GTK_GRID(grid), listen_label, 0, 0, 1, 1);

    ctx->listen_port_spin = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(1, 65535, 1));
    gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(ctx->listen_port_spin), 1, 0, 1, 1);

    GtkWidget *sync_label = gtk_label_new("Sync to clock:");
    gtk_label_set_xalign(GTK_LABEL(sync_label), 0.0);
    gtk_grid_attach(GTK_GRID(grid), sync_label, 0, 1, 1, 1);

    ctx->sync_toggle_settings = GTK_CHECK_BUTTON(gtk_check_button_new());
    gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(ctx->sync_toggle_settings), 1, 1, 1, 1);

    GtkWidget *jitter_label = gtk_label_new("Jitter Latency (ms):");
    gtk_label_set_xalign(GTK_LABEL(jitter_label), 0.0);
    gtk_grid_attach(GTK_GRID(grid), jitter_label, 0, 2, 1, 1);

    ctx->jitter_latency_spin = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(1, 500, 1));
    gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(ctx->jitter_latency_spin), 1, 2, 1, 1);

    GtkWidget *queue_label = gtk_label_new("Max Queue Buffers:");
    gtk_label_set_xalign(GTK_LABEL(queue_label), 0.0);
    gtk_grid_attach(GTK_GRID(grid), queue_label, 0, 3, 1, 1);

    ctx->queue_max_buffers_spin = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(0, 2000, 1));
    gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(ctx->queue_max_buffers_spin), 1, 3, 1, 1);

    ctx->jitter_drop_toggle = GTK_CHECK_BUTTON(gtk_check_button_new_with_label("Drop packets exceeding latency"));
    gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(ctx->jitter_drop_toggle), 0, 4, 2, 1);

    ctx->jitter_do_lost_toggle = GTK_CHECK_BUTTON(gtk_check_button_new_with_label("Emit lost packet notifications"));
    gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(ctx->jitter_do_lost_toggle), 0, 5, 2, 1);

    ctx->jitter_post_drop_toggle = GTK_CHECK_BUTTON(gtk_check_button_new_with_label("Post drop messages on bus"));
    gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(ctx->jitter_post_drop_toggle), 0, 6, 2, 1);

    GtkWidget *apply_button = gtk_button_new_with_label("Apply Settings");
    g_signal_connect(apply_button, "clicked", G_CALLBACK(on_settings_apply_clicked), ctx);
    gtk_box_append(GTK_BOX(page), apply_button);

    sync_settings_controls(ctx);

    GtkWidget *hint = gtk_label_new("Applying changes restarts the viewer to bind the new settings.");
    gtk_label_set_xalign(GTK_LABEL(hint), 0.0);
   gtk_box_append(GTK_BOX(page), hint);

    return page;
}

static GtkWidget *build_stats_page(GuiContext *ctx) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(page, 12);
    gtk_widget_set_margin_bottom(page, 12);
    gtk_widget_set_margin_start(page, 12);
    gtk_widget_set_margin_end(page, 12);

    GtkWidget *controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(page), controls);

    GtkWidget *range_label = gtk_label_new("Time range:");
    gtk_label_set_xalign(GTK_LABEL(range_label), 0.0);
    gtk_box_append(GTK_BOX(controls), range_label);

    const char *options[] = {
        "Last 1 minute",
        "Last 5 minutes",
        "Last 10 minutes",
        NULL
    };
    ctx->stats_range_dropdown = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(options));
    gtk_widget_set_hexpand(GTK_WIDGET(ctx->stats_range_dropdown), FALSE);

    guint default_index = 1;
    if (fabs(ctx->stats_range_seconds - 60.0) < 0.1) default_index = 0;
    else if (fabs(ctx->stats_range_seconds - 600.0) < 0.1) default_index = 2;
    gtk_drop_down_set_selected(ctx->stats_range_dropdown, default_index);
    g_signal_connect(ctx->stats_range_dropdown, "notify::selected", G_CALLBACK(stats_range_changed), ctx);
    gtk_box_append(GTK_BOX(controls), GTK_WIDGET(ctx->stats_range_dropdown));

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 12);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_box_append(GTK_BOX(page), grid);

    static const char *titles[STATS_METRIC_COUNT] = {
        "Inbound Rate (bps)",
        "RTP Lost Packets",
        "RTP Duplicate Packets",
        "RTP Reordered Packets",
        "RTP Jitter (ms)",
        "Decoder FPS (avg)"
    };

    for (int i = 0; i < STATS_METRIC_COUNT; i++) {
        GtkWidget *frame = gtk_frame_new(titles[i]);
        gtk_widget_set_hexpand(frame, TRUE);
        gtk_widget_set_vexpand(frame, TRUE);
        int row = i / 2;
        int col = i % 2;
        gtk_grid_attach(GTK_GRID(grid), frame, col, row, 1, 1);

        GtkDrawingArea *area = GTK_DRAWING_AREA(gtk_drawing_area_new());
        gtk_widget_set_hexpand(GTK_WIDGET(area), TRUE);
        gtk_widget_set_vexpand(GTK_WIDGET(area), TRUE);
        gtk_drawing_area_set_draw_func(area, stats_chart_draw, ctx, NULL);
        g_object_set_data(G_OBJECT(area), "stats-metric", GINT_TO_POINTER(i));
        gtk_frame_set_child(GTK_FRAME(frame), GTK_WIDGET(area));
        ctx->stats_charts[i] = area;
    }

    return page;
}

static void stats_range_changed(GObject *dropdown, GParamSpec *pspec, gpointer user_data) {
    (void)pspec;
    GuiContext *ctx = user_data;
    if (!ctx || !GTK_IS_DROP_DOWN(dropdown)) return;
    guint selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(dropdown));
    double seconds = 300.0;
    switch (selected) {
        case 0: seconds = 60.0; break;
        case 1: seconds = 300.0; break;
        case 2: seconds = 600.0; break;
        default: break;
    }
    ctx->stats_range_seconds = seconds;
    for (int i = 0; i < STATS_METRIC_COUNT; i++) {
        if (ctx->stats_charts[i]) {
            gtk_widget_queue_draw(GTK_WIDGET(ctx->stats_charts[i]));
        }
    }
}

static void stats_chart_draw(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer user_data) {
    GuiContext *ctx = user_data;
    if (!ctx || width <= 0 || height <= 0) return;
    gint metric_val = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(area), "stats-metric"));
    if (metric_val < 0 || metric_val >= STATS_METRIC_COUNT) return;
    StatsMetric metric = (StatsMetric)metric_val;

    cairo_save(cr);
    cairo_set_source_rgb(cr, 0.10, 0.10, 0.12);
    cairo_paint(cr);

    cairo_set_source_rgba(cr, 1, 1, 1, 0.1);
    cairo_rectangle(cr, 0.5, 0.5, width - 1.0, height - 1.0);
    cairo_stroke(cr);

    if (!ctx->stats_history || ctx->stats_history->len == 0) {
        cairo_set_source_rgb(cr, 0.7, 0.7, 0.7);
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 12.0);
        cairo_move_to(cr, 10, height / 2.0);
        cairo_show_text(cr, "No data yet");
        cairo_restore(cr);
        return;
    }

    double range = MAX(ctx->stats_range_seconds, 60.0);
    double now = g_get_monotonic_time() / 1e6;
    double start_time = now - range;

    StatsSample *samples = (StatsSample *)ctx->stats_history->data;
    guint len = ctx->stats_history->len;
    guint start_index = 0;
    while (start_index < len && samples[start_index].timestamp < start_time) {
        start_index++;
    }
    if (start_index == len) {
        start_index = len > 0 ? len - 1 : 0;
    }

    double min_val = G_MAXDOUBLE;
    double max_val = -G_MAXDOUBLE;
    for (guint i = start_index; i < len; i++) {
        double v = stats_metric_value(&samples[i], metric);
        if (!isfinite(v)) continue;
        if (v < min_val) min_val = v;
        if (v > max_val) max_val = v;
    }

    if (min_val == G_MAXDOUBLE || max_val == -G_MAXDOUBLE) {
        cairo_set_source_rgb(cr, 0.7, 0.7, 0.7);
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 12.0);
        cairo_move_to(cr, 10, height / 2.0);
        cairo_show_text(cr, "No samples in range");
        cairo_restore(cr);
        return;
    }

    if (min_val == max_val) {
        double delta = fabs(min_val) > 1.0 ? fabs(min_val) * 0.05 : 1.0;
        min_val -= delta;
        max_val += delta;
    }

    const double padding = 12.0;
    double plot_width = MAX(1.0, width - 2 * padding);
    double plot_height = MAX(1.0, height - 2 * padding);

    cairo_set_source_rgba(cr, 1, 1, 1, 0.1);
    for (int i = 1; i < 4; i++) {
        double y = padding + (plot_height / 4.0) * i;
        cairo_move_to(cr, padding, y);
        cairo_line_to(cr, padding + plot_width, y);
    }
    cairo_stroke(cr);

    cairo_set_source_rgb(cr, 0.3, 0.7, 1.0);
    cairo_set_line_width(cr, 1.5);
    gboolean path_started = FALSE;
    for (guint i = start_index; i < len; i++) {
        const StatsSample *sample = &samples[i];
        double x_ratio = (sample->timestamp - start_time) / range;
        if (x_ratio < 0.0) x_ratio = 0.0;
        if (x_ratio > 1.0) x_ratio = 1.0;
        double x = padding + x_ratio * plot_width;
        double value = stats_metric_value(sample, metric);
        double y_ratio = (value - min_val) / (max_val - min_val);
        double y = padding + (1.0 - y_ratio) * plot_height;
        if (!path_started) {
            cairo_move_to(cr, x, y);
            path_started = TRUE;
        } else {
            cairo_line_to(cr, x, y);
        }
    }
    cairo_stroke(cr);

    double latest_value = stats_metric_value(&samples[len - 1], metric);
    cairo_set_source_rgb(cr, 0.85, 0.85, 0.85);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 12.0);
    char label[64];
    if (metric == STATS_METRIC_RATE) {
        char formatted[32];
        format_bitrate(latest_value, formatted, sizeof(formatted));
        g_snprintf(label, sizeof(label), "%s", formatted);
    } else {
        g_snprintf(label, sizeof(label), "%.2f", latest_value);
    }
    cairo_move_to(cr, padding, padding + 12.0);
    cairo_show_text(cr, label);

    cairo_restore(cr);
}

static void build_ui(GuiContext *ctx) {
    GtkWidget *window = gtk_application_window_new(ctx->app);
    ctx->window = GTK_WINDOW(window);
    gtk_window_set_title(ctx->window, "UDP H.265 Viewer");
    gtk_window_set_default_size(ctx->window, 960, 720);
    g_signal_connect(window, "close-request", G_CALLBACK(on_window_close_request), ctx);

    ctx->notebook = GTK_NOTEBOOK(gtk_notebook_new());
    gtk_window_set_child(ctx->window, GTK_WIDGET(ctx->notebook));

    GtkWidget *monitor_page = build_monitor_page(ctx);
    gtk_notebook_append_page(ctx->notebook, monitor_page, gtk_label_new("Monitor"));

    GtkWidget *settings_page = build_settings_page(ctx);
    gtk_notebook_append_page(ctx->notebook, settings_page, gtk_label_new("Settings"));

    GtkWidget *stats_page = build_stats_page(ctx);
    gtk_notebook_append_page(ctx->notebook, stats_page, gtk_label_new("Stats"));

    g_signal_connect(ctx->notebook, "switch-page", G_CALLBACK(on_notebook_switch_page), ctx);

    gtk_window_present(ctx->window);
}

static void on_notebook_switch_page(GtkNotebook *notebook, GtkWidget *page, guint page_num, gpointer user_data) {
    (void)notebook;
    (void)page;
    (void)page_num;
    GuiContext *ctx = user_data;
    if (!ctx) return;
    if (page_num == 1) {
        sync_settings_controls(ctx);
    } else if (page_num == 2) {
        for (int i = 0; i < STATS_METRIC_COUNT; i++) {
            if (ctx->stats_charts[i]) {
                gtk_widget_queue_draw(GTK_WIDGET(ctx->stats_charts[i]));
            }
        }
    }
}

static void on_app_activate(GtkApplication *app, gpointer user_data) {
    GuiContext *ctx = user_data;
    ctx->app = app;
    build_ui(ctx);
    sync_settings_controls(ctx);
    uv_viewer_set_event_callback(ctx->viewer, viewer_event_callback, ctx);
    refresh_stats(ctx);
    if (!ctx->stats_timeout_id) {
        ctx->stats_timeout_id = g_timeout_add_seconds(1, stats_timeout_cb, ctx);
    }
}

static void on_app_shutdown(GApplication *app, gpointer user_data) {
    (void)app;
    GuiContext *ctx = user_data;
    if (!ctx) return;
    if (ctx->stats_timeout_id) {
        GSource *source = g_main_context_find_source_by_id(NULL, ctx->stats_timeout_id);
        if (source) {
            g_source_destroy(source);
        }
        ctx->stats_timeout_id = 0;
    }
    if (ctx->viewer) {
        uv_viewer_set_event_callback(ctx->viewer, NULL, NULL);
    }
    detach_bound_sink(ctx);
    if (ctx->stats_history) {
        g_array_free(ctx->stats_history, TRUE);
        ctx->stats_history = NULL;
    }
    ctx->status_label = NULL;
    ctx->info_label = NULL;
    ctx->source_list = NULL;
    ctx->video_picture = NULL;
    ctx->source_scroller = NULL;
    ctx->sources_frame = NULL;
    ctx->sources_toggle = NULL;
    ctx->listen_port_spin = NULL;
    ctx->jitter_latency_spin = NULL;
    ctx->sync_toggle_settings = NULL;
    ctx->queue_max_buffers_spin = NULL;
    ctx->jitter_drop_toggle = NULL;
    ctx->jitter_do_lost_toggle = NULL;
    ctx->jitter_post_drop_toggle = NULL;
    ctx->stats_range_dropdown = NULL;
    for (int i = 0; i < STATS_METRIC_COUNT; i++) {
        ctx->stats_charts[i] = NULL;
    }
    ctx->notebook = NULL;
    ctx->paintable_bound = FALSE;
    ctx->window = NULL;
}

int uv_gui_run(UvViewer *viewer, const UvViewerConfig *cfg, const char *program_name) {
    if (!viewer || !cfg) return 1;

    GtkApplication *app = gtk_application_new("com.radeonvrx.viewer", G_APPLICATION_NON_UNIQUE);

    GuiContext *ctx = g_new0(GuiContext, 1);
    ctx->viewer = viewer;
    ctx->current_cfg = *cfg;
    ctx->stats_range_seconds = 300.0;

    g_signal_connect(app, "activate", G_CALLBACK(on_app_activate), ctx);
    g_signal_connect(app, "shutdown", G_CALLBACK(on_app_shutdown), ctx);

    char *argv0 = NULL;
    int argc = 0;
    char **argv = NULL;
    if (program_name && *program_name) {
        argv0 = g_strdup(program_name);
        argc = 1;
        argv = g_new0(char *, 2);
        argv[0] = argv0;
        argv[1] = NULL;
    }

    int status = g_application_run(G_APPLICATION(app), argc, argv);

    if (argv) {
        g_free(argv);
    }
    if (argv0) {
        g_free(argv0);
    }

    g_object_unref(app);
    g_free(ctx);

    return status;
}
