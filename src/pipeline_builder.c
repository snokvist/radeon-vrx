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

static GstPadProbeReturn dec_src_probe(GstPad *pad, GstPadProbeInfo *info, gpointer user_data) {
    (void)pad;
    PipelineController *pc = (PipelineController *)user_data;
    if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER) {
        gint64 now_us = g_get_monotonic_time();
        uv_internal_decoder_stats_push_frame(&pc->viewer->decoder, now_us);
    }
    return GST_PAD_PROBE_OK;
}

static void on_need_data(GstAppSrc *src, guint length, gpointer user_data) {
    (void)src; (void)length;
    PipelineController *pc = (PipelineController *)user_data;
    relay_controller_set_push_enabled(&pc->viewer->relay, TRUE);
}

static void on_enough_data(GstAppSrc *src, gpointer user_data) {
    (void)src;
    PipelineController *pc = (PipelineController *)user_data;
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

static gboolean build_pipeline(PipelineController *pc, GError **error) {
    UvViewer *viewer = pc->viewer;
    pc->appsrc_element   = gst_element_factory_make("appsrc", "src");
    GstElement *capsf_rtp = gst_element_factory_make("capsfilter", "cf_rtp");
    pc->queue0           = gst_element_factory_make("queue", "queue0");
    pc->jitterbuffer     = gst_element_factory_make("rtpjitterbuffer", "jbuf");
    pc->depay            = gst_element_factory_make("rtph265depay", "depay");
    pc->parser           = gst_element_factory_make("h265parse", "parser");
    pc->capsfilter       = gst_element_factory_make("capsfilter", "h265caps");
    pc->decoder          = gst_element_factory_make("vah265dec", "decoder");
    if (!pc->decoder) pc->decoder = gst_element_factory_make("vaapih265dec", "decoder");
    if (!pc->decoder) {
        pc->decoder = gst_element_factory_make("avdec_h265", "decoder");
        if (pc->decoder) uv_log_warn("Using avdec_h265 software decoder fallback");
    }
    pc->queue_postdec    = gst_element_factory_make("queue", "queue_postdec");
    pc->video_convert    = gst_element_factory_make("videoconvert", "video_convert");

    gboolean headless = (g_getenv("WAYLAND_DISPLAY") == NULL && g_getenv("DISPLAY") == NULL);
    gboolean sink_is_fakesink = FALSE;
    if (headless) {
        pc->sink = gst_element_factory_make("fakesink", "sink");
        if (pc->sink) {
            g_object_set(pc->sink, "sync", FALSE, "async", FALSE, NULL);
            sink_is_fakesink = TRUE;
        }
    } else {
        pc->sink = gst_element_factory_make("gtk4paintablesink", "sink");
        if (!pc->sink) pc->sink = gst_element_factory_make("waylandsink", "sink");
        if (!pc->sink) pc->sink = gst_element_factory_make("glimagesink", "sink");
        if (!pc->sink) pc->sink = gst_element_factory_make("xvimagesink", "sink");
        if (!pc->sink) pc->sink = gst_element_factory_make("autovideosink", "sink");
    }
    if (!pc->sink) {
        pc->sink = gst_element_factory_make("fakesink", "sink");
        if (pc->sink) {
            g_object_set(pc->sink, "sync", FALSE, "async", FALSE, NULL);
            sink_is_fakesink = TRUE;
        }
    }

    if (!pc->appsrc_element || !capsf_rtp || !pc->queue0 || !pc->jitterbuffer ||
        !pc->depay || !pc->parser || !pc->capsfilter || !pc->decoder ||
        !pc->queue_postdec || !pc->video_convert || !pc->sink) {
        g_set_error(error, g_quark_from_static_string("uv-viewer"), 10,
                    "Failed to create required GStreamer elements");
        return FALSE;
    }

    pc->pipeline = gst_pipeline_new("uv-udp-h265");
    if (!pc->pipeline) {
        g_set_error(error, g_quark_from_static_string("uv-viewer"), 11,
                    "Failed to create pipeline");
        return FALSE;
    }

    gchar *caps_rtp_str = g_strdup_printf(
        "application/x-rtp,media=video,encoding-name=H265,payload=%d,clock-rate=%d",
        pc->payload_type, pc->clock_rate);
    GstCaps *caps_rtp = gst_caps_from_string(caps_rtp_str);
    g_free(caps_rtp_str);
    if (!caps_rtp) {
        g_set_error(error, g_quark_from_static_string("uv-viewer"), 12,
                    "Failed to build RTP caps");
        return FALSE;
    }

    GstCaps *caps_h265 = gst_caps_from_string("video/x-h265,stream-format=byte-stream,alignment=au");
    if (!caps_h265) {
        gst_caps_unref(caps_rtp);
        g_set_error(error, g_quark_from_static_string("uv-viewer"), 13,
                    "Failed to build H265 caps");
        return FALSE;
    }

    g_object_set(pc->appsrc_element,
                 "is-live", TRUE,
                 "format", GST_FORMAT_BYTES,
                 "block", FALSE,
                 "max-bytes", (guint64)(2 * 1024 * 1024),
                 "stream-type", GST_APP_STREAM_TYPE_STREAM,
                 NULL);
    gst_app_src_set_caps(GST_APP_SRC(pc->appsrc_element), caps_rtp);
    g_object_set(capsf_rtp, "caps", caps_rtp, NULL);
    gst_caps_unref(caps_rtp);

    g_object_set(pc->queue0,
                 "leaky", 2,
                 "max-size-buffers", viewer->config.queue_max_buffers,
                 "max-size-bytes", 0,
                 "max-size-time", (guint64)0,
                 NULL);

    g_object_set(pc->jitterbuffer,
                 "latency", (guint)MAX(viewer->config.jitter_latency_ms, 0u),
                 "drop-on-latency", viewer->config.jitter_drop_on_latency,
                 "do-lost", viewer->config.jitter_do_lost,
                 "post-drop-messages", viewer->config.jitter_post_drop_messages,
                 NULL);

    g_object_set(pc->parser, "config-interval", -1, NULL);
    g_object_set(pc->capsfilter, "caps", caps_h265, NULL);
    gst_caps_unref(caps_h265);

    if (!sink_is_fakesink) {
        g_object_set(pc->sink, "sync", viewer->config.sync_to_clock ? TRUE : FALSE, NULL);
    }

    pc->sink_is_fakesink = sink_is_fakesink;

    gst_bin_add_many(GST_BIN(pc->pipeline),
                     pc->appsrc_element, capsf_rtp, pc->queue0, pc->jitterbuffer,
                     pc->depay, pc->parser, pc->capsfilter,
                     pc->decoder, pc->queue_postdec, pc->video_convert, pc->sink, NULL);

    if (!gst_element_link_many(pc->appsrc_element, capsf_rtp, pc->queue0, pc->jitterbuffer,
                               pc->depay, pc->parser, pc->capsfilter,
                               pc->decoder, pc->queue_postdec, pc->video_convert, pc->sink, NULL)) {
        g_set_error(error, g_quark_from_static_string("uv-viewer"), 14,
                    "Failed to link pipeline");
        return FALSE;
    }

    set_appsrc_callbacks(pc);

    GstPad *dec_src = gst_element_get_static_pad(pc->decoder, "src");
    if (dec_src) {
        pc->decoder_probe_id = gst_pad_add_probe(dec_src, GST_PAD_PROBE_TYPE_BUFFER,
                                                 dec_src_probe, pc, NULL);
        gst_object_unref(dec_src);
    }

    GstBus *bus = gst_element_get_bus(pc->pipeline);
    pc->bus_watch_id = gst_bus_add_watch(bus, bus_cb, pc);
    gst_object_unref(bus);

    relay_controller_set_appsrc(&viewer->relay, GST_APP_SRC(pc->appsrc_element));
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
    pc->viewer = viewer;
    pc->payload_type = viewer->config.payload_type;
    pc->clock_rate = viewer->config.clock_rate;
    pc->sync_to_clock = viewer->config.sync_to_clock;
    return TRUE;
}

void pipeline_controller_deinit(PipelineController *pc) {
    if (!pc) return;
    pipeline_controller_stop(pc);
    if (pc->bus_watch_id) {
        g_source_remove(pc->bus_watch_id);
        pc->bus_watch_id = 0;
    }
    if (pc->pipeline) {
        gst_object_unref(pc->pipeline);
        pc->pipeline = NULL;
    }
    pc->video_convert = NULL;
    if (pc->loop) {
        g_main_loop_unref(pc->loop);
        pc->loop = NULL;
    }
    if (pc->loop_context) {
        g_main_context_unref(pc->loop_context);
        pc->loop_context = NULL;
    }
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
        if (!pc->sink_is_fakesink && pipeline_swap_to_fakesink(pc)) {
            ret = gst_element_set_state(pc->pipeline, GST_STATE_PLAYING);
        }
        if (ret == GST_STATE_CHANGE_FAILURE) {
            g_set_error(error, g_quark_from_static_string("uv-viewer"), 20,
                        "Failed to set pipeline to PLAYING");
            return FALSE;
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
    if (pc->loop) g_main_loop_quit(pc->loop);
    if (pc->loop_thread) {
        g_thread_join(pc->loop_thread);
        pc->loop_thread = NULL;
    }
    if (pc->pipeline) {
        gst_element_set_state(pc->pipeline, GST_STATE_NULL);
    }
}

GstAppSrc *pipeline_controller_get_appsrc(PipelineController *pc) {
    return pc && pc->appsrc_element ? GST_APP_SRC(pc->appsrc_element) : NULL;
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
    guint64 frames_total;
    gint64 first_us, prev_us;
    guint64 prev_frames;

    g_mutex_lock(&pc->viewer->decoder.lock);
    frames_total = pc->viewer->decoder.frames_total;
    first_us = pc->viewer->decoder.first_frame_us;
    prev_us = pc->viewer->decoder.prev_timestamp_us;
    prev_frames = pc->viewer->decoder.prev_frames;
    pc->viewer->decoder.prev_timestamp_us = now_us;
    pc->viewer->decoder.prev_frames = frames_total;
    g_mutex_unlock(&pc->viewer->decoder.lock);

    double inst_fps = 0.0;
    double avg_fps = 0.0;
    if (prev_us != 0 && now_us > prev_us && frames_total >= prev_frames) {
        double dt = (now_us - prev_us) / 1e6;
        if (dt > 0.0) inst_fps = (double)(frames_total - prev_frames) / dt;
    }
    if (first_us != 0 && now_us > first_us) {
        double dt = (now_us - first_us) / 1e6;
        if (dt > 0.0) avg_fps = (double)frames_total / dt;
    }

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
