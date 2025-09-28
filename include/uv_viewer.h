#ifndef UV_VIEWER_H
#define UV_VIEWER_H

#include <glib.h>
#include <gst/gst.h>
#include <stdbool.h>
#include <stdint.h>

G_BEGIN_DECLS

#define UV_VIEWER_ADDR_MAX 64

typedef struct _UvViewer UvViewer;

typedef struct {
    int listen_port;   // UDP port to bind (default: 5600)
    int payload_type;  // RTP payload type (default: 97)
    int clock_rate;    // RTP clock rate (default: 90000)
    bool sync_to_clock; // TRUE to let sink sync to clock
    guint appsrc_queue_size; // future use; set 0 for default
    guint jitter_latency_ms; // jitter buffer latency window (default: 4)
    guint queue_max_buffers; // upstream queue max buffers (default: 96)
    gboolean jitter_drop_on_latency; // drop-late packets
    gboolean jitter_do_lost;         // emit lost events
    gboolean jitter_post_drop_messages; // post drop messages on bus
    gboolean videorate_enabled; // TRUE to insert videorate to enforce a target FPS
    guint videorate_fps_numerator; // target framerate numerator (default: 60)
    guint videorate_fps_denominator; // target framerate denominator (default: 1)
    gboolean audio_enabled; // TRUE to enable audio branch
    guint audio_payload_type; // RTP payload type carrying OPUS (default: 98)
    guint audio_clock_rate; // RTP clock rate for audio (default: 48000)
    guint audio_jitter_latency_ms; // jitter buffer latency for audio (default: 8)
} UvViewerConfig;

typedef struct {
    char address[UV_VIEWER_ADDR_MAX];
    bool selected;
    uint64_t rx_packets;
    uint64_t rx_bytes;
    uint64_t forwarded_packets;
    uint64_t forwarded_bytes;
    double inbound_bitrate_bps;
    uint64_t rtp_unique_packets;
    uint64_t rtp_expected_packets;
    uint64_t rtp_lost_packets;
    uint64_t rtp_duplicate_packets;
    uint64_t rtp_reordered_packets;
    double rfc3550_jitter_ms;
    double seconds_since_last_seen;
} UvSourceStats;

typedef struct {
    guint64 processed;
    guint64 dropped;
    guint64 events;
    gint64 last_jitter_ns;
    gint64 min_jitter_ns;
    gint64 max_jitter_ns;
    double average_abs_jitter_ns;
    double last_proportion;
    int last_quality;
    bool live;
} UvQoSStats;

typedef struct {
    char element_path[128];
    UvQoSStats stats;
} UvNamedQoSStats;

typedef struct {
    guint64 frames_total;
    double instantaneous_fps;
    double average_fps;
    char caps_str[128];
} UvDecoderStats;

typedef struct {
    gint current_level_buffers;
    guint current_level_bytes;
    double current_level_time_ms;
} UvQueueStats;

typedef struct {
    gboolean active;
    gboolean paused;
    gboolean snapshot_mode;
    gboolean snapshot_complete;
    guint width;
    guint height;
    guint filled;
    guint next_index;
    double thresholds_lateness_ms[3];
    double thresholds_size_kb[3];
    double min_lateness_ms;
    double max_lateness_ms;
    double avg_lateness_ms;
    double min_size_kb;
    double max_size_kb;
    double avg_size_kb;
    guint real_frames;
    guint missing_frames;
    guint color_counts_lateness[4];
    guint color_counts_size[4];
    GArray *lateness_ms;   // double values for current snapshot (size width*height)
    GArray *frame_size_kb; // double values for current snapshot (size width*height)
} UvFrameBlockStats;

typedef struct {
    GArray *sources;      // UvSourceStats elements
    GArray *qos_entries;  // UvNamedQoSStats elements
    UvDecoderStats decoder;
    gboolean audio_enabled;
    gboolean audio_active;
    gboolean queue0_valid;
    UvQueueStats queue0;
    gboolean frame_block_valid;
    UvFrameBlockStats frame_block;
} UvViewerStats;

typedef struct {
    const char *descriptive_name; // optional, e.g. "vah265dec"
    GstElement *custom_decoder;   // optional externally created element
} UvPipelineOverrides;

typedef enum {
    UV_VIEWER_EVENT_SOURCE_ADDED,
    UV_VIEWER_EVENT_SOURCE_REMOVED,
    UV_VIEWER_EVENT_SOURCE_SELECTED,
    UV_VIEWER_EVENT_PIPELINE_ERROR,
    UV_VIEWER_EVENT_SHUTDOWN
} UvViewerEventKind;

typedef struct {
    UvViewerEventKind kind;
    int source_index;
    UvSourceStats source_snapshot;
    GError *error; // owned by library, valid during callback only
} UvViewerEvent;

typedef void (*UvViewerEventCallback)(const UvViewerEvent *event, gpointer user_data);

void uv_viewer_config_init(UvViewerConfig *cfg);

UvViewer *uv_viewer_new(const UvViewerConfig *cfg);
void uv_viewer_free(UvViewer *viewer);

bool uv_viewer_start(UvViewer *viewer, GError **error);
void uv_viewer_stop(UvViewer *viewer);

void uv_viewer_set_event_callback(UvViewer *viewer, UvViewerEventCallback cb, gpointer user_data);

bool uv_viewer_select_source(UvViewer *viewer, int index, GError **error);
bool uv_viewer_select_next_source(UvViewer *viewer, GError **error);
int  uv_viewer_get_selected_source(const UvViewer *viewer);

bool uv_viewer_update_pipeline(UvViewer *viewer, const UvPipelineOverrides *overrides, GError **error);

void uv_viewer_frame_block_configure(UvViewer *viewer, gboolean enabled, gboolean snapshot_mode);
void uv_viewer_frame_block_pause(UvViewer *viewer, gboolean paused);
void uv_viewer_frame_block_reset(UvViewer *viewer);
void uv_viewer_frame_block_set_thresholds(UvViewer *viewer,
                                          double green_ms,
                                          double yellow_ms,
                                          double orange_ms);
void uv_viewer_frame_block_set_size_thresholds(UvViewer *viewer,
                                               double green_kb,
                                               double yellow_kb,
                                               double orange_kb);

void uv_viewer_stats_init(UvViewerStats *stats);
void uv_viewer_stats_clear(UvViewerStats *stats);
bool uv_viewer_get_stats(UvViewer *viewer, UvViewerStats *stats);

G_END_DECLS

#endif // UV_VIEWER_H
