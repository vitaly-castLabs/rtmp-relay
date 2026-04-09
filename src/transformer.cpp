#include "transformer.h"

#include <dlfcn.h>
#include <stdexcept>

namespace {

template <typename Fn>
Fn load_sym(void* handle, const char* name) {
    void* sym = dlsym(handle, name);
    if (!sym)
        throw std::runtime_error(std::string("dlsym(") + name + "): " + dlerror());
    return reinterpret_cast<Fn>(sym);
}

} // namespace

TransformerPlugin::TransformerPlugin(const std::string& so_path) {
    handle_ = dlopen(so_path.c_str(), RTLD_NOW);
    if (!handle_)
        throw std::runtime_error(std::string("dlopen(") + so_path + "): " + dlerror());

    create_ = load_sym<create_fn>(handle_, "transformer_create");
    destroy_ = load_sym<destroy_fn>(handle_, "transformer_destroy");
    get_max_size_ = load_sym<get_max_size_fn>(handle_, "transformer_get_max_size");
    transform_ = load_sym<transform_fn>(handle_, "transformer_transform");
}

TransformerPlugin::~TransformerPlugin() {
    if (handle_)
        dlclose(handle_);
}

TransformerContext* TransformerPlugin::create(const char* codec_name, const char* params) const {
    return create_(codec_name, params);
}

void TransformerPlugin::destroy(TransformerContext* ctx) const {
    destroy_(ctx);
}

size_t TransformerPlugin::get_max_size(const TransformerContext* ctx, size_t frame_size) const {
    return get_max_size_(ctx, frame_size);
}

size_t TransformerPlugin::transform(TransformerContext* ctx, const uint8_t* src, size_t src_size, uint8_t* dst) const {
    return transform_(ctx, src, src_size, dst);
}
