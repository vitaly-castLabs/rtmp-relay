#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct TransformerContext;

class TransformerPlugin {
public:
    explicit TransformerPlugin(const std::string& so_path);
    ~TransformerPlugin();

    TransformerPlugin(const TransformerPlugin&) = delete;
    TransformerPlugin& operator=(const TransformerPlugin&) = delete;

    TransformerContext* create(const char* codec_name, const char* params) const;
    void destroy(TransformerContext* ctx) const;
    size_t get_max_size(const TransformerContext* ctx, size_t frame_size) const;
    size_t transform(TransformerContext* ctx, const uint8_t* src, size_t src_size, uint8_t* dst) const;

private:
    void* handle_ = nullptr;

    using create_fn = TransformerContext* (*)(const char*, const char*);
    using destroy_fn = void (*)(TransformerContext*);
    using get_max_size_fn = size_t (*)(const TransformerContext*, size_t);
    using transform_fn = size_t (*)(TransformerContext*, const uint8_t*, size_t, uint8_t*);

    create_fn create_ = nullptr;
    destroy_fn destroy_ = nullptr;
    get_max_size_fn get_max_size_ = nullptr;
    transform_fn transform_ = nullptr;
};
