#define _GNU_SOURCE
#include "uv_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static inline uint32_t load_u32(const void *base, size_t off) {
    return __atomic_load_n((const uint32_t *)((const uint8_t *)base + off), __ATOMIC_ACQUIRE);
}

static inline uint64_t load_u64(const void *base, size_t off) {
    return __atomic_load_n((const uint64_t *)((const uint8_t *)base + off), __ATOMIC_ACQUIRE);
}

static inline void store_u32(void *base, size_t off, uint32_t value, int order) {
    __atomic_store_n((uint32_t *)((uint8_t *)base + off), value, order);
}

static inline void store_u64(void *base, size_t off, uint64_t value) {
    __atomic_store_n((uint64_t *)((uint8_t *)base + off), value, __ATOMIC_RELEASE);
}

static void shm_detach_locked(ShmIngress *si) {
    if (si->base) munmap(si->base, si->map_size);
    si->base = NULL;
    si->map_size = 0;
    si->attached = FALSE;
}

static gboolean shm_try_attach(ShmIngress *si) {
    int fd = shm_open(si->name, O_RDWR, 0);
    if (fd < 0) return FALSE;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < (off_t)VFRM_HEADER_SIZE) {
        close(fd);
        return FALSE;
    }
    void *base = mmap(NULL, (size_t)st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (base == MAP_FAILED) return FALSE;

    uint32_t slots = load_u32(base, VFRM_OFF_SLOT_COUNT);
    uint32_t data_size = load_u32(base, VFRM_OFF_SLOT_DATA_SIZE);
    size_t stride = vfrm_align8(sizeof(uint32_t) + (size_t)data_size);
    uint64_t expected = VFRM_HEADER_SIZE + (uint64_t)slots * stride;
    gboolean valid = load_u32(base, VFRM_OFF_INIT_COMPLETE) == 1 &&
                     load_u32(base, VFRM_OFF_MAGIC) == VFRM_MAGIC &&
                     load_u32(base, VFRM_OFF_VERSION) == VFRM_VERSION &&
                     slots > 0 && (slots & (slots - 1u)) == 0 && data_size > 0 &&
                     expected == (uint64_t)st.st_size &&
                     load_u32(base, VFRM_OFF_TOTAL_SIZE) == (uint32_t)st.st_size;
    if (!valid) {
        munmap(base, (size_t)st.st_size);
        return FALSE;
    }

    g_mutex_lock(&si->lock);
    gboolean replacing_producer = si->source_index >= 0;
    shm_detach_locked(si);
    si->base = base;
    si->map_size = (size_t)st.st_size;
    si->slot_count = slots;
    si->slot_data_size = data_size;
    si->stride = stride;
    si->st_dev = st.st_dev;
    si->st_ino = st.st_ino;
    si->attached = TRUE;
    if (replacing_producer) {
        /* A recreated ring is a brand-new encoded stream. Gate ingress on the
         * next IDR and flush stale parser/decoder state when it arrives. */
        si->waiting_for_idr = TRUE;
        si->stream_reset_pending = TRUE;
    }
    g_mutex_unlock(&si->lock);

    if (si->source_index < 0) {
        char label[UV_VIEWER_ADDR_MAX];
        g_snprintf(label, sizeof(label), "shm:%s", si->name);
        si->source_index = relay_controller_register_shm(si->registry, label);
    } else if (replacing_producer) {
        /* Re-emit selection so the GUI proactively requests a decoder-bootstrap
         * IDR for the replacement producer before the gate has to wait for a
         * natural keyframe. */
        relay_controller_shm_reattached(si->registry, si->source_index);
    }
    uv_log_info("SHM ingress attached to %s (%u slots, %u bytes each)",
                si->name, slots, data_size);
    return TRUE;
}

static gboolean backing_object_current(ShmIngress *si) {
    int fd = shm_open(si->name, O_RDWR, 0);
    if (fd < 0) return FALSE;
    struct stat st;
    gboolean current = fstat(fd, &st) == 0 && st.st_dev == si->st_dev &&
                       st.st_ino == si->st_ino &&
                       load_u32(si->base, VFRM_OFF_INIT_COMPLETE) == 1;
    close(fd);
    return current;
}

static void push_frame(ShmIngress *si, const uint8_t *data, size_t len,
                       const VencFrameMeta *meta) {
    if (len <= sizeof(*meta) || meta->codec != UV_FRAME_CODEC_H265 || meta->reserved != 0) {
        g_mutex_lock(&si->lock);
        si->bad_slots++;
        g_mutex_unlock(&si->lock);
        return;
    }
    const uint8_t *au = data + sizeof(*meta);
    size_t au_len = len - sizeof(*meta);
    relay_controller_shm_frame(si->registry, si->source_index, au, au_len, meta);

    g_mutex_lock(&si->lock);
    si->frames++;
    si->bytes += au_len;
    gboolean is_idr = (meta->flags & UV_FRAME_FLAG_IDR) != 0;
    /* A newly bound appsrc / recreated ring must start on a random-access unit.
     * Drop deltas until the first IDR so we never seed a decoder mid-GOP. */
    gboolean start_stream = si->waiting_for_idr && is_idr && si->appsrc;
    gboolean reset_stream = start_stream && si->stream_reset_pending;
    gboolean skip_delta = si->waiting_for_idr && !is_idr;
    GstAppSrc *appsrc = !skip_delta && (si->push_enabled || start_stream) && si->appsrc
                      ? GST_APP_SRC(gst_object_ref(si->appsrc)) : NULL;
    if (start_stream) {
        si->waiting_for_idr = FALSE;
        si->stream_reset_pending = FALSE;
        /* Force out of a stale enough-data state left by the previous stream so
         * the random-access unit is accepted; callbacks resume flow control. */
        si->push_enabled = TRUE;
    }
    g_mutex_unlock(&si->lock);
    if (!appsrc) return;

    if (reset_stream) {
        /* A recreated ring starts a new encoded stream. Flush stale parser and
         * decoder state before feeding its first access unit. GstAppSrc is
         * referenced above, so this remains safe across a concurrent pipeline
         * rebuild, which clears si->appsrc before destroying the old pipeline. */
        gst_element_send_event(GST_ELEMENT(appsrc), gst_event_new_flush_start());
        gst_element_send_event(GST_ELEMENT(appsrc), gst_event_new_flush_stop(TRUE));
        uv_log_info("SHM ingress %s reset decoder stream after producer restart", si->name);
    }
    GstBuffer *buffer = gst_buffer_new_allocate(NULL, au_len, NULL);
    if (buffer) {
        gst_buffer_fill(buffer, 0, au, au_len);
        if (start_stream) GST_BUFFER_FLAG_SET(buffer, GST_BUFFER_FLAG_DISCONT);
        GstFlowReturn flow = gst_app_src_push_buffer(appsrc, buffer);
        if (flow != GST_FLOW_OK && flow != GST_FLOW_FLUSHING) {
            uv_log_warn("SHM appsrc push returned %s", gst_flow_get_name(flow));
        }
    }
    gst_object_unref(appsrc);
}

static gpointer shm_thread_run(gpointer data) {
    ShmIngress *si = data;
    gint64 last_frame_us = g_get_monotonic_time();
    while (!si->stop) {
        if (!si->attached) {
            if (!shm_try_attach(si)) g_usleep(500000);
            last_frame_us = g_get_monotonic_time();
            continue;
        }
        uint64_t read_idx = load_u64(si->base, VFRM_OFF_READ_IDX);
        uint64_t write_idx = load_u64(si->base, VFRM_OFF_WRITE_IDX);
        gboolean drained = FALSE;
        while (!si->stop && read_idx != write_idx) {
            uint8_t *slot = (uint8_t *)si->base + VFRM_HEADER_SIZE +
                            (read_idx & (si->slot_count - 1u)) * si->stride;
            uint32_t length;
            memcpy(&length, slot, sizeof(length));
            if (length > si->slot_data_size || length < sizeof(VencFrameMeta)) {
                g_mutex_lock(&si->lock);
                si->bad_slots++;
                g_mutex_unlock(&si->lock);
            } else {
                VencFrameMeta meta;
                memcpy(&meta, slot + sizeof(length), sizeof(meta));
                push_frame(si, slot + sizeof(length), length, &meta);
            }
            read_idx++;
            store_u64(si->base, VFRM_OFF_READ_IDX, read_idx);
            write_idx = load_u64(si->base, VFRM_OFF_WRITE_IDX);
            drained = TRUE;
        }
        if (drained) {
            last_frame_us = g_get_monotonic_time();
            continue;
        }
        store_u32(si->base, VFRM_OFF_CONSUMER_WAITING, 1, __ATOMIC_SEQ_CST);
        uint32_t seq = load_u32(si->base, VFRM_OFF_FUTEX_SEQ);
        struct timespec timeout = { .tv_sec = 0, .tv_nsec = 100000000 };
        syscall(SYS_futex, (uint32_t *)((uint8_t *)si->base + VFRM_OFF_FUTEX_SEQ),
                FUTEX_WAIT, seq, &timeout, NULL, 0);
        store_u32(si->base, VFRM_OFF_CONSUMER_WAITING, 0, __ATOMIC_SEQ_CST);
        if (g_get_monotonic_time() - last_frame_us >= 500000 && !backing_object_current(si)) {
            g_mutex_lock(&si->lock);
            shm_detach_locked(si);
            si->reattaches++;
            g_mutex_unlock(&si->lock);
            uv_log_warn("SHM ingress %s detached; waiting for producer", si->name);
        }
    }
    return NULL;
}

gboolean shm_ingress_init(ShmIngress *si, struct _UvViewer *viewer,
                          RelayController *registry) {
    memset(si, 0, sizeof(*si));
    g_mutex_init(&si->lock);
    si->enabled = viewer->config.shm_enabled;
    const char *name = viewer->config.shm_name[0] ? viewer->config.shm_name : "venc_frame_out";
    g_snprintf(si->name, sizeof(si->name), "%s%s", name[0] == '/' ? "" : "/", name);
    si->registry = registry;
    si->source_index = -1;
    return TRUE;
}

void shm_ingress_deinit(ShmIngress *si) {
    if (!si) return;
    shm_ingress_stop(si);
    shm_ingress_set_appsrc(si, NULL);
    g_mutex_lock(&si->lock);
    shm_detach_locked(si);
    g_mutex_unlock(&si->lock);
    g_mutex_clear(&si->lock);
}

gboolean shm_ingress_start(ShmIngress *si) {
    if (!si || !si->enabled || si->thread) return TRUE;
    si->stop = FALSE;
    si->thread = g_thread_new("uv-shm-ingress", shm_thread_run, si);
    return si->thread != NULL;
}

void shm_ingress_stop(ShmIngress *si) {
    if (!si) return;
    si->stop = TRUE;
    if (si->thread) {
        g_thread_join(si->thread);
        si->thread = NULL;
    }
}

void shm_ingress_set_appsrc(ShmIngress *si, GstAppSrc *appsrc) {
    g_mutex_lock(&si->lock);
    if (appsrc) gst_object_ref(appsrc);
    GstAppSrc *old = si->appsrc;
    si->appsrc = appsrc;
    if (appsrc && appsrc != old) {
        /* An appsrc belongs to one parser/decoder instance. Never seed a new
         * instance with inter-predicted frames from the middle of a GOP. */
        si->waiting_for_idr = TRUE;
    }
    g_mutex_unlock(&si->lock);
    if (old) gst_object_unref(old);
}

void shm_ingress_set_push_enabled(ShmIngress *si, gboolean enabled) {
    g_mutex_lock(&si->lock);
    si->push_enabled = enabled;
    g_mutex_unlock(&si->lock);
}

void shm_ingress_snapshot(ShmIngress *si, UvSourceStats *stats) {
    g_mutex_lock(&si->lock);
    stats->shm_attached = si->attached;
    stats->shm_oversize_drops = si->oversize_drops;
    stats->shm_bad_slots = si->bad_slots;
    stats->shm_reattaches = si->reattaches;
    if (si->attached) {
        uint64_t used = load_u64(si->base, VFRM_OFF_WRITE_IDX) -
                        load_u64(si->base, VFRM_OFF_READ_IDX);
        if (used > si->slot_count) used = si->slot_count;
        stats->shm_fill_pct = 100.0 * (double)used / (double)si->slot_count;
    }
    g_mutex_unlock(&si->lock);
}
