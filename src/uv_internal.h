#ifndef UV_INTERNAL_H
#define UV_INTERNAL_H

#include "uv_viewer.h"

#include <gst/app/gstappsrc.h>
#include <arpa/inet.h>
#include <poll.h>
#include <signal.h>

G_BEGIN_DECLS

#define UV_RELAY_MAX_SOURCES 256
#define UV_RELAY_BUF_SIZE 65536
#define UV_RTP_WIN_SIZE 4096
#define UV_RTP_SLOT_EMPTY 0xffffffffu

struct UvFrameBlockState;

typedef struct {
    struct sockaddr_in addr;
    socklen_t addrlen;
    bool in_use;

    uint64_t rx_packets;
    uint64_t rx_bytes;
    uint64_t forwarded_packets;
    uint64_t forwarded_bytes;
    gint64   last_seen_us;

    uint64_t prev_bytes;
    gint64   prev_timestamp_us;

    bool     rtp_initialized;
    uint32_t rtp_cycles;
    uint16_t rtp_last_seq;
    uint32_t rtp_first_ext_seq;
    uint32_t rtp_max_ext_seq;
    uint64_t rtp_unique_packets;
    uint64_t rtp_duplicate_packets;
    uint64_t rtp_reordered_packets;
    uint32_t rtp_seq_slot[UV_RTP_WIN_SIZE];

    bool     jitter_initialized;
   uint32_t jitter_prev_transit;
    double   jitter_value;

    struct UvFrameBlockState *frame_block;
    uint64_t frame_block_accum_bytes;
} UvRelaySource;

typedef struct {
    int listen_port;
    GThread *thread;
    volatile sig_atomic_t running;
    volatile sig_atomic_t push_enabled;

    UvRelaySource sources[UV_RELAY_MAX_SOURCES];
    guint sources_count;
    int selected_index;

    GstAppSrc *appsrc;

    struct {
        gboolean enabled;
        gboolean paused;
        gboolean snapshot_mode;
        guint width;
        guint height;
        double thresholds_ms[3];
        double thresholds_kb[3];
        gboolean reset_requested;
        gboolean thresholds_dirty_ms;
        gboolean thresholds_dirty_kb;
    } frame_block;

    GMutex lock;
    struct _UvViewer *viewer;
} RelayController;

typedef struct {
    guint64 frames_total;
    gint64 first_frame_us;
    guint64 prev_frames;
    gint64 prev_timestamp_us;
    GMutex lock;
} DecoderStats;

typedef struct {
    guint64 processed;
    guint64 dropped;
    guint64 events;
    gint64  last_jitter_ns;
    gint64  min_jitter_ns;
    gint64  max_jitter_ns;
    long double sum_abs_jitter_ns;
    gdouble last_proportion;
    gint    last_quality;
    gboolean live;
} QoSStatsImpl;

typedef struct {
    GHashTable *table;
    GMutex lock;
} QoSDatabase;

typedef struct {
    int payload_type;
    int clock_rate;
    gboolean sync_to_clock;
    gboolean use_videorate;
    guint videorate_fps_num;
    guint videorate_fps_den;

    struct _UvViewer *viewer;

    GstElement *pipeline;
    GstElement *appsrc_element;
    GstElement *queue0;
    GstElement *jitterbuffer;
    GstElement *depay;
    GstElement *parser;
    GstElement *capsfilter;
    GstElement *decoder;
    GstElement *queue_postdec;
    GstElement *video_convert;
    GstElement *videorate;
    GstElement *videorate_caps;
    GstElement *queue_postrate;
    GstElement *sink;

    GThread *loop_thread;
    GMainLoop *loop;
    GMainContext *loop_context;
    guint bus_watch_id;
    gulong decoder_probe_id;
    gboolean sink_is_fakesink;
} PipelineController;

struct _UvViewer {
    UvViewerConfig config;

    RelayController relay;
    PipelineController pipeline;
    DecoderStats decoder;
    QoSDatabase qos;

    GMutex state_lock;
    gboolean started;
    gboolean shutting_down;

    UvViewerEventCallback event_cb;
    gpointer event_cb_data;
};

void uv_internal_decoder_stats_reset(DecoderStats *stats);
void uv_internal_decoder_stats_push_frame(DecoderStats *stats, gint64 now_us);

void uv_internal_qos_db_init(QoSDatabase *db);
void uv_internal_qos_db_clear(QoSDatabase *db);
void uv_internal_qos_db_update(QoSDatabase *db, GstMessage *msg);
void uv_internal_qos_db_snapshot(QoSDatabase *db, UvViewerStats *stats);

void uv_internal_emit_event(struct _UvViewer *viewer, UvViewerEventKind kind, int source_index, const UvRelaySource *source, GError *error);

gboolean relay_controller_init(RelayController *rc, struct _UvViewer *viewer);
void     relay_controller_deinit(RelayController *rc);
gboolean relay_controller_start(RelayController *rc);
void     relay_controller_stop(RelayController *rc);
gboolean relay_controller_select(RelayController *rc, int index, GError **error);
gboolean relay_controller_select_next(RelayController *rc, GError **error);
int      relay_controller_selected(const RelayController *rc);
void     relay_controller_snapshot(RelayController *rc, UvViewerStats *stats, int clock_rate);
void     relay_controller_set_appsrc(RelayController *rc, GstAppSrc *appsrc);
void     relay_controller_set_push_enabled(RelayController *rc, gboolean enabled);
void     relay_controller_frame_block_configure(RelayController *rc, gboolean enabled, gboolean snapshot_mode);
void     relay_controller_frame_block_pause(RelayController *rc, gboolean paused);
void     relay_controller_frame_block_reset(RelayController *rc);
void     relay_controller_frame_block_set_thresholds(RelayController *rc,
                                                     double green_ms,
                                                     double yellow_ms,
                                                     double orange_ms);
void     relay_controller_frame_block_set_size_thresholds(RelayController *rc,
                                                          double green_kb,
                                                          double yellow_kb,
                                                          double orange_kb);

gboolean pipeline_controller_init(PipelineController *pc, struct _UvViewer *viewer, GError **error);
void     pipeline_controller_deinit(PipelineController *pc);
gboolean pipeline_controller_start(PipelineController *pc, GError **error);
void     pipeline_controller_stop(PipelineController *pc);
GstAppSrc *pipeline_controller_get_appsrc(PipelineController *pc);
void     pipeline_controller_snapshot(PipelineController *pc, UvViewerStats *stats);
gboolean pipeline_controller_update(PipelineController *pc, const UvPipelineOverrides *overrides, GError **error);
GstElement *pipeline_controller_get_sink(PipelineController *pc);

GstElement *uv_internal_viewer_get_sink(struct _UvViewer *viewer);

void uv_log_info(const char *fmt, ...) G_GNUC_PRINTF(1, 2);
void uv_log_warn(const char *fmt, ...) G_GNUC_PRINTF(1, 2);
void uv_log_error(const char *fmt, ...) G_GNUC_PRINTF(1, 2);

G_END_DECLS

#endif // UV_INTERNAL_H
