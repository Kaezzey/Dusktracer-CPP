#pragma once

#include "renderer.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

// Bucket ordering and per-bucket denoising shared by the CPU and Vulkan
// renderers (see renderer::bucket_size, bucket_order, progressive_denoise).
namespace dusk_tiles {
struct rect {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    int width() const { return x1 - x0; }
    int height() const { return y1 - y0; }
};

// Every bucket of a width x height image cut into size x size squares, in
// the given renderer::bucket_order. Edge buckets are clipped to the image.
std::vector<rect> make_tiles(int width, int height, int size, int order);

// The renderers' display transform: exposure, Reinhard on luminance (which
// keeps hue), then sRGB. Matches the GPU preview shader byte for byte.
void encode_display(const float* linear, double exposure, std::uint8_t* out);

// Denoises finished buckets on a background thread, for display only.
// Callers write a bucket's colour, albedo and normal (and indirect, when GPU
// firefly reconstruction is on) into the full-frame buffers, then call
// finished(). A bucket is denoised once every bucket its padded window
// touches has finished, so OIDN sees real neighbouring pixels instead of
// black and adjacent buckets agree at their seams.
class denoiser {
public:
    // threads limits OIDN's CPU threads (0 = all) so the render keeps cores
    // for itself. Strength blends raw (0) to denoised (1), as for the final
    // frame. Denoised buckets are encoded for display with this exposure.
    denoiser(int width, int height, int tile_size, const float* color, const float* albedo,
             const float* normal, const float* indirect, double strength, double exposure, int threads,
             render_progress_state* progress, const std::atomic<bool>* cancel);
    ~denoiser();
    denoiser(const denoiser&) = delete;
    denoiser& operator=(const denoiser&) = delete;

    void finished(const rect& tile);
    // Pastes every bucket denoised so far over an encoded 8-bit RGB frame.
    // Buckets are encoded once when denoised, so this only copies rows.
    void overlay(std::vector<std::uint8_t>& rgb) const;
    int denoised() const;
    // Blocks until every queued bucket has been denoised.
    void wait_idle();
    // Discards queued buckets and joins the worker.
    void stop();

private:
    struct impl;
    std::unique_ptr<impl> self;
};
} // namespace dusk_tiles
