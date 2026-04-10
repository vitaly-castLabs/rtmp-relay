#include "transform_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct TransformContext {
    FILE* file;
};

/* AVCC length-prefixed filler NALU (16 bytes total):
 *   4 bytes  length prefix  = 12
 *   1 byte   NALU header    = 0x0C  (nal_unit_type 12 = filler_data)
 *  10 bytes  0xFF           = filler payload
 *   1 byte   0x80           = rbsp_trailing_bits
 */
static const uint8_t filler_nalu[16] = {0x00, 0x00, 0x00, 0x0C, 0x0C, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x80};

TransformContext* transform_create(const char* codec_name, const char* params) {
    if (strcmp(codec_name, "h264") != 0)
        return NULL;

    const char* prefix = "file=";
    const char* p = params ? strstr(params, prefix) : NULL;
    if (!p) {
        fprintf(stderr, "transform: missing file= in params\n");
        return NULL;
    }

    const char* path = p + strlen(prefix);
    FILE* f = fopen(path, "wb");
    if (!f) {
        perror("transform: fopen");
        return NULL;
    }

    TransformContext* ctx = calloc(1, sizeof(*ctx));
    ctx->file = f;
    return ctx;
}

void transform_destroy(TransformContext* ctx) {
    if (ctx->file)
        fclose(ctx->file);
    free(ctx);
}

size_t transform_get_max_size(const TransformContext* ctx, size_t frame_size) {
    (void)ctx;
    return frame_size + sizeof(filler_nalu);
}

static const uint8_t annex_b_start_code[4] = {0x00, 0x00, 0x00, 0x01};

static void write_annex_b(FILE* f, const uint8_t* avcc, size_t avcc_size) {
    size_t offset = 0;
    while (offset + 4 <= avcc_size) {
        uint32_t nalu_len = ((uint32_t)avcc[offset] << 24) | ((uint32_t)avcc[offset + 1] << 16) | ((uint32_t)avcc[offset + 2] << 8) |
                            (uint32_t)avcc[offset + 3];
        offset += 4;
        if (nalu_len > avcc_size - offset)
            break;
        fwrite(annex_b_start_code, 1, sizeof(annex_b_start_code), f);
        fwrite(avcc + offset, 1, nalu_len, f);
        offset += nalu_len;
    }
}

size_t transform_apply(TransformContext* ctx, const uint8_t* src, size_t src_size, uint8_t* dst) {
    if (ctx->file)
        write_annex_b(ctx->file, src, src_size);

    memcpy(dst, src, src_size);
    memcpy(dst + src_size, filler_nalu, sizeof(filler_nalu));
    return src_size + sizeof(filler_nalu);
}
