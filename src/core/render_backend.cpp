#include "core/render_backend.h"
#include "core/gpu_scene.h"
#include "core/hittable_list.h"

render_device_list enumerate_render_devices() noexcept {
#ifdef DUSK_ENABLE_VULKAN_RT
    try {
        return dusk_gpu::enumerate();
    } catch (const std::exception& e) {
        return {{}, e.what()};
    }
#else
    return {{}, "GPU ray tracing is unavailable in this CPU-only build."};
#endif
}

render_result render_selected(const render_selection& selection, const scene& source, const hittable* world,
                              camera& cam, const renderer& settings, std::atomic<bool>* cancel,
                              render_progress_state* progress,
                              const std::function<void(const render_result&)>& callback,
                              std::string& diagnostic,
                              const std::function<void(const std::string&)>& status_callback) {
    if (cancel && cancel->load()) {
        return {};
    }
    if (selection.backend == render_backend::vulkan) {
        try {
#ifdef DUSK_ENABLE_VULKAN_RT
            if (cam.enable_mnee) {
                throw std::runtime_error("Experimental caustics require CPU rendering.");
            }
            auto data = dusk_gpu::compile_scene(source, cancel);
            auto report = [&](const std::string& message) {
                diagnostic = message;
                if (status_callback) status_callback(message);
            };
            auto result = dusk_gpu::render(std::move(data), cam, settings, selection.device_id, cancel, progress,
                                           callback, nullptr, report);
            return result;
#else
            throw std::runtime_error("Vulkan support was not compiled into this build.");
#endif
        } catch (const std::exception& e) {
            diagnostic = std::string("GPU unavailable; using CPU. ") + e.what();
            if (status_callback) {
                status_callback(diagnostic);
            }
            if (cancel && cancel->load()) {
                return {};
            }
        }
    } else {
        diagnostic = "Rendered with CPU.";
    }
    // A successful GPU render needs no CPU BVH. Build one here only when the
    // caller has not supplied a cached world and CPU rendering is necessary.
    std::unique_ptr<hittable_list> fallback_world;
    if (!world) {
        fallback_world = std::make_unique<hittable_list>(build_world_from_scene(source));
        build_emissive_surfaces(source, cam);
        world = fallback_world.get();
    }
    return settings.render(*world, cam, cancel, progress, callback);
}
