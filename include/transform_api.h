#ifndef TRANSFORM_API_H
#define TRANSFORM_API_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct TransformContext TransformContext;

/*
 * Transform plugin API.
 *
 * A shared library (.so) must export the four functions below.
 * The relay creates one TransformContext per media stream.
 * If transform_create returns NULL the stream is passed through unchanged.
 *
 * codec_name values are ffmpeg codec descriptor names: "h264", "aac", "hevc",
 * "av1", etc.  params is a plugin-specific string (may be empty).
 */

TransformContext* transform_create(const char* codec_name, const char* params);
void transform_destroy(TransformContext* ctx);
size_t transform_get_max_size(const TransformContext* ctx, size_t frame_size);
size_t transform_apply(TransformContext* ctx, const uint8_t* src, size_t src_size, uint8_t* dst);

#ifdef __cplusplus
}
#endif

#endif /* TRANSFORM_API_H */
