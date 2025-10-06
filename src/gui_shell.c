#include "gui_shell.h"
#include "uv_internal.h"

#include <gtk/gtk.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define FRAME_BLOCK_DEFAULT_WIDTH    60u
#define FRAME_BLOCK_DEFAULT_HEIGHT  100u
#define FRAME_BLOCK_COLOR_COUNT     4u
#define FRAME_BLOCK_DEFAULT_GREEN_MS   2.0
#define FRAME_BLOCK_DEFAULT_YELLOW_MS 3.5
#define FRAME_BLOCK_DEFAULT_ORANGE_MS 5.0
#define FRAME_BLOCK_DEFAULT_SIZE_GREEN_KB   16.0
#define FRAME_BLOCK_DEFAULT_SIZE_YELLOW_KB  32.0
#define FRAME_BLOCK_DEFAULT_SIZE_ORANGE_KB  64.0
#define FRAME_BLOCK_MISSING_SENTINEL (-1.0)

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
    UvViewer **viewer_slot;
    UvViewer *viewer;
    UvViewerConfig *cfg_slot;
    UvViewerConfig current_cfg;
    GtkApplication *app;
    GtkWindow *window;
    GtkLabel *status_label;
    GtkLabel *info_label;
    GtkDropDown *source_dropdown;
    GtkStringList *source_model;
    GtkLabel *source_detail_label;
    GtkPicture *video_picture;
    GtkWidget *sources_frame;
    GtkToggleButton *sources_toggle;
    guint known_source_count;
    gboolean suppress_source_change;
    guint stats_refresh_interval_ms;
    GtkSpinButton *listen_port_spin;
    GtkSpinButton *jitter_latency_spin;
    GtkCheckButton *sync_toggle_settings;
    GtkSpinButton *queue_max_buffers_spin;
    GtkSpinButton *stats_refresh_spin;
    GtkDropDown *decoder_dropdown;
    GtkDropDown *sink_dropdown;
    GtkCheckButton *videorate_toggle;
    GtkSpinButton *videorate_num_spin;
    GtkSpinButton *videorate_den_spin;
    GtkCheckButton *audio_toggle;
    GtkSpinButton *audio_payload_spin;
    GtkSpinButton *audio_jitter_spin;
    GtkCheckButton *jitter_drop_toggle;
    GtkCheckButton *jitter_do_lost_toggle;
    GtkCheckButton *jitter_post_drop_toggle;
    GtkNotebook *notebook;
    GtkDropDown *stats_range_dropdown;
    GtkDrawingArea *stats_charts[STATS_METRIC_COUNT];
    GtkLabel *stats_live_labels[STATS_METRIC_COUNT];
    GtkLabel *stats_max_labels[STATS_METRIC_COUNT];
    double stats_range_seconds;
    double stats_last_refresh_time;
    GArray *stats_history;
    guint stats_timeout_id;
    GstElement *bound_sink;
    gulong sink_paintable_handler;
    gboolean paintable_bound;

    GtkDrawingArea *frame_block_area;
    GtkDrawingArea *frame_overlay_lateness;
    GtkDrawingArea *frame_overlay_size;
    GtkLabel *frame_overlay_live_labels[2];
    GtkLabel *frame_overlay_max_labels[2];
    GtkToggleButton *frame_block_enable_toggle;
    GtkToggleButton *frame_block_pause_toggle;
    GtkDropDown *frame_block_mode_dropdown;
    GtkDropDown *frame_block_width_dropdown;
    GtkToggleButton *frame_block_metric_toggle;
    GtkSpinButton *frame_block_threshold_spins[3];
    GtkLabel *frame_block_threshold_labels[3];
    GtkCheckButton *frame_block_color_toggles[4];
    GtkLabel *frame_block_summary_label;
    GtkButton *frame_block_reset_button;
    gboolean frame_block_colors_visible[4];
    gboolean frame_block_active;
    gboolean frame_block_paused;
    gboolean frame_block_snapshot_mode;
    gboolean frame_block_snapshot_complete;
    guint frame_block_width;
    guint frame_block_height;
    guint frame_block_filled;
    guint frame_block_next_index;
    double frame_block_thresholds_ms[3];
    double frame_block_thresholds_kb[3];
    double frame_block_min_ms;
    double frame_block_max_ms;
    double frame_block_avg_ms;
    double frame_block_min_kb;
    double frame_block_max_kb;
    double frame_block_avg_kb;
    guint frame_block_color_counts_ms[4];
    guint frame_block_color_counts_kb[4];
    GArray *frame_block_values_lateness; // doubles
    GArray *frame_block_values_size;     // doubles
    guint frame_block_view; // 0 = lateness, 1 = size
    guint frame_block_missing;
    guint frame_block_real_samples;
    gboolean audio_runtime_enabled;
    gboolean audio_active;
} GuiContext;

typedef struct {
    GuiContext *ctx;
    UvViewerEventKind kind;
    int source_index;
    char *address;
    char *error_message;
} UiEvent;

typedef struct {
    double timestamp;
    double rate_bps;
    double lost_packets;
    double dup_packets;
    double reorder_packets;
    double jitter_ms;
    double fps_current;
    double frame_lateness_ms;
    double frame_size_kb;
    gboolean frame_valid;
    gboolean frame_missing;
} StatsSample;

#define FRAME_BLOCK_VIEW_LATENESS 0u
#define FRAME_BLOCK_VIEW_SIZE     1u
#define FRAME_OVERLAY_METRIC_LATENESS 0u
#define FRAME_OVERLAY_METRIC_SIZE     1u

static const guint FRAME_BLOCK_WIDTH_OPTIONS[] = {30u, 60u, 90u, 120u};
#define FRAME_BLOCK_WIDTH_OPTION_COUNT (G_N_ELEMENTS(FRAME_BLOCK_WIDTH_OPTIONS))

static GtkWidget *build_monitor_page(GuiContext *ctx);
static GtkWidget *build_settings_page(GuiContext *ctx);
static GtkWidget *build_stats_page(GuiContext *ctx);
static GtkWidget *build_frame_block_page(GuiContext *ctx);
static void viewer_event_callback(const UvViewerEvent *event, gpointer user_data);
static void on_notebook_switch_page(GtkNotebook *notebook, GtkWidget *page, guint page_num, gpointer user_data);
static void detach_bound_sink(GuiContext *ctx);
static gboolean ensure_video_paintable(GuiContext *ctx);
static void restart_stats_timer(GuiContext *ctx);
static void set_stats_refresh_interval(GuiContext *ctx, guint interval_ms);
static void frame_block_queue_overlay_draws(GuiContext *ctx);
static void stats_range_changed(GObject *dropdown, GParamSpec *pspec, gpointer user_data);
static void stats_chart_draw(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer user_data);
static void frame_block_draw(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer user_data);
static void frame_overlay_draw(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer user_data);
static void stats_history_push(GuiContext *ctx, const StatsSample *sample);
static gboolean stats_history_push_frame_block_updates(GuiContext *ctx,
                                                       const UvFrameBlockStats *fb,
                                                       guint prev_next_index,
                                                       guint prev_filled,
                                                       guint prev_capacity,
                                                       double prev_refresh_time,
                                                       double now,
                                                       const StatsSample *latest_metrics);
static void on_frame_block_enable_toggled(GtkToggleButton *button, gpointer user_data);
static void on_frame_block_pause_toggled(GtkToggleButton *button, gpointer user_data);
static void on_frame_block_mode_changed(GObject *dropdown, GParamSpec *pspec, gpointer user_data);
static void on_frame_block_width_changed(GObject *dropdown, GParamSpec *pspec, gpointer user_data);
static void on_frame_block_metric_toggled(GtkToggleButton *button, gpointer user_data);
static void on_frame_block_reset_clicked(GtkButton *button, gpointer user_data);
static void on_frame_block_threshold_changed(GtkSpinButton *spin, gpointer user_data);
static void on_frame_block_color_toggled(GtkCheckButton *check, gpointer user_data);
static void on_videorate_toggled(GtkCheckButton *button, gpointer user_data);
static void on_audio_toggled(GtkCheckButton *button, gpointer user_data);
static void on_source_dropdown_changed(GObject *dropdown, GParamSpec *pspec, gpointer user_data);

static const char *decoder_option_labels[] = {
    "Auto",
    "Intel VAAPI",
    "NVIDIA",
    "Generic VAAPI",
    "Software (CPU)",
    NULL
};

static guint decoder_pref_to_index(UvDecoderPreference pref) {
    switch (pref) {
        case UV_DECODER_INTEL_VAAPI:   return 1u;
        case UV_DECODER_NVIDIA:        return 2u;
        case UV_DECODER_GENERIC_VAAPI: return 3u;
        case UV_DECODER_SOFTWARE:      return 4u;
        case UV_DECODER_AUTO:
        default:                       return 0u;
    }
}

static UvDecoderPreference decoder_index_to_pref(guint index) {
    switch (index) {
        case 1:  return UV_DECODER_INTEL_VAAPI;
        case 2:  return UV_DECODER_NVIDIA;
        case 3:  return UV_DECODER_GENERIC_VAAPI;
        case 4:  return UV_DECODER_SOFTWARE;
        case 0:
        default: return UV_DECODER_AUTO;
    }
}

static const char *video_sink_option_labels[] = {
    "Auto",
    "GTK4 Paintable",
    "Wayland",
    "GL Image",
    "XVideo",
    "Auto Video",
    "Fakesink",
    NULL
};

static guint video_sink_pref_to_index(UvVideoSinkPreference pref) {
    switch (pref) {
        case UV_VIDEO_SINK_GTK4:      return 1u;
        case UV_VIDEO_SINK_WAYLAND:   return 2u;
        case UV_VIDEO_SINK_GLIMAGE:   return 3u;
        case UV_VIDEO_SINK_XVIMAGE:   return 4u;
        case UV_VIDEO_SINK_AUTOVIDEO: return 5u;
        case UV_VIDEO_SINK_FAKESINK:  return 6u;
        case UV_VIDEO_SINK_AUTO:
        default:                      return 0u;
    }
}

static UvVideoSinkPreference video_sink_index_to_pref(guint index) {
    switch (index) {
        case 1: return UV_VIDEO_SINK_GTK4;
        case 2: return UV_VIDEO_SINK_WAYLAND;
        case 3: return UV_VIDEO_SINK_GLIMAGE;
        case 4: return UV_VIDEO_SINK_XVIMAGE;
        case 5: return UV_VIDEO_SINK_AUTOVIDEO;
        case 6: return UV_VIDEO_SINK_FAKESINK;
        case 0:
        default: return UV_VIDEO_SINK_AUTO;
    }
}

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

static guint frame_block_capacity_for(const GuiContext *ctx) {
    if (!ctx) return FRAME_BLOCK_DEFAULT_WIDTH * FRAME_BLOCK_DEFAULT_HEIGHT;
    guint w = ctx->frame_block_width ? ctx->frame_block_width : FRAME_BLOCK_DEFAULT_WIDTH;
    guint h = ctx->frame_block_height ? ctx->frame_block_height : FRAME_BLOCK_DEFAULT_HEIGHT;
    return w * h;
}

static guint frame_block_width_value_for_index(guint index) {
    if (index >= FRAME_BLOCK_WIDTH_OPTION_COUNT) {
        return FRAME_BLOCK_DEFAULT_WIDTH;
    }
    return FRAME_BLOCK_WIDTH_OPTIONS[index];
}

static guint frame_block_width_index_for_value(guint width) {
    for (guint i = 0; i < FRAME_BLOCK_WIDTH_OPTION_COUNT; i++) {
        if (FRAME_BLOCK_WIDTH_OPTIONS[i] == width) {
            return i;
        }
    }
    for (guint i = 0; i < FRAME_BLOCK_WIDTH_OPTION_COUNT; i++) {
        if (FRAME_BLOCK_WIDTH_OPTIONS[i] == FRAME_BLOCK_DEFAULT_WIDTH) {
            return i;
        }
    }
    return 0;
}

static void frame_block_reset_local_buffers(GuiContext *ctx, guint width, guint height) {
    if (!ctx) return;
    guint w = width ? width : FRAME_BLOCK_DEFAULT_WIDTH;
    guint h = height ? height : FRAME_BLOCK_DEFAULT_HEIGHT;
    guint capacity = w * h;

    if (ctx->frame_block_values_lateness) {
        g_array_set_size(ctx->frame_block_values_lateness, capacity);
        for (guint i = 0; i < ctx->frame_block_values_lateness->len; i++) {
            g_array_index(ctx->frame_block_values_lateness, double, i) = NAN;
        }
    }
    if (ctx->frame_block_values_size) {
        g_array_set_size(ctx->frame_block_values_size, capacity);
        for (guint i = 0; i < ctx->frame_block_values_size->len; i++) {
            g_array_index(ctx->frame_block_values_size, double, i) = NAN;
        }
    }
}

static void frame_block_apply_thresholds(GuiContext *ctx) {
    if (!ctx) return;
    if (!ctx->frame_block_threshold_spins[0] ||
        !ctx->frame_block_threshold_spins[1] ||
        !ctx->frame_block_threshold_spins[2]) {
        return;
    }

    double green = gtk_spin_button_get_value(GTK_SPIN_BUTTON(ctx->frame_block_threshold_spins[0]));
    double yellow = gtk_spin_button_get_value(GTK_SPIN_BUTTON(ctx->frame_block_threshold_spins[1]));
    double orange = gtk_spin_button_get_value(GTK_SPIN_BUTTON(ctx->frame_block_threshold_spins[2]));

    if (ctx->frame_block_view == FRAME_BLOCK_VIEW_SIZE) {
        ctx->frame_block_thresholds_kb[0] = green;
        ctx->frame_block_thresholds_kb[1] = yellow;
        ctx->frame_block_thresholds_kb[2] = orange;
        if (ctx->viewer) {
            uv_viewer_frame_block_set_size_thresholds(ctx->viewer, green, yellow, orange);
        }
    } else {
        ctx->frame_block_thresholds_ms[0] = green;
        ctx->frame_block_thresholds_ms[1] = yellow;
        ctx->frame_block_thresholds_ms[2] = orange;
        if (ctx->viewer) {
            uv_viewer_frame_block_set_thresholds(ctx->viewer, green, yellow, orange);
        }
    }
}

static void frame_block_update_summary(GuiContext *ctx) {
    if (!ctx || !ctx->frame_block_summary_label) return;

    guint capacity = frame_block_capacity_for(ctx);
    if (capacity == 0) capacity = 1;
    double fill_pct = (capacity > 0) ? ((double)ctx->frame_block_filled * 100.0 / (double)capacity) : 0.0;

    const char *mode_str = ctx->frame_block_snapshot_mode ? "Snapshot" : "Continuous";
    const char *run_state;
    if (!ctx->frame_block_active) {
        run_state = "Inactive";
    } else if (ctx->frame_block_snapshot_mode && ctx->frame_block_snapshot_complete) {
        run_state = "Complete";
    } else if (ctx->frame_block_paused) {
        run_state = "Paused";
    } else {
        run_state = "Running";
    }

    GString *summary = g_string_new(NULL);

    if (!ctx->frame_block_active && ctx->frame_block_filled == 0) {
        g_string_append(summary, "Frame block capture disabled.");
    } else {
        g_string_append_printf(summary,
                               "Status: %s (%s%s) | Frames %u/%u (%.1f%%)",
                               run_state,
                               mode_str,
                               (ctx->frame_block_snapshot_mode && ctx->frame_block_snapshot_complete) ? ", complete" : "",
                               ctx->frame_block_filled,
                               capacity,
                               fill_pct);

        if (ctx->frame_block_real_samples > 0) {
            g_string_append(summary, " | Lateness min/avg/max: ");
            g_string_append_printf(summary, "%.2f / %.2f / %.2f ms",
                                   ctx->frame_block_min_ms,
                                   ctx->frame_block_avg_ms,
                                   ctx->frame_block_max_ms);

            g_string_append(summary, " | Size min/avg/max: ");
            g_string_append_printf(summary, "%.2f / %.2f / %.2f KB",
                                   ctx->frame_block_min_kb,
                                   ctx->frame_block_avg_kb,
                                   ctx->frame_block_max_kb);

            const guint *counts = (ctx->frame_block_view == FRAME_BLOCK_VIEW_SIZE)
                ? ctx->frame_block_color_counts_kb
                : ctx->frame_block_color_counts_ms;
            const char *labels[FRAME_BLOCK_COLOR_COUNT] = {"Green", "Yellow", "Orange", "Red"};
            gboolean first_bucket = TRUE;
            const char *bucket_title = (ctx->frame_block_view == FRAME_BLOCK_VIEW_SIZE)
                ? "Size buckets"
                : "Lateness buckets";
            for (guint i = 0; i < FRAME_BLOCK_COLOR_COUNT; i++) {
                if (!ctx->frame_block_colors_visible[i]) continue;
                if (counts[i] == 0) continue;
                if (first_bucket) {
                    g_string_append_printf(summary, " | %s: ", bucket_title);
                    first_bucket = FALSE;
                } else {
                    g_string_append(summary, ", ");
                }
                g_string_append_printf(summary, "%s %u", labels[i], counts[i]);
            }
        } else {
            g_string_append(summary, " | Lateness min/avg/max: -- / -- / -- ms");
            g_string_append(summary, " | Size min/avg/max: -- / -- / -- KB");
        }
    }

    g_string_append_printf(summary, " | missing=%u", ctx->frame_block_missing);
    g_string_append_printf(summary, " | real=%u", ctx->frame_block_real_samples);

    gtk_label_set_text(ctx->frame_block_summary_label, summary->str);
    g_string_free(summary, TRUE);
}

static void frame_block_queue_overlay_draws(GuiContext *ctx) {
    if (!ctx) return;
    if (ctx->frame_overlay_lateness) {
        gtk_widget_queue_draw(GTK_WIDGET(ctx->frame_overlay_lateness));
    }
    if (ctx->frame_overlay_size) {
        gtk_widget_queue_draw(GTK_WIDGET(ctx->frame_overlay_size));
    }
}

static gboolean frame_block_stats_latest(const UvFrameBlockStats *fb,
                                         double *out_lateness_ms,
                                         double *out_size_kb,
                                         gboolean *out_missing) {
    if (out_missing) *out_missing = FALSE;
    if (!fb || !fb->lateness_ms || !fb->frame_size_kb) return FALSE;
    guint capacity = MIN(fb->lateness_ms->len, fb->frame_size_kb->len);
    if (capacity == 0) return FALSE;
    guint filled = fb->filled;
    if (filled == 0) return FALSE;
    if (filled > capacity) filled = capacity;

    guint next_index = fb->next_index % capacity;
    for (guint offset = 0; offset < filled; offset++) {
        guint idx = (next_index + capacity - 1 - offset) % capacity;
        double lateness = g_array_index(fb->lateness_ms, double, idx);
        double size = g_array_index(fb->frame_size_kb, double, idx);
        if (isnan(lateness) || isnan(size)) continue;
        if (lateness < 0.0 || size < 0.0) {
            if (out_missing) *out_missing = TRUE;
            return FALSE;
        }
        if (out_lateness_ms) *out_lateness_ms = lateness;
        if (out_size_kb) *out_size_kb = size;
        return TRUE;
    }

    return FALSE;
}

static gboolean frame_overlay_sample_value(const StatsSample *sample, guint metric, double *out_value, gboolean *missing_flag) {
    if (!sample || !out_value) return FALSE;
    if (missing_flag) {
        *missing_flag = FALSE;
    }
    if (!sample->frame_valid) {
        if (missing_flag && sample->frame_missing) {
            *missing_flag = TRUE;
        }
        return FALSE;
    }

    if (metric == FRAME_OVERLAY_METRIC_SIZE) {
        double v = sample->frame_size_kb;
        if (!isfinite(v) || v < 0.0) {
            if (missing_flag && sample->frame_missing) {
                *missing_flag = TRUE;
            }
            return FALSE;
        }
        *out_value = v;
        if (missing_flag && sample->frame_missing) {
            *missing_flag = TRUE;
        }
        return TRUE;
    }

    double v = sample->frame_lateness_ms;
    if (!isfinite(v) || v < 0.0) {
        if (missing_flag && sample->frame_missing) {
            *missing_flag = TRUE;
        }
        return FALSE;
    }
    *out_value = v;
    if (missing_flag && sample->frame_missing) {
        *missing_flag = TRUE;
    }
    return TRUE;
}

static void frame_block_update_metric_toggle_label(GuiContext *ctx) {
    if (!ctx || !ctx->frame_block_metric_toggle) return;
    const char *label = (ctx->frame_block_view == FRAME_BLOCK_VIEW_SIZE)
                            ? "Viewing: Size"
                            : "Viewing: Lateness";
    gtk_button_set_label(GTK_BUTTON(ctx->frame_block_metric_toggle), label);
}

static void frame_block_sync_controls(GuiContext *ctx, const UvFrameBlockStats *fb) {
    if (!ctx) return;
    gboolean active = fb ? fb->active : ctx->frame_block_active;
    gboolean paused = fb ? fb->paused : ctx->frame_block_paused;
    gboolean snapshot_mode = fb ? fb->snapshot_mode : ctx->frame_block_snapshot_mode;

    if (ctx->frame_block_enable_toggle) {
        gboolean toggled = gtk_toggle_button_get_active(ctx->frame_block_enable_toggle);
        if (toggled != active) {
            g_signal_handlers_block_by_func(ctx->frame_block_enable_toggle, G_CALLBACK(on_frame_block_enable_toggled), ctx);
            gtk_toggle_button_set_active(ctx->frame_block_enable_toggle, active);
            g_signal_handlers_unblock_by_func(ctx->frame_block_enable_toggle, G_CALLBACK(on_frame_block_enable_toggled), ctx);
        }
        gtk_widget_set_sensitive(GTK_WIDGET(ctx->frame_block_enable_toggle), TRUE);
    }

    if (ctx->frame_block_pause_toggle) {
        gboolean pause_state = gtk_toggle_button_get_active(ctx->frame_block_pause_toggle);
        if (pause_state != paused) {
            g_signal_handlers_block_by_func(ctx->frame_block_pause_toggle, G_CALLBACK(on_frame_block_pause_toggled), ctx);
            gtk_toggle_button_set_active(ctx->frame_block_pause_toggle, paused);
            g_signal_handlers_unblock_by_func(ctx->frame_block_pause_toggle, G_CALLBACK(on_frame_block_pause_toggled), ctx);
        }
        gtk_widget_set_sensitive(GTK_WIDGET(ctx->frame_block_pause_toggle), active);
    }

    if (ctx->frame_block_mode_dropdown) {
        if (fb) {
            guint current = gtk_drop_down_get_selected(ctx->frame_block_mode_dropdown);
            guint desired = snapshot_mode ? 1u : 0u;
            if (current != desired) {
                g_signal_handlers_block_by_func(ctx->frame_block_mode_dropdown, G_CALLBACK(on_frame_block_mode_changed), ctx);
                gtk_drop_down_set_selected(ctx->frame_block_mode_dropdown, desired);
                g_signal_handlers_unblock_by_func(ctx->frame_block_mode_dropdown, G_CALLBACK(on_frame_block_mode_changed), ctx);
            }
        }
        gtk_widget_set_sensitive(GTK_WIDGET(ctx->frame_block_mode_dropdown), TRUE);
    }

    if (ctx->frame_block_width_dropdown) {
        guint width_value = ctx->frame_block_width ? ctx->frame_block_width : FRAME_BLOCK_DEFAULT_WIDTH;
        guint desired = frame_block_width_index_for_value(width_value);
        guint current = gtk_drop_down_get_selected(ctx->frame_block_width_dropdown);
        if (current != desired) {
            g_signal_handlers_block_by_func(ctx->frame_block_width_dropdown, G_CALLBACK(on_frame_block_width_changed), ctx);
            gtk_drop_down_set_selected(ctx->frame_block_width_dropdown, desired);
            g_signal_handlers_unblock_by_func(ctx->frame_block_width_dropdown, G_CALLBACK(on_frame_block_width_changed), ctx);
        }
        gtk_widget_set_sensitive(GTK_WIDGET(ctx->frame_block_width_dropdown), TRUE);
    }

    if (ctx->frame_block_metric_toggle) {
        gboolean desired = (ctx->frame_block_view == FRAME_BLOCK_VIEW_SIZE);
        gboolean current = gtk_toggle_button_get_active(ctx->frame_block_metric_toggle);
        if (current != desired) {
            g_signal_handlers_block_by_func(ctx->frame_block_metric_toggle, G_CALLBACK(on_frame_block_metric_toggled), ctx);
            gtk_toggle_button_set_active(ctx->frame_block_metric_toggle, desired);
            g_signal_handlers_unblock_by_func(ctx->frame_block_metric_toggle, G_CALLBACK(on_frame_block_metric_toggled), ctx);
        }
        frame_block_update_metric_toggle_label(ctx);
        gtk_widget_set_sensitive(GTK_WIDGET(ctx->frame_block_metric_toggle), TRUE);
    }

    if (ctx->frame_block_reset_button) {
        gtk_widget_set_sensitive(GTK_WIDGET(ctx->frame_block_reset_button), active);
    }

    const double *thresholds = (ctx->frame_block_view == FRAME_BLOCK_VIEW_SIZE)
        ? ctx->frame_block_thresholds_kb
        : ctx->frame_block_thresholds_ms;
    double step = (ctx->frame_block_view == FRAME_BLOCK_VIEW_SIZE) ? 10.0 : 0.5;
    guint digits = (ctx->frame_block_view == FRAME_BLOCK_VIEW_SIZE) ? 1u : 1u;
    const char *unit = (ctx->frame_block_view == FRAME_BLOCK_VIEW_SIZE) ? "KB" : "ms";

    for (guint i = 0; i < 3; i++) {
        GtkSpinButton *spin = ctx->frame_block_threshold_spins[i];
        if (!spin) continue;
        g_signal_handlers_block_by_func(spin, G_CALLBACK(on_frame_block_threshold_changed), ctx);
        gtk_spin_button_set_digits(spin, digits);
        gtk_spin_button_set_increments(spin, step, step * 5.0);
        gtk_spin_button_set_range(spin, 0.0, ctx->frame_block_view == FRAME_BLOCK_VIEW_SIZE ? 100000.0 : 1000.0);
        gtk_spin_button_set_value(spin, thresholds[i]);
        g_signal_handlers_unblock_by_func(spin, G_CALLBACK(on_frame_block_threshold_changed), ctx);

        GtkLabel *label = ctx->frame_block_threshold_labels[i];
        if (label) {
            const char *base = (i == 0) ? "Green" : (i == 1) ? "Yellow" : "Orange";
            char text[64];
            g_snprintf(text, sizeof(text), "%s threshold (%s)", base, unit);
            gtk_label_set_text(label, text);
        }
    }
}

static void frame_block_draw(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer user_data) {
    (void)area;
    GuiContext *ctx = user_data;

    cairo_save(cr);
    cairo_rectangle(cr, 0, 0, width, height);
    cairo_set_source_rgb(cr, 0.12, 0.12, 0.12);
    cairo_fill(cr);
    cairo_restore(cr);

    if (!ctx) return;

    GArray *values = (ctx->frame_block_view == FRAME_BLOCK_VIEW_SIZE)
        ? ctx->frame_block_values_size
        : ctx->frame_block_values_lateness;
    if (!values) return;

    guint w = ctx->frame_block_width ? ctx->frame_block_width : FRAME_BLOCK_DEFAULT_WIDTH;
    guint h = ctx->frame_block_height ? ctx->frame_block_height : FRAME_BLOCK_DEFAULT_HEIGHT;
    guint capacity = w * h;

    if (capacity == 0) return;

    double cell_w = (w > 0) ? (double)width / (double)w : 0.0;
    double cell_h = (h > 0) ? (double)height / (double)h : 0.0;
    double cell_size = MIN(cell_w, cell_h);
    if (cell_size <= 0.0) return;

    double grid_width = cell_size * (double)w;
    double grid_height = cell_size * (double)h;
    double offset_x = (width - grid_width) / 2.0;
    double offset_y = (height - grid_height) / 2.0;

    const double colors[FRAME_BLOCK_COLOR_COUNT][3] = {
        {0.20, 0.78, 0.24},
        {0.96, 0.85, 0.20},
        {0.96, 0.55, 0.18},
        {0.86, 0.12, 0.18}
    };

    for (guint row = 0; row < h; row++) {
        for (guint col = 0; col < w; col++) {
            guint idx = row * w + col;
            double value = NAN;
            if (idx < values->len) {
                value = g_array_index(values, double, idx);
            }
            gboolean has_value = !isnan(value);
            gboolean is_missing = has_value && value < 0.0;

            double r = 0.22, g = 0.22, b = 0.22;
            if (is_missing) {
                r = g = b = 0.0;
            } else if (has_value) {
                const double *thresholds = (ctx->frame_block_view == FRAME_BLOCK_VIEW_SIZE)
                    ? ctx->frame_block_thresholds_kb
                    : ctx->frame_block_thresholds_ms;
                guint bucket = 0;
                if (value <= thresholds[0]) bucket = 0;
                else if (value <= thresholds[1]) bucket = 1;
                else if (value <= thresholds[2]) bucket = 2;
                else bucket = 3;

                if (bucket < FRAME_BLOCK_COLOR_COUNT) {
                    if (ctx->frame_block_colors_visible[bucket]) {
                        r = colors[bucket][0];
                        g = colors[bucket][1];
                        b = colors[bucket][2];
                    } else {
                        r = g = b = 0.28;
                    }
                }
            }

            double x = offset_x + (double)col * cell_size;
            double y = offset_y + (double)row * cell_size;
            cairo_rectangle(cr, x, y, cell_size, cell_size);
            cairo_set_source_rgb(cr, r, g, b);
            cairo_fill(cr);
        }
    }

    if (ctx->frame_block_active && ctx->frame_block_next_index < capacity) {
        guint idx = ctx->frame_block_next_index;
        guint row = idx / w;
        guint col = idx % w;
        double x = offset_x + (double)col * cell_size;
        double y = offset_y + (double)row * cell_size;
        double inset = MIN(cell_size * 0.15, 1.5);
        double rect_size = cell_size - 2.0 * inset;
        if (rect_size < cell_size * 0.2) {
            rect_size = cell_size * 0.2;
            inset = (cell_size - rect_size) / 2.0;
        }
        cairo_save(cr);
        cairo_rectangle(cr, x + inset, y + inset, rect_size, rect_size);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.6);
        cairo_set_line_width(cr, MAX(cell_size * 0.12, 0.6));
        cairo_stroke(cr);
        cairo_restore(cr);
    }

    if (ctx->frame_block_snapshot_mode && ctx->frame_block_snapshot_complete) {
        cairo_save(cr);
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.45);
        cairo_rectangle(cr, 0, height - 28, width, 28);
        cairo_fill(cr);

        cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 14.0);
        const char *msg = "Snapshot complete";
        cairo_text_extents_t ext;
        cairo_text_extents(cr, msg, &ext);
        double tx = (width - ext.width) / 2.0;
        double ty = height - 10.0;
        cairo_move_to(cr, tx, ty);
        cairo_show_text(cr, msg);
        cairo_restore(cr);
    }
}

static void frame_overlay_draw(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer user_data) {
    GuiContext *ctx = user_data;
    if (!ctx || width <= 0 || height <= 0) return;

    guint metric = FRAME_OVERLAY_METRIC_LATENESS;
    gpointer tag = g_object_get_data(G_OBJECT(area), "overlay-metric");
    if (tag) {
        metric = GPOINTER_TO_UINT(tag);
        if (metric > FRAME_OVERLAY_METRIC_SIZE) {
            metric = FRAME_OVERLAY_METRIC_LATENESS;
        }
    }

    GtkLabel *live_label = GTK_LABEL(g_object_get_data(G_OBJECT(area), "overlay-live-label"));
    GtkLabel *max_label = GTK_LABEL(g_object_get_data(G_OBJECT(area), "overlay-max-label"));
    const char *default_live = "Live: --";
    const char *default_max = "Max: --";

    cairo_save(cr);
    cairo_set_source_rgb(cr, 0.10, 0.10, 0.12);
    cairo_paint(cr);

    cairo_set_source_rgba(cr, 1, 1, 1, 0.1);
    cairo_rectangle(cr, 0.5, 0.5, width - 1.0, height - 1.0);
    cairo_stroke(cr);

    if (!ctx->stats_history || ctx->stats_history->len == 0) {
        if (live_label) gtk_label_set_text(live_label, default_live);
        if (max_label) gtk_label_set_text(max_label, default_max);
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

    double max_val = -G_MAXDOUBLE;
    gboolean any_value = FALSE;
    gboolean missing_seen = FALSE;

    for (guint i = start_index; i < len; i++) {
        double value = 0.0;
        gboolean missing_flag = FALSE;
        if (!frame_overlay_sample_value(&samples[i], metric, &value, &missing_flag)) {
            if (missing_flag) missing_seen = TRUE;
            continue;
        }
        if (value > max_val) max_val = value;
        any_value = TRUE;
    }

    if (!any_value) {
        if (live_label) gtk_label_set_text(live_label, default_live);
        if (max_label) gtk_label_set_text(max_label, default_max);
        cairo_set_source_rgb(cr, 0.7, 0.7, 0.7);
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 12.0);
        cairo_move_to(cr, 10, height / 2.0);
        cairo_show_text(cr, "No samples in range");
        cairo_restore(cr);
        return;
    }

    double peak_value = max_val;
    if (!isfinite(peak_value)) {
        peak_value = 0.0;
    }

    double axis_min = 0.0;
    double axis_max = peak_value;
    if (!isfinite(axis_max) || axis_max <= axis_min) {
        axis_max = axis_min + 1.0;
    }

    const double left_margin = 60.0;
    const double right_margin = 12.0;
    const double top_margin = 16.0;
    const double bottom_margin = 24.0;
    double plot_width = MAX(1.0, width - (left_margin + right_margin));
    double plot_height = MAX(1.0, height - (top_margin + bottom_margin));
    double plot_left = left_margin;
    double plot_top = top_margin;
    double plot_bottom = plot_top + plot_height;
    double plot_right = plot_left + plot_width;

    const int tick_count = 4;
    cairo_set_source_rgba(cr, 1, 1, 1, 0.1);
    for (int i = 0; i <= tick_count; i++) {
        double y = plot_bottom - (plot_height / tick_count) * i;
        cairo_move_to(cr, plot_left, y);
        cairo_line_to(cr, plot_right, y);
    }
    cairo_stroke(cr);

    cairo_set_source_rgb(cr, 0.8, 0.8, 0.85);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 11.0);
    for (int i = 0; i <= tick_count; i++) {
        double fraction = (double)i / (double)tick_count;
        double value = axis_min + (axis_max - axis_min) * fraction;
        double y = plot_bottom - plot_height * fraction;

        char tick_label[64];
        g_snprintf(tick_label, sizeof(tick_label), "%.2f", value);
        cairo_text_extents_t label_extents;
        cairo_text_extents(cr, tick_label, &label_extents);
        double text_x = plot_left - 8.0 - (label_extents.width + label_extents.x_bearing);
        double text_y = y + (label_extents.height / 2.0) - label_extents.y_bearing;
        cairo_move_to(cr, text_x, text_y);
        cairo_show_text(cr, tick_label);
    }

    cairo_set_source_rgb(cr, 0.3, 0.7, 1.0);
    cairo_set_line_width(cr, 1.5);
    gboolean path_started = FALSE;
    double axis_range = axis_max - axis_min;
    if (axis_range <= 0.0) {
        axis_range = 1.0;
    }
    for (guint i = start_index; i < len; i++) {
        const StatsSample *sample = &samples[i];
        double value = 0.0;
        gboolean missing_flag = FALSE;
        if (!frame_overlay_sample_value(sample, metric, &value, &missing_flag)) {
            path_started = FALSE;
            continue;
        }

        if (value < axis_min) value = axis_min;
        double x_ratio = (sample->timestamp - start_time) / range;
        if (x_ratio < 0.0) x_ratio = 0.0;
        if (x_ratio > 1.0) x_ratio = 1.0;
        double x = plot_left + x_ratio * plot_width;

        double y_ratio = (value - axis_min) / axis_range;
        if (y_ratio < 0.0) y_ratio = 0.0;
        if (y_ratio > 1.0) y_ratio = 1.0;
        double y = plot_bottom - y_ratio * plot_height;

        if (!path_started) {
            cairo_move_to(cr, x, y);
            path_started = TRUE;
        } else {
            cairo_line_to(cr, x, y);
        }
    }
    cairo_stroke(cr);

    double latest_value = NAN;
    for (guint i = len; i > start_index; i--) {
        double value = 0.0;
        if (frame_overlay_sample_value(&samples[i - 1], metric, &value, NULL)) {
            latest_value = value;
            break;
        }
    }

    if (!isfinite(latest_value)) {
        if (live_label) gtk_label_set_text(live_label, default_live);
    } else if (metric == FRAME_OVERLAY_METRIC_SIZE) {
        char latest_text[64];
        g_snprintf(latest_text, sizeof(latest_text), "Live: %.2f KB", latest_value);
        if (live_label) gtk_label_set_text(live_label, latest_text);
    } else {
        char latest_text[64];
        g_snprintf(latest_text, sizeof(latest_text), "Live: %.2f ms", latest_value);
        if (live_label) gtk_label_set_text(live_label, latest_text);
    }

    if (isfinite(peak_value)) {
        char max_text[64];
        if (metric == FRAME_OVERLAY_METRIC_SIZE) {
            g_snprintf(max_text, sizeof(max_text), "Max: %.2f KB", peak_value);
        } else {
            g_snprintf(max_text, sizeof(max_text), "Max: %.2f ms", peak_value);
        }
        if (max_label) gtk_label_set_text(max_label, max_text);
    } else if (max_label) {
        gtk_label_set_text(max_label, default_max);
    }

    if (missing_seen) {
        cairo_set_source_rgba(cr, 0.8, 0.6, 0.2, 0.8);
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 11.0);
        cairo_move_to(cr, plot_left + 6.0, plot_top + 14.0);
        cairo_show_text(cr, "Missing frames present");
    }

    cairo_restore(cr);
}

static void update_status(GuiContext *ctx, const char *message) {
    if (!ctx || !ctx->status_label) return;
    gtk_label_set_text(ctx->status_label, message ? message : "");
}

static void update_info_label(GuiContext *ctx) {
    if (!ctx || !ctx->info_label) return;
    char info[160];
    UvViewerConfig *cfg = &ctx->current_cfg;
    guint vr_den = cfg->videorate_fps_denominator ? cfg->videorate_fps_denominator : 1;
    char videorate_info[24];
    if (cfg->videorate_enabled && cfg->videorate_fps_numerator > 0) {
        g_snprintf(videorate_info, sizeof(videorate_info), "%u/%u", cfg->videorate_fps_numerator, vr_den);
    } else {
        g_strlcpy(videorate_info, "off", sizeof(videorate_info));
    }
    const char *audio_state;
    if (!cfg->audio_enabled) {
        audio_state = "off";
    } else if (!ctx->audio_runtime_enabled) {
        audio_state = "error";
    } else if (ctx->audio_active) {
        audio_state = "active";
    } else {
        audio_state = "waiting";
    }
    const char *decoder_pref = decoder_option_labels[decoder_pref_to_index(cfg->decoder_preference)];
    if (!decoder_pref) decoder_pref = "Auto";
    const char *sink_pref = video_sink_option_labels[video_sink_pref_to_index(cfg->video_sink_preference)];
    if (!sink_pref) sink_pref = "Auto";
    g_snprintf(info, sizeof(info),
               "Listening on %d | PT %d | Clock %d | %s | Jitter %ums | Queue buffers %u"
               " | drop=%s | lost=%s | bus-msg=%s | videorate=%s | decoder=%s | sink=%s | audio=%s",
               cfg->listen_port,
               cfg->payload_type,
               cfg->clock_rate,
               cfg->sync_to_clock ? "sync" : "no-sync",
               cfg->jitter_latency_ms,
               cfg->queue_max_buffers,
               cfg->jitter_drop_on_latency ? "on" : "off",
               cfg->jitter_do_lost ? "on" : "off",
               cfg->jitter_post_drop_messages ? "on" : "off",
               videorate_info,
               decoder_pref,
               sink_pref,
               audio_state);
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
    if (ctx->stats_refresh_spin) {
        gtk_spin_button_set_value(ctx->stats_refresh_spin, ctx->stats_refresh_interval_ms);
    }
    if (ctx->decoder_dropdown) {
        gtk_drop_down_set_selected(ctx->decoder_dropdown,
                                   decoder_pref_to_index(ctx->current_cfg.decoder_preference));
    }
    if (ctx->sink_dropdown) {
        gtk_drop_down_set_selected(ctx->sink_dropdown,
                                   video_sink_pref_to_index(ctx->current_cfg.video_sink_preference));
    }
    if (ctx->videorate_toggle) {
        check_set(ctx->videorate_toggle, ctx->current_cfg.videorate_enabled);
    }
    if (ctx->videorate_num_spin) {
        gtk_spin_button_set_value(ctx->videorate_num_spin, ctx->current_cfg.videorate_fps_numerator);
    }
    if (ctx->videorate_den_spin) {
        guint den = ctx->current_cfg.videorate_fps_denominator ? ctx->current_cfg.videorate_fps_denominator : 1;
        gtk_spin_button_set_value(ctx->videorate_den_spin, den);
    }
    gboolean videorate_sensitive = ctx->current_cfg.videorate_enabled;
    if (ctx->videorate_num_spin) {
        gtk_widget_set_sensitive(GTK_WIDGET(ctx->videorate_num_spin), videorate_sensitive);
    }
    if (ctx->videorate_den_spin) {
        gtk_widget_set_sensitive(GTK_WIDGET(ctx->videorate_den_spin), videorate_sensitive);
    }
    if (ctx->audio_toggle) {
        check_set(ctx->audio_toggle, ctx->current_cfg.audio_enabled ? TRUE : FALSE);
    }
    if (ctx->audio_payload_spin) {
        gtk_spin_button_set_value(ctx->audio_payload_spin, ctx->current_cfg.audio_payload_type);
    }
    if (ctx->audio_jitter_spin) {
        gtk_spin_button_set_value(ctx->audio_jitter_spin, ctx->current_cfg.audio_jitter_latency_ms);
    }
    gboolean audio_sensitive = ctx->current_cfg.audio_enabled;
    if (ctx->audio_payload_spin) {
        gtk_widget_set_sensitive(GTK_WIDGET(ctx->audio_payload_spin), audio_sensitive);
    }
    if (ctx->audio_jitter_spin) {
        gtk_widget_set_sensitive(GTK_WIDGET(ctx->audio_jitter_spin), audio_sensitive);
    }
    check_set(ctx->sync_toggle_settings, ctx->current_cfg.sync_to_clock ? TRUE : FALSE);
    check_set(ctx->jitter_drop_toggle, ctx->current_cfg.jitter_drop_on_latency ? TRUE : FALSE);
    check_set(ctx->jitter_do_lost_toggle, ctx->current_cfg.jitter_do_lost ? TRUE : FALSE);
    check_set(ctx->jitter_post_drop_toggle, ctx->current_cfg.jitter_post_drop_messages ? TRUE : FALSE);
    update_info_label(ctx);
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
        case STATS_METRIC_FPS:     return sample->fps_current;
        default:                   return 0.0;
    }
}

static gboolean stats_history_push_frame_block_updates(GuiContext *ctx,
                                                       const UvFrameBlockStats *fb,
                                                       guint prev_next_index,
                                                       guint prev_filled,
                                                       guint prev_capacity,
                                                       double prev_refresh_time,
                                                       double now,
                                                       const StatsSample *latest_metrics) {
    if (!ctx || !fb || !fb->lateness_ms || !fb->frame_size_kb) return FALSE;

    guint width = fb->width ? fb->width : FRAME_BLOCK_DEFAULT_WIDTH;
    guint height = fb->height ? fb->height : FRAME_BLOCK_DEFAULT_HEIGHT;
    guint capacity = width * height;
    if (capacity == 0) return FALSE;

    guint filled = fb->filled;
    if (filled > capacity) filled = capacity;
    guint next_index = fb->next_index;
    if (next_index > capacity) next_index = capacity;

    gboolean treat_all_new = FALSE;
    if (prev_capacity != capacity) treat_all_new = TRUE;
    if (next_index < prev_next_index) treat_all_new = TRUE;
    if (filled < prev_filled) treat_all_new = TRUE;

    guint start = treat_all_new ? 0u : MIN(prev_next_index, filled);
    guint end = treat_all_new ? filled : MIN(next_index, filled);
    if (fb->lateness_ms->len < end) end = fb->lateness_ms->len;
    if (fb->frame_size_kb->len < end) end = fb->frame_size_kb->len;
    if (end <= start) return FALSE;

    guint appendable = 0;
    for (guint idx = start; idx < end; idx++) {
        double lateness = g_array_index(fb->lateness_ms, double, idx);
        double size = g_array_index(fb->frame_size_kb, double, idx);
        gboolean missing = (lateness == FRAME_BLOCK_MISSING_SENTINEL) || (size == FRAME_BLOCK_MISSING_SENTINEL);
        gboolean valid = isfinite(lateness) && isfinite(size) && lateness >= 0.0 && size >= 0.0;
        if (missing || valid) appendable++;
    }
    if (appendable == 0) return FALSE;

    double last_ts = 0.0;
    if (ctx->stats_history && ctx->stats_history->len > 0) {
        StatsSample *existing = (StatsSample *)ctx->stats_history->data;
        last_ts = existing[ctx->stats_history->len - 1].timestamp;
    }

    double base_time = (prev_refresh_time > 0.0) ? prev_refresh_time : now;
    if (base_time < last_ts) base_time = last_ts;
    double span = now - base_time;
    if (span < 0.0) span = 0.0;
    double min_span = (double)appendable * 0.001;
    if (span < min_span) {
        base_time = now - min_span;
        if (base_time < last_ts) base_time = last_ts;
        span = now - base_time;
        if (span < 0.0) span = 0.0;
    }
    double step = (appendable > 0 && span > 0.0) ? (span / (double)appendable) : 0.0;
    double current_ts = base_time;
    double last_output_ts = last_ts;
    gboolean appended_any = FALSE;
    guint emitted_index = 0;

    for (guint idx = start; idx < end; idx++) {
        double lateness = g_array_index(fb->lateness_ms, double, idx);
        double size = g_array_index(fb->frame_size_kb, double, idx);
        gboolean missing = (lateness == FRAME_BLOCK_MISSING_SENTINEL) || (size == FRAME_BLOCK_MISSING_SENTINEL);
        gboolean valid = isfinite(lateness) && isfinite(size) && lateness >= 0.0 && size >= 0.0;
        if (!missing && !valid) {
            continue;
        }

        if (appendable > 0) {
            current_ts += step;
        }
        if (current_ts <= last_output_ts) current_ts = last_output_ts + 1e-6;
        last_output_ts = current_ts;
        emitted_index++;

        StatsSample sample = {0};
        sample.timestamp = current_ts;
        sample.rate_bps = NAN;
        sample.lost_packets = NAN;
        sample.dup_packets = NAN;
        sample.reorder_packets = NAN;
        sample.jitter_ms = NAN;
        sample.fps_current = NAN;

        if (valid) {
            sample.frame_valid = TRUE;
            sample.frame_missing = FALSE;
            sample.frame_lateness_ms = lateness;
            sample.frame_size_kb = size;
        } else {
            sample.frame_valid = FALSE;
            sample.frame_missing = TRUE;
            sample.frame_lateness_ms = NAN;
            sample.frame_size_kb = NAN;
        }

        if (emitted_index == appendable && latest_metrics) {
            sample.rate_bps = latest_metrics->rate_bps;
            sample.lost_packets = latest_metrics->lost_packets;
            sample.dup_packets = latest_metrics->dup_packets;
            sample.reorder_packets = latest_metrics->reorder_packets;
            sample.jitter_ms = latest_metrics->jitter_ms;
            sample.fps_current = latest_metrics->fps_current;
        }

        stats_history_push(ctx, &sample);
        appended_any = TRUE;
    }

    return appended_any;
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

    guint prev_block_next = ctx->frame_block_next_index;
    guint prev_block_filled = ctx->frame_block_filled;
    guint prev_block_capacity = frame_block_capacity_for(ctx);
    double prev_refresh_time = ctx->stats_last_refresh_time;

    UvViewerStats stats = {0};
    uv_viewer_stats_init(&stats);

    if (!uv_viewer_get_stats(ctx->viewer, &stats)) {
        update_status(ctx, "Failed to fetch stats");
        uv_viewer_stats_clear(&stats);
        return;
    }

    ctx->audio_runtime_enabled = stats.audio_enabled;
    ctx->audio_active = stats.audio_active;
    update_info_label(ctx);

    guint source_count = (stats.sources) ? stats.sources->len : 0u;
    guint selected_index = GTK_INVALID_LIST_POSITION;
    UvSourceStats *selected_source = NULL;

    if (source_count == 0) {
        update_status(ctx, "Listening for sources...");
        if (ctx->source_model && ctx->known_source_count > 0) {
            gtk_string_list_splice(ctx->source_model, 0, ctx->known_source_count, NULL);
            ctx->known_source_count = 0;
        }
        if (ctx->source_dropdown) {
            ctx->suppress_source_change = TRUE;
            gtk_drop_down_set_selected(ctx->source_dropdown, GTK_INVALID_LIST_POSITION);
            ctx->suppress_source_change = FALSE;
            gtk_widget_set_sensitive(GTK_WIDGET(ctx->source_dropdown), FALSE);
        }
        if (ctx->source_detail_label) {
            gtk_label_set_text(ctx->source_detail_label, "No sources discovered yet.");
        }
    } else {
        if (ctx->source_model) {
            if (ctx->known_source_count > source_count) {
                guint remove_count = ctx->known_source_count - source_count;
                gtk_string_list_splice(ctx->source_model, source_count, remove_count, NULL);
            }
            for (guint i = ctx->known_source_count; i < source_count; i++) {
                UvSourceStats *src = &g_array_index(stats.sources, UvSourceStats, i);
                char label[128];
                g_snprintf(label, sizeof(label), "%u: %s", i, src->address);
                gtk_string_list_append(ctx->source_model, label);
            }
            ctx->known_source_count = source_count;
        }

        for (guint i = 0; i < source_count; i++) {
            UvSourceStats *src = &g_array_index(stats.sources, UvSourceStats, i);
            if (src->selected) {
                selected_index = i;
                selected_source = src;
                break;
            }
        }

        if (ctx->source_dropdown) {
            gtk_widget_set_sensitive(GTK_WIDGET(ctx->source_dropdown), TRUE);
        }

        if (selected_source && ctx->source_dropdown) {
            guint current = gtk_drop_down_get_selected(ctx->source_dropdown);
            if (current != selected_index) {
                ctx->suppress_source_change = TRUE;
                gtk_drop_down_set_selected(ctx->source_dropdown, selected_index);
                ctx->suppress_source_change = FALSE;
            }
        }

        if (ctx->source_detail_label) {
            if (selected_source) {
                char rate_buf[64];
                format_bitrate(selected_source->inbound_bitrate_bps, rate_buf, sizeof(rate_buf));
                char detail[256];
                g_snprintf(detail, sizeof(detail),
                           "%u: %s\nrx=%" G_GUINT64_FORMAT "/%" G_GUINT64_FORMAT
                           " fwd=%" G_GUINT64_FORMAT "/%" G_GUINT64_FORMAT
                           " rate=%s jitter=%.2fms last_seen=%.1fs",
                           selected_index,
                           selected_source->address,
                           selected_source->rx_packets,
                           selected_source->rx_bytes,
                           selected_source->forwarded_packets,
                           selected_source->forwarded_bytes,
                           rate_buf,
                           selected_source->rfc3550_jitter_ms,
                           selected_source->seconds_since_last_seen >= 0.0 ? selected_source->seconds_since_last_seen : 0.0);
                gtk_label_set_text(ctx->source_detail_label, detail);
            } else {
                gtk_label_set_text(ctx->source_detail_label, "Select a source to view details.");
            }
        }

        update_status(ctx, "");
    }

    double refresh_now = g_get_monotonic_time() / 1e6;

    if (selected_source) {
        StatsSample metrics = {0};
        metrics.timestamp = refresh_now;
        metrics.rate_bps = selected_source->inbound_bitrate_bps;
        metrics.lost_packets = (double)selected_source->rtp_lost_packets;
        metrics.dup_packets = (double)selected_source->rtp_duplicate_packets;
        metrics.reorder_packets = (double)selected_source->rtp_reordered_packets;
        metrics.jitter_ms = selected_source->rfc3550_jitter_ms;
        metrics.fps_current = stats.decoder.instantaneous_fps;

        gboolean appended = FALSE;
        if (stats.frame_block_valid) {
            appended = stats_history_push_frame_block_updates(ctx,
                                                              &stats.frame_block,
                                                              prev_block_next,
                                                              prev_block_filled,
                                                              prev_block_capacity,
                                                              prev_refresh_time,
                                                              refresh_now,
                                                              &metrics);
        }

        if (!appended) {
            double latest_lateness = NAN;
            double latest_size = NAN;
            gboolean latest_missing = FALSE;
            gboolean frame_metrics_valid = FALSE;
            if (stats.frame_block_valid) {
                frame_metrics_valid = frame_block_stats_latest(&stats.frame_block,
                                                              &latest_lateness,
                                                              &latest_size,
                                                              &latest_missing);
            }

            StatsSample sample = metrics;
            sample.frame_valid = frame_metrics_valid;
            sample.frame_missing = latest_missing;
            sample.frame_lateness_ms = frame_metrics_valid ? latest_lateness : NAN;
            sample.frame_size_kb = frame_metrics_valid ? latest_size : NAN;
            stats_history_push(ctx, &sample);
        }

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

    if (stats.frame_block_valid) {
        UvFrameBlockStats *fb = &stats.frame_block;
        ctx->frame_block_active = fb->active;
        ctx->frame_block_paused = fb->paused;
        ctx->frame_block_snapshot_mode = fb->snapshot_mode;
        ctx->frame_block_snapshot_complete = fb->snapshot_complete;
        ctx->frame_block_width = fb->width;
        ctx->frame_block_height = fb->height;
        ctx->frame_block_filled = fb->filled;
        ctx->frame_block_next_index = fb->next_index;
        memcpy(ctx->frame_block_thresholds_ms, fb->thresholds_lateness_ms, sizeof(ctx->frame_block_thresholds_ms));
        memcpy(ctx->frame_block_thresholds_kb, fb->thresholds_size_kb, sizeof(ctx->frame_block_thresholds_kb));
        ctx->frame_block_min_ms = fb->min_lateness_ms;
        ctx->frame_block_max_ms = fb->max_lateness_ms;
        ctx->frame_block_avg_ms = fb->avg_lateness_ms;
        ctx->frame_block_min_kb = fb->min_size_kb;
        ctx->frame_block_max_kb = fb->max_size_kb;
        ctx->frame_block_avg_kb = fb->avg_size_kb;
        ctx->frame_block_real_samples = fb->real_frames;
        ctx->frame_block_missing = fb->missing_frames;
        memcpy(ctx->frame_block_color_counts_ms, fb->color_counts_lateness, sizeof(ctx->frame_block_color_counts_ms));
        memcpy(ctx->frame_block_color_counts_kb, fb->color_counts_size, sizeof(ctx->frame_block_color_counts_kb));

        guint capacity = 0;
        if (fb->lateness_ms) capacity = fb->lateness_ms->len;
        if (capacity == 0) {
            guint w = ctx->frame_block_width ? ctx->frame_block_width : FRAME_BLOCK_DEFAULT_WIDTH;
            guint h = ctx->frame_block_height ? ctx->frame_block_height : FRAME_BLOCK_DEFAULT_HEIGHT;
            capacity = w * h;
        }
        if (!ctx->frame_block_values_lateness) {
            ctx->frame_block_values_lateness = g_array_new(FALSE, TRUE, sizeof(double));
        }
        if (!ctx->frame_block_values_size) {
            ctx->frame_block_values_size = g_array_new(FALSE, TRUE, sizeof(double));
        }
        g_array_set_size(ctx->frame_block_values_lateness, capacity);
        g_array_set_size(ctx->frame_block_values_size, capacity);
        if (fb->lateness_ms && fb->lateness_ms->len == capacity && capacity > 0) {
            memcpy(ctx->frame_block_values_lateness->data, fb->lateness_ms->data, sizeof(double) * capacity);
        } else {
            for (guint i = 0; i < ctx->frame_block_values_lateness->len; i++) {
                g_array_index(ctx->frame_block_values_lateness, double, i) = NAN;
            }
        }
        if (fb->frame_size_kb && fb->frame_size_kb->len == capacity && capacity > 0) {
            memcpy(ctx->frame_block_values_size->data, fb->frame_size_kb->data, sizeof(double) * capacity);
        } else {
            for (guint i = 0; i < ctx->frame_block_values_size->len; i++) {
                g_array_index(ctx->frame_block_values_size, double, i) = NAN;
            }
        }

        frame_block_sync_controls(ctx, fb);
    } else {
        ctx->frame_block_active = FALSE;
        ctx->frame_block_paused = FALSE;
        ctx->frame_block_snapshot_complete = FALSE;
        ctx->frame_block_filled = 0;
        ctx->frame_block_next_index = 0;
        ctx->frame_block_min_ms = 0.0;
        ctx->frame_block_max_ms = 0.0;
        ctx->frame_block_avg_ms = 0.0;
        ctx->frame_block_min_kb = 0.0;
        ctx->frame_block_max_kb = 0.0;
        ctx->frame_block_avg_kb = 0.0;
        ctx->frame_block_real_samples = 0;
        ctx->frame_block_missing = 0;
        memset(ctx->frame_block_color_counts_ms, 0, sizeof(ctx->frame_block_color_counts_ms));
        memset(ctx->frame_block_color_counts_kb, 0, sizeof(ctx->frame_block_color_counts_kb));
        if (ctx->frame_block_values_lateness) {
            g_array_set_size(ctx->frame_block_values_lateness, 0);
        }
        if (ctx->frame_block_values_size) {
            g_array_set_size(ctx->frame_block_values_size, 0);
        }
        frame_block_sync_controls(ctx, NULL);
    }

    frame_block_update_summary(ctx);
    if (ctx->frame_block_area) {
        gtk_widget_queue_draw(GTK_WIDGET(ctx->frame_block_area));
    }
    frame_block_queue_overlay_draws(ctx);

    ctx->stats_last_refresh_time = refresh_now;

    uv_viewer_stats_clear(&stats);
}

static gboolean stats_timeout_cb(gpointer user_data) {
    GuiContext *ctx = user_data;
    if (!ctx || !ctx->window) return G_SOURCE_REMOVE;
    refresh_stats(ctx);
    return G_SOURCE_CONTINUE;
}

static void restart_stats_timer(GuiContext *ctx) {
    if (!ctx) return;
    if (ctx->stats_timeout_id) {
        GSource *source = g_main_context_find_source_by_id(NULL, ctx->stats_timeout_id);
        if (source) {
            g_source_destroy(source);
        }
        ctx->stats_timeout_id = 0;
    }
    if (ctx->stats_refresh_interval_ms > 0) {
        ctx->stats_timeout_id = g_timeout_add(ctx->stats_refresh_interval_ms, stats_timeout_cb, ctx);
    }
}

static void set_stats_refresh_interval(GuiContext *ctx, guint interval_ms) {
    if (!ctx) return;
    guint clamped = interval_ms < 50 ? 50 : interval_ms;
    if (ctx->stats_refresh_interval_ms != clamped || ctx->stats_timeout_id == 0) {
        ctx->stats_refresh_interval_ms = clamped;
        restart_stats_timer(ctx);
    }
    if (ctx->stats_refresh_spin) {
        gtk_spin_button_set_value(ctx->stats_refresh_spin, ctx->stats_refresh_interval_ms);
    }
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

static void on_frame_block_enable_toggled(GtkToggleButton *button, gpointer user_data) {
    GuiContext *ctx = user_data;
    if (!ctx) return;
    gboolean enabled = gtk_toggle_button_get_active(button);
    gboolean snapshot_mode = FALSE;
    if (ctx->frame_block_mode_dropdown) {
        snapshot_mode = (gtk_drop_down_get_selected(ctx->frame_block_mode_dropdown) == 1);
    }
    ctx->frame_block_active = enabled;
    ctx->frame_block_snapshot_mode = snapshot_mode;
    ctx->frame_block_snapshot_complete = FALSE;
    ctx->frame_block_filled = 0;
    ctx->frame_block_next_index = 0;
    ctx->frame_block_min_ms = 0.0;
    ctx->frame_block_max_ms = 0.0;
    ctx->frame_block_avg_ms = 0.0;
    ctx->frame_block_min_kb = 0.0;
    ctx->frame_block_max_kb = 0.0;
    ctx->frame_block_avg_kb = 0.0;
    ctx->frame_block_missing = 0;
    ctx->frame_block_real_samples = 0;
    memset(ctx->frame_block_color_counts_ms, 0, sizeof(ctx->frame_block_color_counts_ms));
    memset(ctx->frame_block_color_counts_kb, 0, sizeof(ctx->frame_block_color_counts_kb));
    if (!enabled) {
        ctx->frame_block_paused = FALSE;
    }

    if (ctx->viewer) {
        uv_viewer_frame_block_configure(ctx->viewer, enabled, snapshot_mode);
        if (enabled) {
            frame_block_apply_thresholds(ctx);
        }
    }

    frame_block_sync_controls(ctx, NULL);
    frame_block_update_summary(ctx);
    if (ctx->frame_block_area) gtk_widget_queue_draw(GTK_WIDGET(ctx->frame_block_area));
    frame_block_queue_overlay_draws(ctx);
}

static void on_frame_block_pause_toggled(GtkToggleButton *button, gpointer user_data) {
    GuiContext *ctx = user_data;
    if (!ctx) return;
    gboolean paused = gtk_toggle_button_get_active(button);
    ctx->frame_block_paused = paused;
    if (ctx->viewer && ctx->frame_block_active) {
        uv_viewer_frame_block_pause(ctx->viewer, paused);
    }
    frame_block_update_summary(ctx);
}

static void on_frame_block_mode_changed(GObject *dropdown, GParamSpec *pspec, gpointer user_data) {
    (void)pspec;
    GuiContext *ctx = user_data;
    if (!ctx || !GTK_IS_DROP_DOWN(dropdown)) return;
    gboolean snapshot_mode = (gtk_drop_down_get_selected(GTK_DROP_DOWN(dropdown)) == 1);
    ctx->frame_block_snapshot_mode = snapshot_mode;
    if (ctx->viewer && ctx->frame_block_active) {
        uv_viewer_frame_block_configure(ctx->viewer, TRUE, snapshot_mode);
    }
    frame_block_update_summary(ctx);
}

static void on_frame_block_width_changed(GObject *dropdown, GParamSpec *pspec, gpointer user_data) {
    (void)pspec;
    GuiContext *ctx = user_data;
    if (!ctx || !GTK_IS_DROP_DOWN(dropdown)) return;

    guint selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(dropdown));
    if (selected == GTK_INVALID_LIST_POSITION) return;

    guint new_width = frame_block_width_value_for_index(selected);
    if (new_width == 0) new_width = FRAME_BLOCK_DEFAULT_WIDTH;
    if (ctx->frame_block_width == new_width) return;

    ctx->frame_block_width = new_width;
    ctx->frame_block_filled = 0;
    ctx->frame_block_next_index = 0;
    ctx->frame_block_snapshot_complete = FALSE;
    ctx->frame_block_min_ms = 0.0;
    ctx->frame_block_max_ms = 0.0;
    ctx->frame_block_avg_ms = 0.0;
    ctx->frame_block_min_kb = 0.0;
    ctx->frame_block_max_kb = 0.0;
    ctx->frame_block_avg_kb = 0.0;
    ctx->frame_block_missing = 0;
    ctx->frame_block_real_samples = 0;
    memset(ctx->frame_block_color_counts_ms, 0, sizeof(ctx->frame_block_color_counts_ms));
    memset(ctx->frame_block_color_counts_kb, 0, sizeof(ctx->frame_block_color_counts_kb));

    frame_block_reset_local_buffers(ctx, new_width, ctx->frame_block_height);
    frame_block_update_summary(ctx);
    if (ctx->viewer) {
        uv_viewer_frame_block_set_width(ctx->viewer, new_width);
    }
    if (ctx->frame_block_area) gtk_widget_queue_draw(GTK_WIDGET(ctx->frame_block_area));
    frame_block_queue_overlay_draws(ctx);
}

static void on_frame_block_metric_toggled(GtkToggleButton *button, gpointer user_data) {
    GuiContext *ctx = user_data;
    if (!ctx || !GTK_IS_TOGGLE_BUTTON(button)) return;
    gboolean active = gtk_toggle_button_get_active(button);
    guint new_view = active ? FRAME_BLOCK_VIEW_SIZE : FRAME_BLOCK_VIEW_LATENESS;
    if (ctx->frame_block_view == new_view) {
        frame_block_update_metric_toggle_label(ctx);
        return;
    }
    ctx->frame_block_view = new_view;
    frame_block_sync_controls(ctx, NULL);
    frame_block_update_summary(ctx);
    if (ctx->frame_block_area) gtk_widget_queue_draw(GTK_WIDGET(ctx->frame_block_area));
    frame_block_queue_overlay_draws(ctx);
}

static void on_frame_block_reset_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    GuiContext *ctx = user_data;
    if (!ctx) return;
    if (ctx->viewer) {
        uv_viewer_frame_block_reset(ctx->viewer);
    }
    ctx->frame_block_filled = 0;
    ctx->frame_block_snapshot_complete = FALSE;
    ctx->frame_block_min_ms = 0.0;
    ctx->frame_block_max_ms = 0.0;
    ctx->frame_block_avg_ms = 0.0;
    ctx->frame_block_min_kb = 0.0;
    ctx->frame_block_max_kb = 0.0;
    ctx->frame_block_avg_kb = 0.0;
    memset(ctx->frame_block_color_counts_ms, 0, sizeof(ctx->frame_block_color_counts_ms));
    memset(ctx->frame_block_color_counts_kb, 0, sizeof(ctx->frame_block_color_counts_kb));
    ctx->frame_block_missing = 0;
    ctx->frame_block_real_samples = 0;
    if (ctx->frame_block_values_lateness) g_array_set_size(ctx->frame_block_values_lateness, 0);
    if (ctx->frame_block_values_size) g_array_set_size(ctx->frame_block_values_size, 0);
    frame_block_sync_controls(ctx, NULL);
    frame_block_update_summary(ctx);
    if (ctx->frame_block_area) gtk_widget_queue_draw(GTK_WIDGET(ctx->frame_block_area));
    frame_block_queue_overlay_draws(ctx);
}

static void on_frame_block_threshold_changed(GtkSpinButton *spin, gpointer user_data) {
    (void)spin;
    GuiContext *ctx = user_data;
    if (!ctx) return;
    frame_block_apply_thresholds(ctx);
    frame_block_update_summary(ctx);
    if (ctx->frame_block_area) gtk_widget_queue_draw(GTK_WIDGET(ctx->frame_block_area));
}

static void on_frame_block_color_toggled(GtkCheckButton *check, gpointer user_data) {
    GuiContext *ctx = user_data;
    if (!ctx) return;
    gpointer pdata = g_object_get_data(G_OBJECT(check), "color-index");
    guint idx = pdata ? (guint)GPOINTER_TO_UINT(pdata) : 0;
    if (idx >= FRAME_BLOCK_COLOR_COUNT) return;
    ctx->frame_block_colors_visible[idx] = gtk_check_button_get_active(check);
    frame_block_update_summary(ctx);
    if (ctx->frame_block_area) gtk_widget_queue_draw(GTK_WIDGET(ctx->frame_block_area));
}

static gboolean gui_restart_with_config(GuiContext *ctx, const UvViewerConfig *cfg) {
    if (!ctx || !ctx->viewer || !cfg) return FALSE;

    if (cfg->listen_port == ctx->current_cfg.listen_port &&
        cfg->sync_to_clock == ctx->current_cfg.sync_to_clock &&
        cfg->jitter_latency_ms == ctx->current_cfg.jitter_latency_ms &&
        cfg->queue_max_buffers == ctx->current_cfg.queue_max_buffers &&
        cfg->videorate_enabled == ctx->current_cfg.videorate_enabled &&
        cfg->videorate_fps_numerator == ctx->current_cfg.videorate_fps_numerator &&
        cfg->videorate_fps_denominator == ctx->current_cfg.videorate_fps_denominator &&
        cfg->decoder_preference == ctx->current_cfg.decoder_preference &&
        cfg->video_sink_preference == ctx->current_cfg.video_sink_preference &&
        cfg->audio_enabled == ctx->current_cfg.audio_enabled &&
        cfg->audio_payload_type == ctx->current_cfg.audio_payload_type &&
        cfg->audio_clock_rate == ctx->current_cfg.audio_clock_rate &&
        cfg->audio_jitter_latency_ms == ctx->current_cfg.audio_jitter_latency_ms &&
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
    if (ctx->viewer_slot) {
        *ctx->viewer_slot = new_viewer;
    }
    ctx->current_cfg = *cfg;
    if (ctx->cfg_slot) {
        *ctx->cfg_slot = *cfg;
    }
    ctx->audio_runtime_enabled = cfg->audio_enabled;
    ctx->audio_active = FALSE;
    if (ctx->stats_history) {
        g_array_set_size(ctx->stats_history, 0);
    }

    frame_block_apply_thresholds(ctx);
    if (ctx->frame_block_active) {
        uv_viewer_frame_block_configure(ctx->viewer, TRUE, ctx->frame_block_snapshot_mode);
        if (ctx->frame_block_paused) {
            uv_viewer_frame_block_pause(ctx->viewer, TRUE);
        }
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
    new_cfg.videorate_enabled = check_get(ctx->videorate_toggle);
    if (ctx->videorate_num_spin) {
        int fps_num = gtk_spin_button_get_value_as_int(ctx->videorate_num_spin);
        if (fps_num < 0) fps_num = 0;
        new_cfg.videorate_fps_numerator = (guint)fps_num;
    }
    if (ctx->videorate_den_spin) {
        int fps_den = gtk_spin_button_get_value_as_int(ctx->videorate_den_spin);
        if (fps_den <= 0) fps_den = 1;
        new_cfg.videorate_fps_denominator = (guint)fps_den;
    }
    new_cfg.audio_enabled = check_get(ctx->audio_toggle);
    if (ctx->audio_payload_spin) {
        int payload = gtk_spin_button_get_value_as_int(ctx->audio_payload_spin);
        if (payload < 0) payload = 0;
        if (payload > 127) payload = 127;
        new_cfg.audio_payload_type = (guint)payload;
    }
    if (ctx->audio_jitter_spin) {
        int aj = gtk_spin_button_get_value_as_int(ctx->audio_jitter_spin);
        if (aj < 0) aj = 0;
        new_cfg.audio_jitter_latency_ms = (guint)aj;
    }
    new_cfg.sync_to_clock = check_get(ctx->sync_toggle_settings);
    new_cfg.jitter_drop_on_latency = check_get(ctx->jitter_drop_toggle);
    new_cfg.jitter_do_lost = check_get(ctx->jitter_do_lost_toggle);
    new_cfg.jitter_post_drop_messages = check_get(ctx->jitter_post_drop_toggle);

    guint new_refresh_interval = ctx->stats_refresh_interval_ms;
    if (ctx->stats_refresh_spin) {
        int refresh = gtk_spin_button_get_value_as_int(ctx->stats_refresh_spin);
        if (refresh < 50) refresh = 50;
        new_refresh_interval = (guint)refresh;
    }
    set_stats_refresh_interval(ctx, new_refresh_interval);

    if (ctx->decoder_dropdown) {
        guint decoder_idx = gtk_drop_down_get_selected(ctx->decoder_dropdown);
        if (decoder_idx == GTK_INVALID_LIST_POSITION) {
            decoder_idx = decoder_pref_to_index(ctx->current_cfg.decoder_preference);
        }
        new_cfg.decoder_preference = decoder_index_to_pref(decoder_idx);
    }
    if (ctx->sink_dropdown) {
        guint sink_idx = gtk_drop_down_get_selected(ctx->sink_dropdown);
        if (sink_idx == GTK_INVALID_LIST_POSITION) {
            sink_idx = video_sink_pref_to_index(ctx->current_cfg.video_sink_preference);
        }
        new_cfg.video_sink_preference = video_sink_index_to_pref(sink_idx);
    }

    if (!gui_restart_with_config(ctx, &new_cfg)) {
        sync_settings_controls(ctx);
    }
}

static void on_videorate_toggled(GtkCheckButton *button, gpointer user_data) {
    GuiContext *ctx = user_data;
    if (!ctx) return;
    gboolean active = button ? gtk_check_button_get_active(button) : FALSE;
    if (ctx->videorate_num_spin) {
        gtk_widget_set_sensitive(GTK_WIDGET(ctx->videorate_num_spin), active);
    }
    if (ctx->videorate_den_spin) {
        gtk_widget_set_sensitive(GTK_WIDGET(ctx->videorate_den_spin), active);
    }
}

static void on_audio_toggled(GtkCheckButton *button, gpointer user_data) {
    GuiContext *ctx = user_data;
    if (!ctx) return;
    gboolean active = button ? gtk_check_button_get_active(button) : FALSE;
    if (ctx->audio_payload_spin) {
        gtk_widget_set_sensitive(GTK_WIDGET(ctx->audio_payload_spin), active);
    }
    if (ctx->audio_jitter_spin) {
        gtk_widget_set_sensitive(GTK_WIDGET(ctx->audio_jitter_spin), active);
    }
}

static void on_source_dropdown_changed(GObject *dropdown, GParamSpec *pspec, gpointer user_data) {
    (void)pspec;
    GuiContext *ctx = user_data;
    if (!ctx || ctx->suppress_source_change || !ctx->viewer) return;
    if (!GTK_IS_DROP_DOWN(dropdown)) return;

    guint selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(dropdown));
    if (selected == GTK_INVALID_LIST_POSITION) return;

    GError *error = NULL;
    if (!uv_viewer_select_source(ctx->viewer, (int)selected, &error)) {
        update_status(ctx, error ? error->message : "Failed to select source");
    } else {
        char msg[128];
        g_snprintf(msg, sizeof(msg), "Selected source %u", selected);
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

    GtkWidget *sources_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_top(sources_box, 6);
    gtk_widget_set_margin_bottom(sources_box, 6);
    gtk_widget_set_margin_start(sources_box, 6);
    gtk_widget_set_margin_end(sources_box, 6);
    gtk_frame_set_child(GTK_FRAME(ctx->sources_frame), sources_box);

    GtkWidget *dropdown_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append(GTK_BOX(sources_box), dropdown_row);

    GtkWidget *dropdown_label = gtk_label_new("Active Stream:");
    gtk_label_set_xalign(GTK_LABEL(dropdown_label), 0.0);
    gtk_box_append(GTK_BOX(dropdown_row), dropdown_label);

    ctx->source_model = gtk_string_list_new(NULL);
    GtkWidget *dropdown = gtk_drop_down_new(G_LIST_MODEL(ctx->source_model), NULL);
    gtk_widget_set_hexpand(dropdown, TRUE);
    ctx->source_dropdown = GTK_DROP_DOWN(dropdown);
    g_signal_connect(dropdown, "notify::selected", G_CALLBACK(on_source_dropdown_changed), ctx);
    gtk_box_append(GTK_BOX(dropdown_row), dropdown);

    ctx->source_detail_label = GTK_LABEL(gtk_label_new("No sources discovered yet."));
    gtk_label_set_xalign(ctx->source_detail_label, 0.0);
    gtk_label_set_wrap(ctx->source_detail_label, TRUE);
    gtk_box_append(GTK_BOX(sources_box), GTK_WIDGET(ctx->source_detail_label));

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

    GtkWidget *stats_refresh_label = gtk_label_new("Stats Refresh (ms):");
    gtk_label_set_xalign(GTK_LABEL(stats_refresh_label), 0.0);
    gtk_grid_attach(GTK_GRID(grid), stats_refresh_label, 0, 4, 1, 1);

    ctx->stats_refresh_spin = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(50, 5000, 10));
    gtk_spin_button_set_increments(ctx->stats_refresh_spin, 10, 100);
    gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(ctx->stats_refresh_spin), 1, 4, 1, 1);

    GtkWidget *decoder_label = gtk_label_new("Decoder:");
    gtk_label_set_xalign(GTK_LABEL(decoder_label), 0.0);
    gtk_grid_attach(GTK_GRID(grid), decoder_label, 0, 5, 1, 1);

    ctx->decoder_dropdown = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(decoder_option_labels));
    gtk_drop_down_set_selected(ctx->decoder_dropdown,
                               decoder_pref_to_index(ctx->current_cfg.decoder_preference));
    gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(ctx->decoder_dropdown), 1, 5, 1, 1);

    GtkWidget *sink_label = gtk_label_new("Video Sink:");
    gtk_label_set_xalign(GTK_LABEL(sink_label), 0.0);
    gtk_grid_attach(GTK_GRID(grid), sink_label, 0, 6, 1, 1);

    ctx->sink_dropdown = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(video_sink_option_labels));
    gtk_drop_down_set_selected(ctx->sink_dropdown,
                               video_sink_pref_to_index(ctx->current_cfg.video_sink_preference));
    gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(ctx->sink_dropdown), 1, 6, 1, 1);

    GtkWidget *videorate_label = gtk_label_new("Videorate:");
    gtk_label_set_xalign(GTK_LABEL(videorate_label), 0.0);
    gtk_grid_attach(GTK_GRID(grid), videorate_label, 0, 7, 1, 1);

    GtkWidget *videorate_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_grid_attach(GTK_GRID(grid), videorate_box, 1, 7, 1, 1);

    ctx->videorate_toggle = GTK_CHECK_BUTTON(gtk_check_button_new_with_label("Enable"));
    gtk_box_append(GTK_BOX(videorate_box), GTK_WIDGET(ctx->videorate_toggle));
    g_signal_connect(ctx->videorate_toggle, "toggled", G_CALLBACK(on_videorate_toggled), ctx);

    GtkWidget *target_label = gtk_label_new("Target FPS:");
    gtk_label_set_xalign(GTK_LABEL(target_label), 0.0);
    gtk_box_append(GTK_BOX(videorate_box), target_label);

    ctx->videorate_num_spin = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(1, 480, 1));
    gtk_box_append(GTK_BOX(videorate_box), GTK_WIDGET(ctx->videorate_num_spin));

    GtkWidget *slash_label = gtk_label_new("/");
    gtk_box_append(GTK_BOX(videorate_box), slash_label);

    ctx->videorate_den_spin = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(1, 1000, 1));
    gtk_box_append(GTK_BOX(videorate_box), GTK_WIDGET(ctx->videorate_den_spin));

    GtkWidget *audio_label = gtk_label_new("Audio:");
    gtk_label_set_xalign(GTK_LABEL(audio_label), 0.0);
    gtk_grid_attach(GTK_GRID(grid), audio_label, 0, 8, 1, 1);

    GtkWidget *audio_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_grid_attach(GTK_GRID(grid), audio_box, 1, 8, 1, 1);

    ctx->audio_toggle = GTK_CHECK_BUTTON(gtk_check_button_new_with_label("Enable"));
    gtk_box_append(GTK_BOX(audio_box), GTK_WIDGET(ctx->audio_toggle));
    g_signal_connect(ctx->audio_toggle, "toggled", G_CALLBACK(on_audio_toggled), ctx);

    GtkWidget *payload_label = gtk_label_new("PT:");
    gtk_label_set_xalign(GTK_LABEL(payload_label), 0.0);
    gtk_box_append(GTK_BOX(audio_box), payload_label);

    ctx->audio_payload_spin = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(0, 127, 1));
    gtk_box_append(GTK_BOX(audio_box), GTK_WIDGET(ctx->audio_payload_spin));

    GtkWidget *audio_jitter_label = gtk_label_new("Jitter (ms):");
    gtk_label_set_xalign(GTK_LABEL(audio_jitter_label), 0.0);
    gtk_box_append(GTK_BOX(audio_box), audio_jitter_label);

    ctx->audio_jitter_spin = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(0, 500, 1));
    gtk_box_append(GTK_BOX(audio_box), GTK_WIDGET(ctx->audio_jitter_spin));

    ctx->jitter_drop_toggle = GTK_CHECK_BUTTON(gtk_check_button_new_with_label("Drop packets exceeding latency"));
    gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(ctx->jitter_drop_toggle), 0, 9, 2, 1);

    ctx->jitter_do_lost_toggle = GTK_CHECK_BUTTON(gtk_check_button_new_with_label("Emit lost packet notifications"));
    gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(ctx->jitter_do_lost_toggle), 0, 10, 2, 1);

    ctx->jitter_post_drop_toggle = GTK_CHECK_BUTTON(gtk_check_button_new_with_label("Post drop messages on bus"));
    gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(ctx->jitter_post_drop_toggle), 0, 11, 2, 1);

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
        "Inbound Rate (Mbps)",
        "RTP Lost Packets",
        "RTP Duplicate Packets",
        "RTP Reordered Packets",
        "RTP Jitter (ms)",
        "Decoder FPS (current)"
    };

    for (int i = 0; i < STATS_METRIC_COUNT; i++) {
        GtkWidget *frame = gtk_frame_new(NULL);
        gtk_widget_set_hexpand(frame, TRUE);
        gtk_widget_set_vexpand(frame, TRUE);
        int row = i / 2;
        int col = i % 2;
        gtk_grid_attach(GTK_GRID(grid), frame, col, row, 1, 1);

        GtkWidget *label_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_widget_set_hexpand(label_box, TRUE);

        GtkWidget *title_label = gtk_label_new(titles[i]);
        gtk_label_set_xalign(GTK_LABEL(title_label), 0.0);
        gtk_widget_set_hexpand(title_label, TRUE);
        gtk_box_append(GTK_BOX(label_box), title_label);

        GtkWidget *live_widget = gtk_label_new("Live: --");
        gtk_label_set_xalign(GTK_LABEL(live_widget), 1.0);
        gtk_widget_set_valign(live_widget, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(label_box), live_widget);

        GtkWidget *max_widget = gtk_label_new("Max: --");
        gtk_label_set_xalign(GTK_LABEL(max_widget), 1.0);
        gtk_widget_set_valign(max_widget, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(label_box), max_widget);

        gtk_frame_set_label_widget(GTK_FRAME(frame), label_box);

        GtkDrawingArea *area = GTK_DRAWING_AREA(gtk_drawing_area_new());
        gtk_widget_set_hexpand(GTK_WIDGET(area), TRUE);
        gtk_widget_set_vexpand(GTK_WIDGET(area), TRUE);
        gtk_drawing_area_set_draw_func(area, stats_chart_draw, ctx, NULL);
        g_object_set_data(G_OBJECT(area), "stats-metric", GINT_TO_POINTER(i));
        g_object_set_data(G_OBJECT(area), "stats-live-label", live_widget);
        g_object_set_data(G_OBJECT(area), "stats-max-label", max_widget);
        gtk_frame_set_child(GTK_FRAME(frame), GTK_WIDGET(area));
        ctx->stats_charts[i] = area;
        ctx->stats_live_labels[i] = GTK_LABEL(live_widget);
        ctx->stats_max_labels[i] = GTK_LABEL(max_widget);
    }

    return page;
}

static GtkWidget *build_frame_block_page(GuiContext *ctx) {
    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_top(content, 12);
    gtk_widget_set_margin_start(content, 12);
    gtk_widget_set_margin_end(content, 12);
    gtk_widget_set_margin_bottom(content, 12);

    for (int i = 0; i < 2; i++) {
        ctx->frame_overlay_live_labels[i] = NULL;
        ctx->frame_overlay_max_labels[i] = NULL;
    }

    GtkWidget *controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(content), controls);

    ctx->frame_block_enable_toggle = GTK_TOGGLE_BUTTON(gtk_toggle_button_new_with_label("Enable Capture"));
    g_signal_connect(ctx->frame_block_enable_toggle, "toggled", G_CALLBACK(on_frame_block_enable_toggled), ctx);
    gtk_box_append(GTK_BOX(controls), GTK_WIDGET(ctx->frame_block_enable_toggle));

    const char *mode_labels[] = {"Continuous", "Snapshot", NULL};
    ctx->frame_block_mode_dropdown = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(mode_labels));
    gtk_drop_down_set_selected(ctx->frame_block_mode_dropdown, ctx->frame_block_snapshot_mode ? 1 : 0);
    g_signal_connect(ctx->frame_block_mode_dropdown, "notify::selected", G_CALLBACK(on_frame_block_mode_changed), ctx);
    gtk_box_append(GTK_BOX(controls), GTK_WIDGET(ctx->frame_block_mode_dropdown));

    const char *width_labels[] = {"30", "60", "90", "120", NULL};
    GtkWidget *width_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *width_label = gtk_label_new("Row width");
    gtk_widget_set_valign(width_label, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(width_box), width_label);
    ctx->frame_block_width_dropdown = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(width_labels));
    guint width_index = frame_block_width_index_for_value(ctx->frame_block_width ? ctx->frame_block_width : FRAME_BLOCK_DEFAULT_WIDTH);
    gtk_drop_down_set_selected(ctx->frame_block_width_dropdown, width_index);
    g_signal_connect(ctx->frame_block_width_dropdown, "notify::selected", G_CALLBACK(on_frame_block_width_changed), ctx);
    gtk_box_append(GTK_BOX(width_box), GTK_WIDGET(ctx->frame_block_width_dropdown));
    gtk_box_append(GTK_BOX(controls), width_box);

    ctx->frame_block_metric_toggle = GTK_TOGGLE_BUTTON(gtk_toggle_button_new_with_label(""));
    gtk_toggle_button_set_active(ctx->frame_block_metric_toggle, ctx->frame_block_view == FRAME_BLOCK_VIEW_SIZE);
    g_signal_connect(ctx->frame_block_metric_toggle, "toggled", G_CALLBACK(on_frame_block_metric_toggled), ctx);
    frame_block_update_metric_toggle_label(ctx);
    gtk_box_append(GTK_BOX(controls), GTK_WIDGET(ctx->frame_block_metric_toggle));

    ctx->frame_block_pause_toggle = GTK_TOGGLE_BUTTON(gtk_toggle_button_new_with_label("Pause"));
    gtk_widget_set_sensitive(GTK_WIDGET(ctx->frame_block_pause_toggle), FALSE);
    g_signal_connect(ctx->frame_block_pause_toggle, "toggled", G_CALLBACK(on_frame_block_pause_toggled), ctx);
    gtk_box_append(GTK_BOX(controls), GTK_WIDGET(ctx->frame_block_pause_toggle));

    ctx->frame_block_reset_button = GTK_BUTTON(gtk_button_new_with_label("Reset"));
    gtk_widget_set_sensitive(GTK_WIDGET(ctx->frame_block_reset_button), FALSE);
    g_signal_connect(ctx->frame_block_reset_button, "clicked", G_CALLBACK(on_frame_block_reset_clicked), ctx);
    gtk_box_append(GTK_BOX(controls), GTK_WIDGET(ctx->frame_block_reset_button));

    GtkWidget *threshold_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(threshold_grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(threshold_grid), 8);

    const char *labels[] = {
        "Green threshold (ms)",
        "Yellow threshold (ms)",
        "Orange threshold (ms)"
    };
    double defaults[] = {
        ctx->frame_block_thresholds_ms[0],
        ctx->frame_block_thresholds_ms[1],
        ctx->frame_block_thresholds_ms[2]
    };

    for (guint i = 0; i < 3; i++) {
        GtkWidget *lbl = gtk_label_new(labels[i]);
        gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
        gtk_grid_attach(GTK_GRID(threshold_grid), lbl, 0, (int)i, 1, 1);
        ctx->frame_block_threshold_labels[i] = GTK_LABEL(lbl);

        GtkSpinButton *spin = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(0.0, 10000.0, 0.5));
        gtk_spin_button_set_digits(spin, 1);
        gtk_spin_button_set_increments(spin, 0.5, 2.5);
        gtk_spin_button_set_value(spin, defaults[i]);
        g_signal_connect(spin, "value-changed", G_CALLBACK(on_frame_block_threshold_changed), ctx);
        gtk_grid_attach(GTK_GRID(threshold_grid), GTK_WIDGET(spin), 1, (int)i, 1, 1);
        ctx->frame_block_threshold_spins[i] = spin;
    }
    gtk_box_append(GTK_BOX(content), threshold_grid);

    GtkWidget *color_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    const char *color_labels[] = {"Green", "Yellow", "Orange", "Red"};
    for (guint i = 0; i < FRAME_BLOCK_COLOR_COUNT; i++) {
        GtkCheckButton *check = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(color_labels[i]));
        gtk_check_button_set_active(check, ctx->frame_block_colors_visible[i]);
        g_object_set_data(G_OBJECT(check), "color-index", GUINT_TO_POINTER(i));
        g_signal_connect(check, "toggled", G_CALLBACK(on_frame_block_color_toggled), ctx);
        ctx->frame_block_color_toggles[i] = check;
        gtk_box_append(GTK_BOX(color_box), GTK_WIDGET(check));
    }
    GtkWidget *color_help = gtk_label_new("?");
    gtk_widget_add_css_class(color_help, "dim-label");
    gtk_widget_set_tooltip_text(color_help,
        "Lateness view colors frames by how much later they arrive than the RTP pacing "
        "(wall-clock delta minus expected timestamp interval, clamped at zero). Size view "
        "uses the same color buckets but maps to total frame size in kilobytes, letting you "
        "compare timing spikes with bandwidth spikes inside the same capture window.");
    gtk_box_append(GTK_BOX(color_box), color_help);
    gtk_box_append(GTK_BOX(content), color_box);

    GtkWidget *frame = gtk_frame_new(NULL);
    gtk_widget_set_hexpand(frame, TRUE);
    gtk_widget_set_vexpand(frame, TRUE);

    GtkWidget *drawing = gtk_drawing_area_new();
    ctx->frame_block_area = GTK_DRAWING_AREA(drawing);
    gtk_widget_set_size_request(drawing, 480, 480);
    gtk_widget_set_hexpand(drawing, TRUE);
    gtk_widget_set_vexpand(drawing, TRUE);
    gtk_drawing_area_set_draw_func(ctx->frame_block_area, frame_block_draw, ctx, NULL);
    gtk_frame_set_child(GTK_FRAME(frame), drawing);
    gtk_box_append(GTK_BOX(content), frame);

    GtkWidget *lateness_frame = gtk_frame_new(NULL);
    gtk_widget_set_hexpand(lateness_frame, TRUE);
    gtk_widget_set_vexpand(lateness_frame, FALSE);

    GtkWidget *lateness_label_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *lateness_title = gtk_label_new("Frame Lateness Timeline");
    gtk_label_set_xalign(GTK_LABEL(lateness_title), 0.0);
    gtk_widget_set_hexpand(lateness_title, TRUE);
    gtk_box_append(GTK_BOX(lateness_label_box), lateness_title);

    GtkWidget *lateness_live = gtk_label_new("Live: --");
    gtk_label_set_xalign(GTK_LABEL(lateness_live), 1.0);
    gtk_widget_set_valign(lateness_live, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(lateness_label_box), lateness_live);

    GtkWidget *lateness_max = gtk_label_new("Max: --");
    gtk_label_set_xalign(GTK_LABEL(lateness_max), 1.0);
    gtk_widget_set_valign(lateness_max, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(lateness_label_box), lateness_max);

    gtk_frame_set_label_widget(GTK_FRAME(lateness_frame), lateness_label_box);

    GtkWidget *lateness_area = gtk_drawing_area_new();
    ctx->frame_overlay_lateness = GTK_DRAWING_AREA(lateness_area);
    gtk_widget_set_size_request(lateness_area, 480, 140);
    gtk_widget_set_hexpand(lateness_area, TRUE);
    gtk_widget_set_vexpand(lateness_area, FALSE);
    g_object_set_data(G_OBJECT(lateness_area), "overlay-metric", GUINT_TO_POINTER(FRAME_OVERLAY_METRIC_LATENESS));
    gtk_drawing_area_set_draw_func(ctx->frame_overlay_lateness, frame_overlay_draw, ctx, NULL);
    g_object_set_data(G_OBJECT(lateness_area), "overlay-live-label", lateness_live);
    g_object_set_data(G_OBJECT(lateness_area), "overlay-max-label", lateness_max);
    ctx->frame_overlay_live_labels[FRAME_OVERLAY_METRIC_LATENESS] = GTK_LABEL(lateness_live);
    ctx->frame_overlay_max_labels[FRAME_OVERLAY_METRIC_LATENESS] = GTK_LABEL(lateness_max);
    gtk_frame_set_child(GTK_FRAME(lateness_frame), lateness_area);
    gtk_box_append(GTK_BOX(content), lateness_frame);

    GtkWidget *size_frame = gtk_frame_new(NULL);
    gtk_widget_set_hexpand(size_frame, TRUE);
    gtk_widget_set_vexpand(size_frame, FALSE);

    GtkWidget *size_label_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *size_title = gtk_label_new("Frame Size Timeline");
    gtk_label_set_xalign(GTK_LABEL(size_title), 0.0);
    gtk_widget_set_hexpand(size_title, TRUE);
    gtk_box_append(GTK_BOX(size_label_box), size_title);

    GtkWidget *size_live = gtk_label_new("Live: --");
    gtk_label_set_xalign(GTK_LABEL(size_live), 1.0);
    gtk_widget_set_valign(size_live, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(size_label_box), size_live);

    GtkWidget *size_max = gtk_label_new("Max: --");
    gtk_label_set_xalign(GTK_LABEL(size_max), 1.0);
    gtk_widget_set_valign(size_max, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(size_label_box), size_max);

    gtk_frame_set_label_widget(GTK_FRAME(size_frame), size_label_box);

    GtkWidget *size_area = gtk_drawing_area_new();
    ctx->frame_overlay_size = GTK_DRAWING_AREA(size_area);
    gtk_widget_set_size_request(size_area, 480, 140);
    gtk_widget_set_hexpand(size_area, TRUE);
    gtk_widget_set_vexpand(size_area, FALSE);
    g_object_set_data(G_OBJECT(size_area), "overlay-metric", GUINT_TO_POINTER(FRAME_OVERLAY_METRIC_SIZE));
    gtk_drawing_area_set_draw_func(ctx->frame_overlay_size, frame_overlay_draw, ctx, NULL);
    g_object_set_data(G_OBJECT(size_area), "overlay-live-label", size_live);
    g_object_set_data(G_OBJECT(size_area), "overlay-max-label", size_max);
    ctx->frame_overlay_live_labels[FRAME_OVERLAY_METRIC_SIZE] = GTK_LABEL(size_live);
    ctx->frame_overlay_max_labels[FRAME_OVERLAY_METRIC_SIZE] = GTK_LABEL(size_max);
    gtk_frame_set_child(GTK_FRAME(size_frame), size_area);
    gtk_box_append(GTK_BOX(content), size_frame);

    GtkWidget *summary_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_box_append(GTK_BOX(content), summary_row);

    ctx->frame_block_summary_label = GTK_LABEL(gtk_label_new("Frame block capture disabled."));
    gtk_label_set_xalign(ctx->frame_block_summary_label, 0.0);
    gtk_label_set_wrap(ctx->frame_block_summary_label, TRUE);
    gtk_widget_set_hexpand(GTK_WIDGET(ctx->frame_block_summary_label), TRUE);
    gtk_box_append(GTK_BOX(summary_row), GTK_WIDGET(ctx->frame_block_summary_label));

    frame_block_apply_thresholds(ctx);
    if (ctx->viewer) {
        uv_viewer_frame_block_set_size_thresholds(ctx->viewer,
                                                 ctx->frame_block_thresholds_kb[0],
                                                 ctx->frame_block_thresholds_kb[1],
                                                 ctx->frame_block_thresholds_kb[2]);
    }
    frame_block_sync_controls(ctx, NULL);
    frame_block_update_summary(ctx);

    GtkWidget *scroller = gtk_scrolled_window_new();
    gtk_widget_set_hexpand(scroller, TRUE);
    gtk_widget_set_vexpand(scroller, TRUE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), content);

    return scroller;
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

    GtkLabel *live_value_label = GTK_LABEL(g_object_get_data(G_OBJECT(area), "stats-live-label"));
    GtkLabel *max_value_label = GTK_LABEL(g_object_get_data(G_OBJECT(area), "stats-max-label"));
    const char *default_live = "Live: --";
    const char *default_max = "Max: --";

    cairo_save(cr);
    cairo_set_source_rgb(cr, 0.10, 0.10, 0.12);
    cairo_paint(cr);

    cairo_set_source_rgba(cr, 1, 1, 1, 0.1);
    cairo_rectangle(cr, 0.5, 0.5, width - 1.0, height - 1.0);
    cairo_stroke(cr);

    if (!ctx->stats_history || ctx->stats_history->len == 0) {
        if (live_value_label) gtk_label_set_text(live_value_label, default_live);
        if (max_value_label) gtk_label_set_text(max_value_label, default_max);
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
        if (live_value_label) gtk_label_set_text(live_value_label, default_live);
        if (max_value_label) gtk_label_set_text(max_value_label, default_max);
        cairo_set_source_rgb(cr, 0.7, 0.7, 0.7);
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 12.0);
        cairo_move_to(cr, 10, height / 2.0);
        cairo_show_text(cr, "No samples in range");
        cairo_restore(cr);
        return;
    }

    double axis_min = 0.0;
    double axis_max = max_val;
    if (!isfinite(axis_max) || axis_max < axis_min) {
        axis_max = axis_min;
    }
    if (axis_max == axis_min) {
        double delta = axis_max > 1.0 ? axis_max * 0.05 : 1.0;
        axis_max += delta;
    }

    const double left_margin = 64.0;
    const double right_margin = 12.0;
    const double top_margin = 12.0;
    const double bottom_margin = 24.0;
    double plot_width = MAX(1.0, width - (left_margin + right_margin));
    double plot_height = MAX(1.0, height - (top_margin + bottom_margin));
    double plot_left = left_margin;
    double plot_top = top_margin;
    double plot_bottom = plot_top + plot_height;
    double plot_right = plot_left + plot_width;

    const int tick_count = 4;
    cairo_set_source_rgba(cr, 1, 1, 1, 0.1);
    for (int i = 0; i <= tick_count; i++) {
        double y = plot_bottom - (plot_height / tick_count) * i;
        cairo_move_to(cr, plot_left, y);
        cairo_line_to(cr, plot_right, y);
    }
    cairo_stroke(cr);

    cairo_set_source_rgb(cr, 0.8, 0.8, 0.85);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 11.0);
    for (int i = 0; i <= tick_count; i++) {
        double fraction = (double)i / (double)tick_count;
        double value = axis_min + (axis_max - axis_min) * fraction;
        double y = plot_bottom - plot_height * fraction;

        char label[64];
        if (metric == STATS_METRIC_RATE) {
            double value_mbps = value / 1e6;
            g_snprintf(label, sizeof(label), "%.2f", value_mbps);
        } else {
            g_snprintf(label, sizeof(label), "%.2f", value);
        }

        cairo_text_extents_t label_extents;
        cairo_text_extents(cr, label, &label_extents);
        double text_x = plot_left - 8.0 - (label_extents.width + label_extents.x_bearing);
        double text_y = y + (label_extents.height / 2.0) - label_extents.y_bearing;
        cairo_move_to(cr, text_x, text_y);
        cairo_show_text(cr, label);
    }

    cairo_set_source_rgb(cr, 0.3, 0.7, 1.0);
    cairo_set_line_width(cr, 1.5);
    gboolean path_started = FALSE;
    for (guint i = start_index; i < len; i++) {
        const StatsSample *sample = &samples[i];
        double x_ratio = (sample->timestamp - start_time) / range;
        if (x_ratio < 0.0) x_ratio = 0.0;
        if (x_ratio > 1.0) x_ratio = 1.0;
        double x = plot_left + x_ratio * plot_width;
        double value = stats_metric_value(sample, metric);
        double y_ratio = (value - axis_min) / (axis_max - axis_min);
        if (y_ratio < 0.0) {
            y_ratio = 0.0;
        } else if (y_ratio > 1.0) {
            y_ratio = 1.0;
        }
        double y = plot_bottom - y_ratio * plot_height;
        if (!path_started) {
            cairo_move_to(cr, x, y);
            path_started = TRUE;
        } else {
            cairo_line_to(cr, x, y);
        }
    }
    cairo_stroke(cr);

    double latest_value = NAN;
    for (gint i = (gint)len - 1; i >= (gint)start_index; i--) {
        double candidate = stats_metric_value(&samples[i], metric);
        if (isfinite(candidate)) {
            latest_value = candidate;
            break;
        }
    }

    if (!isfinite(latest_value)) {
        if (live_value_label) {
            gtk_label_set_text(live_value_label, default_live);
        }
    } else if (metric == STATS_METRIC_RATE) {
        double latest_mbps = latest_value / 1e6;
        char latest_text[64];
        g_snprintf(latest_text, sizeof(latest_text), "Live: %.2f Mbps", latest_mbps);
        if (live_value_label) gtk_label_set_text(live_value_label, latest_text);
    } else {
        char latest_text[64];
        g_snprintf(latest_text, sizeof(latest_text), "Live: %.2f", latest_value);
        if (live_value_label) gtk_label_set_text(live_value_label, latest_text);
    }

    if (isfinite(max_val)) {
        char max_text[64];
        if (metric == STATS_METRIC_RATE) {
            double max_mbps = max_val / 1e6;
            g_snprintf(max_text, sizeof(max_text), "Max: %.2f Mbps", max_mbps);
        } else {
            g_snprintf(max_text, sizeof(max_text), "Max: %.2f", max_val);
        }
        if (max_value_label) gtk_label_set_text(max_value_label, max_text);
    } else {
        if (max_value_label) gtk_label_set_text(max_value_label, default_max);
    }

    cairo_restore(cr);
}

static void build_ui(GuiContext *ctx) {
    GtkWidget *window = gtk_application_window_new(ctx->app);
    ctx->window = GTK_WINDOW(window);
    gtk_window_set_title(ctx->window, "UDP H.265 Viewer");
    gtk_window_set_default_size(ctx->window, 900, 680);
    gtk_window_set_resizable(ctx->window, TRUE);
    g_signal_connect(window, "close-request", G_CALLBACK(on_window_close_request), ctx);

    ctx->notebook = GTK_NOTEBOOK(gtk_notebook_new());
    gtk_window_set_child(ctx->window, GTK_WIDGET(ctx->notebook));

    GtkWidget *monitor_page = build_monitor_page(ctx);
    gtk_notebook_append_page(ctx->notebook, monitor_page, gtk_label_new("Monitor"));

    GtkWidget *settings_page = build_settings_page(ctx);
    gtk_notebook_append_page(ctx->notebook, settings_page, gtk_label_new("Settings"));

    GtkWidget *stats_page = build_stats_page(ctx);
    gtk_notebook_append_page(ctx->notebook, stats_page, gtk_label_new("Stats"));

    GtkWidget *frame_page = build_frame_block_page(ctx);
    gtk_notebook_append_page(ctx->notebook, frame_page, gtk_label_new("Frame Blocks"));

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
    } else if (page_num == 3) {
        if (ctx->frame_block_area) {
            gtk_widget_queue_draw(GTK_WIDGET(ctx->frame_block_area));
        }
        if (ctx->frame_overlay_lateness) {
            gtk_widget_queue_draw(GTK_WIDGET(ctx->frame_overlay_lateness));
        }
        if (ctx->frame_overlay_size) {
            gtk_widget_queue_draw(GTK_WIDGET(ctx->frame_overlay_size));
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
    restart_stats_timer(ctx);
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
    if (ctx->source_model) {
        g_clear_object(&ctx->source_model);
    }
    ctx->source_dropdown = NULL;
    ctx->source_detail_label = NULL;
    ctx->video_picture = NULL;
    ctx->sources_frame = NULL;
    ctx->sources_toggle = NULL;
    ctx->listen_port_spin = NULL;
    ctx->jitter_latency_spin = NULL;
    ctx->sync_toggle_settings = NULL;
    ctx->queue_max_buffers_spin = NULL;
    ctx->stats_refresh_spin = NULL;
    ctx->decoder_dropdown = NULL;
    ctx->sink_dropdown = NULL;
    ctx->videorate_toggle = NULL;
    ctx->videorate_num_spin = NULL;
    ctx->videorate_den_spin = NULL;
    ctx->audio_toggle = NULL;
    ctx->audio_payload_spin = NULL;
    ctx->audio_jitter_spin = NULL;
    ctx->jitter_drop_toggle = NULL;
    ctx->jitter_do_lost_toggle = NULL;
    ctx->jitter_post_drop_toggle = NULL;
    ctx->stats_range_dropdown = NULL;
    for (int i = 0; i < STATS_METRIC_COUNT; i++) {
        ctx->stats_charts[i] = NULL;
        ctx->stats_live_labels[i] = NULL;
        ctx->stats_max_labels[i] = NULL;
    }
    if (ctx->frame_block_values_lateness) {
        g_array_free(ctx->frame_block_values_lateness, TRUE);
        ctx->frame_block_values_lateness = NULL;
    }
    if (ctx->frame_block_values_size) {
        g_array_free(ctx->frame_block_values_size, TRUE);
        ctx->frame_block_values_size = NULL;
    }
    ctx->frame_block_area = NULL;
    ctx->frame_overlay_lateness = NULL;
    ctx->frame_overlay_size = NULL;
    for (int i = 0; i < 2; i++) {
        ctx->frame_overlay_live_labels[i] = NULL;
        ctx->frame_overlay_max_labels[i] = NULL;
    }
    ctx->frame_block_enable_toggle = NULL;
    ctx->frame_block_pause_toggle = NULL;
    ctx->frame_block_mode_dropdown = NULL;
    ctx->frame_block_width_dropdown = NULL;
    ctx->frame_block_metric_toggle = NULL;
    ctx->frame_block_summary_label = NULL;
    ctx->frame_block_reset_button = NULL;
    for (int i = 0; i < 3; i++) {
        ctx->frame_block_threshold_spins[i] = NULL;
        ctx->frame_block_threshold_labels[i] = NULL;
    }
    for (guint i = 0; i < FRAME_BLOCK_COLOR_COUNT; i++) ctx->frame_block_color_toggles[i] = NULL;
    ctx->notebook = NULL;
    ctx->paintable_bound = FALSE;
    ctx->window = NULL;
}

int uv_gui_run(UvViewer **viewer, UvViewerConfig *cfg, const char *program_name) {
    if (!viewer || !*viewer || !cfg) return 1;

    GtkApplication *app = gtk_application_new("com.radeonvrx.viewer", G_APPLICATION_NON_UNIQUE);

    GuiContext *ctx = g_new0(GuiContext, 1);
    ctx->viewer_slot = viewer;
    ctx->viewer = *viewer;
    ctx->cfg_slot = cfg;
    ctx->current_cfg = *cfg;
    ctx->stats_range_seconds = 300.0;
    ctx->stats_last_refresh_time = 0.0;
    ctx->audio_runtime_enabled = cfg->audio_enabled;
    ctx->audio_active = FALSE;
    ctx->frame_block_thresholds_ms[0] = FRAME_BLOCK_DEFAULT_GREEN_MS;
    ctx->frame_block_thresholds_ms[1] = FRAME_BLOCK_DEFAULT_YELLOW_MS;
    ctx->frame_block_thresholds_ms[2] = FRAME_BLOCK_DEFAULT_ORANGE_MS;
    ctx->frame_block_thresholds_kb[0] = FRAME_BLOCK_DEFAULT_SIZE_GREEN_KB;
    ctx->frame_block_thresholds_kb[1] = FRAME_BLOCK_DEFAULT_SIZE_YELLOW_KB;
    ctx->frame_block_thresholds_kb[2] = FRAME_BLOCK_DEFAULT_SIZE_ORANGE_KB;
    ctx->frame_block_width = FRAME_BLOCK_DEFAULT_WIDTH;
    ctx->frame_block_height = FRAME_BLOCK_DEFAULT_HEIGHT;
    ctx->frame_block_values_lateness = g_array_new(FALSE, TRUE, sizeof(double));
    ctx->frame_block_values_size = g_array_new(FALSE, TRUE, sizeof(double));
    ctx->frame_block_view = FRAME_BLOCK_VIEW_LATENESS;
    ctx->frame_block_missing = 0;
    ctx->frame_block_real_samples = 0;
    ctx->known_source_count = 0;
    ctx->suppress_source_change = FALSE;
    ctx->stats_refresh_interval_ms = 200;
    for (guint i = 0; i < FRAME_BLOCK_COLOR_COUNT; i++) {
        ctx->frame_block_colors_visible[i] = TRUE;
    }

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

    if (viewer) {
        *viewer = ctx->viewer;
    }

    g_object_unref(app);
    g_free(ctx);

    return status;
}
