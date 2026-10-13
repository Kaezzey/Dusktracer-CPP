// Bucket rendering: ordering, per-bucket denoising and both backends' bucket
// modes. Included by gpu_tests.cpp; the CPU checks run without a GPU.
#include "core/render_tiles.h"

static void check_bucket_layout() {
    for (int order = 0; order < 3; ++order) {
        for (auto size : {std::array<int, 3>{97, 61, 16}, std::array<int, 3>{256, 256, 64},
                          std::array<int, 3>{1000, 7, 64}, std::array<int, 3>{5, 300, 32}}) {
            const int width = size[0], height = size[1], tile = size[2];
            auto tiles = dusk_tiles::make_tiles(width, height, tile, order);
            std::vector<int> covered(size_t(width) * height, 0);
            for (const auto& t : tiles) {
                require(t.x0 % tile == 0 && t.y0 % tile == 0 && t.width() > 0 && t.height() > 0 &&
                            t.width() <= tile && t.height() <= tile,
                        "buckets are grid aligned and clipped to the image");
                for (int y = t.y0; y < t.y1; ++y)
                    for (int x = t.x0; x < t.x1; ++x) ++covered[size_t(y) * width + x];
            }
            require(std::all_of(covered.begin(), covered.end(), [](int n) { return n == 1; }),
                    "every bucket order covers each pixel exactly once");
        }
    }
    auto spiral = dusk_tiles::make_tiles(320, 320, 64, 0);
    require(spiral.front().x0 == 128 && spiral.front().y0 == 128, "spiral order starts at the centre bucket");
    auto rows = dusk_tiles::make_tiles(320, 128, 64, 2);
    require(rows[1].x0 == 64 && rows[1].y0 == 0 && rows[5].y0 == 64, "row order runs left to right, top to bottom");
    auto hilbert = dusk_tiles::make_tiles(256, 256, 64, 1);
    for (size_t i = 1; i < hilbert.size(); ++i)
        require(std::abs(hilbert[i].x0 - hilbert[i - 1].x0) + std::abs(hilbert[i].y0 - hilbert[i - 1].y0) == 64,
                "consecutive Hilbert buckets are neighbours");
    std::uint8_t encoded[3];
    const float grey[3] = {.18f, .18f, .18f};
    dusk_tiles::encode_display(grey, 1.7, encoded);
    const double lum = .18 * 1.7, mapped = .18 * 1.7 / (1 + lum);
    require(encoded[0] == std::uint8_t(256 * std::clamp(linear_to_gamma(mapped), 0.0, .999)),
            "bucket display encoding matches the renderers' Reinhard and sRGB transform");
}

static void check_bucket_denoiser() {
#ifdef HAVE_OIDN
    constexpr int width = 160, height = 96, tile = 32;
    std::vector<float> truth(size_t(width) * height * 3), noisy(truth.size()), albedo(truth.size(), .5f),
        normal(truth.size(), 0.f);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            for (int c = 0; c < 3; ++c) {
                const size_t i = (size_t(y) * width + x) * 3 + c;
                truth[i] = .15f + .5f * x / width + .1f * c;
                noisy[i] = truth[i] * (1 + (float((i * 73 + 19) % 101) / 100 - .5f) * .6f);
                if (c == 2) normal[i] = 1;
            }
    auto frame = noisy;
    require(denoise_hdr(frame, albedo, normal, width, height, 1).empty(), "full-frame reference denoise");
    render_progress_state progress;
    dusk_tiles::denoiser buckets(width, height, tile, noisy.data(), albedo.data(), normal.data(), nullptr, 1, 1, 0,
                                 &progress, nullptr);
    const auto tiles = dusk_tiles::make_tiles(width, height, tile, 2);
    const int columns = (width + tile - 1) / tile;
    // A corner bucket waits for the neighbours inside its padded window.
    buckets.finished(tiles[0]);
    buckets.wait_idle();
    require(buckets.denoised() == 0, "a bucket is not denoised before its neighbours are rendered");
    buckets.finished(tiles[1]);
    buckets.finished(tiles[columns]);
    buckets.finished(tiles[columns + 1]);
    buckets.wait_idle();
    require(buckets.denoised() >= 1, "a bucket is denoised once its window is rendered");
    for (size_t i = 2; i < tiles.size(); ++i) buckets.finished(tiles[i]);
    buckets.wait_idle();
    require(buckets.denoised() == int(tiles.size()), "every finished bucket is denoised");
    require(progress.active_tiles().empty(), "denoising marks clear once buckets finish");
    std::vector<std::uint8_t> tiled(truth.size(), 0), whole(truth.size());
    buckets.overlay(tiled);
    double difference = 0, raw_error = 0, tiled_error = 0, whole_error = 0;
    for (size_t p = 0; p < truth.size() / 3; ++p) {
        dusk_tiles::encode_display(&frame[p * 3], 1, &whole[p * 3]);
        std::uint8_t raw[3], expected[3];
        dusk_tiles::encode_display(&noisy[p * 3], 1, raw);
        dusk_tiles::encode_display(&truth[p * 3], 1, expected);
        for (int c = 0; c < 3; ++c) {
            difference += std::abs(int(tiled[p * 3 + c]) - int(whole[p * 3 + c]));
            raw_error += std::pow(int(raw[c]) - int(expected[c]), 2);
            tiled_error += std::pow(int(tiled[p * 3 + c]) - int(expected[c]), 2);
            whole_error += std::pow(int(whole[p * 3 + c]) - int(expected[c]), 2);
        }
    }
    difference /= truth.size();
    std::cout << "Bucket denoise: mean difference from full frame " << difference << " levels, MSE "
              << raw_error / truth.size() << " -> " << tiled_error / truth.size() << " (full frame "
              << whole_error / truth.size() << ")\n";
    require(difference < 1.5, "bucket denoising agrees with full-frame denoising");
    require(tiled_error < raw_error * .6 && tiled_error < whole_error * 1.1,
            "bucket denoising removes noise as well as full-frame denoising");
    buckets.stop();
#endif
}

static void check_cpu_buckets() {
    auto source = sphere_scene(scene_material_model::lambert);
    auto cam = test_camera(96, 64, 6);
    auto world = build_world_from_scene(source);
    build_emissive_surfaces(source, cam);
    renderer settings;
    settings.bucket_size = 16;
    settings.adaptive_sampling = false;
    render_progress_state progress;
    int frames = 0;
    bool full_size = true;
    auto image = settings.render(world, cam, nullptr, &progress, [&](const render_result& partial) {
        ++frames;
        full_size = full_size && partial.width == 96 && partial.height == 64 && partial.pixels.size() == 96 * 64 * 3;
    });
    require(frames > 0 && full_size, "CPU buckets publish full-size viewport frames");
    require(image.pixels.size() == 96 * 64 * 3, "CPU bucket render completes");
    require(progress.total_tiles == 24 && progress.completed_tiles == 24, "CPU progress counts each bucket once");
    require(progress.image_width == 96 && progress.image_height == 64 && progress.active_tiles().empty(),
            "no bucket stays marked active after the render");

    // Like Cycles, a bucket takes one sample per pixel per pass: stopped part
    // way, every pixel of the bucket already has an estimate.
    auto one_bucket = test_camera(32, 32, 1000000);
    settings.bucket_size = 32;
    settings.use_denoiser = false;
    std::atomic<bool> cancel{false};
    auto partial = settings.render(world, one_bucket, &cancel, nullptr, [&](const render_result&) { cancel = true; });
    size_t black = 0;
    for (size_t p = 0; p < partial.pixels.size(); p += 3)
        black += partial.pixels[p] == 0 && partial.pixels[p + 1] == 0 && partial.pixels[p + 2] == 0;
    require(black == 0, "a bucket refines all its pixels together instead of filling them one by one");
}

#ifdef DUSK_ENABLE_VULKAN_RT
// Editor-like GPU renders (adaptive sampling, firefly filter, OIDN, a viewport
// callback) in both modes. Run with: DuskGpuTests out.png --bucket-benchmark
static void benchmark_gpu_buckets(const std::string& device) {
    auto source = sphere_scene(scene_material_model::pbr);
    scene_material floor;
    floor.model = scene_material_model::pbr;
    floor.base_color = vec3(.6, .6, .6);
    floor.roughness = .6;
    source.materials.push_back(floor);
    scene_object ground;
    ground.type = scene_object_type::cube;
    ground.center = point3(0, -1.1, 0);
    ground.scale = vec3(12, .2, 12);
    ground.material_index = 1;
    source.objects.push_back(ground);
    auto cam = test_camera(1920, 1080, 256);
    cam.lookfrom = point3(0, 1.5, 6);
    cam.vfov = 40;
    cam.max_depth = 8;
    cam.use_sun = true;
    cam.sun_dir = unit_vector(vec3(-1, 2, 2));
    const auto data = dusk_gpu::compile_scene(source);
    auto run = [&](const char* label, bool buckets, bool tile_denoise, bool adaptive) {
        renderer settings;
        settings.gpu_buckets = buckets;
        settings.progressive_denoise = tile_denoise;
        settings.adaptive_sampling = adaptive;
        int previews = 0;
        auto callback = [&](const render_result& image) { ++previews; render_result copy = image; (void)copy; };
        dusk_gpu::render(data, cam, settings, device, nullptr, nullptr, callback); // warm
        dusk_gpu::render_statistics stats;
        previews = 0;
        auto start = std::chrono::steady_clock::now();
        dusk_gpu::render(data, cam, settings, device, nullptr, nullptr, callback, nullptr, {}, &stats);
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        std::printf("BUCKET BENCH %-34s %9.1f ms  %5llu submissions  %3d previews\n", label, ms,
                    static_cast<unsigned long long>(stats.submissions), previews);
    };
    run("progressive", false, true, true);
    run("buckets", true, true, true);
    run("buckets, no bucket denoise", true, false, true);
    run("progressive, no adaptive", false, true, false);
    run("buckets, no adaptive", true, true, false);
}
static void check_gpu_buckets(const std::string& device) {
    auto source = sphere_scene(scene_material_model::pbr);
    auto cam = test_camera(300, 200, 6);
    auto settings = raw_settings();
    std::vector<float> progressive, bucketed;
    settings.gpu_buckets = false;
    dusk_gpu::render(dusk_gpu::compile_scene(source), cam, settings, device, nullptr, nullptr, {}, &progressive);
    settings.gpu_buckets = true;
    dusk_gpu::render(dusk_gpu::compile_scene(source), cam, settings, device, nullptr, nullptr, {}, &bucketed);
    require(progressive == bucketed, "GPU buckets trace the same samples as progressive refinement");
    // Synchronous single-sample batches guarantee previews while a group traces.
    render_progress_state progress;
    bool saw_bucket = false;
    gpu_option("DUSK_GPU_PREVIEW_MS", "1");
    gpu_option("DUSK_GPU_SYNCHRONOUS", "1");
    gpu_option("DUSK_GPU_BATCH_SAMPLES", "1");
    cam.samples_per_pixel = 24;
    dusk_gpu::render(dusk_gpu::compile_scene(source), cam, settings, device, nullptr, &progress,
                     [&](const render_result&) {
                         for (const auto& tile : progress.active_tiles())
                             saw_bucket = saw_bucket || (tile.stage == render_tile_activity::rendering &&
                                                         tile.width == 256 && tile.height == 200);
                     });
    gpu_option("DUSK_GPU_PREVIEW_MS", "");
    gpu_option("DUSK_GPU_SYNCHRONOUS", "");
    gpu_option("DUSK_GPU_BATCH_SAMPLES", "");
    cam.samples_per_pixel = 6;
    require(saw_bucket, "GPU bucket mode reports the bucket being traced");
    require(progress.active_tiles().empty() && progress.completed_tiles == progress.total_tiles,
            "GPU bucket mode finishes its progress");

    // Bucket denoising for the viewport never changes the saved image.
    settings.use_denoiser = true;
    settings.progressive_denoise = true;
    const auto plain = dusk_gpu::render(dusk_gpu::compile_scene(source), cam, settings, device, nullptr, nullptr, {});
    int previews = 0;
    const auto viewed = dusk_gpu::render(dusk_gpu::compile_scene(source), cam, settings, device, nullptr, nullptr,
                                         [&](const render_result&) { ++previews; });
    require(previews > 0 && plain.pixels == viewed.pixels, "bucket denoising leaves the final frame unchanged");

    // Adaptive sampling stops each group of buckets independently.
    settings = raw_settings();
    settings.adaptive_sampling = true;
    cam = test_camera(600, 300, 4096);
    cam.max_depth = 1;
    bool converged = false;
    render_progress_state adaptive;
    auto result = dusk_gpu::render(dusk_gpu::compile_scene({}), cam, settings, device, nullptr, &adaptive, {},
                                   nullptr, [&](const std::string& phase) {
                                       converged = converged || phase == "Adaptive sampling converged.";
                                   });
    require(converged && result.pixels.size() == 600 * 300 * 3 && adaptive.completed_tiles == adaptive.total_tiles,
            "every bucket group converges early under adaptive sampling");
}
#endif
