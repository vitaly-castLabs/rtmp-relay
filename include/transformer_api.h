#ifndef TRANSFORMER_API_H
#define TRANSFORMER_API_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct TransformerContext TransformerContext;

/*
 * Transformer plugin API.
 *
 * A shared library (.so) must export the four functions below.
 * The relay creates one TransformerContext per media stream.
 * If transformer_create returns NULL the stream is passed through unchanged.
 *
 * codec_name values are ffmpeg codec descriptor names: "h264", "aac", "hevc",
 * "av1", etc.  params is a plugin-specific string (may be empty).
 */

TransformerContext* transformer_create(const char* codec_name, const char* params);
void transformer_destroy(TransformerContext* ctx);
size_t transformer_get_max_size(const TransformerContext* ctx, size_t frame_size);
size_t transformer_transform(TransformerContext* ctx, const uint8_t* src, size_t src_size, uint8_t* dst);

#ifdef __cplusplus
}
#endif

#endif /* TRANSFORMER_API_H */
