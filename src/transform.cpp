#include "transform.h"

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

TransformPlugin::TransformPlugin(const std::string& so_path) {
    handle_ = dlopen(so_path.c_str(), RTLD_NOW);
    if (!handle_)
        throw std::runtime_error(std::string("dlopen(") + so_path + "): " + dlerror());

    create_ = load_sym<create_fn>(handle_, "transform_create");
    destroy_ = load_sym<destroy_fn>(handle_, "transform_destroy");
    get_max_size_ = load_sym<get_max_size_fn>(handle_, "transform_get_max_size");
    apply_ = load_sym<apply_fn>(handle_, "transform_apply");
}

TransformPlugin::~TransformPlugin() {
    if (handle_)
        dlclose(handle_);
}

TransformContext* TransformPlugin::create(const char* codec_name, const char* params) const {
    return create_(codec_name, params);
}

void TransformPlugin::destroy(TransformContext* ctx) const {
    destroy_(ctx);
}

size_t TransformPlugin::get_max_size(const TransformContext* ctx, size_t frame_size) const {
    return get_max_size_(ctx, frame_size);
}

size_t TransformPlugin::apply(TransformContext* ctx, const uint8_t* src, size_t src_size, uint8_t* dst) const {
    return apply_(ctx, src, src_size, dst);
}
