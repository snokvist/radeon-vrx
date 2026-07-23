#include "uv_internal.h"

#include <string.h>

static void ensure_gstreamer_initialized(void) {
    static gsize gst_init_once = 0;
    if (g_once_init_enter(&gst_init_once)) {
        int argc = 0;
        char **argv = NULL;
        gst_init(&argc, &argv);
        g_once_init_leave(&gst_init_once, 1);
    }
}

typedef struct {
    const char *factory_name;
    gboolean requires_nvconv;
    gboolean enable_memory_copy;
} DecoderCandidate;

static const DecoderCandidate decoder_candidates_auto[] = {
    {"vah265dec", FALSE, FALSE},
    {"vaapih265dec", FALSE, FALSE},
    {"nvh265dec", TRUE, FALSE},
    {"nvv4l2decoder", FALSE, TRUE},
    {"nvdec_h265", TRUE, FALSE},
    {NULL, FALSE, FALSE}
};

static const DecoderCandidate decoder_candidates_intel[] = {
    {"vah265dec", FALSE, FALSE},
    {"vaapih265dec", FALSE, FALSE},
    {NULL, FALSE, FALSE}
};

static const DecoderCandidate decoder_candidates_vaapi[] = {
    {"vaapih265dec", FALSE, FALSE},
    {"vah265dec", FALSE, FALSE},
    {NULL, FALSE, FALSE}
};

static const DecoderCandidate decoder_candidates_nvidia[] = {
    {"nvh265dec", TRUE, FALSE},
    {"nvv4l2decoder", FALSE, TRUE},
    {"nvdec_h265", TRUE, FALSE},
    {NULL, FALSE, FALSE}
};

static const DecoderCandidate decoder_candidates_software[] = {
    {"avdec_h265", FALSE, FALSE},
    {NULL, FALSE, FALSE}
};

static const DecoderCandidate *pick_decoder_candidate_list(UvDecoderPreference pref) {
    switch (pref) {
        case UV_DECODER_INTEL_VAAPI:
            return decoder_candidates_intel;
        case UV_DECODER_NVIDIA:
            return decoder_candidates_nvidia;
        case UV_DECODER_GENERIC_VAAPI:
            return decoder_candidates_vaapi;
        case UV_DECODER_SOFTWARE:
            return decoder_candidates_software;
        case UV_DECODER_AUTO:
        default:
            return decoder_candidates_auto;
    }
}

static gboolean configure_video_decoder(PipelineController *pc) {
    const DecoderCandidate *primary = pick_decoder_candidate_list(pc->decoder_preference);
    const DecoderCandidate *fallback = NULL;
    if (pc->decoder_preference == UV_DECODER_AUTO) {
        fallback = decoder_candidates_auto;
    }

    const DecoderCandidate *lists[2] = { primary, fallback };

    for (guint list_idx = 0; list_idx < G_N_ELEMENTS(lists); list_idx++) {
        const DecoderCandidate *candidates = lists[list_idx];
        if (!candidates) continue;
        if (list_idx > 0 && candidates == lists[list_idx - 1]) continue;
        for (const DecoderCandidate *cand = candidates; cand->factory_name; cand++) {
            GstElement *decoder = gst_element_factory_make(cand->factory_name, "decoder");
            if (!decoder) continue;

            if (cand->enable_memory_copy) {
                if (g_object_class_find_property(G_OBJECT_GET_CLASS(decoder), "enable-memory-copy")) {
                    g_object_set(decoder, "enable-memory-copy", TRUE, NULL);
                }
            }

            GstElement *hw_convert = NULL;
            if (cand->requires_nvconv) {
                hw_convert = gst_element_factory_make("nvvidconv", "nvvidconv");
                if (!hw_convert) {
                    uv_log_warn("Decoder %s requires nvvidconv but it was not found; skipping candidate", cand->factory_name);
                    gst_object_unref(decoder);
                    continue;
                }
                if (g_object_class_find_property(G_OBJECT_GET_CLASS(hw_convert), "nvbuf-memory-type")) {
                    g_object_set(hw_convert, "nvbuf-memory-type", 0, NULL);
                }
            }

            pc->decoder = decoder;
            pc->video_hw_convert = hw_convert;
            uv_log_info("Using decoder factory %s", cand->factory_name);
            return TRUE;
        }
        if (pc->decoder) {
            break;
        }
    }

    /* Final fallback to software decoder */
    if (pc->decoder_preference == UV_DECODER_AUTO || pc->decoder_preference == UV_DECODER_SOFTWARE) {
        pc->decoder = gst_element_factory_make("avdec_h265", "decoder");
        if (pc->decoder) {
            uv_log_warn("Falling back to avdec_h265 software decoder");
            pc->video_hw_convert = NULL;
            return TRUE;
        }
    }

    uv_log_error("Failed to create any H.265 decoder element");
    return FALSE;
}

static void set_appsrc_callbacks(PipelineController *pc);

static gboolean bus_cb(GstBus *bus, GstMessage *msg, gpointer user_data) {
    (void)bus;
    PipelineController *pc = (PipelineController *)user_data;
    UvViewer *viewer = pc->viewer;

    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_QOS:
            uv_internal_qos_db_update(&viewer->qos, msg);
            break;
        case GST_MESSAGE_ERROR: {
            GError *err = NULL;
            gchar *dbg = NULL;
            gst_message_parse_error(msg, &err, &dbg);
            uv_log_error("Pipeline error: %s", err ? err->message : "unknown");
            if (dbg) uv_log_warn("Pipeline debug: %s", dbg);
            uv_internal_emit_event(viewer, UV_VIEWER_EVENT_PIPELINE_ERROR, -1, NULL, err);
            if (pc->loop) g_main_loop_quit(pc->loop);
            if (dbg) g_free(dbg);
            if (err) g_error_free(err);
            break;
        }
        case GST_MESSAGE_EOS:
            uv_log_info("Pipeline reached EOS");
            uv_internal_emit_event(viewer, UV_VIEWER_EVENT_SHUTDOWN, -1, NULL, NULL);
            if (pc->loop) g_main_loop_quit(pc->loop);
            break;
        default:
            break;
    }
    return TRUE;
}

static gboolean sink_bounce_idle(gpointer user_data) {
    PipelineController *pc = (PipelineController *)user_data;
    if (!pc) return G_SOURCE_REMOVE;

    GstElement *sink = pc->sink;
    if (sink) {
        uv_log_info("Bouncing video sink (READY -> PLAYING) to recover after caps change");
        gst_element_set_state(sink, GST_STATE_READY);
        gst_element_sync_state_with_parent(sink);
    }
    g_atomic_int_set(&pc->sink_bounce_pending, 0);
    return G_SOURCE_REMOVE;
}

static void schedule_sink_bounce(PipelineController *pc) {
    if (!pc || !pc->loop_context) return;
    if (!g_atomic_int_compare_and_exchange(&pc->sink_bounce_pending, 0, 1)) return;
    GSource *src = g_idle_source_new();
    g_source_set_priority(src, G_PRIORITY_DEFAULT);
    g_source_set_callback(src, sink_bounce_idle, pc, NULL);
    g_source_attach(src, pc->loop_context);
    g_source_unref(src);
}

static void handle_decoder_caps_event(PipelineController *pc, GstEvent *event) {
    GstCaps *caps = NULL;
    gst_event_parse_caps(event, &caps);
    if (!caps || gst_caps_is_empty(caps) || !gst_caps_is_fixed(caps)) return;

    const GstStructure *s = gst_caps_get_structure(caps, 0);
    if (!s) return;

    gint width = 0, height = 0;
    gst_structure_get_int(s, "width", &width);
    gst_structure_get_int(s, "height", &height);
    if (width <= 0 || height <= 0) return;

    gint old_w = pc->last_video_width;
    gint old_h = pc->last_video_height;
    pc->last_video_width = width;
    pc->last_video_height = height;

    if (old_w == 0 || old_h == 0) {
        return; /* first negotiation, nothing to recover from */
    }
    if (old_w == width && old_h == height) {
        return;
    }

    uv_log_info("Decoder output caps changed: %dx%d -> %dx%d (scheduling sink bounce)",
                old_w, old_h, width, height);
    schedule_sink_bounce(pc);
}

static GstPadProbeReturn dec_src_probe(GstPad *pad, GstPadProbeInfo *info, gpointer user_data) {
    (void)pad;
    PipelineController *pc = (PipelineController *)user_data;
    GstPadProbeType ptype = GST_PAD_PROBE_INFO_TYPE(info);

    if (ptype & GST_PAD_PROBE_TYPE_BUFFER) {
        gint64 now_us = g_get_monotonic_time();
        uv_internal_decoder_stats_push_frame(&pc->viewer->decoder, now_us);
    } else if (ptype & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM) {
        GstEvent *event = GST_PAD_PROBE_INFO_EVENT(info);
        if (event && GST_EVENT_TYPE(event) == GST_EVENT_CAPS) {
            handle_decoder_caps_event(pc, event);
        }
    }
    return GST_PAD_PROBE_OK;
}

static GstPadProbeReturn audio_src_probe(GstPad *pad, GstPadProbeInfo *info, gpointer user_data) {
    (void)pad;
    PipelineController *pc = (PipelineController *)user_data;
    if (!pc) return GST_PAD_PROBE_OK;
    if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER) {
        gint64 now_us = g_get_monotonic_time();
        g_mutex_lock(&pc->audio_lock);
        pc->audio_last_buffer_us = now_us;
        g_mutex_unlock(&pc->audio_lock);
    }
    return GST_PAD_PROBE_OK;
}

/* autoaudiosink instantiates its real sink only when it reaches READY, so the
 * sync/async knobs have to be applied to the child as it appears. Leaving
 * async=TRUE on a sink that never receives data keeps the whole pipeline's
 * state change to PLAYING pending, which stalls the video sink too. */
static void audio_sink_deep_element_added(GstBin *bin, GstBin *sub_bin,
                                          GstElement *element, gpointer user_data) {
    (void)bin;
    (void)sub_bin;
    (void)user_data;
    if (!element) return;
    GObjectClass *klass = G_OBJECT_GET_CLASS(element);
    if (g_object_class_find_property(klass, "sync")) {
        g_object_set(element, "sync", FALSE, NULL);
    }
    if (g_object_class_find_property(klass, "async")) {
        g_object_set(element, "async", FALSE, NULL);
    }
}

static void remove_audio_probe(PipelineController *pc) {
    if (!pc || pc->audio_probe_id == 0) return;
    if (!pc->audio_resample) {
        pc->audio_probe_id = 0;
        return;
    }
    GstPad *pad = gst_element_get_static_pad(pc->audio_resample, "src");
    if (pad) {
        gst_pad_remove_probe(pad, pc->audio_probe_id);
        gst_object_unref(pad);
    }
    pc->audio_probe_id = 0;
}

static void on_need_data(GstAppSrc *src, guint length, gpointer user_data) {
    (void)src; (void)length;
    PipelineController *pc = (PipelineController *)user_data;
    if (pipeline_controller_ingress_mode(pc) == UV_INGRESS_SHM)
        shm_ingress_set_push_enabled(&pc->viewer->shm_ingress, TRUE);
    else
        relay_controller_set_push_enabled(&pc->viewer->relay, TRUE);
}

static void on_enough_data(GstAppSrc *src, gpointer user_data) {
    (void)src;
    PipelineController *pc = (PipelineController *)user_data;
    if (pipeline_controller_ingress_mode(pc) == UV_INGRESS_SHM)
        shm_ingress_set_push_enabled(&pc->viewer->shm_ingress, FALSE);
    else
        relay_controller_set_push_enabled(&pc->viewer->relay, FALSE);
}

static gboolean pipeline_swap_to_fakesink(PipelineController *pc) {
    if (!pc || !pc->pipeline || !pc->queue_postdec || !pc->sink) return FALSE;

    GstElement *upstream = pc->video_convert ? pc->video_convert : pc->queue_postdec;
    gst_element_unlink(upstream, pc->sink);
    gst_bin_remove(GST_BIN(pc->pipeline), pc->sink);

    GstElement *fakesink = gst_element_factory_make("fakesink", "sink");
    if (!fakesink) return FALSE;
    g_object_set(fakesink, "sync", FALSE, "async", FALSE, NULL);

    gst_bin_add(GST_BIN(pc->pipeline), fakesink);
    if (!gst_element_link(upstream, fakesink)) {
        gst_bin_remove(GST_BIN(pc->pipeline), fakesink);
        return FALSE;
    }

    pc->sink = fakesink;
    pc->sink_is_fakesink = TRUE;
    return TRUE;
}

static const char *video_sink_preference_to_factory(UvVideoSinkPreference pref) {
    switch (pref) {
        case UV_VIDEO_SINK_GTK4:      return "gtk4paintablesink";
        case UV_VIDEO_SINK_WAYLAND:   return "waylandsink";
        case UV_VIDEO_SINK_GLIMAGE:   return "glimagesink";
        case UV_VIDEO_SINK_XVIMAGE:   return "xvimagesink";
        case UV_VIDEO_SINK_AUTOVIDEO: return "autovideosink";
        case UV_VIDEO_SINK_FAKESINK:  return "fakesink";
        case UV_VIDEO_SINK_AUTO:
        default:
            return NULL;
    }
}

static gboolean pipeline_sink_list_contains(GPtrArray *factories, const char *factory) {
    if (!factories || !factory) return FALSE;
    for (guint i = 0; i < factories->len; i++) {
        const char *existing = (const char *)g_ptr_array_index(factories, i);
        if (g_strcmp0(existing, factory) == 0) {
            return TRUE;
        }
    }
    return FALSE;
}

static void pipeline_add_sink_candidate(GPtrArray *factories, const char *factory) {
    if (!factories || !factory || *factory == '\0') return;
    if (pipeline_sink_list_contains(factories, factory)) return;
    g_ptr_array_add(factories, g_strdup(factory));
}

static const char *pipeline_sink_factory_name_at(const PipelineController *pc, guint index) {
    if (!pc || !pc->sink_factories) return NULL;
    if (index >= pc->sink_factories->len) return NULL;
    return (const char *)g_ptr_array_index(pc->sink_factories, index);
}

static const char *pipeline_current_sink_factory_name(const PipelineController *pc) {
    return pipeline_sink_factory_name_at(pc, pc->sink_factory_index);
}

static void pipeline_detach_current_sink(PipelineController *pc) {
    if (!pc || !pc->sink || !pc->pipeline) return;
    GstElement *upstream = pc->video_convert ? pc->video_convert : pc->queue_postdec;
    gst_element_set_state(pc->sink, GST_STATE_NULL);
    if (upstream) {
        gst_element_unlink(upstream, pc->sink);
    }
    gst_bin_remove(GST_BIN(pc->pipeline), pc->sink);
    pc->sink = NULL;
    pc->sink_is_fakesink = FALSE;
}

static gboolean pipeline_attach_sink_at(PipelineController *pc, guint index) {
    if (!pc || !pc->pipeline) return FALSE;
    const char *factory_name = pipeline_sink_factory_name_at(pc, index);
    if (!factory_name) return FALSE;

    GstElement *sink = gst_element_factory_make(factory_name, "sink");
    if (!sink) {
        return FALSE;
    }

    gboolean is_fakesink = (g_strcmp0(factory_name, "fakesink") == 0);
    if (is_fakesink) {
        g_object_set(sink, "sync", FALSE, "async", FALSE, NULL);
    } else {
        g_object_set(sink, "sync", pc->sync_to_clock ? TRUE : FALSE, NULL);
    }

    GstElement *upstream = pc->video_convert ? pc->video_convert : pc->queue_postdec;
    if (!upstream) {
        gst_object_unref(sink);
        return FALSE;
    }

    gst_bin_add(GST_BIN(pc->pipeline), sink);
    if (!gst_element_link(upstream, sink)) {
        gst_bin_remove(GST_BIN(pc->pipeline), sink);
        return FALSE;
    }

    pc->sink = sink;
    pc->sink_is_fakesink = is_fakesink;
    pc->sink_factory_index = index;
    return TRUE;
}

static gboolean pipeline_attach_sink_from(PipelineController *pc, guint start_index) {
    if (!pc || !pc->sink_factories) return FALSE;
    for (guint idx = start_index; idx < pc->sink_factories->len; idx++) {
        if (pipeline_attach_sink_at(pc, idx)) {
            return TRUE;
        }
    }
    return FALSE;
}

static gboolean build_pipeline(PipelineController *pc, GError **error) {
    UvViewer *viewer = pc->viewer;
    pc->appsrc_element   = gst_element_factory_make("appsrc", "src");
    pc->queue0           = gst_element_factory_make("queue", "queue_ingress");
    if (pc->ingress_mode == UV_INGRESS_UDP) {
        pc->tee = gst_element_factory_make("tee", "tee");
        pc->queue_video_in = gst_element_factory_make("queue", "queue_video_in");
        pc->capsfilter_rtp_video = gst_element_factory_make("capsfilter", "cf_rtp_video");
        pc->jitterbuffer = gst_element_factory_make("rtpjitterbuffer", "jbuf_video");
        pc->depay = gst_element_factory_make("rtph265depay", "depay");
    }
    pc->parser           = gst_element_factory_make("h265parse", "parser");
    pc->capsfilter       = gst_element_factory_make("capsfilter", "h265caps");
    pc->queue_postdec    = gst_element_factory_make("queue", "queue_postdec");
    pc->video_convert    = gst_element_factory_make("videoconvert", "video_convert");

    if (pc->videorate_fps_den == 0) {
        pc->videorate_fps_den = 1;
    }
    gboolean enable_videorate = pc->use_videorate &&
                                pc->videorate_fps_num > 0 &&
                                pc->videorate_fps_den > 0;
    if (pc->use_videorate && !enable_videorate) {
        uv_log_warn("Videorate requested but invalid target FPS %u/%u; disabling",
                    pc->videorate_fps_num, pc->videorate_fps_den);
    }
    pc->use_videorate = enable_videorate;
    if (pc->use_videorate) {
        pc->videorate = gst_element_factory_make("videorate", "videorate");
        pc->videorate_caps = gst_element_factory_make("capsfilter", "videorate_caps");
        pc->queue_postrate = gst_element_factory_make("queue", "queue_postrate");
    }

    if (pc->audio_enabled && pc->ingress_mode == UV_INGRESS_UDP) {
        pc->queue_audio_in = gst_element_factory_make("queue", "queue_audio_in");
        pc->capsfilter_rtp_audio = gst_element_factory_make("capsfilter", "cf_rtp_audio");
        if (pc->audio_use_separate_port) {
            pc->audio_udpsrc = gst_element_factory_make("udpsrc", "audio_udpsrc");
            if (pc->audio_udpsrc) {
                g_object_set(pc->audio_udpsrc,
                             "port", (gint)pc->audio_listen_port,
                             "buffer-size", 524288,
                             "auto-multicast", TRUE,
                             NULL);
            }
        } else {
            /* Shared-port mode: the relay thread demuxes by RTP payload
             * type and pushes audio packets into this appsrc, mirroring
             * waybeam-hub's two-decoder pattern. Without this, audio
             * packets used to flow through the video tee into the H.265
             * decoder — producing the green-frame symptom. */
            pc->audio_appsrc_element = gst_element_factory_make("appsrc", "audio_appsrc");
            if (pc->audio_appsrc_element) {
                GstCaps *caps_audio = gst_caps_new_simple("application/x-rtp",
                                                          "media",         G_TYPE_STRING, "audio",
                                                          "encoding-name", G_TYPE_STRING, "OPUS",
                                                          "payload",       G_TYPE_INT,    (gint)pc->audio_payload_type,
                                                          "clock-rate",    G_TYPE_INT,    (gint)pc->audio_clock_rate,
                                                          NULL);
                g_object_set(pc->audio_appsrc_element,
                             "is-live", TRUE,
                             "format", GST_FORMAT_TIME,
                             "stream-type", GST_APP_STREAM_TYPE_STREAM,
                             "do-timestamp", TRUE,
                             "max-bytes", (guint64)(1024 * 1024),
                             "caps", caps_audio,
                             NULL);
                gst_app_src_set_latency(GST_APP_SRC(pc->audio_appsrc_element), 0, 0);
                gst_caps_unref(caps_audio);
            }
        }
        pc->audio_jitter = gst_element_factory_make("rtpjitterbuffer", "jbuf_audio");
        pc->audio_depay = gst_element_factory_make("rtpopusdepay", "depay_audio");
        pc->audio_decoder = gst_element_factory_make("opusdec", "opus_decoder");
        pc->audio_convert = gst_element_factory_make("audioconvert", "audio_convert");
        pc->audio_resample = gst_element_factory_make("audioresample", "audio_resample");
        pc->audio_queue_sink = gst_element_factory_make("queue", "audio_queue_sink");
        pc->audio_sink = gst_element_factory_make("autoaudiosink", "audio_sink");
        pc->audio_sink_is_fakesink = FALSE;
        if (!pc->audio_sink) pc->audio_sink = gst_element_factory_make("pulsesink", "audio_sink");
        if (!pc->audio_sink) pc->audio_sink = gst_element_factory_make("alsasink", "audio_sink");
        if (!pc->audio_sink) {
            pc->audio_sink = gst_element_factory_make("fakesink", "audio_sink");
            if (pc->audio_sink) {
                g_object_set(pc->audio_sink, "sync", FALSE, "async", FALSE, NULL);
                pc->audio_sink_is_fakesink = TRUE;
            }
        }
    }

    gboolean headless = (g_getenv("WAYLAND_DISPLAY") == NULL && g_getenv("DISPLAY") == NULL);
    if (!pc->sink_factories) {
        pc->sink_factories = g_ptr_array_new_with_free_func(g_free);
    } else {
        g_ptr_array_set_size(pc->sink_factories, 0);
    }
    const char *forced_sink = video_sink_preference_to_factory(pc->video_sink_preference);
    if (forced_sink) {
        pipeline_add_sink_candidate(pc->sink_factories, forced_sink);
    }
    if (headless) {
        pipeline_add_sink_candidate(pc->sink_factories, "fakesink");
    } else {
        const char *candidates[] = {
            "gtk4paintablesink",
            "waylandsink",
            "glimagesink",
            "xvimagesink",
            "autovideosink",
            "fakesink"
        };
        for (guint i = 0; i < G_N_ELEMENTS(candidates); i++) {
            pipeline_add_sink_candidate(pc->sink_factories, candidates[i]);
        }
    }
    if (!pc->sink_factories || pc->sink_factories->len == 0) {
        if (!pc->sink_factories) {
            pc->sink_factories = g_ptr_array_new_with_free_func(g_free);
        }
        pipeline_add_sink_candidate(pc->sink_factories, "fakesink");
    }
    pc->sink = NULL;
    pc->sink_is_fakesink = FALSE;
    pc->sink_factory_index = 0;

    if (!configure_video_decoder(pc)) {
        g_set_error(error, g_quark_from_static_string("uv-viewer"), 10,
                    "Failed to create a suitable H.265 decoder");
        return FALSE;
    }

    if (!pc->appsrc_element || !pc->queue0 ||
        (pc->ingress_mode == UV_INGRESS_UDP &&
         (!pc->tee || !pc->queue_video_in || !pc->capsfilter_rtp_video ||
          !pc->jitterbuffer || !pc->depay)) ||
        !pc->parser || !pc->capsfilter || !pc->decoder ||
        !pc->queue_postdec || !pc->video_convert ||
        (pc->use_videorate && (!pc->videorate || !pc->videorate_caps || !pc->queue_postrate))) {
        g_set_error(error, g_quark_from_static_string("uv-viewer"), 10,
                    "Failed to create required GStreamer elements");
        return FALSE;
    }

    if (pc->audio_enabled) {
        gboolean source_ok = pc->audio_use_separate_port
                           ? (pc->audio_udpsrc != NULL)
                           : (pc->audio_appsrc_element != NULL);
        if (!pc->queue_audio_in || !pc->capsfilter_rtp_audio || !pc->audio_jitter ||
            !pc->audio_depay || !pc->audio_decoder || !pc->audio_convert ||
            !pc->audio_resample || !pc->audio_queue_sink || !pc->audio_sink || !source_ok) {
            uv_log_warn("Audio pipeline requested but missing components; disabling audio");
            if (pc->queue_audio_in) { gst_object_unref(pc->queue_audio_in); pc->queue_audio_in = NULL; }
            if (pc->capsfilter_rtp_audio) { gst_object_unref(pc->capsfilter_rtp_audio); pc->capsfilter_rtp_audio = NULL; }
            if (pc->audio_udpsrc) { gst_object_unref(pc->audio_udpsrc); pc->audio_udpsrc = NULL; }
            if (pc->audio_appsrc_element) { gst_object_unref(pc->audio_appsrc_element); pc->audio_appsrc_element = NULL; }
            if (pc->audio_jitter) { gst_object_unref(pc->audio_jitter); pc->audio_jitter = NULL; }
            if (pc->audio_depay) { gst_object_unref(pc->audio_depay); pc->audio_depay = NULL; }
            if (pc->audio_decoder) { gst_object_unref(pc->audio_decoder); pc->audio_decoder = NULL; }
            if (pc->audio_convert) { gst_object_unref(pc->audio_convert); pc->audio_convert = NULL; }
            if (pc->audio_resample) { gst_object_unref(pc->audio_resample); pc->audio_resample = NULL; }
            if (pc->audio_queue_sink) { gst_object_unref(pc->audio_queue_sink); pc->audio_queue_sink = NULL; }
            if (pc->audio_sink) { gst_object_unref(pc->audio_sink); pc->audio_sink = NULL; }
            pc->audio_enabled = FALSE;
            pc->audio_sink_is_fakesink = FALSE;
        }
    }

    pc->pipeline = gst_pipeline_new("uv-udp-h265");
    if (!pc->pipeline) {
        g_set_error(error, g_quark_from_static_string("uv-viewer"), 11,
                    "Failed to create pipeline");
        return FALSE;
    }

    GstCaps *caps_appsrc = pc->ingress_mode == UV_INGRESS_SHM
                         ? gst_caps_from_string("video/x-h265,stream-format=byte-stream,alignment=au")
                         : gst_caps_new_empty_simple("application/x-rtp");
    if (!caps_appsrc) {
        g_set_error(error, g_quark_from_static_string("uv-viewer"), 12,
                    "Failed to build ingress caps");
        return FALSE;
    }

    GstCaps *caps_h265 = gst_caps_from_string("video/x-h265,stream-format=byte-stream,alignment=au");
    if (!caps_h265) {
        gst_caps_unref(caps_appsrc);
        g_set_error(error, g_quark_from_static_string("uv-viewer"), 13,
                    "Failed to build H265 caps");
        return FALSE;
    }

    g_object_set(pc->appsrc_element,
                 "is-live", TRUE,
                 "format", pc->ingress_mode == UV_INGRESS_SHM ? GST_FORMAT_TIME : GST_FORMAT_BYTES,
                 "do-timestamp", pc->ingress_mode == UV_INGRESS_SHM,
                 "block", FALSE,
                 "max-bytes", (guint64)(2 * 1024 * 1024),
                 "stream-type", GST_APP_STREAM_TYPE_STREAM,
                 NULL);
    gst_app_src_set_caps(GST_APP_SRC(pc->appsrc_element), caps_appsrc);
    gst_caps_unref(caps_appsrc);

    g_object_set(pc->queue0,
                 "leaky", 2,
                 "max-size-buffers", viewer->config.queue_max_buffers,
                 "max-size-bytes", 0,
                 "max-size-time", (guint64)0,
                 NULL);

    if (pc->queue_video_in) {
        g_object_set(pc->queue_video_in,
                     "leaky", 2,
                     "max-size-buffers", 0,
                     "max-size-bytes", 0,
                     "max-size-time", (guint64)0,
                     NULL);
    }

    if (pc->jitterbuffer) g_object_set(pc->jitterbuffer,
                 "latency", (guint)MAX(viewer->config.jitter_latency_ms, 0u),
                 "drop-on-latency", viewer->config.jitter_drop_on_latency,
                 "do-lost", viewer->config.jitter_do_lost,
                 "post-drop-messages", viewer->config.jitter_post_drop_messages,
                 NULL);

    GstCaps *caps_rtp_video = pc->ingress_mode == UV_INGRESS_UDP ? gst_caps_new_simple("application/x-rtp",
                                                  "media", G_TYPE_STRING, "video",
                                                  "encoding-name", G_TYPE_STRING, "H265",
                                                  "payload", G_TYPE_INT, pc->payload_type,
                                                  "clock-rate", G_TYPE_INT, pc->clock_rate,
                                                  NULL) : NULL;
    if (pc->ingress_mode == UV_INGRESS_UDP && !caps_rtp_video) {
        gst_caps_unref(caps_h265);
        g_set_error(error, g_quark_from_static_string("uv-viewer"), 16,
                    "Failed to build video RTP caps");
        return FALSE;
    }
    if (caps_rtp_video) {
        g_object_set(pc->capsfilter_rtp_video, "caps", caps_rtp_video, NULL);
        gst_caps_unref(caps_rtp_video);
    }

    g_object_set(pc->parser, "config-interval", -1, NULL);
    g_object_set(pc->capsfilter, "caps", caps_h265, NULL);
    gst_caps_unref(caps_h265);

    if (pc->use_videorate) {
        g_object_set(pc->videorate, "drop-only", FALSE, NULL);
        GstCaps *fps_caps = gst_caps_new_simple("video/x-raw",
                                               "framerate", GST_TYPE_FRACTION,
                                               pc->videorate_fps_num,
                                               pc->videorate_fps_den,
                                               NULL);
        if (!fps_caps) {
            g_set_error(error, g_quark_from_static_string("uv-viewer"), 15,
                        "Failed to build videorate caps");
            return FALSE;
        }
        g_object_set(pc->videorate_caps, "caps", fps_caps, NULL);
        gst_caps_unref(fps_caps);
    }

    if (pc->audio_enabled) {
        /* Plain thread-boundary queue with generous limits. The previous
         * leaky-upstream + 1-nanosecond cap was hostile to audio — every
         * downstream stall dropped buffers, causing the clipping/delay the
         * user reported. Audio decoding runs on this queue's task, so
         * keeping it simple is the right move. */
        g_object_set(pc->queue_audio_in,
                     "leaky", 0,
                     "max-size-buffers", (guint)400,
                     "max-size-bytes",   (guint)0,
                     "max-size-time",    (guint64)GST_SECOND,
                     NULL);
        guint jbuf_latency = pc->audio_jitter_latency_ms;
        if (jbuf_latency == 0) jbuf_latency = 40; /* sensible default */
        g_object_set(pc->audio_jitter,
                     "latency", jbuf_latency,
                     "drop-on-latency", viewer->config.jitter_drop_on_latency,
                     "do-lost", viewer->config.jitter_do_lost,
                     "post-drop-messages", viewer->config.jitter_post_drop_messages,
                     NULL);

        GstCaps *caps_rtp_audio = gst_caps_new_simple("application/x-rtp",
                                                      "media", G_TYPE_STRING, "audio",
                                                      "encoding-name", G_TYPE_STRING, "OPUS",
                                                      "payload", G_TYPE_INT, (gint)pc->audio_payload_type,
                                                      "clock-rate", G_TYPE_INT, (gint)pc->audio_clock_rate,
                                                      NULL);
        if (!caps_rtp_audio) {
            g_set_error(error, g_quark_from_static_string("uv-viewer"), 17,
                        "Failed to build audio RTP caps");
            return FALSE;
        }
        g_object_set(pc->capsfilter_rtp_audio, "caps", caps_rtp_audio, NULL);
        gst_caps_unref(caps_rtp_audio);

        /* Downstream-leaky queue right before the sink, capped at ~50 ms.
         * Matches the waybeam-hub pattern: keep tail of buffer fresh and
         * drop old ones rather than letting them build up. This is what
         * gives audio low latency without clipping. */
        if (pc->audio_queue_sink) {
            g_object_set(pc->audio_queue_sink,
                         "leaky", 2,                    /* downstream: drop oldest */
                         "max-size-buffers", (guint)0,
                         "max-size-bytes",   (guint)0,
                         "max-size-time",    (guint64)(50 * GST_MSECOND),
                         NULL);
        }
        if (pc->audio_sink && !pc->audio_sink_is_fakesink) {
            /* sync=FALSE so the audio sink doesn't preroll on the first
             * buffer (which would stall the whole pipeline when no audio
             * has arrived yet). async=FALSE so state changes complete
             * immediately. The downstream leaky queue is what bounds the
             * playback latency, not the sink clock. */
            g_object_set(pc->audio_sink, "sync", FALSE, NULL);
            if (g_object_class_find_property(G_OBJECT_GET_CLASS(pc->audio_sink), "async")) {
                g_object_set(pc->audio_sink, "async", FALSE, NULL);
            } else if (GST_IS_BIN(pc->audio_sink)) {
                /* autoaudiosink is a GstBin: it proxies "sync" but has no
                 * "async" property, so the guard above silently did nothing
                 * and its inner sink kept async=TRUE. With no audio on the
                 * separate UDP port that inner sink never prerolls, the
                 * pipeline never completes its state change to PLAYING, and
                 * every other async sink (the video sink) stalls after its
                 * own preroll buffer -> frozen picture. The real sink only
                 * exists once the bin reaches READY, so patch it as it is
                 * added. */
                g_signal_connect(pc->audio_sink, "deep-element-added",
                                 G_CALLBACK(audio_sink_deep_element_added), NULL);
            }
        }
    }

    gst_bin_add_many(GST_BIN(pc->pipeline), pc->appsrc_element, pc->queue0, NULL);
    if (pc->ingress_mode == UV_INGRESS_UDP) {
        gst_bin_add_many(GST_BIN(pc->pipeline), pc->tee, pc->queue_video_in,
                         pc->capsfilter_rtp_video, pc->jitterbuffer, pc->depay, NULL);
    }
    gst_bin_add_many(GST_BIN(pc->pipeline), pc->parser, pc->capsfilter,
                     pc->decoder, pc->queue_postdec, NULL);
    if (pc->video_hw_convert) {
        gst_bin_add(GST_BIN(pc->pipeline), pc->video_hw_convert);
    }
    gst_bin_add(GST_BIN(pc->pipeline), pc->video_convert);
    if (pc->use_videorate) {
        gst_bin_add_many(GST_BIN(pc->pipeline), pc->videorate, pc->videorate_caps, pc->queue_postrate, NULL);
    }

    if (pc->audio_enabled) {
        if (pc->audio_use_separate_port && pc->audio_udpsrc) {
            gst_bin_add(GST_BIN(pc->pipeline), pc->audio_udpsrc);
        }
        if (!pc->audio_use_separate_port && pc->audio_appsrc_element) {
            gst_bin_add(GST_BIN(pc->pipeline), pc->audio_appsrc_element);
        }
        gst_bin_add_many(GST_BIN(pc->pipeline),
                         pc->queue_audio_in,
                         pc->capsfilter_rtp_audio,
                         pc->audio_jitter,
                         pc->audio_depay,
                         pc->audio_decoder,
                         pc->audio_convert,
                         pc->audio_resample,
                         pc->audio_queue_sink,
                         pc->audio_sink,
                         NULL);
    }

    if (!gst_element_link(pc->appsrc_element, pc->queue0)) {
        g_set_error(error, g_quark_from_static_string("uv-viewer"), 14,
                    "Failed to link appsrc to ingress queue");
        return FALSE;
    }

    gboolean video_linked;
    if (pc->ingress_mode == UV_INGRESS_SHM) {
        video_linked = gst_element_link_many(pc->queue0, pc->parser, pc->capsfilter,
                                             pc->decoder, pc->queue_postdec, NULL);
    } else {
        video_linked = gst_element_link_many(pc->queue0, pc->tee, pc->queue_video_in,
                                             pc->capsfilter_rtp_video, pc->jitterbuffer,
                                             pc->depay, pc->parser, pc->capsfilter,
                                             pc->decoder, pc->queue_postdec, NULL);
    }
    if (!video_linked) {
        g_set_error(error, g_quark_from_static_string("uv-viewer"), 14,
                    "Failed to link video branch");
        return FALSE;
    }

    GstElement *tail = pc->queue_postdec;
    if (pc->video_hw_convert) {
        if (!gst_element_link(tail, pc->video_hw_convert)) {
            g_set_error(error, g_quark_from_static_string("uv-viewer"), 14,
                        "Failed to link hardware video converter");
            return FALSE;
        }
        tail = pc->video_hw_convert;
    }
    if (pc->use_videorate) {
        if (!gst_element_link(tail, pc->videorate)) {
            g_set_error(error, g_quark_from_static_string("uv-viewer"), 14,
                        "Failed to link videorate");
            return FALSE;
        }
        tail = pc->videorate;

        if (!gst_element_link(tail, pc->videorate_caps)) {
            g_set_error(error, g_quark_from_static_string("uv-viewer"), 14,
                        "Failed to link videorate capsfilter");
            return FALSE;
        }
        tail = pc->videorate_caps;

        if (!gst_element_link(tail, pc->queue_postrate)) {
            g_set_error(error, g_quark_from_static_string("uv-viewer"), 14,
                        "Failed to link post-videorate queue");
            return FALSE;
        }
        tail = pc->queue_postrate;
    }

    if (!gst_element_link(tail, pc->video_convert)) {
        g_set_error(error, g_quark_from_static_string("uv-viewer"), 14,
                    "Failed to link video convert");
        return FALSE;
    }

    if (!pipeline_attach_sink_from(pc, 0)) {
        g_set_error(error, g_quark_from_static_string("uv-viewer"), 14,
                    "Failed to create and link video sink");
        return FALSE;
    }

    if (pc->audio_enabled) {
        if (pc->audio_use_separate_port && pc->audio_udpsrc) {
            /* Separate UDP port: stamp the expected RTP caps on the udpsrc
             * output so the jitter buffer / depay negotiate immediately. */
            GstCaps *udp_caps = gst_caps_new_simple("application/x-rtp",
                                                    "media",         G_TYPE_STRING, "audio",
                                                    "encoding-name", G_TYPE_STRING, "OPUS",
                                                    "payload",       G_TYPE_INT,    (gint)pc->audio_payload_type,
                                                    "clock-rate",    G_TYPE_INT,    (gint)pc->audio_clock_rate,
                                                    NULL);
            if (udp_caps) {
                g_object_set(pc->audio_udpsrc, "caps", udp_caps, NULL);
                gst_caps_unref(udp_caps);
            }
            if (!gst_element_link(pc->audio_udpsrc, pc->queue_audio_in)) {
                g_set_error(error, g_quark_from_static_string("uv-viewer"), 14,
                            "Failed to link audio udpsrc to audio queue");
                return FALSE;
            }
            uv_log_info("Audio: listening on UDP :%u (separate from video)", pc->audio_listen_port);
        } else if (pc->audio_appsrc_element) {
            /* Shared-port mode: the relay thread will route audio packets
             * (PT == audio_payload_type) into this appsrc, leaving the
             * H.265 decoder to see only its own payload. */
            if (!gst_element_link(pc->audio_appsrc_element, pc->queue_audio_in)) {
                g_set_error(error, g_quark_from_static_string("uv-viewer"), 14,
                            "Failed to link audio appsrc to audio queue");
                return FALSE;
            }
            uv_log_info("Audio: shared with video port, demuxed by PT %u (relay-side)",
                        pc->audio_payload_type);
        } else {
            g_set_error(error, g_quark_from_static_string("uv-viewer"), 14,
                        "Audio enabled but no source available");
            return FALSE;
        }
        if (!gst_element_link_many(pc->queue_audio_in,
                                   pc->capsfilter_rtp_audio,
                                   pc->audio_jitter,
                                   pc->audio_depay,
                                   pc->audio_decoder,
                                   pc->audio_convert,
                                   pc->audio_resample,
                                   pc->audio_queue_sink,
                                   pc->audio_sink,
                                   NULL)) {
            g_set_error(error, g_quark_from_static_string("uv-viewer"), 14,
                        "Failed to link audio branch");
            return FALSE;
        }
    }

    set_appsrc_callbacks(pc);

    GstPad *dec_src = gst_element_get_static_pad(pc->decoder, "src");
    if (dec_src) {
        pc->decoder_probe_id = gst_pad_add_probe(dec_src,
                                                 GST_PAD_PROBE_TYPE_BUFFER |
                                                 GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
                                                 dec_src_probe, pc, NULL);
        gst_object_unref(dec_src);
    }

    if (pc->audio_enabled && pc->audio_resample) {
        GstPad *audio_pad = gst_element_get_static_pad(pc->audio_resample, "src");
        if (audio_pad) {
            pc->audio_probe_id = gst_pad_add_probe(audio_pad, GST_PAD_PROBE_TYPE_BUFFER,
                                                   audio_src_probe, pc, NULL);
            gst_object_unref(audio_pad);
        }
    }

    GstBus *bus = gst_element_get_bus(pc->pipeline);
    pc->bus_watch_id = gst_bus_add_watch(bus, bus_cb, pc);
    gst_object_unref(bus);

    /* Seed the audio appsrc with a silent Opus packet so the downstream
     * chain can negotiate caps and transition out of preroll even when no
     * real audio has arrived yet. Without this primer, both the audio
     * branch and the surrounding pipeline stall on the first state change.
     * Pattern lifted from waybeam-hub's pixelpilot module. */
    if (pc->audio_enabled && !pc->audio_use_separate_port && pc->audio_appsrc_element) {
        static const guint8 silent_opus_payload[3] = { 0xF8, 0xFF, 0xFE };
        guint8 primer[12 + sizeof(silent_opus_payload)];
        memset(primer, 0, sizeof(primer));
        primer[0]  = 0x80;                                  /* V=2 */
        primer[1]  = (guint8)(0x80 | (pc->audio_payload_type & 0x7F)); /* M=1, PT */
        primer[2]  = 0x00; primer[3] = 0x01;                /* seq=1 */
        primer[4]  = 0x00; primer[5] = 0x00;
        primer[6]  = 0x03; primer[7] = 0xC0;                /* ts=960 (20 ms @ 48 kHz) */
        primer[8]  = 0x00; primer[9] = 0x00;
        primer[10] = 0x00; primer[11] = 0x00;               /* ssrc=0 */
        memcpy(primer + 12, silent_opus_payload, sizeof(silent_opus_payload));

        GstBuffer *pb = gst_buffer_new_allocate(NULL, sizeof(primer), NULL);
        if (pb) {
            GstMapInfo map;
            if (gst_buffer_map(pb, &map, GST_MAP_WRITE)) {
                memcpy(map.data, primer, sizeof(primer));
                gst_buffer_unmap(pb, &map);
            }
            GST_BUFFER_FLAG_SET(pb, GST_BUFFER_FLAG_LIVE);
            gst_app_src_push_buffer(GST_APP_SRC(pc->audio_appsrc_element), pb);
        }
    }

    return TRUE;
}

static void set_appsrc_callbacks(PipelineController *pc) {
    GstAppSrcCallbacks callbacks = {
        .need_data = on_need_data,
        .enough_data = on_enough_data,
        .seek_data = NULL
    };
    gst_app_src_set_callbacks(GST_APP_SRC(pc->appsrc_element), &callbacks, pc, NULL);
}

static gpointer pipeline_loop_thread(gpointer data) {
    PipelineController *pc = (PipelineController *)data;
    if (pc->loop_context) {
        g_main_context_push_thread_default(pc->loop_context);
    }
    if (pc->loop) {
        g_main_loop_run(pc->loop);
    }
    if (pc->loop_context) {
        g_main_context_pop_thread_default(pc->loop_context);
    }
    return NULL;
}

gboolean pipeline_controller_init(PipelineController *pc, struct _UvViewer *viewer, GError **error) {
    (void)error;
    g_return_val_if_fail(pc != NULL, FALSE);
    memset(pc, 0, sizeof(*pc));
    pc->ingress_mode = UV_INGRESS_UDP;
    pc->viewer = viewer;
    pc->payload_type = viewer->config.payload_type;
    pc->clock_rate = viewer->config.clock_rate;
    pc->sync_to_clock = viewer->config.sync_to_clock;
    pc->videorate_fps_num = viewer->config.videorate_fps_numerator;
    pc->videorate_fps_den = viewer->config.videorate_fps_denominator;
    if (pc->videorate_fps_den == 0) {
        pc->videorate_fps_den = 1;
    }
    pc->use_videorate = viewer->config.videorate_enabled &&
                        pc->videorate_fps_num > 0 &&
                        pc->videorate_fps_den > 0;
    pc->decoder_preference = viewer->config.decoder_preference;
    pc->video_sink_preference = viewer->config.video_sink_preference;
    pc->audio_enabled = viewer->config.audio_enabled;
    pc->audio_payload_type = viewer->config.audio_payload_type;
    pc->audio_clock_rate = viewer->config.audio_clock_rate;
    if (pc->audio_clock_rate == 0) {
        pc->audio_clock_rate = 48000;
    }
    pc->audio_jitter_latency_ms = viewer->config.audio_jitter_latency_ms;
    pc->audio_use_separate_port = viewer->config.audio_use_separate_port;
    pc->audio_listen_port = viewer->config.audio_listen_port ? viewer->config.audio_listen_port : 5601;
    /* Avoid an obviously broken configuration: separate-port mode but the
     * audio port collides with the video listen port. Downgrade silently. */
    if (pc->audio_use_separate_port &&
        (gint)pc->audio_listen_port == viewer->config.listen_port) {
        pc->audio_use_separate_port = FALSE;
    }
    pc->audio_udpsrc = NULL;
    pc->audio_sink_is_fakesink = FALSE;
    pc->audio_probe_id = 0;
    pc->audio_last_buffer_us = 0;
    pc->audio_active_cached = FALSE;
    g_mutex_init(&pc->audio_lock);
    pc->sink_factories = NULL;
    pc->sink_factory_index = 0;
    return TRUE;
}

void pipeline_controller_deinit(PipelineController *pc) {
    if (!pc) return;
    pipeline_controller_stop(pc);
    if (pc->bus_watch_id) {
        GSource *source = pc->loop_context
                        ? g_main_context_find_source_by_id(pc->loop_context, pc->bus_watch_id)
                        : NULL;
        if (source) g_source_destroy(source);
        pc->bus_watch_id = 0;
    }
    remove_audio_probe(pc);
    if (pc->pipeline) {
        gst_object_unref(pc->pipeline);
        pc->pipeline = NULL;
    }
    pc->video_convert = NULL;
    pc->video_hw_convert = NULL;
    pc->videorate = NULL;
    pc->videorate_caps = NULL;
    pc->queue_postrate = NULL;
    pc->tee = NULL;
    pc->queue_video_in = NULL;
    pc->capsfilter_rtp_video = NULL;
    pc->queue_audio_in = NULL;
    pc->capsfilter_rtp_audio = NULL;
    pc->audio_udpsrc = NULL;
    pc->audio_appsrc_element = NULL;
    pc->audio_jitter = NULL;
    pc->audio_depay = NULL;
    pc->audio_decoder = NULL;
    pc->audio_convert = NULL;
    pc->audio_resample = NULL;
    pc->audio_queue_sink = NULL;
    pc->audio_sink = NULL;
    pc->audio_sink_is_fakesink = FALSE;
    pc->audio_probe_id = 0;
    pc->audio_last_buffer_us = 0;
    pc->audio_active_cached = FALSE;
    g_mutex_clear(&pc->audio_lock);
    if (pc->loop) {
        g_main_loop_unref(pc->loop);
        pc->loop = NULL;
    }
    if (pc->loop_context) {
        g_main_context_unref(pc->loop_context);
        pc->loop_context = NULL;
    }
    if (pc->sink_factories) {
        g_ptr_array_unref(pc->sink_factories);
        pc->sink_factories = NULL;
    }
    pc->sink_factory_index = 0;
}

gboolean pipeline_controller_start(PipelineController *pc, GError **error) {
    g_return_val_if_fail(pc != NULL, FALSE);
    ensure_gstreamer_initialized();

    if (!pc->loop_context) {
        pc->loop_context = g_main_context_new();
    }

    if (!pc->pipeline) {
        if (pc->loop_context) {
            g_main_context_push_thread_default(pc->loop_context);
        }
        gboolean built = build_pipeline(pc, error);
        if (pc->loop_context) {
            g_main_context_pop_thread_default(pc->loop_context);
        }
        if (!built) {
            return FALSE;
        }
    }

    if (!pc->loop) {
        pc->loop = g_main_loop_new(pc->loop_context, FALSE);
    }

    GstStateChangeReturn ret = gst_element_set_state(pc->pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        gst_element_set_state(pc->pipeline, GST_STATE_NULL);

        gboolean started = FALSE;
        while (pc->sink_factories && pc->sink_factory_index + 1 < pc->sink_factories->len) {
            guint next_index = pc->sink_factory_index + 1;
            const char *current_name = pipeline_current_sink_factory_name(pc);
            const char *next_name = pipeline_sink_factory_name_at(pc, next_index);
            uv_log_warn("Video sink factory %s failed to start; trying %s",
                        current_name ? current_name : "unknown",
                        next_name ? next_name : "unknown");

            pipeline_detach_current_sink(pc);
            if (!pipeline_attach_sink_from(pc, next_index)) {
                break;
            }

            ret = gst_element_set_state(pc->pipeline, GST_STATE_PLAYING);
            if (ret != GST_STATE_CHANGE_FAILURE) {
                started = TRUE;
                break;
            }

            gst_element_set_state(pc->pipeline, GST_STATE_NULL);
        }

        if (!started) {
            if (!pc->sink_is_fakesink && pipeline_swap_to_fakesink(pc)) {
                if (pc->sink_factories) {
                    for (guint idx = 0; idx < pc->sink_factories->len; idx++) {
                        const char *candidate = pipeline_sink_factory_name_at(pc, idx);
                        if (candidate && g_strcmp0(candidate, "fakesink") == 0) {
                            pc->sink_factory_index = idx;
                            break;
                        }
                    }
                }
                ret = gst_element_set_state(pc->pipeline, GST_STATE_PLAYING);
                if (ret != GST_STATE_CHANGE_FAILURE) {
                    uv_log_warn("Falling back to fakesink after sink failures");
                    started = TRUE;
                }
            }

            if (!started) {
                g_set_error(error, g_quark_from_static_string("uv-viewer"), 20,
                            "Failed to set pipeline to PLAYING");
                return FALSE;
            }
        }
    }

    if (ret != GST_STATE_CHANGE_FAILURE) {
        const char *sink_name = pipeline_current_sink_factory_name(pc);
        if (!sink_name && pc->sink_is_fakesink) {
            sink_name = "fakesink";
        }
        if (sink_name) {
            uv_log_info("Using video sink factory %s", sink_name);
        }
    }

    if (!pc->loop_thread) {
        pc->loop_thread = g_thread_new("uv-gst-loop", pipeline_loop_thread, pc);
        if (!pc->loop_thread) {
            g_set_error(error, g_quark_from_static_string("uv-viewer"), 21,
                        "Failed to create pipeline thread");
            gst_element_set_state(pc->pipeline, GST_STATE_NULL);
            return FALSE;
        }
    }
    return TRUE;
}

void pipeline_controller_stop(PipelineController *pc) {
    if (!pc) return;
    relay_controller_set_push_enabled(&pc->viewer->relay, FALSE);
    shm_ingress_set_push_enabled(&pc->viewer->shm_ingress, FALSE);
    if (pc->loop) g_main_loop_quit(pc->loop);
    if (pc->loop_thread) {
        g_thread_join(pc->loop_thread);
        pc->loop_thread = NULL;
    }
    remove_audio_probe(pc);
    if (pc->pipeline) {
        gst_element_set_state(pc->pipeline, GST_STATE_NULL);
    }
    g_mutex_lock(&pc->audio_lock);
    pc->audio_last_buffer_us = 0;
    pc->audio_active_cached = FALSE;
    g_mutex_unlock(&pc->audio_lock);
}

GstAppSrc *pipeline_controller_get_appsrc(PipelineController *pc) {
    return pc && pc->appsrc_element ? GST_APP_SRC(pc->appsrc_element) : NULL;
}

GstAppSrc *pipeline_controller_get_audio_appsrc(PipelineController *pc) {
    return pc && pc->audio_appsrc_element ? GST_APP_SRC(pc->audio_appsrc_element) : NULL;
}

void pipeline_controller_snapshot(PipelineController *pc, UvViewerStats *stats) {
    if (!pc || !stats) return;

    if (pc->queue0) {
        gint buffers = 0;
        guint bytes = 0;
        guint64 time_ns = 0;
        g_object_get(pc->queue0,
                     "current-level-buffers", &buffers,
                     "current-level-bytes", &bytes,
                     "current-level-time", &time_ns,
                     NULL);
        stats->queue0_valid = TRUE;
        stats->queue0.current_level_buffers = buffers;
        stats->queue0.current_level_bytes = bytes;
        stats->queue0.current_level_time_ms = (double)time_ns / 1e6;
    } else {
        stats->queue0_valid = FALSE;
    }

    gint64 now_us = g_get_monotonic_time();
    gboolean audio_active = FALSE;
    stats->audio_enabled = pc->audio_enabled;
    g_mutex_lock(&pc->audio_lock);
    if (pc->audio_enabled) {
        gint64 last = pc->audio_last_buffer_us;
        if (last > 0 && now_us > last && (now_us - last) <= 2000000) {
            audio_active = TRUE;
        }
        pc->audio_active_cached = audio_active;
    } else {
        pc->audio_active_cached = FALSE;
    }
    g_mutex_unlock(&pc->audio_lock);
    stats->audio_active = audio_active;
    guint64 frames_total;
    gint64 first_us;
    gint64 last_frame_us;
    double last_snapshot_fps;
    double inst_fps = 0.0;
    double avg_fps = 0.0;
    gboolean recent_frame = FALSE;
    gint64 window_start_us = now_us - 1000000;
    gint64 first_window_us = 0;
    gint64 last_window_us = 0;
    guint window_frames = 0;
    guint oldest_frame_idx;

    g_mutex_lock(&pc->viewer->decoder.lock);
    frames_total = pc->viewer->decoder.frames_total;
    first_us = pc->viewer->decoder.first_frame_us;
    last_frame_us = pc->viewer->decoder.prev_timestamp_us;
    last_snapshot_fps = pc->viewer->decoder.last_snapshot_fps;

    if (first_us != 0 && now_us > first_us) {
        double dt = (now_us - first_us) / 1e6;
        if (dt > 0.0) avg_fps = (double)frames_total / dt;
    }

    /* Compute current FPS from decoded-frame timestamps, not the GUI stats
     * timer interval.  The old snapshot-delta math could report a false
     * dip when a 200 ms GTK timer callback arrived late. */
    oldest_frame_idx = (pc->viewer->decoder.frame_times_head +
                        UV_DECODER_FPS_WINDOW_SAMPLES -
                        pc->viewer->decoder.frame_times_count) %
                       UV_DECODER_FPS_WINDOW_SAMPLES;
    for (guint i = 0; i < pc->viewer->decoder.frame_times_count; i++) {
        guint idx = (oldest_frame_idx + i) % UV_DECODER_FPS_WINDOW_SAMPLES;
        gint64 frame_us = pc->viewer->decoder.frame_times_us[idx];
        if (frame_us < window_start_us || frame_us > now_us) continue;
        if (first_window_us == 0) first_window_us = frame_us;
        last_window_us = frame_us;
        window_frames++;
    }

    if (last_frame_us > 0 && now_us > last_frame_us) {
        recent_frame = (now_us - last_frame_us) < 500000; // 0.5s
    }

    if (recent_frame && window_frames >= 2 && last_window_us > first_window_us) {
        inst_fps = (double)(window_frames - 1u) * 1000000.0 /
                   (double)(last_window_us - first_window_us);
    } else if (recent_frame && last_snapshot_fps > 0.0) {
        inst_fps = last_snapshot_fps;
    } else if (avg_fps > 0.0) {
        inst_fps = avg_fps;
    }

    pc->viewer->decoder.last_snapshot_fps = inst_fps;
    g_mutex_unlock(&pc->viewer->decoder.lock);

    stats->decoder.frames_total = frames_total;
    stats->decoder.instantaneous_fps = inst_fps;
    stats->decoder.average_fps = avg_fps;
    stats->decoder.caps_str[0] = '\0';

    if (pc->decoder) {
        GstPad *pad = gst_element_get_static_pad(pc->decoder, "src");
        if (pad) {
            GstCaps *caps = gst_pad_get_current_caps(pad);
            if (caps && !gst_caps_is_empty(caps)) {
                gchar *caps_str = gst_caps_to_string(caps);
                if (caps_str) {
                    g_strlcpy(stats->decoder.caps_str, caps_str, sizeof(stats->decoder.caps_str));
                    g_free(caps_str);
                }
            }
            if (caps) gst_caps_unref(caps);
            gst_object_unref(pad);
        }
    }
}

gboolean pipeline_controller_update(PipelineController *pc, const UvPipelineOverrides *overrides, GError **error) {
    (void)pc; (void)overrides;
    g_set_error(error, g_quark_from_static_string("uv-viewer"), 30,
                "Pipeline overrides not implemented yet");
    return FALSE;
}

GstElement *pipeline_controller_get_sink(PipelineController *pc) {
    return pc ? pc->sink : NULL;
}

UvIngressMode pipeline_controller_ingress_mode(PipelineController *pc) {
    return pc ? (UvIngressMode)__atomic_load_n(&pc->ingress_mode, __ATOMIC_ACQUIRE)
              : UV_INGRESS_UDP;
}

void pipeline_controller_set_ingress_mode(PipelineController *pc, UvIngressMode mode) {
    if (pc) __atomic_store_n(&pc->ingress_mode, (int)mode, __ATOMIC_RELEASE);
}
