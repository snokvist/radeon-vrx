#ifndef FRAME_SHM_FORMAT_H
#define FRAME_SHM_FORMAT_H

#include <stddef.h>
#include <stdint.h>

#define VFRM_MAGIC 0x5646524Du
#define VFRM_VERSION 1u
#define VFRM_HEADER_SIZE 192u
#define VFRM_DEFAULT_SLOT_COUNT 16u
#define VFRM_DEFAULT_SLOT_DATA_SIZE (512u * 1024u)

#define VFRM_OFF_MAGIC 0u
#define VFRM_OFF_VERSION 4u
#define VFRM_OFF_SLOT_COUNT 8u
#define VFRM_OFF_SLOT_DATA_SIZE 12u
#define VFRM_OFF_TOTAL_SIZE 16u
#define VFRM_OFF_EPOCH 20u
#define VFRM_OFF_INIT_COMPLETE 24u
#define VFRM_OFF_WRITE_IDX 64u
#define VFRM_OFF_FUTEX_SEQ 72u
#define VFRM_OFF_READ_IDX 128u
#define VFRM_OFF_CONSUMER_WAITING 136u

#define UV_FRAME_CODEC_H265 0x01u
#define UV_FRAME_FLAG_IDR 0x01u
#define UV_FRAME_FLAG_GDR 0x02u     /* GDR rolling intra-refresh stripe active */
#define UV_FRAME_FLAG_ENHANCE 0x04u /* SVC-T enhance layer (droppable) */

/* Bytes 6-7 were a single "reserved" u16 (producer <= v0.42.x). waybeam_venc
 * #179 (>= v0.43.0) repurposed them into the GDR cycle position/length, which
 * are non-zero on every non-IDR frame whenever intra-refresh is active. Older
 * consumers that rejected reserved != 0 dropped every delta frame; do not
 * reintroduce that check. */
typedef struct {
    uint32_t pts;
    uint8_t codec;
    uint8_t flags;
    uint8_t gdr_pos; /* 0-based position in the GDR cycle (0 when inactive) */
    uint8_t gdr_len; /* GDR cycle length in frames (0 when inactive) */
} VencFrameMeta;

_Static_assert(sizeof(VencFrameMeta) == 8, "VencFrameMeta must be 8 bytes");

static inline size_t vfrm_align8(size_t value) {
    return (value + 7u) & ~(size_t)7u;
}

#endif
