#ifndef UV_INTERNAL_H
#define UV_INTERNAL_H

#include "uv_viewer.h"
#include "frame_shm_format.h"

#include <gst/app/gstappsrc.h>
#include <arpa/inet.h>
#include <poll.h>
#include <signal.h>

G_BEGIN_DECLS

#define UV_RELAY_MAX_SOURCES 256
#define UV_RELAY_BUF_SIZE 65536
#define UV_RTP_WIN_SIZE 4096
#define UV_RTP_SLOT_EMPTY 0xffffffffu
#define UV_SOURCE_FRAME_FPS_WINDOW_SAMPLES 512u
#define UV_DECODER_FPS_WINDOW_SAMPLES 512u
#define UV_RELEASE_CHUNK_RING 512u
#define UV_RELEASE_FRAME_RING 1024u
#define UV_RELEASE_DEFAULT_GAP_US 500.0
/* Inter-arrival deltas collected before auto-calibration runs its 2-means.
 * At ~30 pkts/frame * 60 fps (~1800 pps) this fills in under a second. */
#define UV_RELEASE_CALIB_SAMPLES 1500u

struct UvFrameBlockState;

typedef struct {
    UvSourceKind kind;
    char label[UV_VIEWER_ADDR_MAX];
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
    uint32_t rtp_first_ext_seq;
    uint32_t rtp_max_ext_seq;
    uint32_t rtp_bad_seq;
    uint64_t rtp_unique_packets;
    uint64_t rtp_duplicate_packets;
    uint64_t rtp_reordered_packets;
    uint32_t rtp_seq_slot[UV_RTP_WIN_SIZE];
    uint64_t rtp_marker_frames;
    gint64   frame_times_us[UV_SOURCE_FRAME_FPS_WINDOW_SAMPLES];
    guint    frame_times_head;
    guint    frame_times_count;

    bool     jitter_initialized;
    uint32_t jitter_prev_transit;
    double   jitter_value;

    struct UvFrameBlockState *frame_block;
    uint64_t frame_block_accum_bytes;

    /* Per-frame release timing (Part A: span + chunks-per-frame). A frame is
     * "open" from its first RTP packet until its marker packet. */
    gboolean frame_open;
    gint64   frame_first_pkt_us;   /* arrival of the frame's first packet */
    guint    frame_chunk_count;    /* release bursts the frame spanned so far */
    guint    frame_pkts;           /* packets in the current frame */
    gboolean frame_overlap;        /* first packet joined the previous burst */

    /* Independent marker-cadence baseline (decoupled from the frame-block grid
     * state so the cadence ring works even when the grid is disabled). */
    gboolean have_marker_baseline;
    gint64   last_marker_us;
    uint32_t last_marker_ts;
    double   frame_period_ms;      /* EWMA of marker-to-marker RTP-ts period */

    /* Per-frame ring for the cadence timeline. Lazily allocated only while
     * frame_release is enabled on the selected source (NULL otherwise). */
    UvReleaseFrame *frame_ring;    /* UV_RELEASE_FRAME_RING entries when non-NULL */
    guint    frame_ring_head;
    guint    frame_ring_count;

    /* Release-burst (FEC chunk) accumulator + ring (Part B). A chunk is a run
     * of packets whose inter-arrival gap stayed below frame_release.gap_us. */
    gint64        last_pkt_us;         /* arrival of the previous unique packet */
    gboolean      chunk_open;
    gint64        chunk_start_us;
    double        chunk_gap_ms;        /* idle gap that preceded this chunk */
    guint         chunk_pkts;
    guint         chunk_bytes;
    guint         chunk_frames;        /* distinct RTP timestamps in this chunk */
    uint32_t      chunk_last_ts;
    UvReleaseChunk *release_ring;      /* UV_RELEASE_CHUNK_RING entries, lazy (NULL off) */
    guint         release_head;        /* next write slot */
    guint         release_count;       /* filled entries (<= ring size) */
    guint64       release_total;       /* lifetime chunks since reset */
    guint64       release_overlap;     /* lifetime chunks with frames >= 2 */

    /* HEVC stream composition counters (computed from RTP payload). */
    uint64_t hevc_idr_count;
    uint64_t hevc_cra_count;
    uint64_t hevc_trail_count;
    uint64_t hevc_vps_count;
    uint64_t hevc_sps_count;
    uint64_t hevc_pps_count;
    uint64_t hevc_aud_count;
    uint64_t hevc_sei_count;
    uint64_t hevc_other_nal_count;
    uint64_t rtp_ap_packets;
    uint64_t rtp_fu_packets;
    gint64   last_keyframe_us;   /* g_get_monotonic_time of most recent IDR/CRA */
    gint64   prev_keyframe_us;   /* one before that, for interval calculation */
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
    GstAppSrc *audio_appsrc; /* optional, used for shared-port audio demux */

    struct {
        gboolean enabled;
        gboolean paused;
        gboolean snapshot_mode;
        guint width;
        guint height;
        double thresholds_ms[3];
        double thresholds_kb[3];
        double thresholds_span[3];   /* span_ms metric (computed at snapshot) */
        double thresholds_chunks[3]; /* chunks-per-frame metric */
        double thresholds_fpc[3];    /* frames-per-chunk metric */
        gboolean reset_requested;
        gboolean thresholds_dirty_ms;
        gboolean thresholds_dirty_kb;
    } frame_block;

    struct {
        gboolean enabled;
        gboolean paused;
        double gap_us;            /* burst separator: gap above this = new chunk */
        gboolean reset_requested;
        /* Auto-calibration of gap_us. While active, every selected-source
         * inter-arrival delta is logged; once UV_RELEASE_CALIB_SAMPLES land we
         * 2-means the log10(delta) distribution and place gap_us in the valley
         * between the intra-burst and inter-burst clusters. calib_seq bumps when
         * a fresh result is ready so the GUI can apply it exactly once. */
        gboolean calib_active;
        guint    calib_count;
        guint    calib_seq;
        double   calib_gap_us;    /* last suggestion (clamped to spin range) */
        gboolean calib_confident; /* clusters were cleanly bimodal */
        double  *calib_samples;   /* log10(delta_us), lazily allocated */
    } frame_release;

    /* Restream: verbatim UDP forward of the selected source's raw datagrams.
     * All fields are guarded by RelayController.lock. The send socket is opened
     * when restream is enabled with a resolvable destination and closed on
     * disable / teardown. */
    struct {
        gboolean           enabled;
        int                fd;          /* UDP send socket, -1 when closed */
        gboolean           dest_valid;  /* dest resolved and socket ready */
        struct sockaddr_in dest;
        char               dest_addr[UV_VIEWER_ADDR_MAX];
        guint16            dest_port;
        uint64_t           tx_packets;
        uint64_t           tx_bytes;
        uint64_t           tx_errors;
    } restream;

    GMutex lock;
    struct _UvViewer *viewer;
} RelayController;

typedef struct {
    char name[UV_SHM_NAME_MAX];
    gboolean enabled;
    void *base;
    size_t map_size;
    uint32_t slot_count;
    uint32_t slot_data_size;
    size_t stride;
    dev_t st_dev;
    ino_t st_ino;
    gboolean attached;
    GThread *thread;
    volatile gboolean stop;
    volatile gboolean push_enabled;
    gboolean stream_reset_pending;
    GMutex lock;
    GstAppSrc *appsrc;
    RelayController *registry;
    int source_index;
    uint64_t frames;
    uint64_t bytes;
    uint64_t full_drops_seen;
    uint64_t oversize_drops;
    uint64_t bad_slots;
    uint64_t reattaches;
} ShmIngress;

typedef enum {
    UV_INGRESS_UDP = 0,
    UV_INGRESS_SHM
} UvIngressMode;

#define UV_SIDECAR_AVG_WINDOW 64u

typedef struct {
    int fd;                          /* UDP socket (-1 = closed) */
    guint16 local_port;              /* bound port (0 = unknown) */
    GThread *thread;
    volatile sig_atomic_t running;

    GMutex lock;

    gboolean enabled;                /* config snapshot */
    guint16 encoder_port;            /* config: encoder's sidecar UDP port */

    char target_addr[UV_VIEWER_ADDR_MAX]; /* current encoder IP (selected source) */
    gboolean target_valid;

    gint64 last_frame_us;
    gint64 last_subscribe_us;        /* monotonic of last SUBSCRIBE we sent */

    /* Accumulators. */
    uint64_t frames_received;
    uint64_t idr_inserted_count;
    uint64_t scene_change_count;
    uint64_t keyframes_count;

    /* Last frame snapshot. */
    uint32_t last_ssrc;
    uint64_t last_frame_id;
    uint32_t last_rtp_timestamp;
    uint16_t last_seq_count;
    uint32_t last_frame_size_bytes;
    uint8_t  last_frame_type;
    uint8_t  last_qp;
    uint8_t  last_complexity;
    uint8_t  last_scene_change;
    uint8_t  last_gop_state;
    uint8_t  last_idr_inserted;
    uint16_t last_frames_since_idr;

    /* Sliding window for averages. */
    uint8_t  qp_window[UV_SIDECAR_AVG_WINDOW];
    uint8_t  cx_window[UV_SIDECAR_AVG_WINDOW];
    guint    window_head;
    guint    window_count;

    /* Latest transport trailer (if seen). */
    gboolean transport_info_seen;
    uint8_t  encoder_fill_pct;
    uint8_t  encoder_in_pressure;
    uint32_t encoder_transport_drops;
    uint32_t encoder_pressure_drops;
    uint32_t encoder_packets_sent;

    struct _UvViewer *viewer;
} SidecarController;

typedef struct {
    guint64 frames_total;
    gint64 first_frame_us;
    gint64 prev_timestamp_us;
    double last_snapshot_fps;
    gint64 frame_times_us[UV_DECODER_FPS_WINDOW_SAMPLES];
    guint frame_times_head;
    guint frame_times_count;
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
    int ingress_mode; /* UvIngressMode; int for __atomic ops */
    int payload_type;
    int clock_rate;
    gboolean sync_to_clock;
    gboolean use_videorate;
    guint videorate_fps_num;
    guint videorate_fps_den;
    UvDecoderPreference decoder_preference;
    UvVideoSinkPreference video_sink_preference;
    gboolean audio_enabled;
    guint audio_payload_type;
    guint audio_clock_rate;
    guint audio_jitter_latency_ms;
    gboolean audio_use_separate_port;
    guint audio_listen_port;

    struct _UvViewer *viewer;

    GstElement *pipeline;
    GstElement *appsrc_element;
    GstElement *queue0;
    GstElement *tee;
    GstElement *queue_video_in;
    GstElement *capsfilter_rtp_video;
    GstElement *jitterbuffer;
    GstElement *depay;
    GstElement *parser;
    GstElement *capsfilter;
    GstElement *decoder;
    GstElement *queue_postdec;
    GstElement *video_hw_convert;
    GstElement *video_convert;
    GstElement *videorate;
    GstElement *videorate_caps;
    GstElement *queue_postrate;
    GstElement *sink;

    GstElement *queue_audio_in;
    GstElement *capsfilter_rtp_audio;
    GstElement *audio_udpsrc;            /* used when audio_use_separate_port */
    GstElement *audio_appsrc_element;    /* used when sharing the video UDP port */
    GstElement *audio_jitter;
    GstElement *audio_depay;
    GstElement *audio_decoder;
    GstElement *audio_convert;
    GstElement *audio_resample;
    GstElement *audio_queue_sink; /* downstream leaky queue, drops stale audio */
    GstElement *audio_sink;

    GThread *loop_thread;
    GMainLoop *loop;
    GMainContext *loop_context;
    guint bus_watch_id;
    gulong decoder_probe_id;
    gint last_video_width;
    gint last_video_height;
    gint sink_bounce_pending;
    gboolean sink_is_fakesink;
    GPtrArray *sink_factories;
    guint sink_factory_index;
    gulong audio_probe_id;
    gboolean audio_sink_is_fakesink;
    gint64 audio_last_buffer_us;
    GMutex audio_lock;
    gboolean audio_active_cached;
} PipelineController;

struct _UvViewer {
    UvViewerConfig config;

    RelayController relay;
    PipelineController pipeline;
    DecoderStats decoder;
    QoSDatabase qos;
    SidecarController sidecar;
    ShmIngress shm_ingress;

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
void uv_internal_populate_source_stats(const UvRelaySource *src, int clock_rate, gint64 now_us, UvSourceStats *out);

gboolean relay_controller_init(RelayController *rc, struct _UvViewer *viewer);
void     relay_controller_deinit(RelayController *rc);
gboolean relay_controller_start(RelayController *rc);
void     relay_controller_stop(RelayController *rc);
gboolean relay_controller_select(RelayController *rc, int index, GError **error);
gboolean relay_controller_select_next(RelayController *rc, GError **error);
int      relay_controller_selected(const RelayController *rc);
void     relay_controller_snapshot(RelayController *rc, UvViewerStats *stats, int clock_rate);
void     relay_controller_set_appsrc(RelayController *rc, GstAppSrc *appsrc);
void     relay_controller_set_audio_appsrc(RelayController *rc, GstAppSrc *audio_appsrc);
void     relay_controller_set_push_enabled(RelayController *rc, gboolean enabled);
int      relay_controller_register_shm(RelayController *rc, const char *label);
void     relay_controller_shm_frame(RelayController *rc, int idx, const uint8_t *au,
                                    size_t len, const VencFrameMeta *meta);
UvSourceKind relay_controller_source_kind(RelayController *rc, int index);
void     relay_controller_frame_block_configure(RelayController *rc, gboolean enabled, gboolean snapshot_mode);
void     relay_controller_frame_block_pause(RelayController *rc, gboolean paused);
void     relay_controller_frame_block_reset(RelayController *rc);
void     relay_controller_frame_block_set_width(RelayController *rc, guint width);
void     relay_controller_frame_block_set_thresholds(RelayController *rc,
                                                     double green_ms,
                                                     double yellow_ms,
                                                     double orange_ms);
void     relay_controller_frame_block_set_size_thresholds(RelayController *rc,
                                                          double green_kb,
                                                          double yellow_kb,
                                                          double orange_kb);
void     relay_controller_frame_block_set_span_thresholds(RelayController *rc,
                                                          double green_ms,
                                                          double yellow_ms,
                                                          double orange_ms);
void     relay_controller_frame_block_set_chunk_thresholds(RelayController *rc,
                                                           double green,
                                                           double yellow,
                                                           double orange);
void     relay_controller_frame_block_set_overlap_thresholds(RelayController *rc,
                                                             double green,
                                                             double yellow,
                                                             double orange);
void     relay_controller_frame_release_configure(RelayController *rc, gboolean enabled);
void     relay_controller_frame_release_pause(RelayController *rc, gboolean paused);
void     relay_controller_frame_release_reset(RelayController *rc);
void     relay_controller_frame_release_set_gap_us(RelayController *rc, double gap_us);
void     relay_controller_frame_release_calibrate(RelayController *rc);
void     relay_controller_set_restream(RelayController *rc, gboolean enabled,
                                       const char *address, guint16 port);
void     relay_controller_restream_snapshot(RelayController *rc, UvRestreamStats *out);

gboolean sidecar_controller_init(SidecarController *sc, struct _UvViewer *viewer);
void     sidecar_controller_deinit(SidecarController *sc);
gboolean sidecar_controller_start(SidecarController *sc);
void     sidecar_controller_stop(SidecarController *sc);
void     sidecar_controller_snapshot(SidecarController *sc, UvViewerStats *stats);
void     sidecar_controller_set_target(SidecarController *sc, const char *address);

gboolean relay_controller_get_selected_address(RelayController *rc, char *out, size_t outlen);

gboolean pipeline_controller_init(PipelineController *pc, struct _UvViewer *viewer, GError **error);
void     pipeline_controller_deinit(PipelineController *pc);
gboolean pipeline_controller_start(PipelineController *pc, GError **error);
void     pipeline_controller_stop(PipelineController *pc);
GstAppSrc *pipeline_controller_get_appsrc(PipelineController *pc);
GstAppSrc *pipeline_controller_get_audio_appsrc(PipelineController *pc);
void     pipeline_controller_snapshot(PipelineController *pc, UvViewerStats *stats);
gboolean pipeline_controller_update(PipelineController *pc, const UvPipelineOverrides *overrides, GError **error);
GstElement *pipeline_controller_get_sink(PipelineController *pc);
UvIngressMode pipeline_controller_ingress_mode(PipelineController *pc);
void pipeline_controller_set_ingress_mode(PipelineController *pc, UvIngressMode mode);

gboolean shm_ingress_init(ShmIngress *si, struct _UvViewer *viewer,
                          RelayController *registry);
void shm_ingress_deinit(ShmIngress *si);
gboolean shm_ingress_start(ShmIngress *si);
void shm_ingress_stop(ShmIngress *si);
void shm_ingress_set_appsrc(ShmIngress *si, GstAppSrc *appsrc);
void shm_ingress_set_push_enabled(ShmIngress *si, gboolean enabled);
void shm_ingress_snapshot(ShmIngress *si, UvSourceStats *stats);

GstElement *uv_internal_viewer_get_sink(struct _UvViewer *viewer);

void uv_log_info(const char *fmt, ...) G_GNUC_PRINTF(1, 2);
void uv_log_warn(const char *fmt, ...) G_GNUC_PRINTF(1, 2);
void uv_log_error(const char *fmt, ...) G_GNUC_PRINTF(1, 2);

G_END_DECLS

#endif // UV_INTERNAL_H
