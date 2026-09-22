#pragma once
#include "renderer.h"
#include "scene.h"
#include <string>

enum class render_backend { cpu, vulkan };
struct render_device {
    std::string id, name, reason;
    uint32_t vendor = 0;
    bool discrete = false, compatible = false;
};
struct render_device_list {
    std::vector<render_device> devices;
    std::string diagnostic;
};
struct render_selection {
    render_backend backend = render_backend::cpu;
    std::string device_id;
};
// Enumeration is read-only. Missing SDK/loader/driver is a normal CPU-only case.
render_device_list enumerate_render_devices() noexcept;
// Called only on the render worker, using value snapshots of scene/camera/settings.
// GPU errors are reported and transparently retry the same scene on the CPU.
render_result render_selected(const render_selection&, const scene&, const hittable* cpu_world, camera&,
                              const renderer&, std::atomic<bool>*, render_progress_state*,
                              const std::function<void(const render_result&)>&, std::string& diagnostic,
                              const std::function<void(const std::string&)>& status_callback = {});

namespace dusk_gpu {
struct snapshot;
render_result render(snapshot, camera, const renderer&, const std::string& device_id,
                     std::atomic<bool>*, render_progress_state*,
                     const std::function<void(const render_result&)>&,
                     std::vector<float>* linear_output = nullptr,
                     const std::function<void(const std::string&)>& status_callback = {});
render_device_list enumerate();
} // namespace dusk_gpu
