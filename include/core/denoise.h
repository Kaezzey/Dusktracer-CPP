#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <string>
#include <vector>
#ifdef HAVE_OIDN
#include <OpenImageDenoise/oidn.hpp>
#endif

// Filter scene-linear radiance before tone mapping. Failure or cancellation
// leaves the original image intact; strength is our blend, not an OIDN option.
inline std::string denoise_hdr(std::vector<float>& color, const std::vector<float>& albedo,
                               const std::vector<float>& normal, int width, int height, double strength,
                               const std::atomic<bool>* cancel = nullptr) {
    if (strength <= 0 || (cancel && cancel->load())) {
        return {};
    }
#ifdef HAVE_OIDN
    try {
        const size_t values = size_t(width) * height * 3;
        if (width <= 0 || height <= 0 || color.size() != values || albedo.size() != values ||
            normal.size() != values) {
            return "Denoiser input dimensions do not match.";
        }
        // Host buffers are directly accessible on CPU, avoiding a second GPU
        // allocation while the Vulkan render still owns its scene and output.
        auto device = oidn::newDevice(oidn::DeviceType::CPU);
        device.commit();
        auto filter = device.newFilter("RT");
        std::vector<float> filtered(values);
        filter.setImage("color", color.data(), oidn::Format::Float3, width, height);
        filter.setImage("albedo", const_cast<float*>(albedo.data()), oidn::Format::Float3, width, height);
        filter.setImage("normal", const_cast<float*>(normal.data()), oidn::Format::Float3, width, height);
        filter.setImage("output", filtered.data(), oidn::Format::Float3, width, height);
        filter.set("hdr", true);
        filter.set("srgb", false);
        filter.set("cleanAux", false);
        if (cancel) {
            filter.setProgressMonitorFunction(
                [](void* data, double) { return !static_cast<const std::atomic<bool>*>(data)->load(); },
                const_cast<std::atomic<bool>*>(cancel));
        }
        filter.commit();
        filter.execute();
        const char* message = nullptr;
        auto error = device.getError(message);
        if (cancel && cancel->load()) {
            return {};
        }
        if (error != oidn::Error::None) {
            return message ? message : "Unknown denoiser error.";
        }
        if (!std::all_of(filtered.begin(), filtered.end(), [](float v) { return std::isfinite(v); })) {
            return "Denoiser returned non-finite pixels.";
        }
        float blend = float(std::clamp(strength, 0.0, 1.0));
        for (size_t i = 0; i < values; ++i) {
            color[i] += blend * (std::max(0.f, filtered[i]) - color[i]);
        }
        return {};
    } catch (const std::exception& e) {
        return e.what();
    }
#else
    return "OpenImageDenoise is unavailable in this build.";
#endif
}
