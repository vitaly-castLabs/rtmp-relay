#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct TransformContext;

class TransformPlugin {
public:
    explicit TransformPlugin(const std::string& so_path);
    ~TransformPlugin();

    TransformPlugin(const TransformPlugin&) = delete;
    TransformPlugin& operator=(const TransformPlugin&) = delete;

    TransformContext* create(const char* codec_name, const char* params) const;
    void destroy(TransformContext* ctx) const;
    size_t get_max_size(const TransformContext* ctx, size_t frame_size) const;
    size_t apply(TransformContext* ctx, const uint8_t* src, size_t src_size, uint8_t* dst) const;

private:
    void* handle_ = nullptr;

    using create_fn = TransformContext* (*)(const char*, const char*);
    using destroy_fn = void (*)(TransformContext*);
    using get_max_size_fn = size_t (*)(const TransformContext*, size_t);
    using apply_fn = size_t (*)(TransformContext*, const uint8_t*, size_t, uint8_t*);

    create_fn create_ = nullptr;
    destroy_fn destroy_ = nullptr;
    get_max_size_fn get_max_size_ = nullptr;
    apply_fn apply_ = nullptr;
};
