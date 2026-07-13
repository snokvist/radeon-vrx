#include "gui_shell.h"
#include "uv_internal.h"

#include <gtk/gtk.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define FRAME_BLOCK_DEFAULT_WIDTH    60u
#define FRAME_BLOCK_DEFAULT_HEIGHT  100u
#define FRAME_BLOCK_COLOR_COUNT     4u
#define FRAME_BLOCK_DEFAULT_GREEN_MS   1.0
#define FRAME_BLOCK_DEFAULT_YELLOW_MS 2.0
#define FRAME_BLOCK_DEFAULT_ORANGE_MS 3.0
#define FRAME_BLOCK_DEFAULT_SIZE_GREEN_KB   8.0
#define FRAME_BLOCK_DEFAULT_SIZE_YELLOW_KB  16.0
#define FRAME_BLOCK_DEFAULT_SIZE_ORANGE_KB  32.0
#define FRAME_BLOCK_DEFAULT_SPAN_GREEN_MS   8.0
#define FRAME_BLOCK_DEFAULT_SPAN_YELLOW_MS  16.0
#define FRAME_BLOCK_DEFAULT_SPAN_ORANGE_MS  25.0
#define FRAME_BLOCK_DEFAULT_CHUNK_GREEN     1.0
#define FRAME_BLOCK_DEFAULT_CHUNK_YELLOW    2.0
#define FRAME_BLOCK_DEFAULT_CHUNK_ORANGE    3.0
#define FRAME_BLOCK_DEFAULT_FPC_GREEN       1.0
#define FRAME_BLOCK_DEFAULT_FPC_YELLOW      2.0
#define FRAME_BLOCK_DEFAULT_FPC_ORANGE      3.0
#define FRAME_BLOCK_MISSING_SENTINEL (-1.0)
#define SHM_RECOVERY_PORT 8092u

typedef enum {
    STATS_METRIC_RATE = 0,
    STATS_METRIC_LOST,
    STATS_METRIC_DUP,
    STATS_METRIC_REORDER,
    STATS_METRIC_JITTER,
    STATS_METRIC_INPUT_FPS,
    STATS_METRIC_DECODER_FPS,
    STATS_METRIC_PPS,
    STATS_METRIC_PKT_SIZE,
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
    gboolean pending_source_valid;
    guint pending_source_index;
    gboolean active_source_valid;
    guint active_source_index;
    gboolean preferred_source_valid;
    UvSourceKind preferred_source_kind;
    char preferred_source_address[UV_VIEWER_ADDR_MAX];
    guint shm_recovery_timeout_id;
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
    GtkDropDown   *audio_port_mode_dropdown; /* 0 = shared with video, 1 = separate */
    GtkSpinButton *audio_port_spin;
    GtkLabel      *audio_status_label;
    GtkCheckButton *jitter_drop_toggle;
    GtkCheckButton *jitter_do_lost_toggle;
    GtkCheckButton *jitter_post_drop_toggle;
    /* Restream (verbatim UDP forward of the selected source) — Settings tab. */
    GtkCheckButton *restream_toggle;
    GtkEntry       *restream_host_entry;
    GtkSpinButton  *restream_port_spin;
    GtkLabel       *restream_status_label;
    gboolean        restream_toggle_suppress;
    GtkCheckButton *shm_toggle;
    GtkEntry       *shm_name_entry;
    GtkNotebook *notebook;
    GtkDropDown *stats_range_dropdown;
    GtkDrawingArea *stats_charts[STATS_METRIC_COUNT];
    GtkLabel *stats_live_labels[STATS_METRIC_COUNT];
    GtkLabel *stats_max_labels[STATS_METRIC_COUNT];
    double stats_range_seconds;
    GArray *stats_history;
    guint stats_timeout_id;
    GstElement *bound_sink;
    gulong sink_paintable_handler;
    gboolean paintable_bound;

    GtkDrawingArea *frame_block_area;
    GtkDrawingArea *frame_overlay_lateness;
    GtkDrawingArea *frame_overlay_size;
    GtkDrawingArea *frame_distribution_area;
    GtkLabel *frame_overlay_live_labels[2];
    GtkLabel *frame_overlay_max_labels[2];
    GtkLabel *frame_distribution_stats_label;
    GtkDropDown *frame_overlay_range_dropdown;
    GtkCheckButton *frame_overlay_values_toggle;
    GtkToggleButton *frame_block_enable_toggle;
    GtkToggleButton *frame_block_pause_toggle;
    GtkDropDown *frame_block_mode_dropdown;
    GtkDropDown *frame_block_width_dropdown;
    GtkDropDown *frame_block_metric_dropdown;
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
    double frame_block_thresholds_span[3];
    double frame_block_thresholds_chunks[3];
    double frame_block_min_span;
    double frame_block_max_span;
    double frame_block_avg_span;
    double frame_block_min_chunks;
    double frame_block_max_chunks;
    double frame_block_avg_chunks;
    guint frame_block_color_counts_span[4];
    guint frame_block_color_counts_chunks[4];
    double frame_block_thresholds_fpc[3];
    double frame_block_min_fpc;
    double frame_block_max_fpc;
    double frame_block_avg_fpc;
    guint frame_block_color_counts_fpc[4];
    GArray *frame_block_values_lateness; // doubles
    GArray *frame_block_values_size;     // doubles
    GArray *frame_block_values_span;     // doubles
    GArray *frame_block_values_chunks;   // doubles
    GArray *frame_block_values_fpc;      // doubles (frames-per-chunk / burst overlap)
    guint frame_block_view; // 0=lateness, 1=size, 2=span, 3=chunks, 4=frames-per-chunk
    guint frame_block_missing;
    guint frame_block_real_samples;
    gboolean audio_runtime_enabled;
    gboolean audio_active;
    double frame_overlay_range_seconds;
    gboolean frame_overlay_show_values;
    gboolean frame_overlay_needs_refresh;

    GtkButton *idr_button;
    GtkSpinButton *idr_port_spin;
    gint idr_inflight;

    GtkToggleButton *stats_pause_toggle;
    gboolean stats_paused;

    GtkLabel *stream_detail_label;

    /* Sidecar tab widgets. */
    GtkToggleButton *sidecar_enable_toggle;
    GtkSpinButton *sidecar_port_spin;
    GtkLabel *sidecar_status_label;
    GtkLabel *sidecar_frame_label;
    GtkLabel *sidecar_encoder_label;
    GtkLabel *sidecar_counters_label;
    GtkLabel *sidecar_transport_label;
    gboolean sidecar_toggle_suppress;

    /* Frame Release tab widgets + cached snapshot. */
    GtkToggleButton *frame_release_enable_toggle;
    GtkToggleButton *frame_release_pause_toggle;
    GtkButton *frame_release_reset_button;
    GtkSpinButton *frame_release_gap_spin;
    GtkButton *frame_release_calib_button;
    GtkLabel *frame_release_calib_label;
    gboolean frame_release_calib_pending;  // Auto pressed, awaiting a fresh result
    guint frame_release_calib_seq;         // last calib_seq consumed from snapshots
    GtkDrawingArea *frame_release_timeline_area;
    GtkDrawingArea *frame_release_hist_area;
    GtkLabel *frame_release_summary_label;
    gboolean frame_release_valid;
    gboolean frame_release_active;
    gboolean frame_release_paused;
    double frame_release_gap_us;
    guint64 frame_release_total_chunks;
    guint64 frame_release_overlap_chunks;
    double frame_release_overlap_rate;
    double frame_release_avg_pkts;
    double frame_release_avg_frames;
    guint frame_release_hist[UV_RELEASE_FRAMES_BUCKETS];
    GArray *frame_release_chunks; // UvReleaseChunk, oldest-first
    GArray *frame_release_frames; // UvReleaseFrame, oldest-first (cadence timeline)
    double frame_release_period_ms;
    guint frame_release_window_s; // cadence timeline window in seconds
    GtkDropDown *frame_release_window_dropdown;
    GtkDrawingArea *frame_release_overview_area;
    guint frame_release_sel_start;  // first frame index of the detail selection
    guint frame_release_sel_count;  // number of frames in the selection
    gboolean frame_release_follow;  // TRUE = detail tracks the newest frames
    GtkToggleButton *frame_release_follow_toggle;
    gint frame_release_hover_idx;       // absolute frame index under the cursor (-1 = none)
    double frame_release_hover_px;      // cursor x within the detail area (valid when hover_idx >= 0)
    guint frame_release_pan_anchor_start; // sel_start captured at detail pan drag-begin
} GuiContext;

typedef struct {
    GuiContext *ctx;
    UvViewerEventKind kind;
    int source_index;
    char *address;
    UvSourceKind source_kind;
    char *error_message;
} UiEvent;

typedef struct {
    double timestamp;
    double rate_bps;
    double lost_packets;      // cumulative count (kept for delta arithmetic)
    double dup_packets;
    double reorder_packets;
    double rx_packets;        // cumulative RX packets (kept for delta arithmetic)
    double rx_bytes;          // cumulative RX bytes
    double lost_pps;          // per-second rate vs. previous sample (charted)
    double dup_pps;
    double reorder_pps;
    double pps;               // RX packets/sec vs. previous sample (charted)
    double pkt_size_bytes;    // mean RX bytes/packet over the interval (charted)
    double jitter_ms;
    double input_fps;
    double decoder_fps_current;
    double frame_lateness_ms;
    double frame_size_kb;
    gboolean frame_valid;
    gboolean frame_missing;
} StatsSample;

typedef struct {
    double x;
    double y;
    double value;
} FrameOverlayPoint;

#define FRAME_BLOCK_VIEW_LATENESS 0u
#define FRAME_BLOCK_VIEW_SIZE     1u
#define FRAME_BLOCK_VIEW_SPAN     2u
#define FRAME_BLOCK_VIEW_CHUNKS   3u
#define FRAME_BLOCK_VIEW_FPC      4u
#define FRAME_BLOCK_VIEW_COUNT    5u
#define FRAME_OVERLAY_METRIC_LATENESS 0u
#define FRAME_OVERLAY_METRIC_SIZE     1u

static const guint FRAME_BLOCK_WIDTH_OPTIONS[] = {30u, 60u, 90u, 100u, 120u};
#define FRAME_BLOCK_WIDTH_OPTION_COUNT (G_N_ELEMENTS(FRAME_BLOCK_WIDTH_OPTIONS))

static GtkWidget *build_monitor_page(GuiContext *ctx);
static GtkWidget *build_settings_page(GuiContext *ctx);
static GtkWidget *build_stats_page(GuiContext *ctx);
static GtkWidget *build_frame_block_page(GuiContext *ctx);
static GtkWidget *build_frame_release_page(GuiContext *ctx);
static GtkWidget *build_sidecar_page(GuiContext *ctx);
static void on_sidecar_enable_toggled(GtkToggleButton *btn, gpointer user_data);
static void on_sidecar_port_changed(GtkSpinButton *spin, gpointer user_data);
static void update_sidecar_toggle_label(GuiContext *ctx, gboolean enabled);
static GtkWidget *build_audio_page(GuiContext *ctx);
static void on_audio_apply_clicked(GtkButton *btn, gpointer user_data);
static void on_audio_restart_clicked(GtkButton *btn, gpointer user_data);
static void on_audio_port_mode_changed(GObject *dropdown, GParamSpec *pspec, gpointer user_data);
static void update_audio_status_label(GuiContext *ctx, const UvViewerStats *stats);
static void update_restream_status_label(GuiContext *ctx, const UvRestreamStats *rs);
static void on_restream_toggled(GtkCheckButton *btn, gpointer user_data);
static void viewer_event_callback(const UvViewerEvent *event, gpointer user_data);
static void on_notebook_switch_page(GtkNotebook *notebook, GtkWidget *page, guint page_num, gpointer user_data);
static void detach_bound_sink(GuiContext *ctx);
static gboolean ensure_video_paintable(GuiContext *ctx);
static void restart_stats_timer(GuiContext *ctx);
static void set_stats_refresh_interval(GuiContext *ctx, guint interval_ms);
static void frame_block_queue_overlay_draws(GuiContext *ctx);
static void frame_block_queue_overlay_draws_force(GuiContext *ctx);
static void stats_range_changed(GObject *dropdown, GParamSpec *pspec, gpointer user_data);
static void stats_chart_draw(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer user_data);
static void frame_block_draw(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer user_data);
static void frame_overlay_draw(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer user_data);
static void frame_distribution_draw(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer user_data);
static void on_frame_block_enable_toggled(GtkToggleButton *button, gpointer user_data);
static void on_frame_block_pause_toggled(GtkToggleButton *button, gpointer user_data);
static void on_frame_block_mode_changed(GObject *dropdown, GParamSpec *pspec, gpointer user_data);
static void on_frame_block_width_changed(GObject *dropdown, GParamSpec *pspec, gpointer user_data);
static void on_frame_block_metric_changed(GObject *dropdown, GParamSpec *pspec, gpointer user_data);
static void on_frame_block_reset_clicked(GtkButton *button, gpointer user_data);
static void on_frame_block_threshold_changed(GtkSpinButton *spin, gpointer user_data);
static void on_frame_block_color_toggled(GtkCheckButton *check, gpointer user_data);
static void on_frame_overlay_range_changed(GObject *dropdown, GParamSpec *pspec, gpointer user_data);
static void on_frame_overlay_values_toggled(GtkCheckButton *button, gpointer user_data);
static void on_videorate_toggled(GtkCheckButton *button, gpointer user_data);
static void on_audio_toggled(GtkCheckButton *button, gpointer user_data);
static void on_source_dropdown_changed(GObject *dropdown, GParamSpec *pspec, gpointer user_data);
static void on_idr_button_clicked(GtkButton *button, gpointer user_data);
static void update_idr_button_sensitivity(GuiContext *ctx);
static void install_app_css(void);
static double nice_axis_max(double v);
static void update_frame_overlay_labels(GuiContext *ctx);
static void update_stats_metric_labels(GuiContext *ctx);

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
    if (ctx->frame_block_values_span) {
        g_array_set_size(ctx->frame_block_values_span, capacity);
        for (guint i = 0; i < ctx->frame_block_values_span->len; i++) {
            g_array_index(ctx->frame_block_values_span, double, i) = NAN;
        }
    }
    if (ctx->frame_block_values_chunks) {
        g_array_set_size(ctx->frame_block_values_chunks, capacity);
        for (guint i = 0; i < ctx->frame_block_values_chunks->len; i++) {
            g_array_index(ctx->frame_block_values_chunks, double, i) = NAN;
        }
    }
    if (ctx->frame_block_values_fpc) {
        g_array_set_size(ctx->frame_block_values_fpc, capacity);
        for (guint i = 0; i < ctx->frame_block_values_fpc->len; i++) {
            g_array_index(ctx->frame_block_values_fpc, double, i) = NAN;
        }
    }
}

/* Per-view accessor helpers so the 4 metrics share one code path. */
static GArray *frame_block_view_values(GuiContext *ctx, guint view) {
    switch (view) {
        case FRAME_BLOCK_VIEW_SIZE:   return ctx->frame_block_values_size;
        case FRAME_BLOCK_VIEW_SPAN:   return ctx->frame_block_values_span;
        case FRAME_BLOCK_VIEW_CHUNKS: return ctx->frame_block_values_chunks;
        case FRAME_BLOCK_VIEW_FPC:    return ctx->frame_block_values_fpc;
        case FRAME_BLOCK_VIEW_LATENESS:
        default:                      return ctx->frame_block_values_lateness;
    }
}

static double *frame_block_view_thresholds(GuiContext *ctx, guint view) {
    switch (view) {
        case FRAME_BLOCK_VIEW_SIZE:   return ctx->frame_block_thresholds_kb;
        case FRAME_BLOCK_VIEW_SPAN:   return ctx->frame_block_thresholds_span;
        case FRAME_BLOCK_VIEW_CHUNKS: return ctx->frame_block_thresholds_chunks;
        case FRAME_BLOCK_VIEW_FPC:    return ctx->frame_block_thresholds_fpc;
        case FRAME_BLOCK_VIEW_LATENESS:
        default:                      return ctx->frame_block_thresholds_ms;
    }
}

static guint *frame_block_view_color_counts(GuiContext *ctx, guint view) {
    switch (view) {
        case FRAME_BLOCK_VIEW_SIZE:   return ctx->frame_block_color_counts_kb;
        case FRAME_BLOCK_VIEW_SPAN:   return ctx->frame_block_color_counts_span;
        case FRAME_BLOCK_VIEW_CHUNKS: return ctx->frame_block_color_counts_chunks;
        case FRAME_BLOCK_VIEW_FPC:    return ctx->frame_block_color_counts_fpc;
        case FRAME_BLOCK_VIEW_LATENESS:
        default:                      return ctx->frame_block_color_counts_ms;
    }
}

static const char *frame_block_view_unit(guint view) {
    switch (view) {
        case FRAME_BLOCK_VIEW_SIZE:   return "KB";
        case FRAME_BLOCK_VIEW_SPAN:   return "ms";
        case FRAME_BLOCK_VIEW_CHUNKS: return "";
        case FRAME_BLOCK_VIEW_FPC:    return "";
        case FRAME_BLOCK_VIEW_LATENESS:
        default:                      return "ms";
    }
}

static const char *frame_block_view_name(guint view) {
    switch (view) {
        case FRAME_BLOCK_VIEW_SIZE:   return "Size";
        case FRAME_BLOCK_VIEW_SPAN:   return "Span";
        case FRAME_BLOCK_VIEW_CHUNKS: return "Frame split";
        case FRAME_BLOCK_VIEW_FPC:    return "Frame overlap";
        case FRAME_BLOCK_VIEW_LATENESS:
        default:                      return "Lateness";
    }
}

static double frame_block_view_step(guint view) {
    switch (view) {
        case FRAME_BLOCK_VIEW_SIZE:   return 2.0;
        case FRAME_BLOCK_VIEW_SPAN:   return 0.5;
        case FRAME_BLOCK_VIEW_CHUNKS: return 1.0;
        case FRAME_BLOCK_VIEW_FPC:    return 1.0;
        case FRAME_BLOCK_VIEW_LATENESS:
        default:                      return 0.5;
    }
}

static double frame_block_view_max(guint view) {
    switch (view) {
        case FRAME_BLOCK_VIEW_SIZE:   return 100000.0;
        case FRAME_BLOCK_VIEW_CHUNKS: return 64.0;
        case FRAME_BLOCK_VIEW_FPC:    return 64.0;
        case FRAME_BLOCK_VIEW_SPAN:
        case FRAME_BLOCK_VIEW_LATENESS:
        default:                      return 1000.0;
    }
}

static guint frame_block_view_digits(guint view) {
    return (view == FRAME_BLOCK_VIEW_CHUNKS || view == FRAME_BLOCK_VIEW_FPC) ? 0u : 1u;
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

    double *thresholds = frame_block_view_thresholds(ctx, ctx->frame_block_view);
    thresholds[0] = green;
    thresholds[1] = yellow;
    thresholds[2] = orange;

    if (!ctx->viewer) return;
    switch (ctx->frame_block_view) {
        case FRAME_BLOCK_VIEW_SIZE:
            uv_viewer_frame_block_set_size_thresholds(ctx->viewer, green, yellow, orange);
            break;
        case FRAME_BLOCK_VIEW_SPAN:
            uv_viewer_frame_block_set_span_thresholds(ctx->viewer, green, yellow, orange);
            break;
        case FRAME_BLOCK_VIEW_CHUNKS:
            uv_viewer_frame_block_set_chunk_thresholds(ctx->viewer, green, yellow, orange);
            break;
        case FRAME_BLOCK_VIEW_FPC:
            uv_viewer_frame_block_set_overlap_thresholds(ctx->viewer, green, yellow, orange);
            break;
        case FRAME_BLOCK_VIEW_LATENESS:
        default:
            uv_viewer_frame_block_set_thresholds(ctx->viewer, green, yellow, orange);
            break;
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
            double min_val, avg_val, max_val;
            switch (ctx->frame_block_view) {
                case FRAME_BLOCK_VIEW_SIZE:
                    min_val = ctx->frame_block_min_kb;
                    avg_val = ctx->frame_block_avg_kb;
                    max_val = ctx->frame_block_max_kb;
                    break;
                case FRAME_BLOCK_VIEW_SPAN:
                    min_val = ctx->frame_block_min_span;
                    avg_val = ctx->frame_block_avg_span;
                    max_val = ctx->frame_block_max_span;
                    break;
                case FRAME_BLOCK_VIEW_CHUNKS:
                    min_val = ctx->frame_block_min_chunks;
                    avg_val = ctx->frame_block_avg_chunks;
                    max_val = ctx->frame_block_max_chunks;
                    break;
                case FRAME_BLOCK_VIEW_FPC:
                    min_val = ctx->frame_block_min_fpc;
                    avg_val = ctx->frame_block_avg_fpc;
                    max_val = ctx->frame_block_max_fpc;
                    break;
                case FRAME_BLOCK_VIEW_LATENESS:
                default:
                    min_val = ctx->frame_block_min_ms;
                    avg_val = ctx->frame_block_avg_ms;
                    max_val = ctx->frame_block_max_ms;
                    break;
            }
            const char *unit = frame_block_view_unit(ctx->frame_block_view);
            g_string_append_printf(summary, " | %s min/avg/max: ",
                                   frame_block_view_name(ctx->frame_block_view));
            g_string_append_printf(summary, "%.2f / %.2f / %.2f%s%s",
                                   min_val, avg_val, max_val,
                                   unit[0] ? " " : "", unit);

            const guint *counts = frame_block_view_color_counts(ctx, ctx->frame_block_view);
            const char *labels[FRAME_BLOCK_COLOR_COUNT] = {"Green", "Yellow", "Orange", "Red"};
            gboolean first_bucket = TRUE;
            char bucket_title[48];
            g_snprintf(bucket_title, sizeof(bucket_title), "%s buckets",
                       frame_block_view_name(ctx->frame_block_view));
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
            const char *unit = frame_block_view_unit(ctx->frame_block_view);
            g_string_append_printf(summary, " | %s min/avg/max: -- / -- / --%s%s",
                                   frame_block_view_name(ctx->frame_block_view),
                                   unit[0] ? " " : "", unit);
        }
    }

    g_string_append_printf(summary, " | missing=%u", ctx->frame_block_missing);
    g_string_append_printf(summary, " | real=%u", ctx->frame_block_real_samples);

    gtk_label_set_text(ctx->frame_block_summary_label, summary->str);
    g_string_free(summary, TRUE);
}

static void frame_block_queue_overlay_draws_internal(GuiContext *ctx, gboolean force) {
    if (!ctx) return;
    if (ctx->frame_block_paused && !force) {
        ctx->frame_overlay_needs_refresh = TRUE;
        return;
    }

    ctx->frame_overlay_needs_refresh = FALSE;

    update_frame_overlay_labels(ctx);

    if (ctx->frame_overlay_lateness) {
        gtk_widget_queue_draw(GTK_WIDGET(ctx->frame_overlay_lateness));
    }
    if (ctx->frame_overlay_size) {
        gtk_widget_queue_draw(GTK_WIDGET(ctx->frame_overlay_size));
    }
    if (ctx->frame_distribution_area) {
        gtk_widget_queue_draw(GTK_WIDGET(ctx->frame_distribution_area));
    }
}

static void frame_block_queue_overlay_draws(GuiContext *ctx) {
    frame_block_queue_overlay_draws_internal(ctx, FALSE);
}

static void frame_block_queue_overlay_draws_force(GuiContext *ctx) {
    frame_block_queue_overlay_draws_internal(ctx, TRUE);
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

/* Reload the three threshold spinners from the active view's stored
 * thresholds, plus reconfigure step/digits/range/labels for the unit. */
static void frame_block_reload_threshold_spins(GuiContext *ctx) {
    if (!ctx) return;
    const double *thresholds = frame_block_view_thresholds(ctx, ctx->frame_block_view);
    double step = frame_block_view_step(ctx->frame_block_view);
    guint digits = frame_block_view_digits(ctx->frame_block_view);
    double range_max = frame_block_view_max(ctx->frame_block_view);
    const char *unit = frame_block_view_unit(ctx->frame_block_view);
    const char *base_labels[3] = {"Green", "Yellow", "Orange"};

    for (guint i = 0; i < 3; i++) {
        GtkSpinButton *spin = ctx->frame_block_threshold_spins[i];
        if (!spin) continue;
        g_signal_handlers_block_by_func(spin, G_CALLBACK(on_frame_block_threshold_changed), ctx);
        gtk_spin_button_set_digits(spin, digits);
        gtk_spin_button_set_increments(spin, step, step * 5.0);
        gtk_spin_button_set_range(spin, 0.0, range_max);
        gtk_spin_button_set_value(spin, thresholds[i]);
        g_signal_handlers_unblock_by_func(spin, G_CALLBACK(on_frame_block_threshold_changed), ctx);

        GtkLabel *label = ctx->frame_block_threshold_labels[i];
        if (label) {
            char text[64];
            if (unit[0]) {
                g_snprintf(text, sizeof(text), "%s threshold (%s)", base_labels[i], unit);
            } else {
                g_snprintf(text, sizeof(text), "%s threshold", base_labels[i]);
            }
            gtk_label_set_text(label, text);
        }
    }
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

    if (ctx->frame_block_metric_dropdown) {
        guint desired = ctx->frame_block_view;
        guint current = gtk_drop_down_get_selected(ctx->frame_block_metric_dropdown);
        if (current != desired) {
            g_signal_handlers_block_by_func(ctx->frame_block_metric_dropdown, G_CALLBACK(on_frame_block_metric_changed), ctx);
            gtk_drop_down_set_selected(ctx->frame_block_metric_dropdown, desired);
            g_signal_handlers_unblock_by_func(ctx->frame_block_metric_dropdown, G_CALLBACK(on_frame_block_metric_changed), ctx);
        }
        gtk_widget_set_sensitive(GTK_WIDGET(ctx->frame_block_metric_dropdown), TRUE);
    }

    if (ctx->frame_overlay_values_toggle) {
        gboolean desired = ctx->frame_overlay_show_values;
        gboolean current = gtk_check_button_get_active(ctx->frame_overlay_values_toggle);
        if (current != desired) {
            g_signal_handlers_block_by_func(ctx->frame_overlay_values_toggle,
                                            G_CALLBACK(on_frame_overlay_values_toggled),
                                            ctx);
            gtk_check_button_set_active(ctx->frame_overlay_values_toggle, desired);
            g_signal_handlers_unblock_by_func(ctx->frame_overlay_values_toggle,
                                              G_CALLBACK(on_frame_overlay_values_toggled),
                                              ctx);
        }
        gtk_widget_set_sensitive(GTK_WIDGET(ctx->frame_overlay_values_toggle), TRUE);
    }

    if (ctx->frame_block_reset_button) {
        gtk_widget_set_sensitive(GTK_WIDGET(ctx->frame_block_reset_button), active);
    }

    const double *thresholds = frame_block_view_thresholds(ctx, ctx->frame_block_view);
    double step = frame_block_view_step(ctx->frame_block_view);
    guint digits = frame_block_view_digits(ctx->frame_block_view);
    double range_max = frame_block_view_max(ctx->frame_block_view);
    const char *unit = frame_block_view_unit(ctx->frame_block_view);
    const char *base_labels[3] = {"Green", "Yellow", "Orange"};

    for (guint i = 0; i < 3; i++) {
        GtkSpinButton *spin = ctx->frame_block_threshold_spins[i];
        if (!spin) continue;
        g_signal_handlers_block_by_func(spin, G_CALLBACK(on_frame_block_threshold_changed), ctx);
        gtk_spin_button_set_digits(spin, digits);
        gtk_spin_button_set_increments(spin, step, step * 5.0);
        gtk_spin_button_set_range(spin, 0.0, range_max);

        if (!gtk_widget_has_focus(GTK_WIDGET(spin))) {
            double current = gtk_spin_button_get_value(spin);
            if (fabs(current - thresholds[i]) > 0.0001) {
                gtk_spin_button_set_value(spin, thresholds[i]);
            }
        }

        g_signal_handlers_unblock_by_func(spin, G_CALLBACK(on_frame_block_threshold_changed), ctx);

        GtkLabel *label = ctx->frame_block_threshold_labels[i];
        if (label) {
            char text[64];
            if (unit[0]) {
                g_snprintf(text, sizeof(text), "%s threshold (%s)", base_labels[i], unit);
            } else {
                g_snprintf(text, sizeof(text), "%s threshold", base_labels[i]);
            }
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

    GArray *values = frame_block_view_values(ctx, ctx->frame_block_view);
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
                const double *thresholds = frame_block_view_thresholds(ctx, ctx->frame_block_view);
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

    gboolean show_values = ctx->frame_overlay_show_values;

    cairo_save(cr);
    cairo_set_source_rgb(cr, 0.10, 0.10, 0.12);
    cairo_paint(cr);

    cairo_set_source_rgba(cr, 1, 1, 1, 0.1);
    cairo_rectangle(cr, 0.5, 0.5, width - 1.0, height - 1.0);
    cairo_stroke(cr);

    GArray *points = NULL;

    if (!ctx->stats_history || ctx->stats_history->len == 0) {
        cairo_set_source_rgb(cr, 0.7, 0.7, 0.7);
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 12.0);
        cairo_move_to(cr, 10, height / 2.0);
        cairo_show_text(cr, "No data yet");
        goto cleanup;
    }

    double range = ctx->frame_overlay_range_seconds > 0.0 ? ctx->frame_overlay_range_seconds : 60.0;
    if (range < 1.0) range = 1.0;
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
        cairo_set_source_rgb(cr, 0.7, 0.7, 0.7);
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 12.0);
        cairo_move_to(cr, 10, height / 2.0);
        cairo_show_text(cr, "No samples in range");
        goto cleanup;
    }

    double peak_value = max_val;
    if (!isfinite(peak_value)) {
        peak_value = 0.0;
    }

    double axis_min = 0.0;
    double axis_max = nice_axis_max(peak_value > 0.0 ? peak_value : 1.0);
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

    points = g_array_new(FALSE, FALSE, sizeof(FrameOverlayPoint));

    for (guint i = start_index; i < len; i++) {
        const StatsSample *sample = &samples[i];
        double value = 0.0;
        gboolean missing_flag = FALSE;
        if (!frame_overlay_sample_value(sample, metric, &value, &missing_flag)) {
            if (missing_flag) missing_seen = TRUE;
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

        FrameOverlayPoint point = {x, y, value};
        g_array_append_val(points, point);

        if (!path_started) {
            cairo_move_to(cr, x, y);
            path_started = TRUE;
        } else {
            cairo_line_to(cr, x, y);
        }
    }
    cairo_stroke(cr);

    if (points && points->len > 0) {
        cairo_set_line_width(cr, 1.0);
        cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
        for (guint i = 0; i < points->len; i++) {
            FrameOverlayPoint *pt = &g_array_index(points, FrameOverlayPoint, i);
            cairo_arc(cr, pt->x, pt->y, 2.0, 0, 2 * G_PI);
            cairo_fill(cr);
        }

        if (show_values) {
            cairo_set_source_rgb(cr, 0.92, 0.92, 0.96);
            cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
            cairo_set_font_size(cr, 9.5);
            for (guint i = 0; i < points->len; i++) {
                FrameOverlayPoint *pt = &g_array_index(points, FrameOverlayPoint, i);
                char value_text[64];
                if (metric == FRAME_OVERLAY_METRIC_SIZE) {
                    g_snprintf(value_text, sizeof(value_text), "%.2f KB", pt->value);
                } else {
                    g_snprintf(value_text, sizeof(value_text), "%.2f ms", pt->value);
                }
                cairo_text_extents_t ext;
                cairo_text_extents(cr, value_text, &ext);
                double text_x = pt->x + 4.0;
                if (text_x + ext.width > plot_right) {
                    text_x = plot_right - ext.width - 2.0;
                }
                if (text_x < plot_left) {
                    text_x = plot_left + 2.0;
                }
                double text_y = pt->y - 4.0;
                if (text_y - ext.height < plot_top) {
                    text_y = pt->y + ext.height + 6.0;
                    if (text_y > plot_bottom - 2.0) {
                        text_y = plot_bottom - 2.0;
                    }
                }
                cairo_move_to(cr, text_x, text_y);
                cairo_show_text(cr, value_text);
            }
        }
    }

    if (missing_seen) {
        cairo_set_source_rgba(cr, 0.8, 0.6, 0.2, 0.8);
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 11.0);
        cairo_move_to(cr, plot_left + 6.0, plot_top + 14.0);
        cairo_show_text(cr, "Missing frames present");
    }

cleanup:
    if (points) {
        g_array_free(points, TRUE);
    }
    cairo_restore(cr);
}

static void frame_distribution_draw(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer user_data) {
    (void)area;
    GuiContext *ctx = user_data;
    if (!ctx || width <= 0 || height <= 0) return;

    const char *unit = frame_block_view_unit(ctx->frame_block_view);
    char default_stats[128];
    g_snprintf(default_stats, sizeof(default_stats), "μ: -- %s | σ: -- %s | n: 0", unit, unit);
    if (ctx->frame_distribution_stats_label) {
        gtk_label_set_text(ctx->frame_distribution_stats_label, default_stats);
    }

    cairo_save(cr);
    cairo_set_source_rgb(cr, 0.10, 0.10, 0.12);
    cairo_paint(cr);

    cairo_set_source_rgba(cr, 1, 1, 1, 0.1);
    cairo_rectangle(cr, 0.5, 0.5, width - 1.0, height - 1.0);
    cairo_stroke(cr);

    GArray *values = frame_block_view_values(ctx, ctx->frame_block_view);
    if (!values || values->len == 0) {
        cairo_set_source_rgb(cr, 0.75, 0.75, 0.78);
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 12.0);
        cairo_move_to(cr, 10.0, height / 2.0);
        cairo_show_text(cr, "No frame data captured");
        cairo_restore(cr);
        return;
    }

    double sum = 0.0;
    double sum_sq = 0.0;
    double min_val = G_MAXDOUBLE;
    double max_val = -G_MAXDOUBLE;
    guint count = 0;

    for (guint i = 0; i < values->len; i++) {
        double v = g_array_index(values, double, i);
        if (!isfinite(v) || v < 0.0) continue;
        sum += v;
        sum_sq += v * v;
        if (v < min_val) min_val = v;
        if (v > max_val) max_val = v;
        count++;
    }

    if (count == 0) {
        cairo_set_source_rgb(cr, 0.75, 0.75, 0.78);
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 12.0);
        cairo_move_to(cr, 10.0, height / 2.0);
        cairo_show_text(cr, "No valid frame samples");
        cairo_restore(cr);
        return;
    }

    double mean = sum / (double)count;
    double variance = (sum_sq / (double)count) - (mean * mean);
    if (variance < 0.0) variance = 0.0;
    double stddev = sqrt(variance);

    const guint bin_count = 15u;
    const guint overflow_bins = 2u;
    guint interior_bin_count = (bin_count > overflow_bins) ? (bin_count - overflow_bins) : bin_count;
    if (interior_bin_count == 0u) {
        interior_bin_count = bin_count;
    }

    double span = stddev > 1e-6 ? stddev * 6.0 : 0.0;
    if (span <= 0.0) {
        span = max_val - min_val;
    }
    if (span <= 0.0) {
        span = fabs(mean) * 0.5;
    }
    if (span <= 0.0) {
        span = 1.0;
    }
    double bin_width = span / (double)interior_bin_count;
    if (bin_width <= 0.0) {
        bin_width = 1.0;
    }

    double interior_start = mean - bin_width * ((double)interior_bin_count / 2.0);
    double interior_end = interior_start + bin_width * (double)interior_bin_count;
    double display_start = interior_start - bin_width;

    guint bins[15] = {0};
    double bin_sums[15] = {0.0};
    guint max_bin_count = 0;
    for (guint i = 0; i < values->len; i++) {
        double v = g_array_index(values, double, i);
        if (!isfinite(v) || v < 0.0) continue;
        guint idx;
        if (v < interior_start) {
            idx = 0u;
        } else if (v >= interior_end) {
            idx = bin_count - 1u;
        } else {
            double offset = v - interior_start;
            guint interior_index = (guint)floor(offset / bin_width);
            if (interior_index >= interior_bin_count) {
                interior_index = interior_bin_count - 1u;
            }
            idx = interior_index + 1u;
        }
        bins[idx]++;
        bin_sums[idx] += v;
        if (bins[idx] > max_bin_count) {
            max_bin_count = bins[idx];
        }
    }

    double bin_means[15];
    for (guint i = 0; i < bin_count; i++) {
        bin_means[i] = (bins[i] > 0) ? (bin_sums[i] / (double)bins[i]) : NAN;
    }

    if (max_bin_count == 0) {
        cairo_set_source_rgb(cr, 0.75, 0.75, 0.78);
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 12.0);
        cairo_move_to(cr, 10.0, height / 2.0);
        cairo_show_text(cr, "No histogram data");
        cairo_restore(cr);
        return;
    }

    char stats_text[192];
    g_snprintf(stats_text,
               sizeof(stats_text),
               "μ=%.2f %s | σ=%.2f %s | n=%u | min=%.2f %s | max=%.2f %s",
               mean,
               unit,
               stddev,
               unit,
               count,
               min_val,
               unit,
               max_val,
               unit);
    if (ctx->frame_distribution_stats_label) {
        gtk_label_set_text(ctx->frame_distribution_stats_label, stats_text);
    }

    const double left_margin = 68.0;
    const double right_margin = 24.0;
    const double top_margin = 24.0;
    const double bottom_margin = 40.0;
    double plot_width = MAX(1.0, width - (left_margin + right_margin));
    double plot_height = MAX(1.0, height - (top_margin + bottom_margin));
    double plot_left = left_margin;
    double plot_right = plot_left + plot_width;
    double plot_top = top_margin;
    double plot_bottom = plot_top + plot_height;

    cairo_set_source_rgba(cr, 1, 1, 1, 0.08);
    cairo_move_to(cr, plot_left, plot_bottom);
    cairo_line_to(cr, plot_right, plot_bottom);
    cairo_stroke(cr);

    cairo_set_source_rgba(cr, 1, 1, 1, 0.06);
    for (int i = 1; i < 4; i++) {
        double frac = (double)i / 4.0;
        double y = plot_bottom - plot_height * frac;
        cairo_move_to(cr, plot_left, y);
        cairo_line_to(cr, plot_right, y);
    }
    cairo_stroke(cr);

    cairo_set_source_rgb(cr, 0.35, 0.70, 1.0);
    double bar_width = plot_width / (double)bin_count;
    for (guint i = 0; i < bin_count; i++) {
        double ratio = (double)bins[i] / (double)max_bin_count;
        double bar_height = plot_height * ratio;
        double x = plot_left + (double)i * bar_width;
        double y = plot_bottom - bar_height;
        cairo_rectangle(cr, x + 1.5, y, bar_width - 3.0, bar_height);
        cairo_fill(cr);

        if (bins[i] > 0) {
            cairo_set_source_rgb(cr, 0.95, 0.95, 0.98);
            cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
            cairo_set_font_size(cr, 10.0);
            char count_label[16];
            g_snprintf(count_label, sizeof(count_label), "%u", bins[i]);
            cairo_text_extents_t ext;
            cairo_text_extents(cr, count_label, &ext);
            double cx = x + (bar_width - ext.width) / 2.0 - ext.x_bearing;
            double cy = y - 4.0;
            if (cy < plot_top + 10.0) {
                cy = y + ext.height + 4.0;
            }
            cairo_move_to(cr, cx, cy);
            cairo_show_text(cr, count_label);

            double mean_val = bin_means[i];
            if (isfinite(mean_val)) {
                cairo_set_source_rgb(cr, 0.78, 0.90, 1.0);
                cairo_set_font_size(cr, 9.0);
                char mean_label[48];
                g_snprintf(mean_label, sizeof(mean_label), "μ %.2f %s", mean_val, unit);
                cairo_text_extents_t mean_ext;
                cairo_text_extents(cr, mean_label, &mean_ext);
                double mean_x = x + (bar_width - mean_ext.width) / 2.0 - mean_ext.x_bearing;
                double mean_y = cy - 12.0;
                if (mean_y < plot_top + 8.0) {
                    mean_y = cy + mean_ext.height + 6.0;
                    if (mean_y > plot_bottom - 4.0) {
                        mean_y = plot_bottom - 4.0;
                    }
                }
                cairo_move_to(cr, mean_x, mean_y);
                cairo_show_text(cr, mean_label);
            }

            cairo_set_source_rgb(cr, 0.35, 0.70, 1.0);
        }
    }

    double full_span = bin_width * (double)bin_count;
    double mean_ratio = (mean - display_start) / full_span;
    if (mean_ratio < 0.0) mean_ratio = 0.0;
    if (mean_ratio > 1.0) mean_ratio = 1.0;
    double mean_x = plot_left + mean_ratio * plot_width;
    cairo_set_source_rgba(cr, 1.0, 0.35, 0.35, 0.9);
    cairo_move_to(cr, mean_x, plot_top);
    cairo_line_to(cr, mean_x, plot_bottom);
    cairo_set_line_width(cr, 1.2);
    cairo_stroke(cr);

    cairo_set_source_rgb(cr, 0.82, 0.82, 0.86);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 11.0);

    double left_value = interior_start;
    double right_value = interior_end;
    char tick_text[64];

    g_snprintf(tick_text, sizeof(tick_text), "< %.2f %s", left_value, unit);
    cairo_move_to(cr, plot_left, plot_bottom + 16.0);
    cairo_show_text(cr, tick_text);

    g_snprintf(tick_text, sizeof(tick_text), "μ=%.2f %s", mean, unit);
    cairo_text_extents_t ext;
    cairo_text_extents(cr, tick_text, &ext);
    double mid_x = plot_left + (plot_width - ext.width) / 2.0 - ext.x_bearing;
    cairo_move_to(cr, mid_x, plot_bottom + 32.0);
    cairo_show_text(cr, tick_text);

    g_snprintf(tick_text, sizeof(tick_text), "> %.2f %s", right_value, unit);
    cairo_text_extents(cr, tick_text, &ext);
    double right_x = plot_right - ext.width - ext.x_bearing;
    cairo_move_to(cr, right_x, plot_bottom + 16.0);
    cairo_show_text(cr, tick_text);

    cairo_restore(cr);
}

static void update_status(GuiContext *ctx, const char *message) {
    if (!ctx || !ctx->status_label) return;
    gtk_label_set_text(ctx->status_label, message ? message : "");
}

typedef struct {
    GuiContext *ctx;
    char address[UV_VIEWER_ADDR_MAX];
    guint port;
    char message[192];
    gboolean ok;
    gboolean link_recovery;
} IdrJob;

static gboolean idr_apply_result(gpointer data) {
    IdrJob *job = data;
    if (!job) return G_SOURCE_REMOVE;
    GuiContext *ctx = job->ctx;
    if (ctx && ctx->status_label) {
        update_status(ctx, job->message);
    }
    if (ctx) {
        if (ctx->idr_inflight > 0) ctx->idr_inflight--;
        update_idr_button_sensitivity(ctx);
    }
    g_free(job);
    return G_SOURCE_REMOVE;
}

static gpointer idr_worker(gpointer data) {
    IdrJob *job = data;
    if (!job) return NULL;

    GError *err = NULL;
    GSocketClient *client = g_socket_client_new();
    g_socket_client_set_timeout(client, 3);
    GSocketConnection *conn = g_socket_client_connect_to_host(client,
                                                              job->address,
                                                              (guint16)job->port,
                                                              NULL,
                                                              &err);
    if (!conn) {
        g_snprintf(job->message, sizeof(job->message),
                   "IDR request to %s:%u failed: %s",
                   job->address, job->port,
                   err && err->message ? err->message : "connect failed");
    } else {
        char request[256];
        if (job->link_recovery) {
            g_snprintf(request, sizeof(request),
                       "POST /api/v1/video/recover HTTP/1.0\r\n"
                       "Host: %s:%u\r\n"
                       "User-Agent: udp-h265-viewer\r\n"
                       "Content-Type: application/json\r\n"
                       "Content-Length: 2\r\n"
                       "Connection: close\r\n\r\n{}",
                       job->address, job->port);
        } else {
            g_snprintf(request, sizeof(request),
                       "GET /request/idr HTTP/1.0\r\n"
                       "Host: %s:%u\r\n"
                       "User-Agent: udp-h265-viewer\r\n"
                       "Connection: close\r\n\r\n",
                       job->address, job->port);
        }
        GOutputStream *ostream = g_io_stream_get_output_stream(G_IO_STREAM(conn));
        gsize written = 0;
        if (!g_output_stream_write_all(ostream, request, strlen(request),
                                       &written, NULL, &err)) {
            g_snprintf(job->message, sizeof(job->message),
                       "IDR request to %s:%u failed: %s",
                       job->address, job->port,
                       err && err->message ? err->message : "write failed");
        } else {
            GInputStream *istream = g_io_stream_get_input_stream(G_IO_STREAM(conn));
            char buf[256] = {0};
            gssize n = g_input_stream_read(istream, buf, sizeof(buf) - 1, NULL, &err);
            if (n <= 0) {
                g_snprintf(job->message, sizeof(job->message),
                           "IDR request to %s:%u: no response", job->address, job->port);
            } else {
                buf[n] = '\0';
                char *eol = strpbrk(buf, "\r\n");
                if (eol) *eol = '\0';
                if (strncmp(buf, "HTTP/", 5) == 0 && strstr(buf, " 200") != NULL) {
                    job->ok = TRUE;
                    g_snprintf(job->message, sizeof(job->message),
                               "%s requested via %s:%u (HTTP 200 OK)",
                               job->link_recovery ? "SHM decoder recovery" : "IDR",
                               job->address, job->port);
                } else {
                    g_snprintf(job->message, sizeof(job->message),
                               "IDR request to %s:%u: %s",
                               job->address, job->port, buf);
                }
            }
        }
        g_io_stream_close(G_IO_STREAM(conn), NULL, NULL);
        g_object_unref(conn);
    }

    if (err) g_error_free(err);
    g_object_unref(client);
    g_idle_add(idr_apply_result, job);
    return NULL;
}

static void update_idr_button_sensitivity(GuiContext *ctx) {
    if (!ctx || !ctx->idr_button) return;
    gboolean has_source = ctx->active_source_valid;
    gboolean busy = (ctx->idr_inflight > 0);
    gtk_widget_set_sensitive(GTK_WIDGET(ctx->idr_button), has_source && !busy);
}

static gboolean start_idr_job(GuiContext *ctx, const char *address, guint port,
                              gboolean link_recovery) {
    if (!ctx || !address || !address[0] || ctx->idr_inflight > 0) return FALSE;
    IdrJob *job = g_new0(IdrJob, 1);
    job->ctx = ctx;
    job->port = port;
    job->link_recovery = link_recovery;
    g_strlcpy(job->address, address, sizeof(job->address));

    ctx->idr_inflight++;
    update_idr_button_sensitivity(ctx);
    GThread *t = g_thread_new("uv-idr", idr_worker, job);
    if (!t) {
        ctx->idr_inflight--;
        update_idr_button_sensitivity(ctx);
        g_free(job);
        return FALSE;
    }
    g_thread_unref(t);
    return TRUE;
}

static gboolean request_shm_recovery_after_select(gpointer data) {
    GuiContext *ctx = data;
    if (!ctx) return G_SOURCE_REMOVE;
    ctx->shm_recovery_timeout_id = 0;
    if (!ctx->active_source_valid ||
        !start_idr_job(ctx, "127.0.0.1", SHM_RECOVERY_PORT, TRUE)) {
        return G_SOURCE_REMOVE;
    }
    update_status(ctx, "Requesting SHM decoder recovery...");
    return G_SOURCE_REMOVE;
}

static void schedule_shm_recovery(GuiContext *ctx) {
    if (!ctx) return;
    if (ctx->shm_recovery_timeout_id != 0) {
        g_source_remove(ctx->shm_recovery_timeout_id);
    }
    // Let the replacement pipeline reach PLAYING and appsrc request data.
    ctx->shm_recovery_timeout_id =
        g_timeout_add(500, request_shm_recovery_after_select, ctx);
}

static void on_idr_button_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    GuiContext *ctx = user_data;
    if (!ctx || !ctx->viewer) return;

    if (!ctx->active_source_valid) {
        update_status(ctx, "IDR request skipped: no source is currently locked.");
        return;
    }

    UvViewerStats stats = {0};
    uv_viewer_stats_init(&stats);
    if (!uv_viewer_get_stats(ctx->viewer, &stats)) {
        update_status(ctx, "IDR request failed: could not read viewer stats.");
        uv_viewer_stats_clear(&stats);
        return;
    }

    char address[UV_VIEWER_ADDR_MAX] = {0};
    UvSourceKind source_kind = UV_SOURCE_UDP;
    gboolean found = FALSE;
    if (stats.sources) {
        for (guint i = 0; i < stats.sources->len; i++) {
            const UvSourceStats *src = &g_array_index(stats.sources, UvSourceStats, i);
            if (src->selected) {
                g_strlcpy(address, src->address, sizeof(address));
                source_kind = src->kind;
                found = TRUE;
                break;
            }
        }
    }
    uv_viewer_stats_clear(&stats);

    if (!found || address[0] == '\0') {
        update_status(ctx, "IDR request skipped: no source is currently locked.");
        return;
    }

    gboolean link_recovery = source_kind == UV_SOURCE_SHM;
    guint port = link_recovery ? SHM_RECOVERY_PORT :
        (ctx->current_cfg.idr_http_port ? ctx->current_cfg.idr_http_port : 80);
    const char *target = link_recovery ? "127.0.0.1" : address;

    char msg[160];
    g_snprintf(msg, sizeof(msg), "Requesting %s from %s:%u...",
               link_recovery ? "SHM decoder recovery" : "IDR keyframe",
               target, port);
    update_status(ctx, msg);

    if (!start_idr_job(ctx, target, port, link_recovery)) {
        update_status(ctx, "IDR request failed: could not spawn worker thread.");
    }
}

static void install_app_css(void) {
    static gboolean installed = FALSE;
    if (installed) return;
    installed = TRUE;

    GtkCssProvider *provider = gtk_css_provider_new();
    static const char *css =
        ".uv-status {"
        "  font-weight: 600;"
        "  padding: 4px 8px;"
        "  border-radius: 6px;"
        "}"
        ".uv-status-bar {"
        "  background-color: alpha(@theme_fg_color, 0.06);"
        "  border-radius: 6px;"
        "  padding: 2px 4px;"
        "}"
        ".uv-source-detail {"
        "  font-family: monospace;"
        "  font-size: 0.95em;"
        "}"
        ".uv-info {"
        "  font-size: 0.9em;"
        "  opacity: 0.75;"
        "}"
        ".uv-action-bar {"
        "  padding-top: 4px;"
        "}"
        "button.uv-idr {"
        "  font-weight: 600;"
        "}"
        "frame.uv-video > border {"
        "  border-radius: 8px;"
        "}"
        ".numeric {"
        "  font-feature-settings: \"tnum\";"
        "  font-variant-numeric: tabular-nums;"
        "}";
#if GTK_CHECK_VERSION(4, 12, 0)
    gtk_css_provider_load_from_string(provider, css);
#else
    gtk_css_provider_load_from_data(provider, css, -1);
#endif
    GdkDisplay *display = gdk_display_get_default();
    if (display) {
        gtk_style_context_add_provider_for_display(display,
                                                   GTK_STYLE_PROVIDER(provider),
                                                   GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
    g_object_unref(provider);
}

static void update_info_label(GuiContext *ctx) {
    if (!ctx || !ctx->info_label) return;
    char info[224];
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
               " | drop=%s | lost=%s | bus-msg=%s | videorate=%s | decoder=%s | sink=%s | audio=%s"
               " | IDR port %u",
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
               audio_state,
               cfg->idr_http_port ? cfg->idr_http_port : 80u);
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
    if (ctx->idr_port_spin) {
        guint port = ctx->current_cfg.idr_http_port ? ctx->current_cfg.idr_http_port : 80;
        gtk_spin_button_set_value(ctx->idr_port_spin, port);
    }
    if (ctx->restream_host_entry) {
        gtk_editable_set_text(GTK_EDITABLE(ctx->restream_host_entry),
                              ctx->current_cfg.restream_address);
    }
    if (ctx->restream_port_spin) {
        guint port = ctx->current_cfg.restream_port ? ctx->current_cfg.restream_port : 5600;
        gtk_spin_button_set_value(ctx->restream_port_spin, port);
    }
    if (ctx->restream_toggle) {
        gboolean on = ctx->current_cfg.restream_enabled ? TRUE : FALSE;
        ctx->restream_toggle_suppress = TRUE;
        gtk_check_button_set_active(ctx->restream_toggle, on);
        ctx->restream_toggle_suppress = FALSE;
        if (ctx->restream_host_entry) {
            gtk_widget_set_sensitive(GTK_WIDGET(ctx->restream_host_entry), !on);
        }
        if (ctx->restream_port_spin) {
            gtk_widget_set_sensitive(GTK_WIDGET(ctx->restream_port_spin), !on);
        }
    }
    if (ctx->shm_toggle) check_set(ctx->shm_toggle, ctx->current_cfg.shm_enabled);
    if (ctx->shm_name_entry) {
        gtk_editable_set_text(GTK_EDITABLE(ctx->shm_name_entry), ctx->current_cfg.shm_name);
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
        case STATS_METRIC_LOST:    return sample->lost_pps;
        case STATS_METRIC_DUP:     return sample->dup_pps;
        case STATS_METRIC_REORDER: return sample->reorder_pps;
        case STATS_METRIC_JITTER:  return sample->jitter_ms;
        case STATS_METRIC_INPUT_FPS: return sample->input_fps;
        case STATS_METRIC_DECODER_FPS: return sample->decoder_fps_current;
        case STATS_METRIC_PPS:     return sample->pps;
        case STATS_METRIC_PKT_SIZE: return sample->pkt_size_bytes;
        default:                   return 0.0;
    }
}

static const char *stats_metric_unit(StatsMetric metric) {
    switch (metric) {
        case STATS_METRIC_RATE:        return "Mbps";
        case STATS_METRIC_LOST:
        case STATS_METRIC_DUP:
        case STATS_METRIC_REORDER:     return "pps";
        case STATS_METRIC_JITTER:      return "ms";
        case STATS_METRIC_INPUT_FPS:
        case STATS_METRIC_DECODER_FPS: return "fps";
        case STATS_METRIC_PPS:         return "pps";
        case STATS_METRIC_PKT_SIZE:    return "bytes";
        default:                       return "";
    }
}

/* Round axis_max up to the next "nice" 1/2/5 * 10^k boundary so y-axis ticks
 * stop wobbling on every redraw when the live max creeps up slowly. */
static double nice_axis_max(double v) {
    if (!isfinite(v) || v <= 0.0) return 1.0;
    double exp10 = pow(10.0, floor(log10(v)));
    double frac = v / exp10;
    double nice;
    if (frac <= 1.0)      nice = 1.0;
    else if (frac <= 2.0) nice = 2.0;
    else if (frac <= 5.0) nice = 5.0;
    else                  nice = 10.0;
    return nice * exp10;
}

static void format_metric_value(StatsMetric metric, double value, char *out, size_t outlen) {
    const char *unit = stats_metric_unit(metric);
    if (!isfinite(value)) {
        g_strlcpy(out, "--", outlen);
        return;
    }
    if (metric == STATS_METRIC_RATE) {
        g_snprintf(out, outlen, "%.2f %s", value / 1e6, unit);
    } else if (metric == STATS_METRIC_LOST || metric == STATS_METRIC_DUP ||
               metric == STATS_METRIC_REORDER || metric == STATS_METRIC_PPS) {
        if (value < 10.0) {
            g_snprintf(out, outlen, "%.2f %s", value, unit);
        } else {
            g_snprintf(out, outlen, "%.0f %s", value, unit);
        }
    } else if (metric == STATS_METRIC_PKT_SIZE) {
        g_snprintf(out, outlen, "%.0f %s", value, unit);
    } else {
        g_snprintf(out, outlen, "%.2f %s", value, unit);
    }
}

static const char *sidecar_frame_type_name(uint8_t t) {
    switch (t) {
        case UV_SIDECAR_FRAME_P:   return "P";
        case UV_SIDECAR_FRAME_I:   return "I";
        case UV_SIDECAR_FRAME_IDR: return "IDR";
        default: return "?";
    }
}

static void update_sidecar_panel(GuiContext *ctx, const UvSidecarStats *sc) {
    if (!ctx || !sc) return;

    /* Status pill at the top of the sidecar tab. */
    if (ctx->sidecar_status_label) {
        char line[256];
        if (!sc->enabled) {
            g_strlcpy(line, "Stopped. Press Start to subscribe to the encoder's sidecar.",
                      sizeof(line));
        } else if (!sc->socket_bound) {
            g_strlcpy(line, "Starting — socket not yet open.", sizeof(line));
        } else if (!sc->target_address[0]) {
            g_snprintf(line, sizeof(line),
                       "Running (probe :%u → :%u) — waiting for a locked source.",
                       (unsigned)sc->local_port, (unsigned)sc->target_port);
        } else if (!sc->subscribed) {
            const char *seen;
            char buf[32];
            if (sc->seconds_since_last_frame < 0.0) {
                seen = "no frames yet";
            } else {
                g_snprintf(buf, sizeof(buf), "last frame %.1fs ago",
                           sc->seconds_since_last_frame);
                seen = buf;
            }
            g_snprintf(line, sizeof(line),
                       "Subscribing to %s:%u (probe :%u) — %s",
                       sc->target_address, (unsigned)sc->target_port,
                       (unsigned)sc->local_port, seen);
        } else {
            g_snprintf(line, sizeof(line),
                       "SUBSCRIBED to %s:%u  •  probe :%u  •  last frame %.2fs ago",
                       sc->target_address, (unsigned)sc->target_port,
                       (unsigned)sc->local_port, sc->seconds_since_last_frame);
        }
        gtk_label_set_text(ctx->sidecar_status_label, line);
    }

    char frame_line[256]    = {0};
    char enc_line[256]      = {0};
    char counters_line[224] = {0};
    char trans_line[256]    = {0};

    if (sc->enabled && sc->frames_received > 0) {
        g_snprintf(frame_line, sizeof(frame_line),
                   "Last frame: %s  •  %u bytes  •  %u RTP packets  •  ssrc=0x%08x  •  frame_id=%" G_GUINT64_FORMAT,
                   sidecar_frame_type_name(sc->last_frame_type),
                   (unsigned)sc->last_frame_size_bytes,
                   (unsigned)sc->last_seq_count,
                   (unsigned)sc->last_ssrc,
                   sc->last_frame_id);

        g_snprintf(enc_line, sizeof(enc_line),
                   "QP %u (avg %.1f)  •  complexity %u (avg %.1f)  •  scene_change=%s"
                   "  •  gop_state=%u  •  frames since IDR=%u",
                   (unsigned)sc->last_qp, sc->avg_qp,
                   (unsigned)sc->last_complexity, sc->avg_complexity,
                   sc->last_scene_change ? "YES" : "no",
                   (unsigned)sc->last_gop_state,
                   (unsigned)sc->last_frames_since_idr);

        g_snprintf(counters_line, sizeof(counters_line),
                   "Frames %" G_GUINT64_FORMAT
                   "  •  IDR insertions %" G_GUINT64_FORMAT
                   "  •  scene changes %" G_GUINT64_FORMAT
                   "  •  keyframes (I+IDR) %" G_GUINT64_FORMAT,
                   sc->frames_received,
                   sc->idr_inserted_count,
                   sc->scene_change_count,
                   sc->keyframes_count);

        if (sc->transport_info_seen) {
            g_snprintf(trans_line, sizeof(trans_line),
                       "Queue %u%%  •  pressure %s"
                       "  •  transport drops %u  •  pressure drops %u  •  packets sent %u",
                       (unsigned)sc->encoder_fill_pct,
                       sc->encoder_in_pressure ? "ASSERTED" : "—",
                       (unsigned)sc->encoder_transport_drops,
                       (unsigned)sc->encoder_pressure_drops,
                       (unsigned)sc->encoder_packets_sent);
        } else {
            g_strlcpy(trans_line,
                      "Transport trailer not yet seen on this encoder.",
                      sizeof(trans_line));
        }
    }

    if (ctx->sidecar_frame_label)     gtk_label_set_text(ctx->sidecar_frame_label, frame_line);
    if (ctx->sidecar_encoder_label)   gtk_label_set_text(ctx->sidecar_encoder_label, enc_line);
    if (ctx->sidecar_counters_label)  gtk_label_set_text(ctx->sidecar_counters_label, counters_line);
    if (ctx->sidecar_transport_label) gtk_label_set_text(ctx->sidecar_transport_label, trans_line);

    /* Keep the toggle button text in sync with reality (in case state
     * changes via a different code path, e.g. uv_viewer_set_sidecar). */
    if (ctx->sidecar_enable_toggle && !ctx->sidecar_toggle_suppress) {
        gboolean active = gtk_toggle_button_get_active(ctx->sidecar_enable_toggle);
        if (active != sc->enabled) {
            ctx->sidecar_toggle_suppress = TRUE;
            gtk_toggle_button_set_active(ctx->sidecar_enable_toggle, sc->enabled);
            ctx->sidecar_toggle_suppress = FALSE;
        }
        update_sidecar_toggle_label(ctx, sc->enabled);
    }
    if (ctx->sidecar_port_spin) {
        gtk_widget_set_sensitive(GTK_WIDGET(ctx->sidecar_port_spin), !sc->enabled);
    }
}

static void update_frame_overlay_labels(GuiContext *ctx) {
    if (!ctx) return;
    const char *units[2] = {"ms", "KB"};
    for (int m = 0; m < 2; m++) {
        GtkLabel *live_label = ctx->frame_overlay_live_labels[m];
        GtkLabel *max_label  = ctx->frame_overlay_max_labels[m];
        if (!ctx->stats_history || ctx->stats_history->len == 0) {
            if (live_label) gtk_label_set_text(live_label, "Live: --");
            if (max_label)  gtk_label_set_text(max_label, "Max: --");
            continue;
        }

        double range = ctx->frame_overlay_range_seconds > 0.0
                     ? ctx->frame_overlay_range_seconds : 60.0;
        if (range < 1.0) range = 1.0;
        double now = g_get_monotonic_time() / 1e6;
        double start_time = now - range;

        StatsSample *samples = (StatsSample *)ctx->stats_history->data;
        guint len = ctx->stats_history->len;
        guint start_index = 0;
        while (start_index < len && samples[start_index].timestamp < start_time) {
            start_index++;
        }
        if (start_index == len) start_index = len > 0 ? len - 1 : 0;

        double latest = NAN;
        double peak = -G_MAXDOUBLE;
        for (guint i = start_index; i < len; i++) {
            double v = 0.0;
            if (frame_overlay_sample_value(&samples[i], (guint)m, &v, NULL)) {
                if (v > peak) peak = v;
            }
        }
        for (gint i = (gint)len - 1; i >= (gint)start_index; i--) {
            double v = 0.0;
            if (frame_overlay_sample_value(&samples[i], (guint)m, &v, NULL)) {
                latest = v; break;
            }
        }

        char buf[80];
        if (live_label) {
            if (isfinite(latest)) {
                g_snprintf(buf, sizeof(buf), "Live: %.2f %s", latest, units[m]);
            } else {
                g_strlcpy(buf, "Live: --", sizeof(buf));
            }
            gtk_label_set_text(live_label, buf);
        }
        if (max_label) {
            if (peak == -G_MAXDOUBLE) {
                g_strlcpy(buf, "Max: --", sizeof(buf));
            } else {
                g_snprintf(buf, sizeof(buf), "Max: %.2f %s", peak, units[m]);
            }
            gtk_label_set_text(max_label, buf);
        }
    }
}

static void update_stats_metric_labels(GuiContext *ctx) {
    if (!ctx) return;
    if (!ctx->stats_history || ctx->stats_history->len == 0) {
        for (int i = 0; i < STATS_METRIC_COUNT; i++) {
            if (ctx->stats_live_labels[i]) gtk_label_set_text(ctx->stats_live_labels[i], "Live: --");
            if (ctx->stats_max_labels[i])  gtk_label_set_text(ctx->stats_max_labels[i],  "Max: --");
        }
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

    for (int m = 0; m < STATS_METRIC_COUNT; m++) {
        StatsMetric metric = (StatsMetric)m;
        double latest = NAN;
        double max_val = -G_MAXDOUBLE;
        for (guint i = start_index; i < len; i++) {
            double v = stats_metric_value(&samples[i], metric);
            if (!isfinite(v)) continue;
            if (v > max_val) max_val = v;
        }
        for (gint i = (gint)len - 1; i >= (gint)start_index; i--) {
            double v = stats_metric_value(&samples[i], metric);
            if (isfinite(v)) { latest = v; break; }
        }

        char buf[80];
        if (ctx->stats_live_labels[m]) {
            char value_text[64];
            format_metric_value(metric, latest, value_text, sizeof(value_text));
            g_snprintf(buf, sizeof(buf), "Live: %s", value_text);
            gtk_label_set_text(ctx->stats_live_labels[m], buf);
        }
        if (ctx->stats_max_labels[m]) {
            char value_text[64];
            format_metric_value(metric, (max_val == -G_MAXDOUBLE) ? NAN : max_val,
                                value_text, sizeof(value_text));
            g_snprintf(buf, sizeof(buf), "Max: %s", value_text);
            gtk_label_set_text(ctx->stats_max_labels[m], buf);
        }
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

/* ------------------------------------------------------------------ */
/* Frame Release tab                                                    */
/* ------------------------------------------------------------------ */

static void frame_release_update_summary(GuiContext *ctx) {
    if (!ctx || !ctx->frame_release_summary_label) return;

    const char *state;
    if (!ctx->frame_release_valid || !ctx->frame_release_active) {
        state = "Inactive";
    } else if (ctx->frame_release_paused) {
        state = "Paused";
    } else {
        state = "Running";
    }

    GString *s = g_string_new(NULL);
    g_string_append_printf(s,
        "Release: %s | gap=%.0fµs | chunks=%" G_GUINT64_FORMAT
        " | overlap=%" G_GUINT64_FORMAT " (%.1f%%) | avg pkts/chunk=%.2f"
        " | avg frames/chunk=%.2f",
        state,
        ctx->frame_release_gap_us,
        ctx->frame_release_total_chunks,
        ctx->frame_release_overlap_chunks,
        ctx->frame_release_overlap_rate * 100.0,
        ctx->frame_release_avg_pkts,
        ctx->frame_release_avg_frames);

    gtk_label_set_text(ctx->frame_release_summary_label, s->str);
    g_string_free(s, TRUE);
}

/* Status color of a single frame, shared by the overview and detail panes.
 * Severity order (worst first): overlap (red) > late (amber) > on-time (green). */
static void frame_release_frame_color(const UvReleaseFrame *f, double period_ms,
                                      double *r, double *g, double *b) {
    if (!f) { *r = 0.20; *g = 0.74; *b = 0.30; return; }
    if (f->overlap) {
        *r = 0.86; *g = 0.16; *b = 0.18;                       /* cross-frame burst */
    } else if (period_ms > 0.0 && f->lateness_ms > 0.5 * period_ms) {
        *r = 0.96; *g = 0.55; *b = 0.18;                       /* late */
    } else {
        *r = 0.20; *g = 0.74; *b = 0.30;                       /* on time */
    }
}

/* Severity rank for downsampled worst-of-pixel coloring (higher = worse). */
static int frame_release_frame_severity(const UvReleaseFrame *f, double period_ms) {
    if (!f) return 0;
    if (f->overlap) return 2;
    if (period_ms > 0.0 && f->lateness_ms > 0.5 * period_ms) return 1;
    return 0;
}

/* Overview health strip: maps every cached frame across the full width and
 * colors each pixel by the WORST status of the frames that fall in it, so red
 * clusters survive heavy downsampling (len can exceed the pixel width). The
 * translucent selection rectangle marks the slice the detail pane shows. */
static void frame_release_overview_draw(GtkDrawingArea *area, cairo_t *cr,
                                        int width, int height, gpointer user_data) {
    (void)area;
    GuiContext *ctx = user_data;

    cairo_save(cr);
    cairo_set_source_rgb(cr, 0.10, 0.10, 0.12);
    cairo_rectangle(cr, 0, 0, width, height);
    cairo_fill(cr);
    cairo_restore(cr);

    if (!ctx || width <= 0 || height <= 0) return;

    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);

    GArray *frames = ctx->frame_release_frames;
    guint len = frames ? frames->len : 0;
    if (len == 0) {
        cairo_set_source_rgb(cr, 0.7, 0.7, 0.7);
        cairo_set_font_size(cr, 12.0);
        cairo_move_to(cr, 10.0, height / 2.0 + 4.0);
        cairo_show_text(cr, ctx->frame_release_active
                            ? "Waiting for frame cadence..."
                            : "Frame release capture disabled — click Enable Capture to begin.");
        return;
    }

    double period_ms = ctx->frame_release_period_ms;
    double w = (double)width;

    /* Accumulate the worst severity per column, then paint each column once. */
    for (int px = 0; px < width; px++) {
        guint i0 = (guint)((double)px / w * (double)len);
        guint i1 = (guint)((double)(px + 1) / w * (double)len);
        if (i1 <= i0) i1 = i0 + 1;
        if (i1 > len) i1 = len;

        int worst = -1;
        for (guint i = i0; i < i1; i++) {
            const UvReleaseFrame *f = &g_array_index(frames, UvReleaseFrame, i);
            int sev = frame_release_frame_severity(f, period_ms);
            if (sev > worst) worst = sev;
        }
        if (worst < 0) continue;

        double r, g, b;
        if (worst >= 2)      { r = 0.86; g = 0.16; b = 0.18; }
        else if (worst >= 1) { r = 0.96; g = 0.55; b = 0.18; }
        else                 { r = 0.20; g = 0.74; b = 0.30; }
        cairo_set_source_rgb(cr, r, g, b);
        cairo_rectangle(cr, (double)px, 0.0, 1.0, (double)height);
        cairo_fill(cr);

        /* U3: dark notch at the top of overlap columns so red clusters stay
         * distinguishable in grayscale. */
        if (worst >= 2) {
            cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
            cairo_rectangle(cr, (double)px, 0.0, 1.0, 2.0);
            cairo_fill(cr);
        }
    }

    /* Selection rectangle (translucent fill + bright outline). */
    guint sel_start = ctx->frame_release_sel_start;
    guint sel_count = ctx->frame_release_sel_count;
    if (sel_count > 0 && sel_start < len) {
        guint sel_end = sel_start + sel_count;
        if (sel_end > len) sel_end = len;
        double sx0 = (double)sel_start / (double)len * w;
        double sx1 = (double)sel_end / (double)len * w;
        if (sx1 - sx0 < 2.0) sx1 = sx0 + 2.0;

        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.18);
        cairo_rectangle(cr, sx0, 0.0, sx1 - sx0, (double)height);
        cairo_fill(cr);

        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.85);
        cairo_set_line_width(cr, 1.0);
        cairo_rectangle(cr, sx0 + 0.5, 0.5, (sx1 - sx0) - 1.0, (double)height - 1.0);
        cairo_stroke(cr);
    }
}

/* Detail (Gantt-style) view of the SELECTED slice over wall-clock time.
 * X axis is time, newest at the right. Faint vertical gridlines mark the
 * expected frame cadence (frame_period_ms), phase-aligned to the newest
 * frame, so each cell is one "metronome tick". Each frame is a single
 * horizontal bar on one lane, from its first packet to its marker packet;
 * the silence between bars is the inter-frame gap.
 *
 * Reading it: healthy = short green bars hugging the left edge of each
 * gridline cell with silence between them. Problems = bars drifting right
 * within a cell (late), bars spanning a gridline (frame straddles a tick),
 * or red bars (this frame's first packet shared a release burst with the
 * previous frame => cross-frame burst => drop risk). */
static void frame_release_timeline_draw(GtkDrawingArea *area, cairo_t *cr,
                                        int width, int height, gpointer user_data) {
    (void)area;
    GuiContext *ctx = user_data;

    cairo_save(cr);
    cairo_set_source_rgb(cr, 0.10, 0.10, 0.12);
    cairo_rectangle(cr, 0, 0, width, height);
    cairo_fill(cr);
    cairo_restore(cr);

    if (!ctx || width <= 0 || height <= 0) return;

    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);

    GArray *frames = ctx->frame_release_frames;
    guint len = frames ? frames->len : 0;
    guint sel_start = ctx->frame_release_sel_start;
    guint sel_count = ctx->frame_release_sel_count;
    if (sel_start >= len) sel_count = 0;
    else if (sel_start + sel_count > len) sel_count = len - sel_start;

    if (len == 0 || sel_count == 0) {
        cairo_set_source_rgb(cr, 0.7, 0.7, 0.7);
        cairo_set_font_size(cr, 12.0);
        cairo_move_to(cr, 10.0, height / 2.0);
        cairo_show_text(cr, ctx->frame_release_active
                            ? "Waiting for frame cadence..."
                            : "Frame release capture disabled — click Enable Capture to begin.");
        return;
    }

    double period_ms = ctx->frame_release_period_ms;

    const UvReleaseFrame *first = &g_array_index(frames, UvReleaseFrame, sel_start);
    const UvReleaseFrame *last = &g_array_index(frames, UvReleaseFrame, sel_start + sel_count - 1);
    gint64 t_start = first->first_us;
    gint64 t_end = last->marker_us;
    double span = (double)(t_end - t_start);
    if (span <= 0.0) span = 1.0;

    /* U2: only draw inline labels when each frame's slot is wide enough for
     * text; otherwise rely on the hover readout (avoids overprinted mush). */
    double slot_w = (double)width / (double)sel_count;
    gboolean labelled = (slot_w >= 48.0);

    /* Faint cadence gridlines, walking left from the newest frame. */
    if (period_ms > 0.0) {
        double period_us = period_ms * 1000.0;
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.08);
        cairo_set_line_width(cr, 1.0);
        for (gint64 t = t_end; t >= t_start; t -= (gint64)period_us) {
            double gx = (double)(t - t_start) / span * (double)width;
            cairo_move_to(cr, gx, 0.0);
            cairo_line_to(cr, gx, (double)height);
            cairo_stroke(cr);
        }
    }

    /* Single lane, centered vertically. */
    const double bar_h = 18.0;
    double y = ((double)height - bar_h) / 2.0;

    for (guint i = sel_start; i < sel_start + sel_count; i++) {
        const UvReleaseFrame *f = &g_array_index(frames, UvReleaseFrame, i);

        double x0 = (double)(f->first_us - t_start) / span * (double)width;
        double x1 = (double)(f->marker_us - t_start) / span * (double)width;
        if (x0 < 0.0) x0 = 0.0;        /* clamp left edge into view */
        if (x1 > (double)width) x1 = (double)width;
        double bw = x1 - x0;
        if (bw < 2.0) bw = 2.0;        /* keep tight single-burst frames visible */

        double r, g, b;
        frame_release_frame_color(f, period_ms, &r, &g, &b);
        cairo_set_source_rgb(cr, r, g, b);
        cairo_rectangle(cr, x0, y, bw, bar_h);
        cairo_fill(cr);

        /* U3: non-color severity cue (redundant with red/amber/green). */
        int sev = frame_release_frame_severity(f, period_ms);
        double cx = x0 + bw * 0.5;
        if (sev >= 2) {
            /* OVERLAP: diagonal hatch across the bar + downward caret above. */
            cairo_save(cr);
            cairo_rectangle(cr, x0, y, bw, bar_h);
            cairo_clip(cr);
            cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.55);
            cairo_set_line_width(cr, 1.0);
            for (double hx = x0 - bar_h; hx < x1 + bar_h; hx += 5.0) {
                cairo_move_to(cr, hx, y + bar_h);
                cairo_line_to(cr, hx + bar_h, y);
                cairo_stroke(cr);
            }
            cairo_restore(cr);

            cairo_set_source_rgb(cr, 0.92, 0.92, 0.95);
            cairo_move_to(cr, cx - 4.0, y - 6.0);
            cairo_line_to(cr, cx + 4.0, y - 6.0);
            cairo_line_to(cr, cx, y - 1.0);
            cairo_close_path(cr);
            cairo_fill(cr);
        } else if (sev >= 1) {
            /* LATE: small hollow dot above the bar. */
            cairo_set_source_rgb(cr, 0.92, 0.92, 0.95);
            cairo_set_line_width(cr, 1.0);
            cairo_arc(cr, cx, y - 4.0, 2.0, 0.0, 2.0 * G_PI);
            cairo_stroke(cr);
        }

        if (labelled) {
            /* Thin marker tick at the frame's marker time. */
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.55);
            cairo_set_line_width(cr, 1.0);
            cairo_move_to(cr, x1, y - 4.0);
            cairo_line_to(cr, x1, y + bar_h + 4.0);
            cairo_stroke(cr);

            char lbl[48];
            g_snprintf(lbl, sizeof(lbl), "%.1fms %up", f->lateness_ms, f->pkts);
            cairo_set_source_rgb(cr, 0.82, 0.82, 0.85);
            cairo_set_font_size(cr, 9.0);
            cairo_move_to(cr, x0, y + bar_h + 14.0);
            cairo_show_text(cr, lbl);
        }
    }

    /* Baseline axis along the bottom. */
    cairo_set_source_rgb(cr, 0.45, 0.45, 0.48);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, 0.0, (double)height - 0.5);
    cairo_line_to(cr, (double)width, (double)height - 0.5);
    cairo_stroke(cr);

    /* Left/right time ticks span the selection: left = -<span>s, right = newest. */
    if (width > 120) {
        cairo_set_source_rgb(cr, 0.70, 0.70, 0.73);
        cairo_set_font_size(cr, 10.0);
        cairo_text_extents_t ext;
        const char *now_lbl = "newest";
        cairo_text_extents(cr, now_lbl, &ext);
        cairo_move_to(cr, (double)width - ext.width - 4.0, (double)height - 4.0);
        cairo_show_text(cr, now_lbl);

        char past_lbl[24];
        g_snprintf(past_lbl, sizeof(past_lbl), "-%.1fs", span / 1.0e6);
        cairo_move_to(cr, 4.0, (double)height - 4.0);
        cairo_show_text(cr, past_lbl);
    }

    /* Legend (U3 colorblind cue + U6 backward-only overlap note). */
    cairo_set_source_rgb(cr, 0.80, 0.80, 0.83);
    cairo_set_font_size(cr, 10.0);
    cairo_move_to(cr, 6.0, 12.0);
    cairo_show_text(cr,
        "green=on-time \xc2\xb7 amber(\xe2\x80\xa2)=late \xc2\xb7 red(hatch)=cross-frame burst");
    cairo_move_to(cr, 6.0, 24.0);
    cairo_show_text(cr, "a cross-frame burst is marked on the later frame.");

    /* U2: hover crosshair + readout box. */
    if (ctx->frame_release_hover_idx >= 0 &&
        (guint)ctx->frame_release_hover_idx >= sel_start &&
        (guint)ctx->frame_release_hover_idx < sel_start + sel_count) {
        guint hidx = (guint)ctx->frame_release_hover_idx;
        const UvReleaseFrame *hf = &g_array_index(frames, UvReleaseFrame, hidx);
        double hx = ctx->frame_release_hover_px;
        if (hx < 0.0) hx = 0.0;
        if (hx > (double)width) hx = (double)width;

        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.30);
        cairo_set_line_width(cr, 1.0);
        cairo_move_to(cr, hx, 0.0);
        cairo_line_to(cr, hx, (double)height);
        cairo_stroke(cr);

        char readout[96];
        g_snprintf(readout, sizeof(readout),
                   "#%u  late %.1fms  %up  %u burst(s)  %s",
                   hidx - sel_start, hf->lateness_ms, hf->pkts, hf->chunks,
                   hf->overlap ? "OVERLAP" : "ok");

        cairo_set_font_size(cr, 11.0);
        cairo_text_extents_t rext;
        cairo_text_extents(cr, readout, &rext);
        double pad = 5.0;
        double box_w = rext.width + pad * 2.0;
        double box_h = rext.height + pad * 2.0;
        double box_x = hx + 8.0;
        double box_y = 30.0;
        if (box_x + box_w > (double)width) box_x = (double)width - box_w - 2.0;
        if (box_x < 2.0) box_x = 2.0;
        if (box_y + box_h > (double)height) box_y = (double)height - box_h - 2.0;

        cairo_set_source_rgba(cr, 0.05, 0.05, 0.07, 0.92);
        cairo_rectangle(cr, box_x, box_y, box_w, box_h);
        cairo_fill(cr);
        cairo_set_source_rgb(cr, 0.92, 0.92, 0.95);
        cairo_move_to(cr, box_x + pad - rext.x_bearing,
                      box_y + pad - rext.y_bearing);
        cairo_show_text(cr, readout);
    }
}

static void frame_release_hist_draw(GtkDrawingArea *area, cairo_t *cr,
                                    int width, int height, gpointer user_data) {
    (void)area;
    GuiContext *ctx = user_data;

    cairo_save(cr);
    cairo_set_source_rgb(cr, 0.10, 0.10, 0.12);
    cairo_rectangle(cr, 0, 0, width, height);
    cairo_fill(cr);
    cairo_restore(cr);

    if (!ctx || width <= 0 || height <= 0) return;

    const char *labels[UV_RELEASE_FRAMES_BUCKETS] = {"1", "2", "3", "4+ frames"};
    guint max_val = 1;
    for (guint i = 0; i < UV_RELEASE_FRAMES_BUCKETS; i++) {
        if (ctx->frame_release_hist[i] > max_val) max_val = ctx->frame_release_hist[i];
    }

    double pad = 8.0;
    double label_h = 16.0;
    double plot_h = (double)height - label_h - pad;
    if (plot_h < 1.0) plot_h = 1.0;
    double slot_w = ((double)width - pad * 2.0) / (double)UV_RELEASE_FRAMES_BUCKETS;
    double bar_w = slot_w * 0.6;

    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 11.0);

    for (guint i = 0; i < UV_RELEASE_FRAMES_BUCKETS; i++) {
        double v = (double)ctx->frame_release_hist[i] / (double)max_val;
        double bh = v * plot_h;
        double x = pad + (double)i * slot_w + (slot_w - bar_w) / 2.0;
        double y = pad + (plot_h - bh);

        if (i == 0) {
            cairo_set_source_rgb(cr, 0.20, 0.74, 0.30); /* greenish: clean */
        } else {
            cairo_set_source_rgb(cr, 0.82, 0.22, 0.22); /* reddish: straddles */
        }
        cairo_rectangle(cr, x, y, bar_w, bh);
        cairo_fill(cr);

        char num[32];
        g_snprintf(num, sizeof(num), "%u", ctx->frame_release_hist[i]);
        cairo_text_extents_t ext;
        cairo_set_source_rgb(cr, 0.85, 0.85, 0.88);
        cairo_text_extents(cr, num, &ext);
        cairo_move_to(cr, x + (bar_w - ext.width) / 2.0, y - 3.0);
        cairo_show_text(cr, num);

        cairo_set_source_rgb(cr, 0.75, 0.75, 0.78);
        cairo_text_extents(cr, labels[i], &ext);
        cairo_move_to(cr, pad + (double)i * slot_w + (slot_w - ext.width) / 2.0,
                      (double)height - 4.0);
        cairo_show_text(cr, labels[i]);
    }
}

static void refresh_frame_release(GuiContext *ctx, const UvViewerStats *stats) {
    if (!ctx) return;

    if (stats && stats->frame_release_valid) {
        const UvReleaseStats *fr = &stats->frame_release;
        ctx->frame_release_valid = TRUE;
        ctx->frame_release_active = fr->active;
        ctx->frame_release_paused = fr->paused;
        ctx->frame_release_gap_us = fr->gap_us;
        ctx->frame_release_total_chunks = fr->total_chunks;
        ctx->frame_release_overlap_chunks = fr->overlap_chunks;
        ctx->frame_release_overlap_rate = fr->overlap_rate;
        ctx->frame_release_avg_pkts = fr->avg_pkts_per_chunk;
        ctx->frame_release_avg_frames = fr->avg_frames_per_chunk;
        ctx->frame_release_period_ms = fr->frame_period_ms;
        memcpy(ctx->frame_release_hist, fr->hist_frames, sizeof(ctx->frame_release_hist));

        if (!ctx->frame_release_chunks) {
            ctx->frame_release_chunks = g_array_new(FALSE, TRUE, sizeof(UvReleaseChunk));
        }
        guint n = fr->chunks ? fr->chunks->len : 0;
        g_array_set_size(ctx->frame_release_chunks, n);
        if (n > 0) {
            memcpy(ctx->frame_release_chunks->data, fr->chunks->data,
                   sizeof(UvReleaseChunk) * n);
        }

        if (!ctx->frame_release_frames) {
            ctx->frame_release_frames = g_array_new(FALSE, TRUE, sizeof(UvReleaseFrame));
        }
        guint nf = fr->frames ? fr->frames->len : 0;
        g_array_set_size(ctx->frame_release_frames, nf);
        if (nf > 0) {
            memcpy(ctx->frame_release_frames->data, fr->frames->data,
                   sizeof(UvReleaseFrame) * nf);
        }

        /* Auto-calibration result: apply once on a calib_seq change. While no
         * request is outstanding, track the latest seq so a stale increment
         * (e.g. a result that landed while the page was hidden) never fires. */
        if (ctx->frame_release_calib_pending && fr->calib_seq != ctx->frame_release_calib_seq) {
            ctx->frame_release_calib_seq = fr->calib_seq;
            ctx->frame_release_calib_pending = FALSE;
            if (ctx->frame_release_calib_button) {
                gtk_button_set_label(ctx->frame_release_calib_button, "Auto");
                gtk_widget_set_sensitive(GTK_WIDGET(ctx->frame_release_calib_button), TRUE);
            }
            if (fr->calib_confident) {
                /* Setting the spin value fires on_frame_release_gap_changed,
                 * which forwards the new gap to the relay. */
                if (ctx->frame_release_gap_spin) {
                    gtk_spin_button_set_value(ctx->frame_release_gap_spin, fr->calib_gap_us);
                }
                if (ctx->frame_release_calib_label) {
                    char b[64];
                    g_snprintf(b, sizeof(b), "auto → %.0f µs", fr->calib_gap_us);
                    gtk_label_set_text(ctx->frame_release_calib_label, b);
                }
            } else if (ctx->frame_release_calib_label) {
                gtk_label_set_text(ctx->frame_release_calib_label,
                                   "auto: no clear burst pattern — gap kept");
            }
        } else if (!ctx->frame_release_calib_pending) {
            ctx->frame_release_calib_seq = fr->calib_seq;
        }
    } else {
        ctx->frame_release_valid = FALSE;
        ctx->frame_release_active = FALSE;
        ctx->frame_release_paused = FALSE;
        ctx->frame_release_total_chunks = 0;
        ctx->frame_release_overlap_chunks = 0;
        ctx->frame_release_overlap_rate = 0.0;
        ctx->frame_release_avg_pkts = 0.0;
        ctx->frame_release_avg_frames = 0.0;
        ctx->frame_release_period_ms = 0.0;
        memset(ctx->frame_release_hist, 0, sizeof(ctx->frame_release_hist));
        if (ctx->frame_release_chunks) {
            g_array_set_size(ctx->frame_release_chunks, 0);
        }
        if (ctx->frame_release_frames) {
            g_array_set_size(ctx->frame_release_frames, 0);
        }
    }

    /* Maintain the detail selection over the freshly cached frame array. */
    guint n = ctx->frame_release_frames ? ctx->frame_release_frames->len : 0;
    if (n == 0) {
        ctx->frame_release_sel_start = 0;
        ctx->frame_release_sel_count = 0;
    } else if (ctx->frame_release_follow) {
        double period_ms = ctx->frame_release_period_ms;
        guint window_s = ctx->frame_release_window_s ? ctx->frame_release_window_s : 2u;
        guint want;
        if (period_ms > 0.0) {
            want = (guint)((double)window_s * 1000.0 / period_ms + 0.5);
        } else {
            want = 120u;  /* sane default when cadence not yet known */
        }
        if (want < 4u) want = 4u;
        if (want > n) want = n;
        ctx->frame_release_sel_count = want;
        ctx->frame_release_sel_start = n - want;
    } else {
        /* Frozen selection: clamp into the valid range. NOTE: while not
         * following AND live, new frames keep arriving, so a frozen window
         * drifts relative to the newest frame — pausing capture is the
         * intended inspect workflow. */
        /* U4: clamp start into [0,n) first, then count into
         * [4, n-start] (or count==n when n < 4), so we never force count
         * below 4 only to immediately violate the range. */
        if (ctx->frame_release_sel_start >= n) ctx->frame_release_sel_start = n - 1;
        guint avail = n - ctx->frame_release_sel_start;
        guint min_count = (n < 4u) ? n : 4u;
        if (ctx->frame_release_sel_count < min_count) ctx->frame_release_sel_count = min_count;
        if (ctx->frame_release_sel_count > avail) ctx->frame_release_sel_count = avail;
    }

    frame_release_update_summary(ctx);
    if (ctx->frame_release_overview_area) {
        gtk_widget_queue_draw(GTK_WIDGET(ctx->frame_release_overview_area));
    }
    if (ctx->frame_release_timeline_area) {
        gtk_widget_queue_draw(GTK_WIDGET(ctx->frame_release_timeline_area));
    }
    if (ctx->frame_release_hist_area) {
        gtk_widget_queue_draw(GTK_WIDGET(ctx->frame_release_hist_area));
    }
}

static void on_frame_release_enable_toggled(GtkToggleButton *button, gpointer user_data) {
    GuiContext *ctx = user_data;
    if (!ctx || !GTK_IS_TOGGLE_BUTTON(button)) return;
    gboolean enabled = gtk_toggle_button_get_active(button);
    if (ctx->viewer) {
        uv_viewer_frame_release_configure(ctx->viewer, enabled);
    }
    if (ctx->frame_release_pause_toggle) {
        gtk_widget_set_sensitive(GTK_WIDGET(ctx->frame_release_pause_toggle), enabled);
    }
    if (ctx->frame_release_reset_button) {
        gtk_widget_set_sensitive(GTK_WIDGET(ctx->frame_release_reset_button), enabled);
    }
    if (ctx->frame_release_calib_button) {
        /* Sampling only runs while the feature is on (release_process_packet
         * is gated on it), so disabling the feature cancels any pending pass. */
        gtk_widget_set_sensitive(GTK_WIDGET(ctx->frame_release_calib_button), enabled);
        if (!enabled) {
            ctx->frame_release_calib_pending = FALSE;
            gtk_button_set_label(ctx->frame_release_calib_button, "Auto");
        }
    }
}

static void on_frame_release_pause_toggled(GtkToggleButton *button, gpointer user_data) {
    GuiContext *ctx = user_data;
    if (!ctx || !GTK_IS_TOGGLE_BUTTON(button)) return;
    if (ctx->viewer) {
        uv_viewer_frame_release_pause(ctx->viewer, gtk_toggle_button_get_active(button));
    }
}

static void on_frame_release_reset_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    GuiContext *ctx = user_data;
    if (!ctx) return;
    if (ctx->viewer) {
        uv_viewer_frame_release_reset(ctx->viewer);
    }
    if (ctx->frame_release_chunks) {
        g_array_set_size(ctx->frame_release_chunks, 0);
    }
    if (ctx->frame_release_frames) {
        g_array_set_size(ctx->frame_release_frames, 0);
    }
    ctx->frame_release_total_chunks = 0;
    ctx->frame_release_overlap_chunks = 0;
    ctx->frame_release_overlap_rate = 0.0;
    ctx->frame_release_avg_pkts = 0.0;
    ctx->frame_release_avg_frames = 0.0;
    ctx->frame_release_period_ms = 0.0;
    memset(ctx->frame_release_hist, 0, sizeof(ctx->frame_release_hist));
    ctx->frame_release_sel_start = 0;
    ctx->frame_release_sel_count = 0;
    frame_release_update_summary(ctx);
    if (ctx->frame_release_overview_area) gtk_widget_queue_draw(GTK_WIDGET(ctx->frame_release_overview_area));
    if (ctx->frame_release_timeline_area) gtk_widget_queue_draw(GTK_WIDGET(ctx->frame_release_timeline_area));
    if (ctx->frame_release_hist_area) gtk_widget_queue_draw(GTK_WIDGET(ctx->frame_release_hist_area));
}

static void on_frame_release_gap_changed(GtkSpinButton *spin, gpointer user_data) {
    GuiContext *ctx = user_data;
    if (!ctx || !GTK_IS_SPIN_BUTTON(spin)) return;
    double gap_us = gtk_spin_button_get_value(spin);
    ctx->frame_release_gap_us = gap_us;
    if (ctx->viewer) {
        uv_viewer_frame_release_set_gap_us(ctx->viewer, gap_us);
    }
}

static void on_frame_release_calib_clicked(GtkButton *button, gpointer user_data) {
    GuiContext *ctx = user_data;
    if (!ctx || !ctx->viewer) return;
    /* One-shot: kick the relay-side sampler and wait for the result via the
     * snapshot poll. The baseline seq is already tracked while idle, so any
     * bump from here on is our result. */
    ctx->frame_release_calib_pending = TRUE;
    uv_viewer_frame_release_calibrate(ctx->viewer);
    if (button) {
        gtk_button_set_label(button, "Calibrating…");
        gtk_widget_set_sensitive(GTK_WIDGET(button), FALSE);
    }
    if (ctx->frame_release_calib_label) {
        gtk_label_set_text(ctx->frame_release_calib_label, "auto: sampling…");
    }
}

static void on_frame_release_window_changed(GObject *dropdown, GParamSpec *pspec, gpointer user_data) {
    (void)pspec;
    GuiContext *ctx = user_data;
    if (!ctx || !GTK_IS_DROP_DOWN(dropdown)) return;
    /* Window selector is purely a display knob: no core call, just redraw. */
    static const guint window_seconds[] = {1u, 2u, 5u, 10u};
    guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(dropdown));
    if (sel >= G_N_ELEMENTS(window_seconds)) sel = 1u;
    ctx->frame_release_window_s = window_seconds[sel];
    /* Window now means "follow window length": the next refresh recomputes the
     * selection while following. Just queue redraws here. */
    if (ctx->frame_release_overview_area) {
        gtk_widget_queue_draw(GTK_WIDGET(ctx->frame_release_overview_area));
    }
    if (ctx->frame_release_timeline_area) {
        gtk_widget_queue_draw(GTK_WIDGET(ctx->frame_release_timeline_area));
    }
}

static void on_frame_release_follow_toggled(GtkToggleButton *button, gpointer user_data) {
    GuiContext *ctx = user_data;
    if (!ctx || !GTK_IS_TOGGLE_BUTTON(button)) return;
    ctx->frame_release_follow = gtk_toggle_button_get_active(button);
    /* The next refresh applies the new mode; redraw immediately for feedback. */
    if (ctx->frame_release_overview_area) {
        gtk_widget_queue_draw(GTK_WIDGET(ctx->frame_release_overview_area));
    }
    if (ctx->frame_release_timeline_area) {
        gtk_widget_queue_draw(GTK_WIDGET(ctx->frame_release_timeline_area));
    }
}

/* U1: freeze the live buffer when the user starts inspecting it, so the slice
 * under the cursor does not drift. Only engages Pause when capture is running
 * and not already paused, and only if the toggle exists and is sensitive. */
static void frame_release_auto_pause(GuiContext *ctx) {
    if (!ctx) return;
    if (!ctx->frame_release_active || ctx->frame_release_paused) return;
    if (!ctx->frame_release_pause_toggle) return;
    GtkWidget *w = GTK_WIDGET(ctx->frame_release_pause_toggle);
    if (!gtk_widget_get_sensitive(w)) return;
    gtk_toggle_button_set_active(ctx->frame_release_pause_toggle, TRUE);
}

/* Translate an overview x-pixel range into a frame-index selection. */
static void frame_release_set_selection_from_px(GuiContext *ctx, double px_lo,
                                                double px_hi, int width) {
    if (!ctx || width <= 0) return;
    guint len = ctx->frame_release_frames ? ctx->frame_release_frames->len : 0;
    if (len == 0) return;

    if (px_lo > px_hi) { double t = px_lo; px_lo = px_hi; px_hi = t; }
    double w = (double)width;
    if (px_lo < 0.0) px_lo = 0.0;
    if (px_hi > w) px_hi = w;

    const guint min_count = 4u;
    guint i_lo = (guint)(px_lo / w * (double)len);
    guint i_hi = (guint)(px_hi / w * (double)len);
    if (i_lo >= len) i_lo = len - 1;
    if (i_hi > len) i_hi = len;

    guint count = (i_hi > i_lo) ? (i_hi - i_lo) : 0;
    if (px_hi - px_lo < 3.0 || count < min_count) {
        /* Treat as a click: a default-width window centered on the point. */
        guint def = (len < 60u) ? len : 60u;
        guint center = (guint)((px_lo + px_hi) * 0.5 / w * (double)len);
        if (center >= len) center = len - 1;
        guint half = def / 2u;
        i_lo = (center > half) ? (center - half) : 0u;
        count = def;
        if (i_lo + count > len) {
            i_lo = (len > count) ? (len - count) : 0u;
            count = len - i_lo;
        }
    }
    if (count < min_count) count = (len < min_count) ? len : min_count;
    if (i_lo + count > len) i_lo = len - count;

    ctx->frame_release_sel_start = i_lo;
    ctx->frame_release_sel_count = count;
    ctx->frame_release_follow = FALSE;
    if (ctx->frame_release_follow_toggle) {
        gtk_toggle_button_set_active(ctx->frame_release_follow_toggle, FALSE);
    }
    if (ctx->frame_release_overview_area) {
        gtk_widget_queue_draw(GTK_WIDGET(ctx->frame_release_overview_area));
    }
    if (ctx->frame_release_timeline_area) {
        gtk_widget_queue_draw(GTK_WIDGET(ctx->frame_release_timeline_area));
    }
}

static void on_frame_release_overview_drag_begin(GtkGestureDrag *gesture,
                                                 double start_x, double start_y,
                                                 gpointer user_data) {
    (void)start_y;
    GuiContext *ctx = user_data;
    if (!ctx || !ctx->frame_release_overview_area) return;
    frame_release_auto_pause(ctx);  /* U1: freeze before the user inspects */
    int width = gtk_widget_get_width(GTK_WIDGET(ctx->frame_release_overview_area));
    frame_release_set_selection_from_px(ctx, start_x, start_x, width);
    (void)gesture;
}

static void on_frame_release_overview_drag_update(GtkGestureDrag *gesture,
                                                  double offset_x, double offset_y,
                                                  gpointer user_data) {
    (void)offset_y;
    GuiContext *ctx = user_data;
    if (!ctx || !ctx->frame_release_overview_area) return;
    double start_x = 0.0, start_y = 0.0;
    gtk_gesture_drag_get_start_point(gesture, &start_x, &start_y);
    int width = gtk_widget_get_width(GTK_WIDGET(ctx->frame_release_overview_area));
    frame_release_set_selection_from_px(ctx, start_x, start_x + offset_x, width);
}

/* U2: map a detail-pane cursor x to the absolute frame index within the
 * current selection. Returns -1 if there is no usable selection. Picks the
 * selection frame whose [first_us,marker_us] contains the cursor time, else
 * the nearest by marker_us. */
static gint frame_release_hover_index_at(GuiContext *ctx, double px, int width) {
    if (!ctx || width <= 0) return -1;
    GArray *frames = ctx->frame_release_frames;
    guint len = frames ? frames->len : 0;
    guint sel_start = ctx->frame_release_sel_start;
    guint sel_count = ctx->frame_release_sel_count;
    if (sel_start >= len) return -1;
    if (sel_start + sel_count > len) sel_count = len - sel_start;
    if (sel_count == 0) return -1;

    const UvReleaseFrame *first = &g_array_index(frames, UvReleaseFrame, sel_start);
    const UvReleaseFrame *last = &g_array_index(frames, UvReleaseFrame, sel_start + sel_count - 1);
    gint64 t_start = first->first_us;
    gint64 t_end = last->marker_us;
    double span = (double)(t_end - t_start);
    if (span <= 0.0) span = 1.0;

    if (px < 0.0) px = 0.0;
    if (px > (double)width) px = (double)width;
    gint64 t = t_start + (gint64)(px / (double)width * span);

    gint best = -1;
    gint64 best_dist = G_MAXINT64;
    for (guint i = sel_start; i < sel_start + sel_count; i++) {
        const UvReleaseFrame *f = &g_array_index(frames, UvReleaseFrame, i);
        if (t >= f->first_us && t <= f->marker_us) return (gint)i;
        gint64 d = (t < f->marker_us) ? (f->marker_us - t) : (t - f->marker_us);
        if (d < best_dist) { best_dist = d; best = (gint)i; }
    }
    return best;
}

static void on_frame_release_timeline_motion(GtkEventControllerMotion *motion,
                                             double x, double y, gpointer user_data) {
    (void)motion;
    (void)y;
    GuiContext *ctx = user_data;
    if (!ctx || !ctx->frame_release_timeline_area) return;
    int width = gtk_widget_get_width(GTK_WIDGET(ctx->frame_release_timeline_area));
    ctx->frame_release_hover_idx = frame_release_hover_index_at(ctx, x, width);
    ctx->frame_release_hover_px = x;
    gtk_widget_queue_draw(GTK_WIDGET(ctx->frame_release_timeline_area));
}

static void on_frame_release_timeline_leave(GtkEventControllerMotion *motion,
                                            gpointer user_data) {
    (void)motion;
    GuiContext *ctx = user_data;
    if (!ctx) return;
    ctx->frame_release_hover_idx = -1;
    if (ctx->frame_release_timeline_area) {
        gtk_widget_queue_draw(GTK_WIDGET(ctx->frame_release_timeline_area));
    }
}

/* U4: scroll-to-zoom on the detail pane. Up = zoom in, down = zoom out,
 * keeping the frame under the cursor roughly fixed. */
static gboolean on_frame_release_timeline_scroll(GtkEventControllerScroll *scroll,
                                                 double dx, double dy,
                                                 gpointer user_data) {
    (void)scroll;
    (void)dx;
    GuiContext *ctx = user_data;
    if (!ctx || !ctx->frame_release_timeline_area || dy == 0.0) return FALSE;
    GArray *frames = ctx->frame_release_frames;
    guint len = frames ? frames->len : 0;
    if (len == 0) return FALSE;

    int width = gtk_widget_get_width(GTK_WIDGET(ctx->frame_release_timeline_area));
    if (width <= 0) return FALSE;

    guint sel_start = ctx->frame_release_sel_start;
    guint sel_count = ctx->frame_release_sel_count;
    if (sel_start >= len) sel_start = 0;
    if (sel_count == 0 || sel_start + sel_count > len) {
        sel_count = (len < 4u) ? len : 4u;
        if (sel_start + sel_count > len) sel_start = len - sel_count;
    }

    /* Frame currently under the cursor (anchor for the zoom). */
    gint anchor = frame_release_hover_index_at(ctx, ctx->frame_release_hover_px, width);
    if (anchor < 0) anchor = (gint)(sel_start + sel_count / 2u);
    double frac = (double)((guint)anchor - sel_start) / (double)sel_count;

    double new_count_d = (dy < 0.0) ? ((double)sel_count / 1.3) : ((double)sel_count * 1.3);
    guint new_count = (guint)(new_count_d + 0.5);
    if (new_count < 4u) new_count = 4u;
    if (new_count > len) new_count = len;

    /* Keep the anchor frame at the same fractional position. */
    double new_start_d = (double)anchor - frac * (double)new_count;
    gint new_start = (gint)(new_start_d + 0.5);
    if (new_start < 0) new_start = 0;
    if ((guint)new_start + new_count > len) new_start = (gint)(len - new_count);
    if (new_start < 0) new_start = 0;

    ctx->frame_release_sel_start = (guint)new_start;
    ctx->frame_release_sel_count = new_count;
    ctx->frame_release_follow = FALSE;
    if (ctx->frame_release_follow_toggle) {
        gtk_toggle_button_set_active(ctx->frame_release_follow_toggle, FALSE);
    }
    frame_release_auto_pause(ctx);
    if (ctx->frame_release_overview_area) {
        gtk_widget_queue_draw(GTK_WIDGET(ctx->frame_release_overview_area));
    }
    if (ctx->frame_release_timeline_area) {
        gtk_widget_queue_draw(GTK_WIDGET(ctx->frame_release_timeline_area));
    }
    return TRUE;
}

/* U4: horizontal drag-to-pan on the detail pane. */
static void on_frame_release_timeline_pan_begin(GtkGestureDrag *gesture,
                                                double start_x, double start_y,
                                                gpointer user_data) {
    (void)gesture;
    (void)start_x;
    (void)start_y;
    GuiContext *ctx = user_data;
    if (!ctx) return;
    ctx->frame_release_pan_anchor_start = ctx->frame_release_sel_start;
    frame_release_auto_pause(ctx);
}

static void on_frame_release_timeline_pan_update(GtkGestureDrag *gesture,
                                                 double offset_x, double offset_y,
                                                 gpointer user_data) {
    (void)gesture;
    (void)offset_y;
    GuiContext *ctx = user_data;
    if (!ctx || !ctx->frame_release_timeline_area) return;
    GArray *frames = ctx->frame_release_frames;
    guint len = frames ? frames->len : 0;
    if (len == 0) return;
    int width = gtk_widget_get_width(GTK_WIDGET(ctx->frame_release_timeline_area));
    if (width <= 0) return;

    guint sel_count = ctx->frame_release_sel_count;
    if (sel_count == 0 || sel_count > len) sel_count = (len < 4u) ? len : 4u;

    double dframes = -offset_x / (double)width * (double)sel_count;
    double new_start_d = (double)ctx->frame_release_pan_anchor_start + dframes;
    gint new_start = (gint)(new_start_d + (new_start_d >= 0.0 ? 0.5 : -0.5));
    if (new_start < 0) new_start = 0;
    if ((guint)new_start + sel_count > len) new_start = (gint)(len - sel_count);
    if (new_start < 0) new_start = 0;

    ctx->frame_release_sel_start = (guint)new_start;
    ctx->frame_release_sel_count = sel_count;
    ctx->frame_release_follow = FALSE;
    if (ctx->frame_release_follow_toggle) {
        gtk_toggle_button_set_active(ctx->frame_release_follow_toggle, FALSE);
    }
    if (ctx->frame_release_overview_area) {
        gtk_widget_queue_draw(GTK_WIDGET(ctx->frame_release_overview_area));
    }
    if (ctx->frame_release_timeline_area) {
        gtk_widget_queue_draw(GTK_WIDGET(ctx->frame_release_timeline_area));
    }
}

static void refresh_stats(GuiContext *ctx) {
    if (!ctx || !ctx->viewer) return;

    if (!ctx->paintable_bound) {
        ensure_video_paintable(ctx);
    }

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
    guint viewer_selected_index = GTK_INVALID_LIST_POSITION;
    UvSourceStats *viewer_selected_source = NULL;
    UvSourceStats *pending_source = NULL;

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
        ctx->pending_source_valid = FALSE;
        ctx->pending_source_index = GTK_INVALID_LIST_POSITION;
        ctx->active_source_valid = FALSE;
        ctx->active_source_index = GTK_INVALID_LIST_POSITION;
        if (ctx->source_detail_label) {
            gtk_label_set_text(ctx->source_detail_label, "No sources discovered yet.");
        }
        if (ctx->stream_detail_label) {
            gtk_label_set_text(ctx->stream_detail_label, "");
        }
        update_idr_button_sensitivity(ctx);
    } else {
        guint existing_items = 0;
        if (ctx->source_model) {
            existing_items = g_list_model_get_n_items(G_LIST_MODEL(ctx->source_model));
        }

        char **labels = NULL;
        if (ctx->source_model && source_count > 0) {
            labels = g_new0(char *, source_count + 1);
        }

        for (guint i = 0; i < source_count; i++) {
            UvSourceStats *src = &g_array_index(stats.sources, UvSourceStats, i);

            if (labels) {
                labels[i] = g_strdup_printf("%u: %s", i, src->address);
            }

            if (ctx->pending_source_valid && ctx->pending_source_index == i) {
                pending_source = src;
            }

            if (!viewer_selected_source && src->selected) {
                viewer_selected_index = i;
                viewer_selected_source = src;
            }
        }

        if (!viewer_selected_source) {
            for (guint i = 0; i < source_count; i++) {
                UvSourceStats *src = &g_array_index(stats.sources, UvSourceStats, i);
                if (src->selected) {
                    viewer_selected_index = i;
                    viewer_selected_source = src;
                    break;
                }
            }
        }

        if (labels) {
            labels[source_count] = NULL;
        }

        if (ctx->source_model) {
            gboolean list_changed = (existing_items != source_count);

            if (!list_changed) {
                for (guint i = 0; i < source_count; i++) {
                    GObject *item = g_list_model_get_item(G_LIST_MODEL(ctx->source_model), i);
                    const char *current = gtk_string_object_get_string(GTK_STRING_OBJECT(item));
                    if (g_strcmp0(current, labels[i]) != 0) {
                        list_changed = TRUE;
                    }
                    g_object_unref(item);
                    if (list_changed) {
                        break;
                    }
                }
            }

            if (list_changed) {
                gboolean old_suppress = ctx->suppress_source_change;
                ctx->suppress_source_change = TRUE;
                gtk_string_list_splice(ctx->source_model, 0, existing_items, (const char * const *)labels);
                ctx->suppress_source_change = old_suppress;
            }

            ctx->known_source_count = source_count;
        }

        if (ctx->source_dropdown) {
            gtk_widget_set_sensitive(GTK_WIDGET(ctx->source_dropdown), TRUE);
        }

        if (ctx->pending_source_valid) {
            if (ctx->pending_source_index >= source_count) {
                ctx->pending_source_valid = FALSE;
                ctx->pending_source_index = GTK_INVALID_LIST_POSITION;
            } else if (viewer_selected_index == ctx->pending_source_index) {
                ctx->pending_source_valid = FALSE;
            }
        }

        if (ctx->source_dropdown) {
            guint desired = viewer_selected_index;
            if (ctx->pending_source_valid) {
                desired = ctx->pending_source_index;
            }
            if (desired != GTK_INVALID_LIST_POSITION) {
                guint current = gtk_drop_down_get_selected(ctx->source_dropdown);
                if (current != desired) {
                    ctx->suppress_source_change = TRUE;
                    gtk_drop_down_set_selected(ctx->source_dropdown, desired);
                    ctx->suppress_source_change = FALSE;
                }
            }
        }

        if (ctx->source_detail_label) {
            gboolean waiting_for_switch = ctx->pending_source_valid;
            UvSourceStats *detail_source = viewer_selected_source;
            guint detail_index = viewer_selected_index;
            if (ctx->pending_source_valid) {
                if (pending_source) {
                    detail_source = pending_source;
                } else {
                    detail_source = NULL;
                }
                detail_index = ctx->pending_source_index;
            }

            if (detail_source) {
                char rate_buf[64];
                format_bitrate(detail_source->inbound_bitrate_bps, rate_buf, sizeof(rate_buf));
                char detail[512];
                char jitter_buf[32];
                if (detail_source->kind == UV_SOURCE_SHM)
                    g_strlcpy(jitter_buf, "—", sizeof(jitter_buf));
                else
                    g_snprintf(jitter_buf, sizeof(jitter_buf), "%.2fms",
                               detail_source->rfc3550_jitter_ms);
                g_snprintf(detail, sizeof(detail),
                           "%u: %s\nrx=%" G_GUINT64_FORMAT "/%" G_GUINT64_FORMAT
                           " fwd=%" G_GUINT64_FORMAT "/%" G_GUINT64_FORMAT
                           " rate=%s input_fps=%.2f jitter=%s last_seen=%.1fs",
                           detail_index,
                           detail_source->address,
                           detail_source->rx_packets,
                           detail_source->rx_bytes,
                           detail_source->forwarded_packets,
                           detail_source->forwarded_bytes,
                           rate_buf,
                           detail_source->rtp_marker_fps,
                           jitter_buf,
                           detail_source->seconds_since_last_seen >= 0.0 ? detail_source->seconds_since_last_seen : 0.0);
                if (detail_source->kind == UV_SOURCE_SHM) {
                    char ring[192];
                    g_snprintf(ring, sizeof(ring),
                               "\nring=%s fill=%.1f%% full=%" G_GUINT64_FORMAT
                               " oversize=%" G_GUINT64_FORMAT " bad=%" G_GUINT64_FORMAT
                               " reattaches=%" G_GUINT64_FORMAT,
                               detail_source->shm_attached ? "attached" : "stale",
                               detail_source->shm_fill_pct, detail_source->shm_full_drops,
                               detail_source->shm_oversize_drops, detail_source->shm_bad_slots,
                               detail_source->shm_reattaches);
                    g_strlcat(detail, ring, sizeof(detail));
                }
                if (waiting_for_switch) {
                    char switching[320];
                    g_snprintf(switching, sizeof(switching), "Switching to %s", detail);
                    gtk_label_set_text(ctx->source_detail_label, switching);
                } else {
                    gtk_label_set_text(ctx->source_detail_label, detail);
                }

                if (ctx->stream_detail_label) {
                    uint64_t kf_total = detail_source->hevc_idr_count + detail_source->hevc_cra_count;
                    uint64_t total_pkts = detail_source->kind == UV_SOURCE_SHM
                                        ? 0 : detail_source->rtp_unique_packets;
                    double frag_pct = (total_pkts > 0)
                                    ? (100.0 * (double)detail_source->rtp_fu_packets / (double)total_pkts)
                                    : 0.0;
                    char kf_age_buf[24];
                    if (detail_source->seconds_since_keyframe < 0.0) {
                        g_strlcpy(kf_age_buf, "n/a", sizeof(kf_age_buf));
                    } else if (detail_source->seconds_since_keyframe >= 60.0) {
                        g_snprintf(kf_age_buf, sizeof(kf_age_buf), "%.0fm%.0fs",
                                   floor(detail_source->seconds_since_keyframe / 60.0),
                                   fmod(detail_source->seconds_since_keyframe, 60.0));
                    } else {
                        g_snprintf(kf_age_buf, sizeof(kf_age_buf), "%.1fs",
                                   detail_source->seconds_since_keyframe);
                    }
                    char kf_gap_buf[24];
                    if (detail_source->last_keyframe_interval_seconds < 0.0) {
                        g_strlcpy(kf_gap_buf, "—", sizeof(kf_gap_buf));
                    } else {
                        g_snprintf(kf_gap_buf, sizeof(kf_gap_buf), "%.2fs",
                                   detail_source->last_keyframe_interval_seconds);
                    }
                    char stream_detail[384];
                    char ap_buf[32], fu_buf[32];
                    if (detail_source->kind == UV_SOURCE_SHM) {
                        g_strlcpy(ap_buf, "—", sizeof(ap_buf));
                        g_strlcpy(fu_buf, "—", sizeof(fu_buf));
                    } else {
                        g_snprintf(ap_buf, sizeof(ap_buf), "%" G_GUINT64_FORMAT,
                                   detail_source->rtp_ap_packets);
                        g_snprintf(fu_buf, sizeof(fu_buf), "%" G_GUINT64_FORMAT,
                                   detail_source->rtp_fu_packets);
                    }
                    g_snprintf(stream_detail, sizeof(stream_detail),
                               "HEVC  IDR=%" G_GUINT64_FORMAT "  CRA=%" G_GUINT64_FORMAT
                               "  trail=%" G_GUINT64_FORMAT "  VPS/SPS/PPS=%" G_GUINT64_FORMAT
                               "/%" G_GUINT64_FORMAT "/%" G_GUINT64_FORMAT
                               "  SEI=%" G_GUINT64_FORMAT "  AUD=%" G_GUINT64_FORMAT
                               "  •  AP=%s  FU=%s"
                               " (%.1f%% frag)"
                               "  •  KF total=%" G_GUINT64_FORMAT "  last %s ago  Δ %s",
                               detail_source->hevc_idr_count,
                               detail_source->hevc_cra_count,
                               detail_source->hevc_trail_count,
                               detail_source->hevc_vps_count,
                               detail_source->hevc_sps_count,
                               detail_source->hevc_pps_count,
                               detail_source->hevc_sei_count,
                               detail_source->hevc_aud_count,
                               ap_buf,
                               fu_buf,
                               frag_pct,
                               kf_total,
                               kf_age_buf,
                               kf_gap_buf);
                    gtk_label_set_text(ctx->stream_detail_label, stream_detail);
                }
                gboolean udp_controls = detail_source->kind != UV_SOURCE_SHM;
                if (ctx->restream_toggle)
                    gtk_widget_set_sensitive(GTK_WIDGET(ctx->restream_toggle), udp_controls);
                if (ctx->restream_host_entry)
                    gtk_widget_set_sensitive(GTK_WIDGET(ctx->restream_host_entry),
                                             udp_controls && !ctx->current_cfg.restream_enabled);
                if (ctx->restream_port_spin)
                    gtk_widget_set_sensitive(GTK_WIDGET(ctx->restream_port_spin),
                                             udp_controls && !ctx->current_cfg.restream_enabled);
                if (ctx->sidecar_enable_toggle)
                    gtk_widget_set_sensitive(GTK_WIDGET(ctx->sidecar_enable_toggle), udp_controls);
                if (ctx->sidecar_port_spin)
                    gtk_widget_set_sensitive(GTK_WIDGET(ctx->sidecar_port_spin),
                                             udp_controls && !ctx->current_cfg.sidecar_enabled);
            } else if (waiting_for_switch) {
                char message[128];
                g_snprintf(message, sizeof(message), "Switching to source %u...", detail_index);
                gtk_label_set_text(ctx->source_detail_label, message);
                if (ctx->stream_detail_label) gtk_label_set_text(ctx->stream_detail_label, "");
            } else {
                gtk_label_set_text(ctx->source_detail_label, "Select a source to view details.");
                if (ctx->stream_detail_label) gtk_label_set_text(ctx->stream_detail_label, "");
            }
        }

        if (!ctx->pending_source_valid) {
            update_status(ctx, "");
        } else {
            char msg[96];
            g_snprintf(msg, sizeof(msg), "Switching to source %u...", ctx->pending_source_index);
            update_status(ctx, msg);
        }

        if (viewer_selected_source) {
            ctx->active_source_valid = TRUE;
            ctx->active_source_index = viewer_selected_index;
        } else if (!ctx->pending_source_valid) {
            ctx->active_source_valid = FALSE;
            ctx->active_source_index = GTK_INVALID_LIST_POSITION;
        }

        update_idr_button_sensitivity(ctx);

        if (labels) {
            g_strfreev(labels);
        }
    }

    if (viewer_selected_source && !ctx->stats_paused) {
        StatsSample sample = {0};
        sample.timestamp = g_get_monotonic_time() / 1e6;
        sample.rate_bps = viewer_selected_source->inbound_bitrate_bps;
        gboolean shm_source = viewer_selected_source->kind == UV_SOURCE_SHM;
        sample.lost_packets = shm_source ? NAN : (double)viewer_selected_source->rtp_lost_packets;
        sample.dup_packets = shm_source ? NAN : (double)viewer_selected_source->rtp_duplicate_packets;
        sample.reorder_packets = shm_source ? NAN : (double)viewer_selected_source->rtp_reordered_packets;
        sample.rx_packets = (double)viewer_selected_source->rx_packets;
        sample.rx_bytes = (double)viewer_selected_source->rx_bytes;
        if (ctx->stats_history && ctx->stats_history->len > 0) {
            const StatsSample *prev = &g_array_index(ctx->stats_history, StatsSample,
                                                    ctx->stats_history->len - 1);
            double dt = sample.timestamp - prev->timestamp;
            if (dt > 1e-3) {
                double dl = sample.lost_packets - prev->lost_packets;
                double dd = sample.dup_packets - prev->dup_packets;
                double dr = sample.reorder_packets - prev->reorder_packets;
                /* Counter resets (source switch / restart) read as huge negatives;
                 * clamp so the chart doesn't dive into the basement. */
                sample.lost_pps = dl > 0.0 ? dl / dt : 0.0;
                sample.dup_pps = dd > 0.0 ? dd / dt : 0.0;
                sample.reorder_pps = dr > 0.0 ? dr / dt : 0.0;
                double dpkts = sample.rx_packets - prev->rx_packets;
                double dbytes_rx = sample.rx_bytes - prev->rx_bytes;
                sample.pps = dpkts > 0.0 ? dpkts / dt : 0.0;
                sample.pkt_size_bytes = (dpkts > 0.0 && dbytes_rx > 0.0)
                                            ? dbytes_rx / dpkts : 0.0;
            }
        }
        sample.jitter_ms = shm_source ? NAN : viewer_selected_source->rfc3550_jitter_ms;
        sample.input_fps = viewer_selected_source->rtp_marker_fps;
        sample.decoder_fps_current = stats.decoder.instantaneous_fps;
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

        sample.frame_valid = frame_metrics_valid;
        sample.frame_missing = latest_missing;
        sample.frame_lateness_ms = frame_metrics_valid ? latest_lateness : NAN;
        sample.frame_size_kb = frame_metrics_valid ? latest_size : NAN;

        stats_history_push(ctx, &sample);

        update_stats_metric_labels(ctx);

        for (int i = 0; i < STATS_METRIC_COUNT; i++) {
            if (ctx->stats_charts[i]) {
                gtk_widget_queue_draw(GTK_WIDGET(ctx->stats_charts[i]));
            }
        }
    } else if (!ctx->stats_paused) {
        update_stats_metric_labels(ctx);
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
        memcpy(ctx->frame_block_thresholds_span, fb->thresholds_span_ms, sizeof(ctx->frame_block_thresholds_span));
        memcpy(ctx->frame_block_thresholds_chunks, fb->thresholds_chunks, sizeof(ctx->frame_block_thresholds_chunks));
        memcpy(ctx->frame_block_thresholds_fpc, fb->thresholds_fpc, sizeof(ctx->frame_block_thresholds_fpc));
        ctx->frame_block_min_ms = fb->min_lateness_ms;
        ctx->frame_block_max_ms = fb->max_lateness_ms;
        ctx->frame_block_avg_ms = fb->avg_lateness_ms;
        ctx->frame_block_min_kb = fb->min_size_kb;
        ctx->frame_block_max_kb = fb->max_size_kb;
        ctx->frame_block_avg_kb = fb->avg_size_kb;
        ctx->frame_block_min_span = fb->min_span_ms;
        ctx->frame_block_max_span = fb->max_span_ms;
        ctx->frame_block_avg_span = fb->avg_span_ms;
        ctx->frame_block_min_chunks = fb->min_chunks;
        ctx->frame_block_max_chunks = fb->max_chunks;
        ctx->frame_block_avg_chunks = fb->avg_chunks;
        ctx->frame_block_min_fpc = fb->min_fpc;
        ctx->frame_block_max_fpc = fb->max_fpc;
        ctx->frame_block_avg_fpc = fb->avg_fpc;
        ctx->frame_block_real_samples = fb->real_frames;
        ctx->frame_block_missing = fb->missing_frames;
        memcpy(ctx->frame_block_color_counts_ms, fb->color_counts_lateness, sizeof(ctx->frame_block_color_counts_ms));
        memcpy(ctx->frame_block_color_counts_kb, fb->color_counts_size, sizeof(ctx->frame_block_color_counts_kb));
        memcpy(ctx->frame_block_color_counts_span, fb->color_counts_span, sizeof(ctx->frame_block_color_counts_span));
        memcpy(ctx->frame_block_color_counts_chunks, fb->color_counts_chunks, sizeof(ctx->frame_block_color_counts_chunks));
        memcpy(ctx->frame_block_color_counts_fpc, fb->color_counts_fpc, sizeof(ctx->frame_block_color_counts_fpc));

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
        if (!ctx->frame_block_values_span) {
            ctx->frame_block_values_span = g_array_new(FALSE, TRUE, sizeof(double));
        }
        if (!ctx->frame_block_values_chunks) {
            ctx->frame_block_values_chunks = g_array_new(FALSE, TRUE, sizeof(double));
        }
        if (!ctx->frame_block_values_fpc) {
            ctx->frame_block_values_fpc = g_array_new(FALSE, TRUE, sizeof(double));
        }
        g_array_set_size(ctx->frame_block_values_lateness, capacity);
        g_array_set_size(ctx->frame_block_values_size, capacity);
        g_array_set_size(ctx->frame_block_values_span, capacity);
        g_array_set_size(ctx->frame_block_values_chunks, capacity);
        g_array_set_size(ctx->frame_block_values_fpc, capacity);
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
        if (fb->span_ms && fb->span_ms->len == capacity && capacity > 0) {
            memcpy(ctx->frame_block_values_span->data, fb->span_ms->data, sizeof(double) * capacity);
        } else {
            for (guint i = 0; i < ctx->frame_block_values_span->len; i++) {
                g_array_index(ctx->frame_block_values_span, double, i) = NAN;
            }
        }
        if (fb->chunks_per_frame && fb->chunks_per_frame->len == capacity && capacity > 0) {
            memcpy(ctx->frame_block_values_chunks->data, fb->chunks_per_frame->data, sizeof(double) * capacity);
        } else {
            for (guint i = 0; i < ctx->frame_block_values_chunks->len; i++) {
                g_array_index(ctx->frame_block_values_chunks, double, i) = NAN;
            }
        }
        if (fb->frames_per_chunk && fb->frames_per_chunk->len == capacity && capacity > 0) {
            memcpy(ctx->frame_block_values_fpc->data, fb->frames_per_chunk->data, sizeof(double) * capacity);
        } else {
            for (guint i = 0; i < ctx->frame_block_values_fpc->len; i++) {
                g_array_index(ctx->frame_block_values_fpc, double, i) = NAN;
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
        ctx->frame_block_min_span = 0.0;
        ctx->frame_block_max_span = 0.0;
        ctx->frame_block_avg_span = 0.0;
        ctx->frame_block_min_chunks = 0.0;
        ctx->frame_block_max_chunks = 0.0;
        ctx->frame_block_avg_chunks = 0.0;
        ctx->frame_block_min_fpc = 0.0;
        ctx->frame_block_max_fpc = 0.0;
        ctx->frame_block_avg_fpc = 0.0;
        ctx->frame_block_real_samples = 0;
        ctx->frame_block_missing = 0;
        memset(ctx->frame_block_color_counts_ms, 0, sizeof(ctx->frame_block_color_counts_ms));
        memset(ctx->frame_block_color_counts_kb, 0, sizeof(ctx->frame_block_color_counts_kb));
        memset(ctx->frame_block_color_counts_span, 0, sizeof(ctx->frame_block_color_counts_span));
        memset(ctx->frame_block_color_counts_chunks, 0, sizeof(ctx->frame_block_color_counts_chunks));
        memset(ctx->frame_block_color_counts_fpc, 0, sizeof(ctx->frame_block_color_counts_fpc));
        if (ctx->frame_block_values_lateness) {
            g_array_set_size(ctx->frame_block_values_lateness, 0);
        }
        if (ctx->frame_block_values_size) {
            g_array_set_size(ctx->frame_block_values_size, 0);
        }
        if (ctx->frame_block_values_span) {
            g_array_set_size(ctx->frame_block_values_span, 0);
        }
        if (ctx->frame_block_values_chunks) {
            g_array_set_size(ctx->frame_block_values_chunks, 0);
        }
        if (ctx->frame_block_values_fpc) {
            g_array_set_size(ctx->frame_block_values_fpc, 0);
        }
        frame_block_sync_controls(ctx, NULL);
    }

    /* Frame Release telemetry (independent of the frame-block grid). */
    refresh_frame_release(ctx, &stats);

    frame_block_update_summary(ctx);
    if (ctx->frame_block_area) {
        gtk_widget_queue_draw(GTK_WIDGET(ctx->frame_block_area));
    }
    frame_block_queue_overlay_draws(ctx);

    update_sidecar_panel(ctx, &stats.sidecar);
    update_audio_status_label(ctx, &stats);
    update_restream_status_label(ctx, &stats.restream);

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
    if (!ctx || !ctx->viewer) return;

    /* Drop the GTK paintable binding before tearing the sink down so we
     * don't keep a dangling ref or signal handler on the old element. */
    detach_bound_sink(ctx);

    GError *error = NULL;
    if (!uv_viewer_restart_pipeline(ctx->viewer, &error)) {
        uv_log_error("Pipeline restart failed: %s",
                     error && error->message ? error->message : "unknown");
        if (error) g_error_free(error);
    }

    /* Re-bind the paintable to the freshly built sink and update stats. */
    ensure_video_paintable(ctx);
    if (ctx->active_source_valid &&
        ctx->preferred_source_kind == UV_SOURCE_SHM) {
        schedule_shm_recovery(ctx);
    }
    refresh_stats(ctx);
}

static void on_next_button_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    GuiContext *ctx = user_data;
    if (!ctx || !ctx->viewer) return;

    guint next_index = GTK_INVALID_LIST_POSITION;
    if (ctx->source_model) {
        guint count = g_list_model_get_n_items(G_LIST_MODEL(ctx->source_model));
        if (count > 0) {
            guint base = GTK_INVALID_LIST_POSITION;
            if (ctx->pending_source_valid) {
                base = ctx->pending_source_index;
            } else if (ctx->source_dropdown) {
                base = gtk_drop_down_get_selected(ctx->source_dropdown);
            }
            if (base == GTK_INVALID_LIST_POSITION || base >= count) {
                if (ctx->source_dropdown) {
                    base = gtk_drop_down_get_selected(ctx->source_dropdown);
                }
                if (base == GTK_INVALID_LIST_POSITION || base >= count) {
                    base = 0;
                }
            }
            next_index = (base + 1) % count;
        }
    }

    if (next_index != GTK_INVALID_LIST_POSITION) {
        ctx->pending_source_index = next_index;
        ctx->pending_source_valid = TRUE;
    } else {
        ctx->pending_source_valid = FALSE;
        ctx->pending_source_index = GTK_INVALID_LIST_POSITION;
    }

    /* Source selection may rebuild the pipeline when crossing ingress types.
     * Release the old sink paintable before it can be destroyed, then bind
     * whichever sink is active after the operation. */
    detach_bound_sink(ctx);
    GError *error = NULL;
    if (!uv_viewer_select_next_source(ctx->viewer, &error)) {
        update_status(ctx, error ? error->message : "Failed to select next source");
        ctx->pending_source_valid = FALSE;
        ctx->pending_source_index = GTK_INVALID_LIST_POSITION;
    } else {
        update_status(ctx, "Selected next source");
        refresh_stats(ctx);
    }
    ensure_video_paintable(ctx);
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
        ctx->frame_overlay_needs_refresh = FALSE;
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
    frame_block_queue_overlay_draws_force(ctx);
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
    if (!paused && ctx->frame_overlay_needs_refresh) {
        frame_block_queue_overlay_draws_force(ctx);
    }
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
    frame_block_queue_overlay_draws_force(ctx);
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
    frame_block_queue_overlay_draws_force(ctx);
}

static void on_frame_block_metric_changed(GObject *dropdown, GParamSpec *pspec, gpointer user_data) {
    (void)pspec;
    GuiContext *ctx = user_data;
    if (!ctx || !GTK_IS_DROP_DOWN(dropdown)) return;
    guint new_view = gtk_drop_down_get_selected(GTK_DROP_DOWN(dropdown));
    if (new_view >= FRAME_BLOCK_VIEW_COUNT) new_view = FRAME_BLOCK_VIEW_LATENESS;
    if (ctx->frame_block_view == new_view) return;
    ctx->frame_block_view = new_view;
    /* Reload spinners from the newly-active metric's stored thresholds. */
    frame_block_reload_threshold_spins(ctx);
    frame_block_sync_controls(ctx, NULL);
    frame_block_update_summary(ctx);
    if (ctx->frame_block_area) gtk_widget_queue_draw(GTK_WIDGET(ctx->frame_block_area));
    frame_block_queue_overlay_draws_force(ctx);
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
    ctx->frame_block_min_span = 0.0;
    ctx->frame_block_max_span = 0.0;
    ctx->frame_block_avg_span = 0.0;
    ctx->frame_block_min_chunks = 0.0;
    ctx->frame_block_max_chunks = 0.0;
    ctx->frame_block_avg_chunks = 0.0;
    ctx->frame_block_min_fpc = 0.0;
    ctx->frame_block_max_fpc = 0.0;
    ctx->frame_block_avg_fpc = 0.0;
    memset(ctx->frame_block_color_counts_ms, 0, sizeof(ctx->frame_block_color_counts_ms));
    memset(ctx->frame_block_color_counts_kb, 0, sizeof(ctx->frame_block_color_counts_kb));
    memset(ctx->frame_block_color_counts_span, 0, sizeof(ctx->frame_block_color_counts_span));
    memset(ctx->frame_block_color_counts_chunks, 0, sizeof(ctx->frame_block_color_counts_chunks));
    memset(ctx->frame_block_color_counts_fpc, 0, sizeof(ctx->frame_block_color_counts_fpc));
    ctx->frame_block_missing = 0;
    ctx->frame_block_real_samples = 0;
    if (ctx->frame_block_values_lateness) g_array_set_size(ctx->frame_block_values_lateness, 0);
    if (ctx->frame_block_values_size) g_array_set_size(ctx->frame_block_values_size, 0);
    if (ctx->frame_block_values_span) g_array_set_size(ctx->frame_block_values_span, 0);
    if (ctx->frame_block_values_chunks) g_array_set_size(ctx->frame_block_values_chunks, 0);
    if (ctx->frame_block_values_fpc) g_array_set_size(ctx->frame_block_values_fpc, 0);
    frame_block_sync_controls(ctx, NULL);
    frame_block_update_summary(ctx);
    if (ctx->frame_block_area) gtk_widget_queue_draw(GTK_WIDGET(ctx->frame_block_area));
    frame_block_queue_overlay_draws_force(ctx);
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

static void on_frame_overlay_range_changed(GObject *dropdown, GParamSpec *pspec, gpointer user_data) {
    (void)pspec;
    GuiContext *ctx = user_data;
    if (!ctx || !GTK_IS_DROP_DOWN(dropdown)) return;

    guint selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(dropdown));
    double new_range = 60.0;
    switch (selected) {
        case 0: new_range = 15.0; break;
        case 1: new_range = 30.0; break;
        case 2: default: new_range = 60.0; break;
    }

    if (ctx->frame_overlay_range_seconds != new_range) {
        ctx->frame_overlay_range_seconds = new_range;
        frame_block_queue_overlay_draws_force(ctx);
    }
}

static void on_frame_overlay_values_toggled(GtkCheckButton *button, gpointer user_data) {
    GuiContext *ctx = user_data;
    if (!ctx || !GTK_IS_CHECK_BUTTON(button)) return;

    gboolean show_values = gtk_check_button_get_active(button);
    if (ctx->frame_overlay_show_values != show_values) {
        ctx->frame_overlay_show_values = show_values;
    }

    frame_block_queue_overlay_draws_force(ctx);
}

static gboolean gui_restart_with_config_ex(GuiContext *ctx,
                                            const UvViewerConfig *cfg,
                                            gboolean force_restart);

static gboolean gui_restart_with_config(GuiContext *ctx, const UvViewerConfig *cfg) {
    return gui_restart_with_config_ex(ctx, cfg, FALSE);
}

static gboolean gui_restart_with_config_ex(GuiContext *ctx,
                                            const UvViewerConfig *cfg,
                                            gboolean force_restart) {
    if (!ctx || !ctx->viewer || !cfg) return FALSE;

    if (!force_restart &&
        cfg->listen_port == ctx->current_cfg.listen_port &&
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
        cfg->audio_use_separate_port == ctx->current_cfg.audio_use_separate_port &&
        cfg->audio_listen_port == ctx->current_cfg.audio_listen_port &&
        cfg->shm_enabled == ctx->current_cfg.shm_enabled &&
        g_strcmp0(cfg->shm_name, ctx->current_cfg.shm_name) == 0 &&
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
    /* Audio settings are owned by the Audio tab — preserve them. */
    new_cfg.audio_enabled            = ctx->current_cfg.audio_enabled;
    new_cfg.audio_payload_type       = ctx->current_cfg.audio_payload_type;
    new_cfg.audio_jitter_latency_ms  = ctx->current_cfg.audio_jitter_latency_ms;
    new_cfg.audio_clock_rate         = ctx->current_cfg.audio_clock_rate;
    new_cfg.audio_use_separate_port  = ctx->current_cfg.audio_use_separate_port;
    new_cfg.audio_listen_port        = ctx->current_cfg.audio_listen_port;
    new_cfg.sync_to_clock = check_get(ctx->sync_toggle_settings);
    new_cfg.jitter_drop_on_latency = check_get(ctx->jitter_drop_toggle);
    new_cfg.jitter_do_lost = check_get(ctx->jitter_do_lost_toggle);
    new_cfg.jitter_post_drop_messages = check_get(ctx->jitter_post_drop_toggle);
    new_cfg.shm_enabled = check_get(ctx->shm_toggle);
    if (ctx->shm_name_entry) {
        const char *name = gtk_editable_get_text(GTK_EDITABLE(ctx->shm_name_entry));
        while (name && *name == '/') name++;
        g_strlcpy(new_cfg.shm_name, name && *name ? name : "venc_frame_out",
                  sizeof(new_cfg.shm_name));
    }

    if (ctx->idr_port_spin) {
        int port = gtk_spin_button_get_value_as_int(ctx->idr_port_spin);
        if (port < 1) port = 1;
        if (port > 65535) port = 65535;
        new_cfg.idr_http_port = (guint)port;
        ctx->current_cfg.idr_http_port = (guint)port;
    }
    /* Sidecar telemetry has its own tab + runtime start/stop — keep the
     * applied-on-restart settings flow free of it. */
    new_cfg.sidecar_enabled = ctx->current_cfg.sidecar_enabled;
    new_cfg.sidecar_port    = ctx->current_cfg.sidecar_port;

    /* Restream is toggled live on this tab; preserve its enabled state and
     * capture any pending edits to the destination so the rebuilt viewer
     * (uv_viewer_start) resumes forwarding to the same target. */
    new_cfg.restream_enabled = ctx->current_cfg.restream_enabled;
    if (ctx->restream_host_entry) {
        const char *host = gtk_editable_get_text(GTK_EDITABLE(ctx->restream_host_entry));
        g_strlcpy(new_cfg.restream_address, host ? host : "", sizeof(new_cfg.restream_address));
        g_strlcpy(ctx->current_cfg.restream_address, host ? host : "",
                  sizeof(ctx->current_cfg.restream_address));
    }
    if (ctx->restream_port_spin) {
        int rp = gtk_spin_button_get_value_as_int(ctx->restream_port_spin);
        if (rp < 1) rp = 1;
        if (rp > 65535) rp = 65535;
        new_cfg.restream_port = (guint16)rp;
        ctx->current_cfg.restream_port = (guint16)rp;
    }

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
    if (ctx->audio_port_mode_dropdown) {
        gtk_widget_set_sensitive(GTK_WIDGET(ctx->audio_port_mode_dropdown), active);
    }
    if (ctx->audio_port_spin) {
        guint mode = ctx->audio_port_mode_dropdown
                   ? gtk_drop_down_get_selected(ctx->audio_port_mode_dropdown) : 0;
        gtk_widget_set_sensitive(GTK_WIDGET(ctx->audio_port_spin),
                                 active && mode == 1);
    }
}

static void on_source_dropdown_changed(GObject *dropdown, GParamSpec *pspec, gpointer user_data) {
    (void)pspec;
    GuiContext *ctx = user_data;
    if (!ctx || ctx->suppress_source_change || !ctx->viewer) return;
    if (!GTK_IS_DROP_DOWN(dropdown)) return;

    guint selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(dropdown));
    if (selected == GTK_INVALID_LIST_POSITION) return;

    if (ctx->pending_source_valid && ctx->pending_source_index == selected) {
        return;
    }

    if (!ctx->pending_source_valid && ctx->active_source_valid &&
        ctx->active_source_index == selected) {
        return;
    }

    ctx->pending_source_index = selected;
    ctx->pending_source_valid = TRUE;

    /* Switching UDP <-> SHM rebuilds the pipeline and replaces its video
     * sink. Do not leave GtkPicture bound to the old sink's paintable. */
    detach_bound_sink(ctx);
    GError *error = NULL;
    if (!uv_viewer_select_source(ctx->viewer, (int)selected, &error)) {
        update_status(ctx, error ? error->message : "Failed to select source");
        ctx->pending_source_valid = FALSE;
        ctx->pending_source_index = GTK_INVALID_LIST_POSITION;
    } else {
        char msg[128];
        g_snprintf(msg, sizeof(msg), "Selected source %u", selected);
        update_status(ctx, msg);
        refresh_stats(ctx);
    }
    ensure_video_paintable(ctx);
    if (error) {
        g_error_free(error);
    }
}

static gboolean on_window_close_request(GtkWindow *window, gpointer user_data) {
    (void)window;
    GuiContext *ctx = user_data;
    if (!ctx || !ctx->viewer) return FALSE;
    uv_viewer_set_event_callback(ctx->viewer, NULL, NULL);
    if (ctx->shm_recovery_timeout_id) {
        g_source_remove(ctx->shm_recovery_timeout_id);
        ctx->shm_recovery_timeout_id = 0;
    }
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
            if (event->source_index >= 0 && ctx->preferred_source_valid &&
                event->source_kind == ctx->preferred_source_kind &&
                g_strcmp0(event->address, ctx->preferred_source_address) == 0) {
                ctx->pending_source_valid = TRUE;
                ctx->pending_source_index = (guint)event->source_index;
                detach_bound_sink(ctx);
                GError *error = NULL;
                if (!uv_viewer_select_source(ctx->viewer, event->source_index,
                                             &error)) {
                    update_status(ctx, error && error->message
                                           ? error->message
                                           : "Failed to restore source selection");
                    ctx->pending_source_valid = FALSE;
                    ctx->pending_source_index = GTK_INVALID_LIST_POSITION;
                }
                ensure_video_paintable(ctx);
                if (error) g_error_free(error);
            }
            refresh_stats(ctx);
            break;
        }
        case UV_VIEWER_EVENT_SOURCE_SELECTED: {
            ctx->pending_source_valid = FALSE;
            ctx->pending_source_index = GTK_INVALID_LIST_POSITION;
            if (event->source_index >= 0) {
                ctx->active_source_valid = TRUE;
                ctx->active_source_index = (guint)event->source_index;
                ctx->preferred_source_valid = TRUE;
                ctx->preferred_source_kind = event->source_kind;
                g_strlcpy(ctx->preferred_source_address,
                          event->address ? event->address : "",
                          sizeof(ctx->preferred_source_address));
                if (event->source_kind == UV_SOURCE_SHM) {
                    schedule_shm_recovery(ctx);
                }
            } else {
                ctx->active_source_valid = FALSE;
                ctx->active_source_index = GTK_INVALID_LIST_POSITION;
            }
            char msg[256];
            g_snprintf(msg, sizeof(msg), "Selected [%d] %s",
                       event->source_index, event->address ? event->address : "");
            update_status(ctx, msg);
            refresh_stats(ctx);
            break;
        }
        case UV_VIEWER_EVENT_SOURCE_REMOVED: {
            if (ctx->pending_source_valid &&
                event->source_index >= 0 &&
                (guint)event->source_index == ctx->pending_source_index) {
                ctx->pending_source_valid = FALSE;
                ctx->pending_source_index = GTK_INVALID_LIST_POSITION;
            }
            if (event->source_index >= 0 && ctx->active_source_valid &&
                (guint)event->source_index == ctx->active_source_index) {
                ctx->active_source_valid = FALSE;
                ctx->active_source_index = GTK_INVALID_LIST_POSITION;
            }
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
    copy->source_kind = event->source_snapshot.kind;
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
    gtk_label_set_wrap(ctx->status_label, TRUE);
    gtk_widget_add_css_class(GTK_WIDGET(ctx->status_label), "uv-status");
    gtk_widget_add_css_class(GTK_WIDGET(ctx->status_label), "uv-status-bar");
    gtk_box_append(GTK_BOX(page), GTK_WIDGET(ctx->status_label));

    GtkWidget *video_frame = gtk_frame_new("Video Preview");
    gtk_widget_add_css_class(video_frame, "uv-video");
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
    g_object_ref(ctx->source_model);
    GtkWidget *dropdown = gtk_drop_down_new(G_LIST_MODEL(ctx->source_model), NULL);
    gtk_widget_set_hexpand(dropdown, TRUE);
    ctx->source_dropdown = GTK_DROP_DOWN(dropdown);
    g_signal_connect(dropdown, "notify::selected", G_CALLBACK(on_source_dropdown_changed), ctx);
    gtk_box_append(GTK_BOX(dropdown_row), dropdown);

    ctx->source_detail_label = GTK_LABEL(gtk_label_new("No sources discovered yet."));
    gtk_label_set_xalign(ctx->source_detail_label, 0.0);
    gtk_label_set_wrap(ctx->source_detail_label, TRUE);
    gtk_label_set_selectable(ctx->source_detail_label, TRUE);
    gtk_widget_add_css_class(GTK_WIDGET(ctx->source_detail_label), "uv-source-detail");
    gtk_box_append(GTK_BOX(sources_box), GTK_WIDGET(ctx->source_detail_label));

    ctx->stream_detail_label = GTK_LABEL(gtk_label_new(""));
    gtk_label_set_xalign(ctx->stream_detail_label, 0.0);
    gtk_label_set_wrap(ctx->stream_detail_label, TRUE);
    gtk_label_set_selectable(ctx->stream_detail_label, TRUE);
    gtk_widget_add_css_class(GTK_WIDGET(ctx->stream_detail_label), "uv-source-detail");
    gtk_widget_add_css_class(GTK_WIDGET(ctx->stream_detail_label), "uv-info");
    gtk_widget_set_tooltip_text(GTK_WIDGET(ctx->stream_detail_label),
                                "HEVC NAL-unit counts decoded from the RTP payload. "
                                "'KF' is the most recent keyframe (IDR or CRA). "
                                "Long KF gap with low SPS/PPS rate suggests intra-refresh / GDR mode.");
    gtk_box_append(GTK_BOX(sources_box), GTK_WIDGET(ctx->stream_detail_label));

    GtkWidget *button_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(button_box, "uv-action-bar");
    gtk_widget_add_css_class(button_box, "toolbar");
    gtk_box_append(GTK_BOX(page), button_box);

    ctx->idr_button = GTK_BUTTON(gtk_button_new_with_label("Request IDR"));
    gtk_widget_add_css_class(GTK_WIDGET(ctx->idr_button), "suggested-action");
    gtk_widget_add_css_class(GTK_WIDGET(ctx->idr_button), "uv-idr");
    gtk_widget_set_tooltip_text(GTK_WIDGET(ctx->idr_button),
                                "Ask the currently locked encoder for an IDR keyframe "
                                "via HTTP GET /request/idr. Useful to recover after a "
                                "freeze, link blip, or stream join. (Ctrl+I)");
    g_signal_connect(ctx->idr_button, "clicked", G_CALLBACK(on_idr_button_clicked), ctx);
    gtk_box_append(GTK_BOX(button_box), GTK_WIDGET(ctx->idr_button));

    GtkWidget *refresh_button = gtk_button_new_with_label("Restart Pipeline");
    gtk_widget_set_tooltip_text(refresh_button,
                                "Tear down and rebuild the GStreamer pipeline. "
                                "Useful when a mid-stream resolution / SPS change leaves "
                                "the picture frozen. (Ctrl+R)");
    g_signal_connect(refresh_button, "clicked", G_CALLBACK(on_refresh_button_clicked), ctx);
    gtk_box_append(GTK_BOX(button_box), refresh_button);

    GtkWidget *next_button = gtk_button_new_with_label("Select Next");
    gtk_widget_set_tooltip_text(next_button,
                                "Switch to the next discovered source. (Ctrl+N)");
    g_signal_connect(next_button, "clicked", G_CALLBACK(on_next_button_clicked), ctx);
    gtk_box_append(GTK_BOX(button_box), next_button);

    ctx->sources_toggle = GTK_TOGGLE_BUTTON(gtk_toggle_button_new_with_label("Hide Sources"));
    gtk_widget_set_tooltip_text(GTK_WIDGET(ctx->sources_toggle),
                                "Collapse the source selector to maximize the video pane.");
    g_signal_connect(ctx->sources_toggle, "toggled", G_CALLBACK(on_sources_toggle_toggled), ctx);
    gtk_box_append(GTK_BOX(button_box), GTK_WIDGET(ctx->sources_toggle));

    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);
    gtk_box_append(GTK_BOX(button_box), spacer);

    GtkWidget *quit_button = gtk_button_new_with_label("Quit");
    gtk_widget_add_css_class(quit_button, "flat");
    g_signal_connect(quit_button, "clicked", G_CALLBACK(on_quit_button_clicked), ctx);
    gtk_box_append(GTK_BOX(button_box), GTK_WIDGET(quit_button));

    gtk_toggle_button_set_active(ctx->sources_toggle, TRUE);

    update_idr_button_sensitivity(ctx);

    return page;
}

static void update_restream_status_label(GuiContext *ctx, const UvRestreamStats *rs) {
    if (!ctx || !ctx->restream_status_label) return;
    char line[256];
    if (!rs || !rs->enabled) {
        g_snprintf(line, sizeof(line), "Restream: off");
    } else if (!rs->active) {
        g_snprintf(line, sizeof(line), "Restream: enabled (no valid destination)");
    } else {
        g_snprintf(line, sizeof(line),
                   "Restream \xE2\x86\x92 %s:%u   tx=%" G_GUINT64_FORMAT " pkts / %.1f MB%s",
                   rs->address, rs->port, rs->tx_packets,
                   (double)rs->tx_bytes / (1024.0 * 1024.0),
                   rs->tx_errors ? "   (send errors)" : "");
    }
    gtk_label_set_text(ctx->restream_status_label, line);
}

static void on_restream_toggled(GtkCheckButton *btn, gpointer user_data) {
    GuiContext *ctx = user_data;
    if (!ctx || ctx->restream_toggle_suppress) return;
    gboolean want = gtk_check_button_get_active(btn);

    const char *host = "";
    if (ctx->restream_host_entry) {
        host = gtk_editable_get_text(GTK_EDITABLE(ctx->restream_host_entry));
    }
    guint16 port = ctx->current_cfg.restream_port ? ctx->current_cfg.restream_port : 5600;
    if (ctx->restream_port_spin) {
        int v = gtk_spin_button_get_value_as_int(ctx->restream_port_spin);
        if (v >= 1 && v <= 65535) port = (guint16)v;
    }

    if (want && (!host || !host[0])) {
        update_status(ctx, "Restream: enter a destination host first");
        ctx->restream_toggle_suppress = TRUE;
        gtk_check_button_set_active(btn, FALSE);
        ctx->restream_toggle_suppress = FALSE;
        return;
    }

    ctx->current_cfg.restream_enabled = want;
    g_strlcpy(ctx->current_cfg.restream_address, host ? host : "",
              sizeof(ctx->current_cfg.restream_address));
    ctx->current_cfg.restream_port = port;
    if (ctx->cfg_slot) {
        ctx->cfg_slot->restream_enabled = want;
        g_strlcpy(ctx->cfg_slot->restream_address, host ? host : "",
                  sizeof(ctx->cfg_slot->restream_address));
        ctx->cfg_slot->restream_port = port;
    }

    if (ctx->viewer) {
        uv_viewer_set_restream(ctx->viewer, want, host, port);
    }

    /* Freeze the destination fields while a forward is live so the displayed
     * target always matches what the relay is actually sending to. */
    if (ctx->restream_host_entry) {
        gtk_widget_set_sensitive(GTK_WIDGET(ctx->restream_host_entry), !want);
    }
    if (ctx->restream_port_spin) {
        gtk_widget_set_sensitive(GTK_WIDGET(ctx->restream_port_spin), !want);
    }
    update_status(ctx, want ? "Restream started" : "Restream stopped");
}

static GtkWidget *build_settings_page(GuiContext *ctx) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(page, 12);
    gtk_widget_set_margin_bottom(page, 12);
    gtk_widget_set_margin_start(page, 12);
    gtk_widget_set_margin_end(page, 12);

    ctx->info_label = GTK_LABEL(gtk_label_new(""));
    gtk_label_set_xalign(ctx->info_label, 0.0);
    gtk_label_set_wrap(ctx->info_label, TRUE);
    gtk_widget_add_css_class(GTK_WIDGET(ctx->info_label), "uv-info");
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

    /* Audio settings now live on the dedicated Audio tab. */

    ctx->jitter_drop_toggle = GTK_CHECK_BUTTON(gtk_check_button_new_with_label("Drop packets exceeding latency"));
    gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(ctx->jitter_drop_toggle), 0, 9, 2, 1);

    ctx->jitter_do_lost_toggle = GTK_CHECK_BUTTON(gtk_check_button_new_with_label("Emit lost packet notifications"));
    gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(ctx->jitter_do_lost_toggle), 0, 10, 2, 1);

    ctx->jitter_post_drop_toggle = GTK_CHECK_BUTTON(gtk_check_button_new_with_label("Post drop messages on bus"));
    gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(ctx->jitter_post_drop_toggle), 0, 11, 2, 1);

    GtkWidget *idr_port_label = gtk_label_new("IDR HTTP Port:");
    gtk_label_set_xalign(GTK_LABEL(idr_port_label), 0.0);
    gtk_widget_set_tooltip_text(idr_port_label,
                                "TCP port used by the 'Request IDR' button to reach the "
                                "encoder's /request/idr endpoint (default 80).");
    gtk_grid_attach(GTK_GRID(grid), idr_port_label, 0, 12, 1, 1);

    ctx->idr_port_spin = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(1, 65535, 1));
    gtk_widget_set_tooltip_text(GTK_WIDGET(ctx->idr_port_spin),
                                "Applies on next Apply Settings; the button targets this port "
                                "on the currently locked source's IP.");
    gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(ctx->idr_port_spin), 1, 12, 1, 1);

    /* Restream: verbatim UDP forward of the currently selected source. Live
     * controls (no viewer restart) — the destination is frozen while active. */
    GtkWidget *restream_header = gtk_label_new("Restream (verbatim UDP forward)");
    gtk_label_set_xalign(GTK_LABEL(restream_header), 0.0);
    gtk_widget_add_css_class(restream_header, "uv-info");
    gtk_widget_set_margin_top(restream_header, 6);
    gtk_grid_attach(GTK_GRID(grid), restream_header, 0, 13, 2, 1);

    GtkWidget *restream_host_label = gtk_label_new("Restream Host:");
    gtk_label_set_xalign(GTK_LABEL(restream_host_label), 0.0);
    gtk_grid_attach(GTK_GRID(grid), restream_host_label, 0, 14, 1, 1);

    ctx->restream_host_entry = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(ctx->restream_host_entry, "e.g. 192.168.2.30");
    gtk_widget_set_tooltip_text(GTK_WIDGET(ctx->restream_host_entry),
                                "Destination IPv4 address for the raw UDP forward of the "
                                "currently selected source.");
    gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(ctx->restream_host_entry), 1, 14, 1, 1);

    GtkWidget *restream_port_label = gtk_label_new("Restream Port:");
    gtk_label_set_xalign(GTK_LABEL(restream_port_label), 0.0);
    gtk_grid_attach(GTK_GRID(grid), restream_port_label, 0, 15, 1, 1);

    ctx->restream_port_spin = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(1, 65535, 1));
    gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(ctx->restream_port_spin), 1, 15, 1, 1);

    ctx->restream_toggle = GTK_CHECK_BUTTON(gtk_check_button_new_with_label("Enable restream"));
    gtk_widget_set_tooltip_text(GTK_WIDGET(ctx->restream_toggle),
                                "Forward every raw datagram from the selected source to the "
                                "destination above, unchanged. Toggles live.");
    g_signal_connect(ctx->restream_toggle, "toggled", G_CALLBACK(on_restream_toggled), ctx);
    gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(ctx->restream_toggle), 0, 16, 2, 1);

    ctx->restream_status_label = GTK_LABEL(gtk_label_new("Restream: off"));
    gtk_label_set_xalign(ctx->restream_status_label, 0.0);
    gtk_label_set_selectable(ctx->restream_status_label, TRUE);
    gtk_widget_add_css_class(GTK_WIDGET(ctx->restream_status_label), "uv-source-detail");
    gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(ctx->restream_status_label), 0, 17, 2, 1);

    GtkWidget *shm_header = gtk_label_new("SHM Ingress");
    gtk_label_set_xalign(GTK_LABEL(shm_header), 0.0);
    gtk_widget_add_css_class(shm_header, "uv-info");
    gtk_widget_set_margin_top(shm_header, 6);
    gtk_grid_attach(GTK_GRID(grid), shm_header, 0, 18, 2, 1);

    ctx->shm_toggle = GTK_CHECK_BUTTON(gtk_check_button_new_with_label("Enable SHM ingress"));
    gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(ctx->shm_toggle), 0, 19, 2, 1);

    GtkWidget *shm_name_label = gtk_label_new("Ring Name:");
    gtk_label_set_xalign(GTK_LABEL(shm_name_label), 0.0);
    gtk_grid_attach(GTK_GRID(grid), shm_name_label, 0, 20, 1, 1);
    ctx->shm_name_entry = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(ctx->shm_name_entry, "venc_frame_out");
    gtk_widget_set_tooltip_text(GTK_WIDGET(ctx->shm_name_entry),
                                "POSIX object at /dev/shm/<name>; a leading slash is optional.");
    gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(ctx->shm_name_entry), 1, 20, 1, 1);

    GtkWidget *apply_button = gtk_button_new_with_label("Apply Settings");
    gtk_widget_add_css_class(apply_button, "suggested-action");
    g_signal_connect(apply_button, "clicked", G_CALLBACK(on_settings_apply_clicked), ctx);
    gtk_box_append(GTK_BOX(page), apply_button);

    sync_settings_controls(ctx);

    GtkWidget *hint = gtk_label_new("Applying changes restarts the viewer to bind the new settings.");
    gtk_label_set_xalign(GTK_LABEL(hint), 0.0);
   gtk_box_append(GTK_BOX(page), hint);

    return page;
}

static void on_stats_pause_toggled(GtkToggleButton *btn, gpointer user_data) {
    GuiContext *ctx = user_data;
    if (!ctx) return;
    ctx->stats_paused = gtk_toggle_button_get_active(btn);
    gtk_button_set_label(GTK_BUTTON(btn), ctx->stats_paused ? "Resume" : "Pause");
}

static void on_stats_reset_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    GuiContext *ctx = user_data;
    if (!ctx) return;
    if (ctx->stats_history) {
        g_array_set_size(ctx->stats_history, 0);
    }
    update_stats_metric_labels(ctx);
    update_frame_overlay_labels(ctx);
    for (int i = 0; i < STATS_METRIC_COUNT; i++) {
        if (ctx->stats_charts[i]) gtk_widget_queue_draw(GTK_WIDGET(ctx->stats_charts[i]));
    }
    if (ctx->frame_overlay_lateness) gtk_widget_queue_draw(GTK_WIDGET(ctx->frame_overlay_lateness));
    if (ctx->frame_overlay_size)     gtk_widget_queue_draw(GTK_WIDGET(ctx->frame_overlay_size));
}

static GtkWidget *build_stats_page(GuiContext *ctx) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_top(page, 12);
    gtk_widget_set_margin_bottom(page, 12);
    gtk_widget_set_margin_start(page, 12);
    gtk_widget_set_margin_end(page, 12);

    /* Controls row: time range + pause / reset. The source / bitrate /
     * jitter / fps banner is intentionally absent — the same info is
     * already on the Monitor tab and visible on the per-chart "Live"
     * labels, so a third copy was just adding visual noise. */
    GtkWidget *controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(page), controls);

    GtkWidget *range_label = gtk_label_new("Time range:");
    gtk_label_set_xalign(GTK_LABEL(range_label), 0.0);
    gtk_box_append(GTK_BOX(controls), range_label);

    const char *options[] = {
        "Last 30 seconds",
        "Last 1 minute",
        "Last 5 minutes",
        "Last 10 minutes",
        NULL
    };
    ctx->stats_range_dropdown = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(options));
    gtk_widget_set_hexpand(GTK_WIDGET(ctx->stats_range_dropdown), FALSE);

    guint default_index = 2;
    if (fabs(ctx->stats_range_seconds - 30.0) < 0.1) default_index = 0;
    else if (fabs(ctx->stats_range_seconds - 60.0) < 0.1) default_index = 1;
    else if (fabs(ctx->stats_range_seconds - 600.0) < 0.1) default_index = 3;
    gtk_drop_down_set_selected(ctx->stats_range_dropdown, default_index);
    g_signal_connect(ctx->stats_range_dropdown, "notify::selected", G_CALLBACK(stats_range_changed), ctx);
    gtk_box_append(GTK_BOX(controls), GTK_WIDGET(ctx->stats_range_dropdown));

    GtkWidget *ctrl_spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(ctrl_spacer, TRUE);
    gtk_box_append(GTK_BOX(controls), ctrl_spacer);

    GtkWidget *reset_btn = gtk_button_new_with_label("Reset");
    gtk_widget_add_css_class(reset_btn, "flat");
    gtk_widget_set_tooltip_text(reset_btn, "Clear the recorded history buffer.");
    g_signal_connect(reset_btn, "clicked", G_CALLBACK(on_stats_reset_clicked), ctx);
    gtk_box_append(GTK_BOX(controls), reset_btn);

    GtkWidget *pause_btn = gtk_toggle_button_new_with_label("Pause");
    ctx->stats_pause_toggle = GTK_TOGGLE_BUTTON(pause_btn);
    gtk_widget_set_tooltip_text(pause_btn,
                                "Freeze the chart and labels so you can examine "
                                "a glitch. New samples are dropped while paused.");
    g_signal_connect(pause_btn, "toggled", G_CALLBACK(on_stats_pause_toggled), ctx);
    gtk_box_append(GTK_BOX(controls), pause_btn);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 12);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_box_append(GTK_BOX(page), grid);

    static const char *titles[STATS_METRIC_COUNT] = {
        "Inbound Rate (Mbps)",
        "RTP Lost (pkt/s)",
        "RTP Duplicate (pkt/s)",
        "RTP Reordered (pkt/s)",
        "RTP Jitter (ms)",
        "Input Frame FPS",
        "Decoder FPS (current)",
        "Packets/sec (PPS)",
        "Avg Packet Size (bytes)"
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
        gtk_label_set_single_line_mode(GTK_LABEL(live_widget), TRUE);
        gtk_label_set_width_chars(GTK_LABEL(live_widget), 18);
        gtk_label_set_max_width_chars(GTK_LABEL(live_widget), 18);
        gtk_widget_add_css_class(live_widget, "numeric");
        gtk_box_append(GTK_BOX(label_box), live_widget);

        GtkWidget *max_widget = gtk_label_new("Max: --");
        gtk_label_set_xalign(GTK_LABEL(max_widget), 1.0);
        gtk_widget_set_valign(max_widget, GTK_ALIGN_CENTER);
        gtk_label_set_single_line_mode(GTK_LABEL(max_widget), TRUE);
        gtk_label_set_width_chars(GTK_LABEL(max_widget), 18);
        gtk_label_set_max_width_chars(GTK_LABEL(max_widget), 18);
        gtk_widget_add_css_class(max_widget, "numeric");
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

static void on_audio_port_mode_changed(GObject *dropdown, GParamSpec *pspec, gpointer user_data) {
    (void)pspec;
    GuiContext *ctx = user_data;
    if (!ctx || !GTK_IS_DROP_DOWN(dropdown)) return;
    guint mode = gtk_drop_down_get_selected(GTK_DROP_DOWN(dropdown));
    gboolean separate = (mode == 1);
    gboolean audio_on = ctx->audio_toggle ? check_get(ctx->audio_toggle) : FALSE;
    if (ctx->audio_port_spin) {
        gtk_widget_set_sensitive(GTK_WIDGET(ctx->audio_port_spin), audio_on && separate);
    }
}

static void audio_collect_cfg_from_ui(GuiContext *ctx, UvViewerConfig *new_cfg) {
    *new_cfg = ctx->current_cfg;

    new_cfg->audio_enabled = ctx->audio_toggle ? check_get(ctx->audio_toggle) : FALSE;
    if (ctx->audio_payload_spin) {
        int v = gtk_spin_button_get_value_as_int(ctx->audio_payload_spin);
        if (v < 0) v = 0;
        if (v > 127) v = 127;
        new_cfg->audio_payload_type = (guint)v;
    }
    if (ctx->audio_jitter_spin) {
        int v = gtk_spin_button_get_value_as_int(ctx->audio_jitter_spin);
        if (v < 0) v = 0;
        new_cfg->audio_jitter_latency_ms = (guint)v;
    }
    if (ctx->audio_port_mode_dropdown) {
        guint mode = gtk_drop_down_get_selected(ctx->audio_port_mode_dropdown);
        new_cfg->audio_use_separate_port = (mode == 1);
    }
    if (ctx->audio_port_spin) {
        int v = gtk_spin_button_get_value_as_int(ctx->audio_port_spin);
        if (v < 1) v = 1;
        if (v > 65535) v = 65535;
        new_cfg->audio_listen_port = (guint)v;
    }

    if (new_cfg->audio_use_separate_port &&
        (gint)new_cfg->audio_listen_port == new_cfg->listen_port) {
        update_status(ctx,
            "Audio: separate port would collide with video — keeping shared mode.");
        new_cfg->audio_use_separate_port = FALSE;
    }
}

static void on_audio_apply_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    GuiContext *ctx = user_data;
    if (!ctx) return;

    UvViewerConfig new_cfg;
    audio_collect_cfg_from_ui(ctx, &new_cfg);

    if (!gui_restart_with_config(ctx, &new_cfg)) {
        sync_settings_controls(ctx);
    } else {
        update_status(ctx, "Audio settings applied.");
    }
}

static void on_audio_restart_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    GuiContext *ctx = user_data;
    if (!ctx) return;

    UvViewerConfig new_cfg;
    audio_collect_cfg_from_ui(ctx, &new_cfg);

    /* Force rebuild even if config matches current — that's the point of
     * the explicit Restart button: kick a stuck audio chain without
     * having to change a setting first. */
    if (!gui_restart_with_config_ex(ctx, &new_cfg, TRUE)) {
        sync_settings_controls(ctx);
    } else {
        update_status(ctx, "Audio pipeline restarted.");
    }
}

static GtkWidget *build_audio_page(GuiContext *ctx) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(page, 12);
    gtk_widget_set_margin_bottom(page, 12);
    gtk_widget_set_margin_start(page, 12);
    gtk_widget_set_margin_end(page, 12);

    /* Status frame at the top. */
    GtkWidget *status_frame = gtk_frame_new("Status");
    gtk_widget_set_hexpand(status_frame, TRUE);
    gtk_box_append(GTK_BOX(page), status_frame);
    ctx->audio_status_label = GTK_LABEL(gtk_label_new("Audio disabled."));
    gtk_label_set_xalign(ctx->audio_status_label, 0.0);
    gtk_label_set_wrap(ctx->audio_status_label, TRUE);
    gtk_widget_add_css_class(GTK_WIDGET(ctx->audio_status_label), "uv-source-detail");
    gtk_widget_set_margin_top(GTK_WIDGET(ctx->audio_status_label), 6);
    gtk_widget_set_margin_bottom(GTK_WIDGET(ctx->audio_status_label), 6);
    gtk_widget_set_margin_start(GTK_WIDGET(ctx->audio_status_label), 8);
    gtk_widget_set_margin_end(GTK_WIDGET(ctx->audio_status_label), 8);
    gtk_frame_set_child(GTK_FRAME(status_frame), GTK_WIDGET(ctx->audio_status_label));

    /* Configuration grid. */
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_box_append(GTK_BOX(page), grid);

    GtkWidget *enable_label = gtk_label_new("Enable audio:");
    gtk_label_set_xalign(GTK_LABEL(enable_label), 0.0);
    gtk_grid_attach(GTK_GRID(grid), enable_label, 0, 0, 1, 1);
    ctx->audio_toggle = GTK_CHECK_BUTTON(gtk_check_button_new());
    gtk_widget_set_tooltip_text(GTK_WIDGET(ctx->audio_toggle),
                                "Build the Opus receive branch in the pipeline.");
    g_signal_connect(ctx->audio_toggle, "toggled", G_CALLBACK(on_audio_toggled), ctx);
    gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(ctx->audio_toggle), 1, 0, 1, 1);

    GtkWidget *pt_label = gtk_label_new("Payload type:");
    gtk_label_set_xalign(GTK_LABEL(pt_label), 0.0);
    gtk_grid_attach(GTK_GRID(grid), pt_label, 0, 1, 1, 1);
    ctx->audio_payload_spin = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(0, 127, 1));
    gtk_widget_set_tooltip_text(GTK_WIDGET(ctx->audio_payload_spin),
                                "RTP payload type for the OPUS audio packets (default 98).");
    gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(ctx->audio_payload_spin), 1, 1, 1, 1);

    GtkWidget *jitter_label = gtk_label_new("Jitter latency (ms):");
    gtk_label_set_xalign(GTK_LABEL(jitter_label), 0.0);
    gtk_grid_attach(GTK_GRID(grid), jitter_label, 0, 2, 1, 1);
    ctx->audio_jitter_spin = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(0, 2000, 5));
    gtk_widget_set_tooltip_text(GTK_WIDGET(ctx->audio_jitter_spin),
                                "rtpjitterbuffer latency for audio in ms. Opus packets are "
                                "typically 20 ms apart, so anything below ~30 ms will under-"
                                "run on most links. 40-100 ms is a sensible range.");
    gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(ctx->audio_jitter_spin), 1, 2, 1, 1);

    GtkWidget *mode_label = gtk_label_new("Port mode:");
    gtk_label_set_xalign(GTK_LABEL(mode_label), 0.0);
    gtk_widget_set_tooltip_text(mode_label,
                                "\"Shared with video\" demuxes audio out of the same RTP stream "
                                "by payload type (default). \"Separate UDP port\" binds a "
                                "second listener on its own port.");
    gtk_grid_attach(GTK_GRID(grid), mode_label, 0, 3, 1, 1);
    const char *mode_options[] = {"Shared with video port", "Separate UDP port", NULL};
    ctx->audio_port_mode_dropdown = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(mode_options));
    g_signal_connect(ctx->audio_port_mode_dropdown, "notify::selected",
                     G_CALLBACK(on_audio_port_mode_changed), ctx);
    gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(ctx->audio_port_mode_dropdown), 1, 3, 1, 1);

    GtkWidget *port_label = gtk_label_new("Audio UDP port:");
    gtk_label_set_xalign(GTK_LABEL(port_label), 0.0);
    gtk_grid_attach(GTK_GRID(grid), port_label, 0, 4, 1, 1);
    ctx->audio_port_spin = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(1, 65535, 1));
    gtk_widget_set_tooltip_text(GTK_WIDGET(ctx->audio_port_spin),
                                "Used only when the port mode is \"Separate UDP port\". "
                                "Must differ from the video listen port.");
    gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(ctx->audio_port_spin), 1, 4, 1, 1);

    /* Apply / Restart buttons. */
    GtkWidget *button_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(page), button_row);

    GtkWidget *apply_btn = gtk_button_new_with_label("Apply Audio Settings");
    gtk_widget_add_css_class(apply_btn, "suggested-action");
    gtk_widget_set_tooltip_text(apply_btn,
                                "Rebuild the pipeline with the new audio config. "
                                "Video keeps running on its existing listen port.");
    g_signal_connect(apply_btn, "clicked", G_CALLBACK(on_audio_apply_clicked), ctx);
    gtk_box_append(GTK_BOX(button_row), apply_btn);

    GtkWidget *restart_btn = gtk_button_new_with_label("Restart Audio");
    gtk_widget_set_tooltip_text(restart_btn,
                                "Force a pipeline rebuild with the current audio settings "
                                "even if nothing changed — kicks the audio branch out of "
                                "a glitch (stuck jitter buffer, silent after a network "
                                "blip) without touching any config.");
    g_signal_connect(restart_btn, "clicked", G_CALLBACK(on_audio_restart_clicked), ctx);
    gtk_box_append(GTK_BOX(button_row), restart_btn);

    GtkWidget *hint = gtk_label_new(
        "Apply restarts the GStreamer pipeline with the values above. "
        "Restart Audio does the same rebuild but with no config changes.");
    gtk_label_set_xalign(GTK_LABEL(hint), 0.0);
    gtk_label_set_wrap(GTK_LABEL(hint), TRUE);
    gtk_widget_add_css_class(hint, "uv-info");
    gtk_box_append(GTK_BOX(page), hint);

    /* Prime the controls from the current config. */
    if (ctx->audio_toggle) {
        check_set(ctx->audio_toggle, ctx->current_cfg.audio_enabled ? TRUE : FALSE);
    }
    if (ctx->audio_payload_spin) {
        gtk_spin_button_set_value(ctx->audio_payload_spin, ctx->current_cfg.audio_payload_type);
    }
    if (ctx->audio_jitter_spin) {
        gtk_spin_button_set_value(ctx->audio_jitter_spin, ctx->current_cfg.audio_jitter_latency_ms);
    }
    if (ctx->audio_port_mode_dropdown) {
        gtk_drop_down_set_selected(ctx->audio_port_mode_dropdown,
                                   ctx->current_cfg.audio_use_separate_port ? 1 : 0);
    }
    if (ctx->audio_port_spin) {
        guint p = ctx->current_cfg.audio_listen_port ? ctx->current_cfg.audio_listen_port : 5601;
        gtk_spin_button_set_value(ctx->audio_port_spin, p);
    }
    /* Initial sensitivity. */
    on_audio_toggled(ctx->audio_toggle, ctx);

    return page;
}

static void update_audio_status_label(GuiContext *ctx, const UvViewerStats *stats) {
    if (!ctx || !ctx->audio_status_label) return;
    const UvViewerConfig *cfg = &ctx->current_cfg;
    if (!cfg->audio_enabled) {
        gtk_label_set_text(ctx->audio_status_label, "Audio disabled.");
        return;
    }
    const char *state;
    if (!stats || !stats->audio_enabled) {
        state = "audio branch failed to build";
    } else if (stats->audio_active) {
        state = "ACTIVE — buffers flowing";
    } else {
        state = "waiting for packets";
    }
    char line[224];
    if (cfg->audio_use_separate_port) {
        g_snprintf(line, sizeof(line),
                   "%s  •  separate UDP :%u  •  PT %u  •  jitter %u ms  •  clock %u Hz",
                   state,
                   cfg->audio_listen_port,
                   cfg->audio_payload_type,
                   cfg->audio_jitter_latency_ms,
                   cfg->audio_clock_rate ? cfg->audio_clock_rate : 48000u);
    } else {
        g_snprintf(line, sizeof(line),
                   "%s  •  shared with video on :%d  •  PT %u  •  jitter %u ms  •  clock %u Hz",
                   state,
                   cfg->listen_port,
                   cfg->audio_payload_type,
                   cfg->audio_jitter_latency_ms,
                   cfg->audio_clock_rate ? cfg->audio_clock_rate : 48000u);
    }
    gtk_label_set_text(ctx->audio_status_label, line);
}

static void update_sidecar_toggle_label(GuiContext *ctx, gboolean enabled) {
    if (!ctx || !ctx->sidecar_enable_toggle) return;
    gtk_button_set_label(GTK_BUTTON(ctx->sidecar_enable_toggle),
                         enabled ? "Stop telemetry" : "Start telemetry");
    if (enabled) {
        gtk_widget_remove_css_class(GTK_WIDGET(ctx->sidecar_enable_toggle), "suggested-action");
        gtk_widget_add_css_class(GTK_WIDGET(ctx->sidecar_enable_toggle), "destructive-action");
    } else {
        gtk_widget_remove_css_class(GTK_WIDGET(ctx->sidecar_enable_toggle), "destructive-action");
        gtk_widget_add_css_class(GTK_WIDGET(ctx->sidecar_enable_toggle), "suggested-action");
    }
}

static void on_sidecar_enable_toggled(GtkToggleButton *btn, gpointer user_data) {
    GuiContext *ctx = user_data;
    if (!ctx || ctx->sidecar_toggle_suppress) return;
    if (!ctx->viewer) return;

    gboolean want_on = gtk_toggle_button_get_active(btn);
    guint port = ctx->current_cfg.sidecar_port ? ctx->current_cfg.sidecar_port : 5602;
    if (ctx->sidecar_port_spin) {
        int v = gtk_spin_button_get_value_as_int(ctx->sidecar_port_spin);
        if (v >= 1 && v <= 65535) port = (guint)v;
    }

    uv_viewer_set_sidecar_enabled(ctx->viewer, want_on, port);
    ctx->current_cfg.sidecar_enabled = want_on;
    ctx->current_cfg.sidecar_port = port;
    if (ctx->cfg_slot) {
        ctx->cfg_slot->sidecar_enabled = want_on;
        ctx->cfg_slot->sidecar_port = port;
    }
    update_sidecar_toggle_label(ctx, want_on);
    if (ctx->sidecar_port_spin) {
        gtk_widget_set_sensitive(GTK_WIDGET(ctx->sidecar_port_spin), !want_on);
    }
}

static void on_sidecar_port_changed(GtkSpinButton *spin, gpointer user_data) {
    GuiContext *ctx = user_data;
    if (!ctx) return;
    int v = gtk_spin_button_get_value_as_int(spin);
    if (v < 1) v = 1;
    if (v > 65535) v = 65535;
    ctx->current_cfg.sidecar_port = (guint)v;
    if (ctx->cfg_slot) ctx->cfg_slot->sidecar_port = (guint)v;
}

static GtkWidget *build_sidecar_page(GuiContext *ctx) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_top(page, 12);
    gtk_widget_set_margin_bottom(page, 12);
    gtk_widget_set_margin_start(page, 12);
    gtk_widget_set_margin_end(page, 12);

    /* Controls row: Start/Stop, port spin. */
    GtkWidget *controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(controls, "toolbar");
    gtk_widget_add_css_class(controls, "uv-action-bar");
    gtk_box_append(GTK_BOX(page), controls);

    ctx->sidecar_enable_toggle = GTK_TOGGLE_BUTTON(gtk_toggle_button_new_with_label("Start telemetry"));
    gtk_widget_add_css_class(GTK_WIDGET(ctx->sidecar_enable_toggle), "suggested-action");
    gtk_widget_set_tooltip_text(GTK_WIDGET(ctx->sidecar_enable_toggle),
                                "Open a UDP probe socket and subscribe to the locked source's "
                                "waybeam_venc sidecar telemetry channel. Stops cleanly when "
                                "toggled off.");
    g_signal_connect(ctx->sidecar_enable_toggle, "toggled",
                     G_CALLBACK(on_sidecar_enable_toggled), ctx);
    gtk_box_append(GTK_BOX(controls), GTK_WIDGET(ctx->sidecar_enable_toggle));

    GtkWidget *port_label = gtk_label_new("Encoder port:");
    gtk_widget_set_valign(port_label, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(controls), port_label);

    ctx->sidecar_port_spin = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(1, 65535, 1));
    gtk_widget_set_tooltip_text(GTK_WIDGET(ctx->sidecar_port_spin),
                                "UDP port on the encoder side that hosts the sidecar listener "
                                "(default 5602). Editable only when telemetry is stopped.");
    gtk_widget_set_valign(GTK_WIDGET(ctx->sidecar_port_spin), GTK_ALIGN_CENTER);
    g_signal_connect(ctx->sidecar_port_spin, "value-changed",
                     G_CALLBACK(on_sidecar_port_changed), ctx);
    gtk_box_append(GTK_BOX(controls), GTK_WIDGET(ctx->sidecar_port_spin));

    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);
    gtk_box_append(GTK_BOX(controls), spacer);

    /* Status frame. */
    GtkWidget *status_frame = gtk_frame_new("Status");
    gtk_widget_set_hexpand(status_frame, TRUE);
    gtk_box_append(GTK_BOX(page), status_frame);
    ctx->sidecar_status_label = GTK_LABEL(gtk_label_new(
        "Stopped. Press Start to subscribe to the encoder's sidecar."));
    gtk_label_set_xalign(ctx->sidecar_status_label, 0.0);
    gtk_label_set_wrap(ctx->sidecar_status_label, TRUE);
    gtk_label_set_selectable(ctx->sidecar_status_label, TRUE);
    gtk_widget_add_css_class(GTK_WIDGET(ctx->sidecar_status_label), "uv-source-detail");
    gtk_widget_set_margin_top(GTK_WIDGET(ctx->sidecar_status_label), 6);
    gtk_widget_set_margin_bottom(GTK_WIDGET(ctx->sidecar_status_label), 6);
    gtk_widget_set_margin_start(GTK_WIDGET(ctx->sidecar_status_label), 8);
    gtk_widget_set_margin_end(GTK_WIDGET(ctx->sidecar_status_label), 8);
    gtk_frame_set_child(GTK_FRAME(status_frame), GTK_WIDGET(ctx->sidecar_status_label));

    /* Last frame frame. */
    GtkWidget *frame_frame = gtk_frame_new("Last frame");
    gtk_widget_set_hexpand(frame_frame, TRUE);
    gtk_box_append(GTK_BOX(page), frame_frame);
    ctx->sidecar_frame_label = GTK_LABEL(gtk_label_new(""));
    gtk_label_set_xalign(ctx->sidecar_frame_label, 0.0);
    gtk_label_set_wrap(ctx->sidecar_frame_label, TRUE);
    gtk_label_set_selectable(ctx->sidecar_frame_label, TRUE);
    gtk_widget_add_css_class(GTK_WIDGET(ctx->sidecar_frame_label), "uv-source-detail");
    gtk_widget_set_margin_top(GTK_WIDGET(ctx->sidecar_frame_label), 6);
    gtk_widget_set_margin_bottom(GTK_WIDGET(ctx->sidecar_frame_label), 6);
    gtk_widget_set_margin_start(GTK_WIDGET(ctx->sidecar_frame_label), 8);
    gtk_widget_set_margin_end(GTK_WIDGET(ctx->sidecar_frame_label), 8);
    gtk_frame_set_child(GTK_FRAME(frame_frame), GTK_WIDGET(ctx->sidecar_frame_label));

    /* Encoder rate-control frame. */
    GtkWidget *enc_frame = gtk_frame_new("Encoder rate control");
    gtk_widget_set_hexpand(enc_frame, TRUE);
    gtk_box_append(GTK_BOX(page), enc_frame);
    ctx->sidecar_encoder_label = GTK_LABEL(gtk_label_new(""));
    gtk_label_set_xalign(ctx->sidecar_encoder_label, 0.0);
    gtk_label_set_wrap(ctx->sidecar_encoder_label, TRUE);
    gtk_label_set_selectable(ctx->sidecar_encoder_label, TRUE);
    gtk_widget_add_css_class(GTK_WIDGET(ctx->sidecar_encoder_label), "uv-source-detail");
    gtk_widget_set_margin_top(GTK_WIDGET(ctx->sidecar_encoder_label), 6);
    gtk_widget_set_margin_bottom(GTK_WIDGET(ctx->sidecar_encoder_label), 6);
    gtk_widget_set_margin_start(GTK_WIDGET(ctx->sidecar_encoder_label), 8);
    gtk_widget_set_margin_end(GTK_WIDGET(ctx->sidecar_encoder_label), 8);
    gtk_frame_set_child(GTK_FRAME(enc_frame), GTK_WIDGET(ctx->sidecar_encoder_label));

    /* Counters frame. */
    GtkWidget *counters_frame = gtk_frame_new("Lifetime counters");
    gtk_widget_set_hexpand(counters_frame, TRUE);
    gtk_box_append(GTK_BOX(page), counters_frame);
    ctx->sidecar_counters_label = GTK_LABEL(gtk_label_new(""));
    gtk_label_set_xalign(ctx->sidecar_counters_label, 0.0);
    gtk_label_set_wrap(ctx->sidecar_counters_label, TRUE);
    gtk_label_set_selectable(ctx->sidecar_counters_label, TRUE);
    gtk_widget_add_css_class(GTK_WIDGET(ctx->sidecar_counters_label), "uv-source-detail");
    gtk_widget_set_margin_top(GTK_WIDGET(ctx->sidecar_counters_label), 6);
    gtk_widget_set_margin_bottom(GTK_WIDGET(ctx->sidecar_counters_label), 6);
    gtk_widget_set_margin_start(GTK_WIDGET(ctx->sidecar_counters_label), 8);
    gtk_widget_set_margin_end(GTK_WIDGET(ctx->sidecar_counters_label), 8);
    gtk_frame_set_child(GTK_FRAME(counters_frame), GTK_WIDGET(ctx->sidecar_counters_label));

    /* Encoder transport queue frame. */
    GtkWidget *trans_frame = gtk_frame_new("Encoder transport queue");
    gtk_widget_set_hexpand(trans_frame, TRUE);
    gtk_widget_set_tooltip_text(trans_frame,
                                "Optional encoder-side queue snapshot. Only some waybeam_venc "
                                "transports emit this trailer.");
    gtk_box_append(GTK_BOX(page), trans_frame);
    ctx->sidecar_transport_label = GTK_LABEL(gtk_label_new(""));
    gtk_label_set_xalign(ctx->sidecar_transport_label, 0.0);
    gtk_label_set_wrap(ctx->sidecar_transport_label, TRUE);
    gtk_label_set_selectable(ctx->sidecar_transport_label, TRUE);
    gtk_widget_add_css_class(GTK_WIDGET(ctx->sidecar_transport_label), "uv-source-detail");
    gtk_widget_set_margin_top(GTK_WIDGET(ctx->sidecar_transport_label), 6);
    gtk_widget_set_margin_bottom(GTK_WIDGET(ctx->sidecar_transport_label), 6);
    gtk_widget_set_margin_start(GTK_WIDGET(ctx->sidecar_transport_label), 8);
    gtk_widget_set_margin_end(GTK_WIDGET(ctx->sidecar_transport_label), 8);
    gtk_frame_set_child(GTK_FRAME(trans_frame), GTK_WIDGET(ctx->sidecar_transport_label));

    /* Prime widget state from the current config. */
    if (ctx->sidecar_port_spin) {
        guint port = ctx->current_cfg.sidecar_port ? ctx->current_cfg.sidecar_port : 5602;
        gtk_spin_button_set_value(ctx->sidecar_port_spin, port);
    }
    if (ctx->sidecar_enable_toggle) {
        ctx->sidecar_toggle_suppress = TRUE;
        gtk_toggle_button_set_active(ctx->sidecar_enable_toggle, ctx->current_cfg.sidecar_enabled);
        ctx->sidecar_toggle_suppress = FALSE;
        update_sidecar_toggle_label(ctx, ctx->current_cfg.sidecar_enabled);
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
    ctx->frame_distribution_stats_label = NULL;
    ctx->frame_distribution_area = NULL;
    ctx->frame_overlay_range_dropdown = NULL;
    ctx->frame_overlay_values_toggle = NULL;

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

    const char *width_labels[] = {"30", "60", "90", "100", "120", NULL};
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

    const char *metric_labels[] = {"Lateness (ms)", "Size (KB)", "Span (ms)",
                                   "Frame split (chunks/frame)", "Burst overlap (frames/chunk)", NULL};
    GtkWidget *metric_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *metric_label = gtk_label_new("Metric");
    gtk_widget_set_valign(metric_label, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(metric_box), metric_label);
    ctx->frame_block_metric_dropdown = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(metric_labels));
    if (ctx->frame_block_view >= FRAME_BLOCK_VIEW_COUNT) {
        ctx->frame_block_view = FRAME_BLOCK_VIEW_LATENESS;
    }
    gtk_drop_down_set_selected(ctx->frame_block_metric_dropdown, ctx->frame_block_view);
    g_signal_connect(ctx->frame_block_metric_dropdown, "notify::selected", G_CALLBACK(on_frame_block_metric_changed), ctx);
    gtk_box_append(GTK_BOX(metric_box), GTK_WIDGET(ctx->frame_block_metric_dropdown));
    gtk_box_append(GTK_BOX(controls), metric_box);

    const char *range_labels[] = {"15s", "30s", "60s", NULL};
    GtkWidget *range_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *range_label = gtk_label_new("Timeline Range");
    gtk_widget_set_valign(range_label, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(range_box), range_label);
    ctx->frame_overlay_range_dropdown = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(range_labels));
    double configured_range = ctx->frame_overlay_range_seconds > 0.0 ? ctx->frame_overlay_range_seconds : 60.0;
    guint range_index = 2;
    if (configured_range <= 20.0) {
        range_index = 0;
    } else if (configured_range <= 45.0) {
        range_index = 1;
    }
    gtk_drop_down_set_selected(ctx->frame_overlay_range_dropdown, range_index);
    g_signal_connect(ctx->frame_overlay_range_dropdown, "notify::selected", G_CALLBACK(on_frame_overlay_range_changed), ctx);
    gtk_box_append(GTK_BOX(range_box), GTK_WIDGET(ctx->frame_overlay_range_dropdown));
    gtk_box_append(GTK_BOX(controls), range_box);

    ctx->frame_overlay_values_toggle = GTK_CHECK_BUTTON(gtk_check_button_new_with_label("Show Values"));
    gtk_check_button_set_active(ctx->frame_overlay_values_toggle, ctx->frame_overlay_show_values);
    g_signal_connect(ctx->frame_overlay_values_toggle, "toggled", G_CALLBACK(on_frame_overlay_values_toggled), ctx);
    gtk_box_append(GTK_BOX(controls), GTK_WIDGET(ctx->frame_overlay_values_toggle));

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

    GtkWidget *distribution_frame = gtk_frame_new(NULL);
    gtk_widget_set_hexpand(distribution_frame, TRUE);
    gtk_widget_set_vexpand(distribution_frame, FALSE);

    GtkWidget *distribution_label_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *distribution_title = gtk_label_new("Frame Distribution (Normal)");
    gtk_label_set_xalign(GTK_LABEL(distribution_title), 0.0);
    gtk_widget_set_hexpand(distribution_title, TRUE);
    gtk_box_append(GTK_BOX(distribution_label_box), distribution_title);

    const char *dist_unit = (ctx->frame_block_view == FRAME_BLOCK_VIEW_SIZE) ? "KB" : "ms";
    char default_stats[128];
    g_snprintf(default_stats, sizeof(default_stats), "μ: -- %s | σ: -- %s | n: 0", dist_unit, dist_unit);
    GtkWidget *distribution_stats = gtk_label_new(default_stats);
    gtk_label_set_xalign(GTK_LABEL(distribution_stats), 1.0);
    gtk_widget_set_hexpand(distribution_stats, TRUE);
    gtk_label_set_single_line_mode(GTK_LABEL(distribution_stats), TRUE);
    gtk_label_set_width_chars(GTK_LABEL(distribution_stats), 56);
    gtk_label_set_max_width_chars(GTK_LABEL(distribution_stats), 56);
    gtk_label_set_ellipsize(GTK_LABEL(distribution_stats), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class(distribution_stats, "numeric");
    gtk_box_append(GTK_BOX(distribution_label_box), distribution_stats);

    gtk_frame_set_label_widget(GTK_FRAME(distribution_frame), distribution_label_box);

    GtkWidget *distribution_area = gtk_drawing_area_new();
    ctx->frame_distribution_area = GTK_DRAWING_AREA(distribution_area);
    gtk_widget_set_size_request(distribution_area, 480, 280);
    gtk_widget_set_hexpand(distribution_area, TRUE);
    gtk_widget_set_vexpand(distribution_area, FALSE);
    gtk_drawing_area_set_draw_func(ctx->frame_distribution_area, frame_distribution_draw, ctx, NULL);
    gtk_frame_set_child(GTK_FRAME(distribution_frame), distribution_area);
    gtk_box_append(GTK_BOX(content), distribution_frame);
    ctx->frame_distribution_stats_label = GTK_LABEL(distribution_stats);

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
    gtk_label_set_single_line_mode(GTK_LABEL(lateness_live), TRUE);
    gtk_label_set_width_chars(GTK_LABEL(lateness_live), 18);
    gtk_label_set_max_width_chars(GTK_LABEL(lateness_live), 18);
    gtk_widget_add_css_class(lateness_live, "numeric");
    gtk_box_append(GTK_BOX(lateness_label_box), lateness_live);

    GtkWidget *lateness_max = gtk_label_new("Max: --");
    gtk_label_set_xalign(GTK_LABEL(lateness_max), 1.0);
    gtk_widget_set_valign(lateness_max, GTK_ALIGN_CENTER);
    gtk_label_set_single_line_mode(GTK_LABEL(lateness_max), TRUE);
    gtk_label_set_width_chars(GTK_LABEL(lateness_max), 18);
    gtk_label_set_max_width_chars(GTK_LABEL(lateness_max), 18);
    gtk_widget_add_css_class(lateness_max, "numeric");
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
    gtk_label_set_single_line_mode(GTK_LABEL(size_live), TRUE);
    gtk_label_set_width_chars(GTK_LABEL(size_live), 18);
    gtk_label_set_max_width_chars(GTK_LABEL(size_live), 18);
    gtk_widget_add_css_class(size_live, "numeric");
    gtk_box_append(GTK_BOX(size_label_box), size_live);

    GtkWidget *size_max = gtk_label_new("Max: --");
    gtk_label_set_xalign(GTK_LABEL(size_max), 1.0);
    gtk_widget_set_valign(size_max, GTK_ALIGN_CENTER);
    gtk_label_set_single_line_mode(GTK_LABEL(size_max), TRUE);
    gtk_label_set_width_chars(GTK_LABEL(size_max), 18);
    gtk_label_set_max_width_chars(GTK_LABEL(size_max), 18);
    gtk_widget_add_css_class(size_max, "numeric");
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
        uv_viewer_frame_block_set_span_thresholds(ctx->viewer,
                                                 ctx->frame_block_thresholds_span[0],
                                                 ctx->frame_block_thresholds_span[1],
                                                 ctx->frame_block_thresholds_span[2]);
        uv_viewer_frame_block_set_chunk_thresholds(ctx->viewer,
                                                  ctx->frame_block_thresholds_chunks[0],
                                                  ctx->frame_block_thresholds_chunks[1],
                                                  ctx->frame_block_thresholds_chunks[2]);
        uv_viewer_frame_block_set_overlap_thresholds(ctx->viewer,
                                                    ctx->frame_block_thresholds_fpc[0],
                                                    ctx->frame_block_thresholds_fpc[1],
                                                    ctx->frame_block_thresholds_fpc[2]);
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

static GtkWidget *build_frame_release_page(GuiContext *ctx) {
    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_top(content, 12);
    gtk_widget_set_margin_start(content, 12);
    gtk_widget_set_margin_end(content, 12);
    gtk_widget_set_margin_bottom(content, 12);

    /* Controls row. */
    GtkWidget *controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(content), controls);

    ctx->frame_release_enable_toggle = GTK_TOGGLE_BUTTON(gtk_toggle_button_new_with_label("Enable Capture"));
    g_signal_connect(ctx->frame_release_enable_toggle, "toggled", G_CALLBACK(on_frame_release_enable_toggled), ctx);
    gtk_box_append(GTK_BOX(controls), GTK_WIDGET(ctx->frame_release_enable_toggle));

    ctx->frame_release_pause_toggle = GTK_TOGGLE_BUTTON(gtk_toggle_button_new_with_label("Pause"));
    gtk_widget_set_sensitive(GTK_WIDGET(ctx->frame_release_pause_toggle), FALSE);
    g_signal_connect(ctx->frame_release_pause_toggle, "toggled", G_CALLBACK(on_frame_release_pause_toggled), ctx);
    gtk_box_append(GTK_BOX(controls), GTK_WIDGET(ctx->frame_release_pause_toggle));

    ctx->frame_release_reset_button = GTK_BUTTON(gtk_button_new_with_label("Reset"));
    gtk_widget_set_sensitive(GTK_WIDGET(ctx->frame_release_reset_button), FALSE);
    g_signal_connect(ctx->frame_release_reset_button, "clicked", G_CALLBACK(on_frame_release_reset_clicked), ctx);
    gtk_box_append(GTK_BOX(controls), GTK_WIDGET(ctx->frame_release_reset_button));

    GtkWidget *gap_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *gap_label = gtk_label_new("Gap (µs)");
    gtk_widget_set_valign(gap_label, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(gap_box), gap_label);
    ctx->frame_release_gap_spin = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(50.0, 20000.0, 50.0));
    gtk_spin_button_set_digits(ctx->frame_release_gap_spin, 0);
    gtk_spin_button_set_value(ctx->frame_release_gap_spin,
                              ctx->frame_release_gap_us > 0.0 ? ctx->frame_release_gap_us : 500.0);
    gtk_widget_set_tooltip_text(GTK_WIDGET(ctx->frame_release_gap_spin),
        "Idle inter-arrival gap that separates one FEC release burst from the next. "
        "Packets closer together than this are one burst; a longer silence starts a "
        "new one. Too low fragments a single burst into fake chunks (overlap detection "
        "stops working); too high merges adjacent bursts and hides real cross-frame "
        "overlap. The sweet spot sits between the intra-burst packet spacing (tens of "
        "µs) and the inter-burst spacing (hundreds of µs to ms) — it scales with link "
        "rate / MCS, so press Auto to measure it for the current stream.");
    g_signal_connect(ctx->frame_release_gap_spin, "value-changed", G_CALLBACK(on_frame_release_gap_changed), ctx);
    gtk_box_append(GTK_BOX(gap_box), GTK_WIDGET(ctx->frame_release_gap_spin));

    ctx->frame_release_calib_button = GTK_BUTTON(gtk_button_new_with_label("Auto"));
    gtk_widget_set_sensitive(GTK_WIDGET(ctx->frame_release_calib_button), FALSE);
    gtk_widget_set_tooltip_text(GTK_WIDGET(ctx->frame_release_calib_button),
        "Measure a good Gap (µs) for the current stream: samples ~1 s of packet "
        "arrival timing and splits the intra-burst vs inter-burst clusters, placing "
        "the gap in the valley between them. Applies the result automatically when "
        "the stream is clearly bursty; leaves the gap untouched if no burst pattern "
        "is found (e.g. an evenly paced sender).");
    g_signal_connect(ctx->frame_release_calib_button, "clicked",
                     G_CALLBACK(on_frame_release_calib_clicked), ctx);
    gtk_box_append(GTK_BOX(gap_box), GTK_WIDGET(ctx->frame_release_calib_button));

    ctx->frame_release_calib_label = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(ctx->frame_release_calib_label), "dim-label");
    gtk_label_set_xalign(ctx->frame_release_calib_label, 0.0);
    gtk_box_append(GTK_BOX(gap_box), GTK_WIDGET(ctx->frame_release_calib_label));
    gtk_box_append(GTK_BOX(controls), gap_box);

    const char *window_labels[] = {"1 s", "2 s", "5 s", "10 s", NULL};
    GtkWidget *window_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *window_label = gtk_label_new("Follow window");
    gtk_widget_set_valign(window_label, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(window_label,
        "How much history the detail shows while Follow latest is on; drag/scroll "
        "the detail or drag the overview to inspect a fixed slice.");
    gtk_box_append(GTK_BOX(window_box), window_label);
    ctx->frame_release_window_dropdown = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(window_labels));
    guint window_index = 1; /* default 2 s */
    switch (ctx->frame_release_window_s) {
        case 1u:  window_index = 0; break;
        case 5u:  window_index = 2; break;
        case 10u: window_index = 3; break;
        case 2u:
        default:  window_index = 1; break;
    }
    gtk_drop_down_set_selected(ctx->frame_release_window_dropdown, window_index);
    g_signal_connect(ctx->frame_release_window_dropdown, "notify::selected",
                     G_CALLBACK(on_frame_release_window_changed), ctx);
    gtk_box_append(GTK_BOX(window_box), GTK_WIDGET(ctx->frame_release_window_dropdown));
    gtk_box_append(GTK_BOX(controls), window_box);

    ctx->frame_release_follow_toggle =
        GTK_TOGGLE_BUTTON(gtk_toggle_button_new_with_label("Follow latest"));
    gtk_toggle_button_set_active(ctx->frame_release_follow_toggle, ctx->frame_release_follow);
    g_signal_connect(ctx->frame_release_follow_toggle, "toggled",
                     G_CALLBACK(on_frame_release_follow_toggled), ctx);
    gtk_box_append(GTK_BOX(controls), GTK_WIDGET(ctx->frame_release_follow_toggle));

    GtkWidget *help = gtk_label_new("?");
    gtk_widget_add_css_class(help, "dim-label");
    gtk_widget_set_tooltip_text(help,
        "wfb-ng releases RTP packets in Reed-Solomon FEC-block bursts (\"chunks\"). A burst "
        "boundary does not align to frame boundaries, so one burst can carry the tail of frame "
        "N plus the head of frame N+1. Those cross-frame bursts (red, multi-stripe) are the ones "
        "that turn into downstream frame drops. Gap (µs) is the idle threshold that separates "
        "one burst from the next — press Auto to measure it for the current stream instead of "
        "guessing. A cross-frame burst is marked on the later frame.");
    gtk_box_append(GTK_BOX(controls), help);

    /* Overview health strip (whole capture, drag to select). */
    GtkWidget *overview_frame = gtk_frame_new(NULL);
    gtk_widget_set_hexpand(overview_frame, TRUE);
    gtk_widget_set_vexpand(overview_frame, FALSE);

    GtkWidget *overview_title = gtk_label_new("Overview (whole capture — drag to select)");
    gtk_label_set_xalign(GTK_LABEL(overview_title), 0.0);
    gtk_frame_set_label_widget(GTK_FRAME(overview_frame), overview_title);

    GtkWidget *overview_area = gtk_drawing_area_new();
    ctx->frame_release_overview_area = GTK_DRAWING_AREA(overview_area);
    gtk_widget_set_size_request(overview_area, 900, 46);
    gtk_widget_set_hexpand(overview_area, TRUE);
    gtk_widget_set_vexpand(overview_area, FALSE);
    gtk_drawing_area_set_draw_func(ctx->frame_release_overview_area, frame_release_overview_draw, ctx, NULL);

    GtkGesture *overview_drag = gtk_gesture_drag_new();
    g_signal_connect(overview_drag, "drag-begin",
                     G_CALLBACK(on_frame_release_overview_drag_begin), ctx);
    g_signal_connect(overview_drag, "drag-update",
                     G_CALLBACK(on_frame_release_overview_drag_update), ctx);
    gtk_widget_add_controller(overview_area, GTK_EVENT_CONTROLLER(overview_drag));

    gtk_frame_set_child(GTK_FRAME(overview_frame), overview_area);
    gtk_box_append(GTK_BOX(content), overview_frame);

    /* Detail (selected range) — single-lane Gantt of the selection. */
    GtkWidget *timeline_frame = gtk_frame_new(NULL);
    gtk_widget_set_hexpand(timeline_frame, TRUE);
    gtk_widget_set_vexpand(timeline_frame, TRUE);

    GtkWidget *timeline_title = gtk_label_new("Detail (selected range)");
    gtk_label_set_xalign(GTK_LABEL(timeline_title), 0.0);
    gtk_frame_set_label_widget(GTK_FRAME(timeline_frame), timeline_title);

    GtkWidget *timeline_area = gtk_drawing_area_new();
    ctx->frame_release_timeline_area = GTK_DRAWING_AREA(timeline_area);
    gtk_widget_set_size_request(timeline_area, 900, 260);
    gtk_widget_set_hexpand(timeline_area, TRUE);
    gtk_widget_set_vexpand(timeline_area, TRUE);
    gtk_drawing_area_set_draw_func(ctx->frame_release_timeline_area, frame_release_timeline_draw, ctx, NULL);

    /* U2: hover readout. */
    GtkEventController *timeline_motion = gtk_event_controller_motion_new();
    g_signal_connect(timeline_motion, "motion",
                     G_CALLBACK(on_frame_release_timeline_motion), ctx);
    g_signal_connect(timeline_motion, "leave",
                     G_CALLBACK(on_frame_release_timeline_leave), ctx);
    gtk_widget_add_controller(timeline_area, timeline_motion);

    /* U4: scroll-to-zoom. */
    GtkEventController *timeline_scroll =
        gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    g_signal_connect(timeline_scroll, "scroll",
                     G_CALLBACK(on_frame_release_timeline_scroll), ctx);
    gtk_widget_add_controller(timeline_area, timeline_scroll);

    /* U4: drag-to-pan. */
    GtkGesture *timeline_pan = gtk_gesture_drag_new();
    g_signal_connect(timeline_pan, "drag-begin",
                     G_CALLBACK(on_frame_release_timeline_pan_begin), ctx);
    g_signal_connect(timeline_pan, "drag-update",
                     G_CALLBACK(on_frame_release_timeline_pan_update), ctx);
    gtk_widget_add_controller(timeline_area, GTK_EVENT_CONTROLLER(timeline_pan));

    gtk_frame_set_child(GTK_FRAME(timeline_frame), timeline_area);
    gtk_box_append(GTK_BOX(content), timeline_frame);

    /* Histogram of frames-per-chunk. */
    GtkWidget *hist_frame = gtk_frame_new(NULL);
    gtk_widget_set_hexpand(hist_frame, FALSE);
    gtk_widget_set_vexpand(hist_frame, FALSE);

    GtkWidget *hist_title = gtk_label_new("Frames per chunk");
    gtk_label_set_xalign(GTK_LABEL(hist_title), 0.0);
    gtk_frame_set_label_widget(GTK_FRAME(hist_frame), hist_title);

    GtkWidget *hist_area = gtk_drawing_area_new();
    ctx->frame_release_hist_area = GTK_DRAWING_AREA(hist_area);
    gtk_widget_set_size_request(hist_area, 300, 140);
    gtk_widget_set_hexpand(hist_area, FALSE);
    gtk_widget_set_vexpand(hist_area, FALSE);
    gtk_widget_set_halign(hist_area, GTK_ALIGN_START);
    gtk_drawing_area_set_draw_func(ctx->frame_release_hist_area, frame_release_hist_draw, ctx, NULL);
    gtk_frame_set_child(GTK_FRAME(hist_frame), hist_area);
    gtk_box_append(GTK_BOX(content), hist_frame);

    /* Summary label. */
    ctx->frame_release_summary_label = GTK_LABEL(gtk_label_new("Release: Inactive"));
    gtk_label_set_xalign(ctx->frame_release_summary_label, 0.0);
    gtk_label_set_wrap(ctx->frame_release_summary_label, TRUE);
    gtk_widget_set_hexpand(GTK_WIDGET(ctx->frame_release_summary_label), TRUE);
    gtk_box_append(GTK_BOX(content), GTK_WIDGET(ctx->frame_release_summary_label));

    frame_release_update_summary(ctx);

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
        case 0: seconds = 30.0; break;
        case 1: seconds = 60.0; break;
        case 2: seconds = 300.0; break;
        case 3: seconds = 600.0; break;
        default: break;
    }
    ctx->stats_range_seconds = seconds;
    update_stats_metric_labels(ctx);
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

    double max_val = -G_MAXDOUBLE;
    double sum_val = 0.0;
    guint  sum_count = 0;
    for (guint i = start_index; i < len; i++) {
        double v = stats_metric_value(&samples[i], metric);
        if (!isfinite(v)) continue;
        if (v > max_val) max_val = v;
        sum_val += v;
        sum_count++;
    }

    if (max_val == -G_MAXDOUBLE) {
        cairo_set_source_rgb(cr, 0.7, 0.7, 0.7);
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 12.0);
        cairo_move_to(cr, 10, height / 2.0);
        cairo_show_text(cr, "No samples in range");
        cairo_restore(cr);
        return;
    }

    double axis_min = 0.0;
    double axis_max = nice_axis_max(max_val > 0.0 ? max_val : 1.0);

    const double left_margin = 64.0;
    const double right_margin = 12.0;
    const double top_margin = 12.0;
    const double bottom_margin = 28.0;
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
            g_snprintf(label, sizeof(label), "%.1f", value / 1e6);
        } else if (axis_max >= 100.0) {
            g_snprintf(label, sizeof(label), "%.0f", value);
        } else {
            g_snprintf(label, sizeof(label), "%.1f", value);
        }

        cairo_text_extents_t label_extents;
        cairo_text_extents(cr, label, &label_extents);
        double text_x = plot_left - 8.0 - (label_extents.width + label_extents.x_bearing);
        double text_y = y + (label_extents.height / 2.0) - label_extents.y_bearing;
        cairo_move_to(cr, text_x, text_y);
        cairo_show_text(cr, label);
    }

    /* X-axis time labels: rightmost is "now", others step back in seconds.
     * Using fixed labels (not relative to the latest sample) keeps the
     * positions stable across redraws. */
    cairo_set_source_rgba(cr, 0.7, 0.7, 0.75, 0.85);
    cairo_set_font_size(cr, 10.0);
    const int x_tick_count = 4;
    for (int i = 0; i <= x_tick_count; i++) {
        double fraction = (double)i / (double)x_tick_count;
        double x = plot_left + fraction * plot_width;
        double secs_ago = range * (1.0 - fraction);
        char tlabel[24];
        if (secs_ago < 1.0) {
            g_strlcpy(tlabel, "now", sizeof(tlabel));
        } else if (secs_ago >= 60.0) {
            g_snprintf(tlabel, sizeof(tlabel), "-%.0fm", secs_ago / 60.0);
        } else {
            g_snprintf(tlabel, sizeof(tlabel), "-%.0fs", secs_ago);
        }
        cairo_text_extents_t te;
        cairo_text_extents(cr, tlabel, &te);
        double tx = x - te.width * 0.5 - te.x_bearing;
        if (tx < plot_left) tx = plot_left;
        if (tx + te.width > plot_right) tx = plot_right - te.width;
        cairo_move_to(cr, tx, plot_bottom + 14.0);
        cairo_show_text(cr, tlabel);
    }

    /* Average line — faint, dashed. */
    if (sum_count > 1) {
        double avg = sum_val / (double)sum_count;
        double y_ratio = (avg - axis_min) / (axis_max - axis_min);
        if (y_ratio >= 0.0 && y_ratio <= 1.0) {
            double y = plot_bottom - y_ratio * plot_height;
            cairo_save(cr);
            double dashes[] = {3.0, 4.0};
            cairo_set_dash(cr, dashes, 2, 0);
            cairo_set_line_width(cr, 1.0);
            cairo_set_source_rgba(cr, 1.0, 0.85, 0.4, 0.6);
            cairo_move_to(cr, plot_left, y);
            cairo_line_to(cr, plot_right, y);
            cairo_stroke(cr);
            cairo_restore(cr);
        }
    }

    /* Data path. */
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
        if (!isfinite(value)) { path_started = FALSE; continue; }
        double y_ratio = (value - axis_min) / (axis_max - axis_min);
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

    cairo_restore(cr);
}

static gboolean accel_request_idr(GtkWidget *widget, GVariant *args, gpointer user_data) {
    (void)widget;
    (void)args;
    on_idr_button_clicked(NULL, user_data);
    return TRUE;
}

static gboolean accel_restart(GtkWidget *widget, GVariant *args, gpointer user_data) {
    (void)widget;
    (void)args;
    on_refresh_button_clicked(NULL, user_data);
    return TRUE;
}

static gboolean accel_next_source(GtkWidget *widget, GVariant *args, gpointer user_data) {
    (void)widget;
    (void)args;
    on_next_button_clicked(NULL, user_data);
    return TRUE;
}

static void install_window_accelerators(GuiContext *ctx, GtkWidget *window) {
    GtkShortcutController *controller = GTK_SHORTCUT_CONTROLLER(gtk_shortcut_controller_new());
    gtk_shortcut_controller_set_scope(controller, GTK_SHORTCUT_SCOPE_GLOBAL);

    gtk_shortcut_controller_add_shortcut(controller,
        gtk_shortcut_new(gtk_shortcut_trigger_parse_string("<Control>i"),
                         gtk_callback_action_new(accel_request_idr, ctx, NULL)));
    gtk_shortcut_controller_add_shortcut(controller,
        gtk_shortcut_new(gtk_shortcut_trigger_parse_string("<Control>r"),
                         gtk_callback_action_new(accel_restart, ctx, NULL)));
    gtk_shortcut_controller_add_shortcut(controller,
        gtk_shortcut_new(gtk_shortcut_trigger_parse_string("<Control>n"),
                         gtk_callback_action_new(accel_next_source, ctx, NULL)));

    gtk_widget_add_controller(window, GTK_EVENT_CONTROLLER(controller));
}

static void build_ui(GuiContext *ctx) {
    install_app_css();

    GtkWidget *window = gtk_application_window_new(ctx->app);
    ctx->window = GTK_WINDOW(window);
    gtk_window_set_title(ctx->window, "UDP H.265 Viewer");
    gtk_window_set_default_size(ctx->window, 1080, 760);
    gtk_window_set_resizable(ctx->window, TRUE);
    g_signal_connect(window, "close-request", G_CALLBACK(on_window_close_request), ctx);

    install_window_accelerators(ctx, window);

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

    GtkWidget *release_page = build_frame_release_page(ctx);
    gtk_notebook_append_page(ctx->notebook, release_page, gtk_label_new("Frame Release"));

    GtkWidget *audio_page = build_audio_page(ctx);
    gtk_notebook_append_page(ctx->notebook, audio_page, gtk_label_new("Audio"));

    GtkWidget *sidecar_page = build_sidecar_page(ctx);
    gtk_notebook_append_page(ctx->notebook, sidecar_page, gtk_label_new("Sidecar"));

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
    } else if (page_num == 4) {
        if (ctx->frame_release_timeline_area) {
            gtk_widget_queue_draw(GTK_WIDGET(ctx->frame_release_timeline_area));
        }
        if (ctx->frame_release_hist_area) {
            gtk_widget_queue_draw(GTK_WIDGET(ctx->frame_release_hist_area));
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
    ctx->audio_port_mode_dropdown = NULL;
    ctx->audio_port_spin = NULL;
    ctx->audio_status_label = NULL;
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
    if (ctx->frame_block_values_span) {
        g_array_free(ctx->frame_block_values_span, TRUE);
        ctx->frame_block_values_span = NULL;
    }
    if (ctx->frame_block_values_chunks) {
        g_array_free(ctx->frame_block_values_chunks, TRUE);
        ctx->frame_block_values_chunks = NULL;
    }
    if (ctx->frame_block_values_fpc) {
        g_array_free(ctx->frame_block_values_fpc, TRUE);
        ctx->frame_block_values_fpc = NULL;
    }
    if (ctx->frame_release_chunks) {
        g_array_free(ctx->frame_release_chunks, TRUE);
        ctx->frame_release_chunks = NULL;
    }
    if (ctx->frame_release_frames) {
        g_array_free(ctx->frame_release_frames, TRUE);
        ctx->frame_release_frames = NULL;
    }
    ctx->frame_release_enable_toggle = NULL;
    ctx->frame_release_pause_toggle = NULL;
    ctx->frame_release_reset_button = NULL;
    ctx->frame_release_gap_spin = NULL;
    ctx->frame_release_calib_button = NULL;
    ctx->frame_release_calib_label = NULL;
    ctx->frame_release_calib_pending = FALSE;
    ctx->frame_release_calib_seq = 0;
    ctx->frame_release_window_dropdown = NULL;
    ctx->frame_release_follow_toggle = NULL;
    ctx->frame_release_overview_area = NULL;
    ctx->frame_release_timeline_area = NULL;
    ctx->frame_release_hist_area = NULL;
    ctx->frame_release_summary_label = NULL;
    ctx->frame_block_area = NULL;
    ctx->frame_overlay_lateness = NULL;
    ctx->frame_overlay_size = NULL;
    ctx->frame_distribution_area = NULL;
    for (int i = 0; i < 2; i++) {
        ctx->frame_overlay_live_labels[i] = NULL;
        ctx->frame_overlay_max_labels[i] = NULL;
    }
    ctx->frame_distribution_stats_label = NULL;
    ctx->frame_block_enable_toggle = NULL;
    ctx->frame_block_pause_toggle = NULL;
    ctx->frame_block_mode_dropdown = NULL;
    ctx->frame_block_width_dropdown = NULL;
    ctx->frame_block_metric_dropdown = NULL;
    ctx->frame_overlay_range_dropdown = NULL;
    ctx->frame_overlay_values_toggle = NULL;
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
    ctx->idr_button = NULL;
    ctx->idr_port_spin = NULL;
    ctx->stats_pause_toggle = NULL;
    ctx->stream_detail_label = NULL;
    ctx->sidecar_enable_toggle = NULL;
    ctx->sidecar_port_spin = NULL;
    ctx->sidecar_status_label = NULL;
    ctx->sidecar_frame_label = NULL;
    ctx->sidecar_encoder_label = NULL;
    ctx->sidecar_counters_label = NULL;
    ctx->sidecar_transport_label = NULL;
    ctx->restream_toggle = NULL;
    ctx->restream_host_entry = NULL;
    ctx->restream_port_spin = NULL;
    ctx->restream_status_label = NULL;
    ctx->restream_toggle_suppress = FALSE;
    ctx->shm_toggle = NULL;
    ctx->shm_name_entry = NULL;
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
    ctx->frame_overlay_range_seconds = 60.0;
    ctx->frame_overlay_show_values = FALSE;
    ctx->frame_overlay_needs_refresh = FALSE;
    ctx->audio_runtime_enabled = cfg->audio_enabled;
    ctx->audio_active = FALSE;
    ctx->frame_block_thresholds_ms[0] = FRAME_BLOCK_DEFAULT_GREEN_MS;
    ctx->frame_block_thresholds_ms[1] = FRAME_BLOCK_DEFAULT_YELLOW_MS;
    ctx->frame_block_thresholds_ms[2] = FRAME_BLOCK_DEFAULT_ORANGE_MS;
    ctx->frame_block_thresholds_kb[0] = FRAME_BLOCK_DEFAULT_SIZE_GREEN_KB;
    ctx->frame_block_thresholds_kb[1] = FRAME_BLOCK_DEFAULT_SIZE_YELLOW_KB;
    ctx->frame_block_thresholds_kb[2] = FRAME_BLOCK_DEFAULT_SIZE_ORANGE_KB;
    ctx->frame_block_thresholds_span[0] = FRAME_BLOCK_DEFAULT_SPAN_GREEN_MS;
    ctx->frame_block_thresholds_span[1] = FRAME_BLOCK_DEFAULT_SPAN_YELLOW_MS;
    ctx->frame_block_thresholds_span[2] = FRAME_BLOCK_DEFAULT_SPAN_ORANGE_MS;
    ctx->frame_block_thresholds_chunks[0] = FRAME_BLOCK_DEFAULT_CHUNK_GREEN;
    ctx->frame_block_thresholds_chunks[1] = FRAME_BLOCK_DEFAULT_CHUNK_YELLOW;
    ctx->frame_block_thresholds_chunks[2] = FRAME_BLOCK_DEFAULT_CHUNK_ORANGE;
    ctx->frame_block_thresholds_fpc[0] = FRAME_BLOCK_DEFAULT_FPC_GREEN;
    ctx->frame_block_thresholds_fpc[1] = FRAME_BLOCK_DEFAULT_FPC_YELLOW;
    ctx->frame_block_thresholds_fpc[2] = FRAME_BLOCK_DEFAULT_FPC_ORANGE;
    ctx->frame_block_width = FRAME_BLOCK_DEFAULT_WIDTH;
    ctx->frame_block_height = FRAME_BLOCK_DEFAULT_HEIGHT;
    ctx->frame_block_values_lateness = g_array_new(FALSE, TRUE, sizeof(double));
    ctx->frame_block_values_size = g_array_new(FALSE, TRUE, sizeof(double));
    ctx->frame_block_values_span = g_array_new(FALSE, TRUE, sizeof(double));
    ctx->frame_block_values_chunks = g_array_new(FALSE, TRUE, sizeof(double));
    ctx->frame_block_values_fpc = g_array_new(FALSE, TRUE, sizeof(double));
    ctx->frame_block_view = FRAME_BLOCK_VIEW_LATENESS;
    ctx->frame_release_chunks = g_array_new(FALSE, TRUE, sizeof(UvReleaseChunk));
    ctx->frame_release_frames = g_array_new(FALSE, TRUE, sizeof(UvReleaseFrame));
    ctx->frame_release_period_ms = 0.0;
    ctx->frame_release_window_s = 2u;
    ctx->frame_release_gap_us = 500.0;
    ctx->frame_release_follow = TRUE;
    ctx->frame_release_sel_start = 0;
    ctx->frame_release_sel_count = 0;
    ctx->frame_release_hover_idx = -1;
    ctx->frame_release_hover_px = 0.0;
    ctx->frame_release_pan_anchor_start = 0;
    ctx->frame_block_missing = 0;
    ctx->frame_block_real_samples = 0;
    ctx->known_source_count = 0;
    ctx->suppress_source_change = FALSE;
    ctx->pending_source_valid = FALSE;
    ctx->pending_source_index = GTK_INVALID_LIST_POSITION;
    ctx->active_source_valid = FALSE;
    ctx->active_source_index = GTK_INVALID_LIST_POSITION;
    ctx->preferred_source_valid = FALSE;
    ctx->preferred_source_address[0] = '\0';
    ctx->shm_recovery_timeout_id = 0;
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
