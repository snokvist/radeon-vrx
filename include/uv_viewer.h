#ifndef UV_VIEWER_H
#define UV_VIEWER_H

#include <glib.h>
#include <gst/gst.h>
#include <stdbool.h>
#include <stdint.h>

G_BEGIN_DECLS

#define UV_VIEWER_ADDR_MAX 64

typedef struct _UvViewer UvViewer;

typedef enum {
    UV_DECODER_AUTO = 0,
    UV_DECODER_INTEL_VAAPI,
    UV_DECODER_NVIDIA,
    UV_DECODER_GENERIC_VAAPI,
    UV_DECODER_SOFTWARE
} UvDecoderPreference;

typedef enum {
    UV_VIDEO_SINK_AUTO = 0,
    UV_VIDEO_SINK_GTK4,
    UV_VIDEO_SINK_WAYLAND,
    UV_VIDEO_SINK_GLIMAGE,
    UV_VIDEO_SINK_XVIMAGE,
    UV_VIDEO_SINK_AUTOVIDEO,
    UV_VIDEO_SINK_FAKESINK
} UvVideoSinkPreference;

typedef struct {
    int listen_port;   // UDP port to bind (default: 5600)
    int payload_type;  // RTP payload type (default: 97)
    int clock_rate;    // RTP clock rate (default: 90000)
    bool sync_to_clock; // TRUE to let sink sync to clock
    guint appsrc_queue_size; // future use; set 0 for default
    guint jitter_latency_ms; // jitter buffer latency window (default: 24)
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
    gboolean audio_use_separate_port; // TRUE: audio comes in on its own UDP port
    guint audio_listen_port;          // UDP port for audio when use_separate_port is set
    UvDecoderPreference decoder_preference;
    UvVideoSinkPreference video_sink_preference;
    guint idr_http_port; // TCP port for the encoder's /request/idr endpoint (default: 80)
    gboolean sidecar_enabled; // subscribe to the encoder's RTP sidecar telemetry
    guint sidecar_port;       // UDP port of the encoder's sidecar listener (default: 5602)
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
    uint64_t rtp_marker_frames;
    double rtp_marker_fps;
    double rfc3550_jitter_ms;
    double seconds_since_last_seen;

    /* HEVC NAL composition counts (since source first seen). Detected from
     * the RTP payload header; works for waybeam_venc and any RFC 7798
     * packetiser. Counts NAL *units* (one per FU-start, walks aggregation
     * packets). */
    uint64_t hevc_idr_count;        /* NAL 19 (IDR_W_RADL) + 20 (IDR_N_LP) */
    uint64_t hevc_cra_count;        /* NAL 21 (clean random access) */
    uint64_t hevc_trail_count;      /* NAL 0 (TRAIL_N) + 1 (TRAIL_R) */
    uint64_t hevc_vps_count;        /* NAL 32 */
    uint64_t hevc_sps_count;        /* NAL 33 */
    uint64_t hevc_pps_count;        /* NAL 34 */
    uint64_t hevc_aud_count;        /* NAL 35 access unit delimiter */
    uint64_t hevc_sei_count;        /* NAL 39 prefix SEI + 40 suffix SEI */
    uint64_t hevc_other_nal_count;
    uint64_t rtp_ap_packets;        /* RFC 7798 aggregation packets (type 48) */
    uint64_t rtp_fu_packets;        /* RFC 7798 fragmentation packets (type 49) */
    double seconds_since_keyframe;  /* time since last IDR/CRA; -1 if none seen */
    double last_keyframe_interval_seconds; /* gap between two most recent keyframes; -1 if unknown */
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

/* Encoder-side telemetry received over the waybeam_venc RTP sidecar
 * protocol (UDP). Subscribed when sidecar_enabled is TRUE and a source
 * is locked; ground truth for things the receiver can only infer from
 * the RTP stream (QP, scene complexity, IDR insertion decisions, etc.). */
typedef enum {
    UV_SIDECAR_FRAME_P = 0,
    UV_SIDECAR_FRAME_I = 1,
    UV_SIDECAR_FRAME_IDR = 2
} UvSidecarFrameType;

typedef struct {
    gboolean enabled;            /* config: sidecar feature is on */
    gboolean socket_bound;       /* probe socket is open */
    gboolean subscribed;         /* a frame arrived within the last few seconds */
    char target_address[UV_VIEWER_ADDR_MAX]; /* encoder IP we're talking to */
    guint16 target_port;         /* encoder sidecar UDP port */
    guint16 local_port;          /* probe-side ephemeral port */

    uint64_t frames_received;
    uint64_t idr_inserted_count;
    uint64_t scene_change_count;
    uint64_t keyframes_count;    /* frames flagged I or IDR */
    double seconds_since_last_frame; /* -1 if none seen yet */

    /* Most recent frame metadata (FRAME + optional ENC_INFO trailer). */
    uint32_t last_ssrc;
    uint64_t last_frame_id;
    uint32_t last_rtp_timestamp;
    uint16_t last_seq_count;
    uint32_t last_frame_size_bytes;
    uint8_t  last_frame_type;        /* UvSidecarFrameType */
    uint8_t  last_qp;
    uint8_t  last_complexity;        /* 0..255 */
    uint8_t  last_scene_change;      /* 0/1 */
    uint8_t  last_gop_state;
    uint8_t  last_idr_inserted;      /* 0/1 */
    uint16_t last_frames_since_idr;

    /* Moving averages across most recent ~64 frames. */
    double avg_qp;
    double avg_complexity;

    /* Optional transport-info trailer (encoder-side output queue). */
    gboolean transport_info_seen;
    uint8_t  encoder_fill_pct;        /* 0..100 */
    uint8_t  encoder_in_pressure;     /* 0/1 backpressure latched */
    uint32_t encoder_transport_drops; /* lifetime */
    uint32_t encoder_pressure_drops;  /* lifetime */
    uint32_t encoder_packets_sent;    /* lifetime */
} UvSidecarStats;

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
    UvSidecarStats sidecar;
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
bool uv_viewer_restart_pipeline(UvViewer *viewer, GError **error);

void uv_viewer_set_event_callback(UvViewer *viewer, UvViewerEventCallback cb, gpointer user_data);

bool uv_viewer_select_source(UvViewer *viewer, int index, GError **error);
bool uv_viewer_select_next_source(UvViewer *viewer, GError **error);
int  uv_viewer_get_selected_source(const UvViewer *viewer);

bool uv_viewer_update_pipeline(UvViewer *viewer, const UvPipelineOverrides *overrides, GError **error);

/* Toggle the sidecar probe at runtime. Pass port=0 to keep the currently
 * configured port. Starts/stops the worker thread synchronously so the
 * GUI no longer has to restart the whole viewer to (un)subscribe. */
void uv_viewer_set_sidecar_enabled(UvViewer *viewer, bool enabled, guint port);

void uv_viewer_frame_block_configure(UvViewer *viewer, gboolean enabled, gboolean snapshot_mode);
void uv_viewer_frame_block_pause(UvViewer *viewer, gboolean paused);
void uv_viewer_frame_block_reset(UvViewer *viewer);
void uv_viewer_frame_block_set_width(UvViewer *viewer, guint width);
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
