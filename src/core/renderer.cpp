#include "../../include/core/dusktracer.h"
#include "../../include/core/renderer.h"
#include "core/denoise.h"
#include "core/render_tiles.h"
#include "../../include/core/interval.h"

#include <cmath>
#include <atomic>
#include <chrono>
#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#ifdef _OPENMP
#include <omp.h>
#endif
#include "../../include/core/image_io.h"
#include "ispc_brdf.h"
#include "ispc_brdf_spec.h"
#include "../../include/core/BVH.h"
#include "../../include/core/packet.h"
#include "../../include/core/materials/material.h"

// 1) Simple overload: forwards to the full version with progress = nullptr
render_result renderer::render(
    const hittable& world,
    camera&         cam,
    std::atomic<bool>* cancel_flag
) const
{
    return render(world, cam, cancel_flag, nullptr, nullptr);
}

// 2) Full version with progress + ETA
render_result renderer::render(
    const hittable&        world,
    camera&                cam,
    std::atomic<bool>*     cancel_flag,
    render_progress_state* progress,
    std::function<void(const render_result&)> progress_callback
) const
{
    using clock = std::chrono::steady_clock;

    cam.initialize();

    render_result out;
    out.width  = cam.image_width;
    out.height = cam.image_height;
    out.pixels.resize(out.width * out.height * 3);

    const int w  = out.width;
    const int h = out.height;
    const int spp    = cam.samples_per_pixel;
    const int depth  = cam.max_depth;
    const bool need_aux_aovs = this->use_denoiser;
    const bool enable_progressive_updates = (bool)progress_callback;

    // HDR linear float buffer (3 floats per pixel) used for optional denoising
    std::vector<float> hdr_buffer((size_t)w * (size_t)h * 3, 0.0f);
    // Albedo (base color) and normal AOVs for OIDN guidance
    std::vector<float> albedo_buffer;
    std::vector<float> normal_buffer;
    if (need_aux_aovs) {
        albedo_buffer.resize((size_t)w * (size_t)h * 3, 0.0f);
        normal_buffer.resize((size_t)w * (size_t)h * 3, 0.0f);
    }

    const int total_scanlines = h;
    // Viewport refresh period while buckets render. Only buckets that changed
    // are re-encoded, so frequent updates stay cheap.
    const auto publish_interval = std::chrono::milliseconds(100);

    // Rolling average per-tile time (seconds). Updated under critical section.
    std::atomic<double> avg_tile_time{0.0};

    if (progress) {
        progress->total_scanlines           = total_scanlines;
        progress->completed_scanlines.store(0);
        progress->eta_seconds.store(0.0);
        progress->elapsed_seconds.store(0.0);
        progress->image_width = w;
        progress->image_height = h;
        progress->clear_tiles();
    }

    auto render_start = clock::now();
    static const interval intensity(0.000, 0.999);

    // Buckets render in parallel; OpenMP's dynamic schedule hands them out
    // in list order, so the bucket order is the order they start.
    const int tile_size = std::clamp(bucket_size, 8, 512);
    const auto tiles = dusk_tiles::make_tiles(w, h, tile_size, bucket_order);
    const int num_tiles = (int)tiles.size();
    // Tile progress bookkeeping (used for ETA calculation)
    std::atomic<int> completed_tiles{0};

    // Recent per-tile timings (protected by the critical section below).
    // We'll keep a short sliding window and use its median to compute ETA
    // which avoids bias from slow startup tiles or occasional outliers.
    std::deque<double> recent_tile_times;
    const int RECENT_TILE_WINDOW = 64;

    if (progress) {
        progress->total_tiles = num_tiles;
        progress->completed_tiles.store(0);
    }

    int worker_count = 1;
#ifdef _OPENMP
    worker_count = std::max(1, omp_get_max_threads());
#endif
    // Running estimates for every pixel of a thread's current bucket.
    struct pixel_state {
        double mean[3] = {0, 0, 0}, m2[3] = {0, 0, 0}, albedo[3] = {0, 0, 0}, normal[3] = {0, 0, 0};
        int n = 0;
        bool done = false;
    };
    std::vector<std::vector<pixel_state>> thread_pixels(worker_count);
    // Passes published per bucket; the viewport re-encodes buckets whose count changed.
    std::unique_ptr<std::atomic<std::uint32_t>[]> tile_passes(new std::atomic<std::uint32_t>[std::max(1, num_tiles)]);
    for (int i = 0; i < num_tiles; ++i) tile_passes[i].store(0);

    // Finished buckets are denoised for the viewport on one background thread,
    // using a quarter of the cores so the render keeps the rest. The hdr
    // buffer already includes exposure, so the display uses unit exposure.
    std::unique_ptr<dusk_tiles::denoiser> tile_denoiser;
    if (enable_progressive_updates && need_aux_aovs && progressive_denoise && denoiser_strength > 0) {
        const int threads = std::max(1, int(std::thread::hardware_concurrency()) / 4);
        tile_denoiser = std::make_unique<dusk_tiles::denoiser>(
            w, h, tile_size, hdr_buffer.data(), albedo_buffer.data(), normal_buffer.data(), nullptr,
            denoiser_strength, 1.0, threads, progress, cancel_flag);
    }
    // The viewport image is built off the render threads: buckets whose
    // running means changed are re-encoded from the HDR buffer, then denoised
    // buckets go on top.
    std::vector<std::uint8_t> frame((size_t)w * (size_t)h * 3, 0);
    std::vector<std::uint32_t> encoded_passes(num_tiles, 0);
    auto publish = [&] {
        for (int ti = 0; ti < num_tiles; ++ti) {
            const std::uint32_t passes = tile_passes[ti].load(std::memory_order_acquire);
            if (passes == encoded_passes[ti]) continue;
            encoded_passes[ti] = passes;
            const auto& t = tiles[ti];
            for (int y = t.y0; y < t.y1; ++y)
                for (int x = t.x0; x < t.x1; ++x) {
                    const size_t p = (size_t(y) * w + x) * 3;
                    dusk_tiles::encode_display(&hdr_buffer[p], 1.0, &frame[p]);
                }
        }
        render_result partial;
        partial.width = w;
        partial.height = h;
        partial.pixels = frame;
        if (tile_denoiser) tile_denoiser->overlay(partial.pixels);
        try {
            progress_callback(partial);
        } catch (...) {
            // A failing viewport must not stop the render.
        }
    };
    std::mutex publisher_mutex;
    std::condition_variable publisher_wake;
    bool publisher_done = false;
    std::thread publisher;
    if (enable_progressive_updates) {
        publisher = std::thread([&] {
            std::unique_lock<std::mutex> lock(publisher_mutex);
            while (!publisher_wake.wait_for(lock, publish_interval, [&] { return publisher_done; })) {
                if (cancel_flag && cancel_flag->load()) continue;
                lock.unlock();
                publish();
                lock.lock();
            }
        });
    }

    // Force a consistent number of threads to match preallocated buffers
    #pragma omp parallel for schedule(dynamic,1) num_threads(worker_count)
    for (int ti = 0; ti < num_tiles; ++ti) {
        if (cancel_flag && cancel_flag->load()) continue;

        int tid = 0;
#ifdef _OPENMP
        tid = omp_get_thread_num();
#endif
        // Safety: ensure tid maps into the preallocated per-thread buffers
        if (tid < 0 || tid >= worker_count) {
            tid = tid % std::max(1, worker_count);
        }

        const dusk_tiles::rect& tile = tiles[ti];
        int x0 = tile.x0;
        int y0 = tile.y0;
        int x1 = tile.x1;
        int y1 = tile.y1;
        const render_tile_activity activity{x0, y0, x1 - x0, y1 - y0, render_tile_activity::rendering};
        if (progress) progress->begin_tile(activity);

        auto tile_start = clock::now();
        // Like Cycles, each pass takes one more sample for every unconverged
        // pixel of the bucket and publishes the running means, so a bucket
        // appears noisy and refines in place rather than filling pixel by pixel.
        const int tw = x1 - x0, count = tw * (y1 - y0);
        auto& pixels = thread_pixels[tid];
        pixels.assign(count, pixel_state{});
        int active = count;
        const int check_interval = std::max(1, adaptive_check_interval);
        for (int s = 0; s < spp && active > 0; ++s) {
            if (cancel_flag && cancel_flag->load()) break;
            for (int k = 0; k < count; ++k) {
                auto& px = pixels[k];
                if (px.done) continue;
                colour albedo(0, 0, 0);
                vec3 normal(0, 0, 0);
                const colour sample = cam.ray_colour(cam.get_ray(x0 + k % tw, y0 + k / tw, s), depth, world,
                                                     need_aux_aovs ? &albedo : nullptr,
                                                     need_aux_aovs ? &normal : nullptr);
                ++px.n;
                for (int c = 0; c < 3; ++c) {
                    // Welford mean and variance of finite, non-negative HDR samples.
                    const double v = std::isfinite(sample[c]) && sample[c] > 0 ? sample[c] : 0.0;
                    const double delta = v - px.mean[c];
                    px.mean[c] += delta / px.n;
                    px.m2[c] += delta * (v - px.mean[c]);
                    if (need_aux_aovs) {
                        px.albedo[c] += (albedo[c] - px.albedo[c]) / px.n;
                        px.normal[c] += normal[c];
                    }
                }
                // Stop a pixel once the standard error of its mean is small.
                if (adaptive_sampling && px.n > 1 && px.n >= adaptive_min_samples && px.n % check_interval == 0) {
                    double worst_relative = 0, worst_absolute = 0;
                    for (int c = 0; c < 3; ++c) {
                        const double error = std::sqrt(px.m2[c] / (px.n - 1) / px.n);
                        worst_absolute = std::max(worst_absolute, error);
                        worst_relative = std::max(worst_relative, std::abs(px.mean[c]) > 1e-12 ? error / std::abs(px.mean[c]) : error);
                    }
                    if (worst_relative < adaptive_rel_threshold || worst_absolute < adaptive_abs_threshold) {
                        px.done = true;
                        --active;
                    }
                }
            }
            for (int k = 0; k < count; ++k) {
                const size_t p = (size_t(y0 + k / tw) * w + x0 + k % tw) * 3;
                for (int c = 0; c < 3; ++c) hdr_buffer[p + c] = float(pixels[k].mean[c] * exposure);
            }
            tile_passes[ti].fetch_add(1, std::memory_order_release);
        }
        if (need_aux_aovs) {
            for (int k = 0; k < count; ++k) {
                const size_t p = (size_t(y0 + k / tw) * w + x0 + k % tw) * 3;
                const auto& px = pixels[k];
                const double length = std::sqrt(px.normal[0] * px.normal[0] + px.normal[1] * px.normal[1] +
                                                px.normal[2] * px.normal[2]);
                for (int c = 0; c < 3; ++c) {
                    albedo_buffer[p + c] = float(px.albedo[c]);
                    normal_buffer[p + c] = length > 0 ? float(px.normal[c] / length) : 0.f;
                }
            }
        }
        if (progress) progress->completed_scanlines += y1 - y0;
        if (progress) progress->end_tile(activity);
        if (tile_denoiser) tile_denoiser->finished(tile);

        // Update ETA using per-tile exponential moving average
        if (progress) {
            // Measure this tile's duration
            auto now = clock::now();
            double tile_seconds = std::chrono::duration<double>(now - tile_start).count();
            
            int tdone = ++completed_tiles; // atomic increment
            progress->completed_tiles.store(tdone);

            // Update EMA and ETA only every few tiles to reduce noise
            const int eta_update_interval = 8; // update every N tiles
            if (tdone % eta_update_interval == 0 || tdone >= num_tiles) {
                #pragma omp critical
                {
                    // Keep the old EMA for compatibility but also record the tile time
                    const double EMA_ALPHA = 0.05; // smooth long-term average
                    double prev_avg = avg_tile_time.load();
                    double new_avg = (prev_avg <= 0.0) ? tile_seconds : (EMA_ALPHA * tile_seconds + (1.0 - EMA_ALPHA) * prev_avg);
                    if (!std::isfinite(new_avg) || new_avg <= 0.0) {
                        new_avg = std::max(0.001, tile_seconds);
                    }
                    avg_tile_time.store(new_avg);

                    // Push this tile time into the recent window and cap its size
                    recent_tile_times.push_back(tile_seconds);
                    if ((int)recent_tile_times.size() > RECENT_TILE_WINDOW) recent_tile_times.pop_front();

                    // Compute median of recent tile times to avoid bias from startup/outliers
                    double median_tile = new_avg;
                    if (!recent_tile_times.empty()) {
                        std::vector<double> tmp(recent_tile_times.begin(), recent_tile_times.end());
                        std::sort(tmp.begin(), tmp.end());
                        size_t m = tmp.size() / 2;
                        if (tmp.size() % 2 == 1) median_tile = tmp[m];
                        else median_tile = 0.5 * (tmp[m-1] + tmp[m]);
                        // sanitize
                        if (!std::isfinite(median_tile) || median_tile <= 0.0) median_tile = new_avg;
                    }

                    // Use the median (recent window) for ETA; this reduces overestimates caused
                    // by a few slow startup tiles or occasional heavy tiles.
                    int remaining = std::max(0, num_tiles - tdone);
                    int workers = 1;
#ifdef _OPENMP
                    workers = std::max(1, omp_get_max_threads());
#endif
                    double eta = median_tile * remaining / (double)workers;
                    if (eta < 0.0) eta = 0.0;

                    // Update elapsed time from render start
                    double elapsed = std::chrono::duration<double>(now - render_start).count();

                    progress->eta_seconds.store(eta);
                    progress->elapsed_seconds.store(elapsed);
                }
            }
        }
    }

    // Queued bucket denoising is superseded by the full-frame pass below.
    // Show every finished bucket while that pass runs.
    if (tile_denoiser) tile_denoiser->stop();
    if (publisher.joinable()) {
        {
            std::lock_guard<std::mutex> lock(publisher_mutex);
            publisher_done = true;
        }
        publisher_wake.notify_all();
        publisher.join();
        if (!cancel_flag || !cancel_flag->load()) publish();
    }
    if (progress) progress->clear_tiles();

    // Optionally run OpenImageDenoise on the linear HDR buffer
    std::vector<float> denoised_buffer;
    denoised_buffer = hdr_buffer; // default: copy

    if (use_denoiser && need_aux_aovs && (!cancel_flag || !cancel_flag->load())) {
        const auto error = denoise_hdr(denoised_buffer, albedo_buffer, normal_buffer,
                                       w, h, denoiser_strength, cancel_flag);
        if (!error.empty()) std::fprintf(stderr, "Denoiser: %s\n", error.c_str());
    }

    // Final tonemap/gamma pass: convert denoised linear floats -> 8-bit
    // Luminance-based Reinhard tone mapping (preserves color saturation)
    auto tonemap_reinhard_lum = [](double r, double g, double b, double& out_r, double& out_g, double& out_b) {
        // Calculate luminance
        double lum = 0.2126 * r + 0.7152 * g + 0.0722 * b;
        if (lum < 1e-6) {
            out_r = out_g = out_b = 0.0;
            return;
        }
        // Tone map luminance only
        double lum_compressed = lum / (lum + 1.0);
        // Scale RGB by the compression ratio to preserve saturation
        double scale = lum_compressed / lum;
        out_r = r * scale;
        out_g = g * scale;
        out_b = b * scale;
    };

    for (int j = 0; j < h; ++j) {
        for (int i = 0; i < w; ++i) {
            int idx = 3 * (j * w + i);
            double r_lin = (double)denoised_buffer[idx + 0];
            double g_lin = (double)denoised_buffer[idx + 1];
            double b_lin = (double)denoised_buffer[idx + 2];

            double r_t, g_t, b_t;
            tonemap_reinhard_lum(r_lin, g_lin, b_lin, r_t, g_t, b_t);

            double r_g = linear_to_gamma(r_t);
            double g_g = linear_to_gamma(g_t);
            double b_g = linear_to_gamma(b_t);

            int rbyte = int(256 * intensity.clamp(r_g));
            int gbyte = int(256 * intensity.clamp(g_g));
            int bbyte = int(256 * intensity.clamp(b_g));

            out.pixels[idx + 0] = static_cast<unsigned char>(rbyte);
            out.pixels[idx + 1] = static_cast<unsigned char>(gbyte);
            out.pixels[idx + 2] = static_cast<unsigned char>(bbyte);
        }
    }

    // Free large temporary HDR buffers before returning to reduce peak resident memory
    {
        std::vector<float>().swap(hdr_buffer);
        std::vector<float>().swap(albedo_buffer);
        std::vector<float>().swap(normal_buffer);
        std::vector<float>().swap(denoised_buffer);
    }

    return out;
}
