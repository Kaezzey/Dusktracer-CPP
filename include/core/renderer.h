#ifndef RENDERER_H
#define RENDERER_H

#include "camera.h"
#include "colour.h"
#include "hittable.h"

#include <vector>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>

// Returned by renderer::render()
struct render_result {
    int width  = 0;
    int height = 0;

    // 8-bit RGB per pixel
    std::vector<std::uint8_t> pixels;
};

// A bucket the renderer is working on, in image pixels. The editor draws
// Blender-style corner marks around these while a render runs.
struct render_tile_activity {
    enum stage_kind { rendering, denoising };
    int x = 0, y = 0, width = 0, height = 0;
    stage_kind stage = rendering;
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
    // Image size the active tiles refer to.
    std::atomic<int> image_width{0}, image_height{0};

    void begin_tile(const render_tile_activity& tile) {
        std::lock_guard<std::mutex> lock(tile_mutex);
        tiles.push_back(tile);
    }
    void end_tile(const render_tile_activity& tile) {
        std::lock_guard<std::mutex> lock(tile_mutex);
        for (size_t i = 0; i < tiles.size(); ++i) {
            if (tiles[i].x == tile.x && tiles[i].y == tile.y && tiles[i].stage == tile.stage) {
                tiles.erase(tiles.begin() + i);
                return;
            }
        }
    }
    void clear_tiles() {
        std::lock_guard<std::mutex> lock(tile_mutex);
        tiles.clear();
    }
    std::vector<render_tile_activity> active_tiles() const {
        std::lock_guard<std::mutex> lock(tile_mutex);
        return tiles;
    }

private:
    mutable std::mutex tile_mutex;
    std::vector<render_tile_activity> tiles;
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
    // Denoise each finished bucket for the viewport while the render
    // continues (like Cycles' tile denoising). A bucket is denoised once its
    // neighbours are rendered, so the network sees real pixels around it.
    // The final image is still denoised as one frame.
    bool progressive_denoise = true;
    // Buckets render in parallel and each finishes at full sample count, like
    // Cycles before 3.0. Edge length in pixels for the CPU; the Vulkan
    // backend always uses its 256-pixel dispatch tiles.
    int bucket_size = 64;
    // Bucket order: 0 = spiral out from the centre, 1 = Hilbert curve,
    // 2 = rows from the top.
    int bucket_order = 0;
    // Vulkan: finish each group of buckets before starting the next, rather
    // than refining the whole frame progressively.
    bool gpu_buckets = true;
    // GPU display reconstruction removes isolated indirect outliers while
    // retaining the raw HDR estimator and camera-visible light/specular energy.
    bool gpu_firefly_filter = true;

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
