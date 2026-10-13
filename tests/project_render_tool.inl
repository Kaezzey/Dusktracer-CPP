// Development tool: render a saved project without the editor.
//   DuskGpuTests out.png --render-project Scene.dusk image.png [samples] [width] [frames-dir] [cpu]
// Lights are mirrored into the camera exactly as the editor does before a
// render. With frames-dir, every viewport frame the renderer publishes is
// saved as frame-NNNN.png; "cpu" renders with the CPU path tracer.
#include "core/project.h"

#ifdef DUSK_ENABLE_VULKAN_RT
static int render_project(const std::string& device, int argc, char** argv) {
    if (argc < 5) {
        std::cerr << "usage: DuskGpuTests out.png --render-project Scene.dusk image.png [samples] [width] "
                     "[frames-dir] [cpu]\n";
        return 2;
    }
    auto document = dusk_project::load(argv[3]);
    camera cam = document.camera_settings;
    cam.point_lights.clear();
    cam.use_sun = false;
    for (const auto& light : document.world.lights) {
        if (light.type == scene_light_type::directional) {
            cam.use_sun = true;
            cam.sun_dir = -light.direction;
            cam.sun_radiance = light.radiance;
            cam.sun_angular_radius = light.angular_radius_deg;
        } else {
            camera::point_light point;
            point.position = light.position;
            point.radiance = light.radiance;
            point.range = light.range;
            cam.point_lights.push_back(point);
        }
    }
    if (argc > 5) cam.samples_per_pixel = std::max(1, std::atoi(argv[5]));
    if (argc > 6) {
        const int width = std::max(16, std::atoi(argv[6]));
        cam.image_height = std::max(16, cam.image_height * width / cam.image_width);
        cam.image_width = width;
    }
    const std::string frames = argc > 7 ? argv[7] : "";
    const bool cpu = argc > 8 && std::string(argv[8]) == "cpu";
    int frame = 0;
    std::function<void(const render_result&)> callback;
    if (!frames.empty()) {
        std::filesystem::create_directories(frames);
        callback = [&](const render_result& image) {
            char name[32];
            std::snprintf(name, sizeof(name), "frame-%04d.png", frame++);
            const auto path = (std::filesystem::path(frames) / name).string();
            stbi_write_png(path.c_str(), image.width, image.height, 3, image.pixels.data(), image.width * 3);
        };
    }
    auto start = std::chrono::steady_clock::now();
    render_result image;
    if (cpu) {
        auto world = build_world_from_scene(document.world);
        build_emissive_surfaces(document.world, cam);
        render_progress_state progress;
        image = document.render_settings.render(world, cam, nullptr, &progress, callback);
    } else {
        image = dusk_gpu::render(dusk_gpu::compile_scene(document.world), cam, document.render_settings, device,
                                 nullptr, nullptr, callback);
    }
    std::cout << "Rendered " << image.width << "x" << image.height << " at " << cam.samples_per_pixel << " spp on "
              << (cpu ? "CPU" : "GPU") << " in "
              << std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() << " s, " << frame
              << " viewport frames\n";
    return stbi_write_png(argv[4], image.width, image.height, 3, image.pixels.data(), image.width * 3) ? 0 : 1;
}
#endif
