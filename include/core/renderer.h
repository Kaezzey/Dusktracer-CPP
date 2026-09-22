#ifndef RENDERER_H
#define RENDERER_H

#include "camera.h"
#include "colour.h"
#include "hittable.h"

#include <vector>
#include <atomic>
#include <cstdint>
#include <functional>

// Returned by renderer::render()
struct render_result {
    int width  = 0;
    int height = 0;

    // 8-bit RGB per pixel
    std::vector<std::uint8_t> pixels;
};

// Progress state shared between renderer and UI
struct render_progress_state {
    std::atomic<int> total_scanlines{0};
    std::atomic<int>    completed_scanlines{0};
    // Tile-based progress (preferred when renderer uses tiles)
    std::atomic<int> total_tiles{0};
    std::atomic<int>    completed_tiles{0};
    std::atomic<double> elapsed_seconds{0.0};
    std::atomic<double> eta_seconds{0.0};
};

class renderer {
public:
    renderer() = default;
    // Exposure (stops-like linear multiplier); editable from the editor UI
    double exposure = 1.0;
    // Denoiser flags (controlled from editor)
    bool use_denoiser = true;
    // Blend between raw (0) and fully denoised (1) final radiance.
    double denoiser_strength = 1.0;
    // Progressive denoising: apply OIDN to partial results during render (like Cycles)
    bool progressive_denoise = false;

    // Photon mapping removed: no-op (was used for caustic photon pre-pass)

    // Adaptive sampling: when enabled, the renderer will estimate per-pixel
    // variance and stop sampling a pixel early when its estimate of the
    // pixel mean has sufficiently low standard error.
    bool adaptive_sampling = true;
    // Minimum samples before considering stopping (raised for fewer checks)
    int adaptive_min_samples = 16;
    // How often (in samples) to check convergence (raised to reduce overhead)
    int adaptive_check_interval = 16;
    // Relative standard error threshold: stop when std_error/mean < threshold
    // Typical values: 0.01 .. 0.03
    double adaptive_rel_threshold = 0.03;
    // Absolute standard error fallback (if mean near zero): stop when std_error < abs
    double adaptive_abs_threshold = 2e-4;

    // OLD signature kept for console main, etc.
    render_result render(
        const hittable&   world,
        camera&           cam,
        std::atomic<bool>* cancel_flag = nullptr
    ) const;

    // NEW signature with progress reporting (used by editor_main.cpp)
    render_result render(
        const hittable&        world,
        camera&                cam,
        std::atomic<bool>*     cancel_flag,
        render_progress_state* progress,
        std::function<void(const render_result&)> progress_callback = nullptr
    ) const;
};

#endif // RENDERER_H
