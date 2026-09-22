#include "core/dusktracer.h"
#include "core/denoise.h"
#include "core/render_backend.h"
#include "core/gpu_scene.h"
#include "core/hittable_list.h"
#ifdef DUSK_ENABLE_VULKAN_RT
#include "../src/gpu/pipeline_cache.h"
#endif
#include <filesystem>
#include <fstream>
#include <chrono>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "external/stb_image_write.h"

namespace {
int checks = 0;
void require(bool condition, const char* message) {
    ++checks;
    if (!condition) {
        throw std::runtime_error(message);
    }
}
void near(double actual, double expected, double tolerance, const char* message) {
    if (!std::isfinite(actual) || std::abs(actual - expected) > tolerance) {
        std::cerr << message << ": " << actual << " versus " << expected << '\n';
        require(false, message);
    }
    ++checks;
}
camera test_camera(int width = 16, int height = 16, int samples = 64) {
    camera cam;
    cam.image_width = width;
    cam.image_height = height;
    cam.samples_per_pixel = samples;
    cam.lookfrom = point3(0, 0, 4);
    cam.lookat = point3(0, 0, 0);
    cam.vfov = 18;
    cam.background = colour(1, 1, 1);
    cam.max_depth = 4;
    cam.use_sun = false;
    return cam;
}
scene sphere_scene(scene_material_model model) {
    scene source;
    scene_material material;
    material.model = model;
    material.base_color = vec3(.5, .2, .1);
    material.roughness = .5;
    source.materials.push_back(material);
    scene_object sphere;
    sphere.material_index = 0;
    sphere.radius = 1;
    source.objects.push_back(sphere);
    return source;
}
renderer raw_settings() {
    renderer settings;
    settings.use_denoiser = false;
    settings.adaptive_sampling = false;
    return settings;
}
double channel_mean(const std::vector<float>& image, int channel) {
    double sum = 0;
    for (size_t i = channel; i < image.size(); i += 3) {
        sum += image[i];
    }
    return sum / (image.size() / 3);
}
#ifdef DUSK_ENABLE_VULKAN_RT
double compare_render(const scene& source, camera cam, const std::string& device, const char* label,
                      double tolerance) {
    auto world = build_world_from_scene(source);
    build_emissive_surfaces(source, cam);
    auto settings = raw_settings();
    auto cpu = settings.render(world, cam);
    auto gpu = dusk_gpu::render(dusk_gpu::compile_scene(source), cam, settings, device, nullptr, nullptr, {});
    double error = 0;
    require(cpu.pixels.size() == gpu.pixels.size(), "CPU/GPU image dimensions match");
    for (size_t p = 0; p < cpu.pixels.size(); ++p) {
        error += std::abs(int(cpu.pixels[p]) - int(gpu.pixels[p]));
    }
    error /= cpu.pixels.size();
    std::cout << label << " MAE: " << error << " / 255\n";
    require(error < tolerance, label);
    return error;
}

void check_graphs(const std::string& device, const std::filesystem::path& folder) {
    auto source = sphere_scene(scene_material_model::diffuse_light);
    auto cam = test_camera(8, 8, 1);
    std::vector<float> linear;
    auto& material = source.materials[0];
    for (auto kind :
         {graph_kind::add, graph_kind::subtract, graph_kind::multiply, graph_kind::divide, graph_kind::lerp,
          graph_kind::one_minus, graph_kind::saturate, graph_kind::power, graph_kind::reroute}) {
        material.graph = {};
        int root = material.graph.add(graph_kind::output, 0, 0);
        int a = material.graph.add(graph_kind::vector, 0, 0);
        int b = material.graph.add(graph_kind::scalar, 0, 0);
        int op = material.graph.add(kind, 0, 0);
        material.graph.find(a)->value = vec3(.3, .4, .8);
        material.graph.find(b)->value = vec3(.25, 0, 0);
        material.graph.connect({a, 0, op, 0});
        if (graph_inputs(kind) > 1) {
            material.graph.connect({b, 0, op, 1});
        }
        material.graph.connect({op, 0, root, 0});
        auto expected = build_rt_material(source, material)->emitted(.5, .5, point3());
        dusk_gpu::render(dusk_gpu::compile_scene(source), cam, raw_settings(), device, nullptr, nullptr, {},
                         &linear);
        for (int c = 0; c < 3; ++c) {
            near(channel_mean(linear, c), expected[c], 2e-6, "GPU graph operation matches CPU");
        }
    }
    const auto texture_path = folder / "gpu-colour.png";
    const unsigned char texel[] = {128, 64, 192, 96};
    require(stbi_write_png(texture_path.string().c_str(), 1, 1, 4, texel, 4) != 0, "write RGBA fixture");
    source.textures.push_back({"RGBA fixture", texture_path.string()});
    for (auto space : {graph_space::automatic, graph_space::srgb, graph_space::linear}) {
        for (int channel = 0; channel < 6; ++channel) {
            material.graph = {};
            int root = material.graph.add(graph_kind::output, 0, 0);
            int texture = material.graph.add(graph_kind::texture_sample, 0, 0);
            material.graph.find(texture)->texture_index = 0;
            material.graph.find(texture)->space = space;
            require(material.graph.connect({texture, channel, root, 0}), "connect texture channel");
            auto expected = build_rt_material(source, material)->emitted(.5, .5, point3());
            dusk_gpu::render(dusk_gpu::compile_scene(source), cam, raw_settings(), device, nullptr, nullptr,
                             {}, &linear);
            for (int c = 0; c < 3; ++c) {
                near(channel_mean(linear, c), expected[c], 2e-6,
                     "GPU texture color/channel/alpha matches CPU");
            }
        }
    }
    const unsigned char checker[] = {255, 0, 0, 255, 0, 255, 0, 0, 0, 0, 255, 255, 255, 255, 255, 0};
    const auto checker_path = folder / "gpu-checker.png";
    require(stbi_write_png(checker_path.string().c_str(), 2, 2, 4, checker, 8) != 0, "write UV fixture");
    source.textures[0].path = checker_path.string();
    material.graph = {};
    int root = material.graph.add(graph_kind::output, 0, 0),
        tex = material.graph.add(graph_kind::texture_sample, 0, 0),
        uv = material.graph.add(graph_kind::texcoord, 0, 0);
    material.graph.find(tex)->texture_index = 0;
    material.graph.find(uv)->value = vec3(3, 2, 0);
    material.graph.connect({uv, 0, tex, 0});
    material.graph.connect({tex, 0, root, 0});
    source.objects[0].rotation_deg = vec3(20, 35, -15);
    compare_render(source, test_camera(48, 48, 4), device, "Rotated sphere UV tiling", .5);
    source.objects[0].scale = vec3(1.4,.7,.9);
    compare_render(source, test_camera(48,48,16), device, "Rotated nonuniform sphere UVs", 1.0);
    source.objects[0].scale = vec3(1,1,1);

    material.model = scene_material_model::pbr;
    material.graph.connect({tex, 4, root, 4});
    auto alpha_camera = test_camera(32, 32, 128);
    alpha_camera.max_depth = 0;
    compare_render(source, alpha_camera, device, "Alpha cutout traversal", .5);

    // Register reuse must handle long chains without a per-hit 256-value stack.
    material.model = scene_material_model::diffuse_light;
    material.graph = {};
    root = material.graph.add(graph_kind::output, 0, 0);
    int previous = material.graph.add(graph_kind::scalar, 0, 0);
    material.graph.find(previous)->value = vec3(.25, 0, 0);
    for (int i = 0; i < 180; ++i) {
        int next = material.graph.add(graph_kind::add, 0, 0);
        material.graph.find(next)->defaults[1] = .001;
        material.graph.connect({previous, 0, next, 0});
        previous = next;
    }
    material.graph.connect({previous, 0, root, 0});
    dusk_gpu::render(dusk_gpu::compile_scene(source), cam, raw_settings(), device, nullptr, nullptr, {},
                     &linear);
    near(channel_mean(linear, 0), .43, 5e-6, "long graph reuses GPU registers");
}

void check_meshes(const std::string& device, const std::filesystem::path& folder) {
    auto obj_path = folder / "gpu-slots.obj";
    std::ofstream obj(obj_path);
    obj << "mtllib gpu-slots.mtl\nv -1 -1 0\nv 0 -1 0\nv 1 -1 0\nv -1 1 0\nv 0 1 0\nv 1 1 0\n"
        << "vt 0 0\nvt 1 0\nvt 0 1\nvt 1 1\nvn 0.2 0.1 1\n"
        << "usemtl left\nf 1/1/1 2/2/1 5/4/1\nf 1/1/1 5/4/1 4/3/1\n"
        << "usemtl right\nf 2/1/1 3/2/1 6/4/1\nf 2/1/1 6/4/1 5/3/1\n";
    obj.close();
    std::ofstream(folder / "gpu-slots.mtl") << "newmtl left\nKd 1 0 0\nnewmtl right\nKd 0 1 0\n";
    scene source;
    scene_material red;
    red.model = scene_material_model::diffuse_light;
    red.base_color = vec3(1, .1, .05);
    scene_material green = red;
    green.base_color = vec3(.05, 1, .1);
    source.materials = {red, green};
    scene_mesh_asset asset;
    asset.file_path = obj_path.string();
    asset.slot_names = {"left", "right"};
    source.meshes.push_back(asset);
    scene_object mesh;
    mesh.type = scene_object_type::mesh_instance;
    mesh.mesh_index = 0;
    mesh.material_index = 0;
    mesh.mesh_slot_materials = {0, 1};
    mesh.rotation_deg = vec3(5, 15, 10);
    mesh.scale = vec3(1.5, .8, 1);
    source.objects.push_back(mesh);
    mesh.translation = vec3(0, 0, -1);
    mesh.mesh_slot_materials = {1, 0};
    source.objects.push_back(mesh);
    auto data = dusk_gpu::compile_scene(source);
    require(data.geometries.size() == 3 && data.instances.size() == 2,
            "mesh instances share one BLAS geometry");
    auto cam = test_camera(64, 48, 8);
    cam.vfov = 42;
    compare_render(source, cam, device, "Transformed mesh slots", .8);

    for (auto& material : source.materials) {
        material.model = scene_material_model::pbr;
        material.roughness = .7;
        int root = material.graph.add(graph_kind::output, 0, 0),
            normal = material.graph.add(graph_kind::vector, 0, 0);
        material.graph.find(normal)->value = vec3(.7, .65, .95);
        material.graph.connect({normal, 0, root, 3});
    }
    cam.samples_per_pixel = 256;
    cam.background = vec3(.15, .15, .15);
    cam.use_sun = true;
    cam.sun_dir = unit_vector(vec3(1, 2, 3));
    cam.sun_radiance = vec3(2, 2, 2);
    cam.sun_shadow_samples = 1;
    compare_render(source, cam, device, "Mesh normal maps and nonuniform scale", .5);
}

void check_transport(const std::string& device) {
    auto source = sphere_scene(scene_material_model::dielectric);
    source.materials[0].base_color = vec3(.9, .95, 1);
    source.objects[0].rotation_deg = vec3(17, 29, 11);
    source.objects[0].scale = vec3(1.3, 1.3, 1.3);
    auto cam = test_camera(24, 24, 256);
    compare_render(source, cam, device, "Transformed glass entry and exit", 2);
    cam.lookfrom = point3(0, 0, 0);
    cam.lookat = point3(.1, .2, -1);
    compare_render(source, cam, device, "Glass camera inside medium", 2);

    source = sphere_scene(scene_material_model::pbr);
    source.materials[0].base_color = vec3(.6, .6, .6);
    scene_material emitter;
    emitter.model = scene_material_model::diffuse_light;
    emitter.emission = vec3(8, 8, 8);
    source.materials.push_back(emitter);
    scene_object light;
    light.type = scene_object_type::cube;
    light.center = point3(0, 2, 2);
    light.scale = vec3(2, .1, 2);
    light.material_index = 1;
    source.objects.push_back(light);
    cam = test_camera(16, 16, 512);
    cam.vfov = .001;
    cam.background = vec3(0, 0, 0);
    cam.max_depth = 2;
    auto world = build_world_from_scene(source);
    build_emissive_surfaces(source, cam);
    double reference = 0;
    for (int sample = 0; sample < 65536; ++sample) {
        reference += cam.ray_colour(ray(cam.lookfrom, vec3(0, 0, -1), 0), cam.max_depth, world).x() / 65536;
    }
    for (int samples : {0, 1, 4}) {
        cam.direct_light_samples = samples;
        std::vector<float> linear;
        dusk_gpu::render(dusk_gpu::compile_scene(source), cam, raw_settings(), device, nullptr, nullptr, {},
                         &linear);
        double mean = channel_mean(linear, 0);
        std::cout << "GPU area NEE " << samples << ": " << mean << " CPU: " << reference << '\n';
        near(mean, reference, .015, "GPU area lighting is independent of NEE sample count");
    }
    cam.direct_light_samples = 1;
    cam.enable_mis = false;
    std::vector<float> no_mis;
    dusk_gpu::render(dusk_gpu::compile_scene(source), cam, raw_settings(), device, nullptr, nullptr, {},
                     &no_mis);
    near(channel_mean(no_mis, 0), reference, .015, "Area lighting mean with MIS disabled");

    source.objects.back().type = scene_object_type::sphere;
    source.objects.back().radius = .6;
    source.objects.back().scale = vec3(1.7,.5,.9);
    source.objects.back().rotation_deg = vec3(20,35,15);
    cam.enable_mis = true;
    world = build_world_from_scene(source);
    build_emissive_surfaces(source,cam);
    reference = 0;
    for (int sample = 0; sample < 65536; ++sample)
        reference += cam.ray_colour(ray(cam.lookfrom,vec3(0,0,-1),0),cam.max_depth,world).x()/65536;
    for (int samples : {1,4}) {
        cam.direct_light_samples = samples;
        std::vector<float> linear;
        dusk_gpu::render(dusk_gpu::compile_scene(source),cam,raw_settings(),device,nullptr,nullptr,{},&linear);
        near(channel_mean(linear,0),reference,.015,"ellipsoid emitter Jacobian agrees with CPU at multiple NEE counts");
    }
    cam.direct_light_samples = 1;

    source.objects.pop_back();
    cam.enable_mis = true;
    cam.vfov = 18;
    cam.point_lights.push_back({point3(-1, 3, 4), colour(40, 30, 20), 20});
    cam.point_lights.push_back({point3(1, 1, 4), colour(20, 30, 40), 20});
    compare_render(source, cam, device, "Point light selection and inverse-square falloff", 2);

    std::atomic<bool> cancelled{false};
    render_progress_state progress;
    bool partial = false;
    cam = test_camera(64, 64, 100000);
    auto start = std::chrono::steady_clock::now();
    auto result = dusk_gpu::render(dusk_gpu::compile_scene(source), cam, raw_settings(), device, &cancelled,
                                   &progress, [&](const render_result& image) {
                                       partial = !image.pixels.empty();
                                       cancelled = true;
                                   });
    require(partial && !result.pixels.empty(), "GPU progressive callback and partial cancellation result");
    require(progress.completed_tiles.load() < progress.total_tiles.load(),
            "GPU cancellation stops new submissions");
    require(std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() < 5,
            "GPU cancellation completes within the bounded work interval");

    renderer adaptive = raw_settings();
    adaptive.adaptive_sampling = true;
    source = sphere_scene(scene_material_model::lambert);
    cam = test_camera();
    auto result_adaptive =
        dusk_gpu::render(dusk_gpu::compile_scene(source), cam, adaptive, device, nullptr, nullptr, {});
    auto result_full =
        dusk_gpu::render(dusk_gpu::compile_scene(source), cam, raw_settings(), device, nullptr, nullptr, {});
    require(result_adaptive.pixels == result_full.pixels, "adaptive sampling retains constant radiance");
}
#endif
} // namespace

#ifdef DUSK_ENABLE_VULKAN_RT
static void check_pipeline_cache() {
    namespace cache = dusk_gpu::pipeline_cache;
    // Cache fixtures stay in their own directory, separate from real driver data.
    const auto previous = std::filesystem::current_path();
    const auto folder = previous / "gpu-cache-tests";
    std::filesystem::create_directories(folder);
    struct restore_directory {
        std::filesystem::path path;
        ~restore_directory() {
            std::filesystem::current_path(path);
        }
    } restore{previous};
    std::filesystem::current_path(folder);
    VkPhysicalDeviceProperties device{};
    device.vendorID = 17;
    device.deviceID = 23;
    device.driverVersion = 5;
    device.pipelineCacheUUID[0] = 11;
    VkPipelineCacheHeaderVersionOne header{32, VK_PIPELINE_CACHE_HEADER_VERSION_ONE, 17, 23, {11}};
    std::vector<uint8_t> payload(64, 42);
    std::memcpy(payload.data(), &header, sizeof(header));
    cache::write(device, 101, payload);
    require(cache::read(device, 101) == payload, "pipeline cache round trip");
    require(cache::read(device, 102).empty(), "shader changes invalidate pipeline cache");
    device.pipelineCacheUUID[0] = 12;
    require(cache::read(device, 101).empty(), "driver cache UUID mismatch is rejected");
    device.pipelineCacheUUID[0] = 11;
    {
        std::fstream file(cache::path(device), std::ios::binary | std::ios::in | std::ios::out);
        file.seekp(-1, std::ios::end);
        file.put(99);
    }
    require(cache::read(device, 101).empty(), "corrupt cache payload is never passed to the driver");
    {
        std::ofstream file(cache::path(device), std::ios::binary | std::ios::trunc);
        file << "short";
    }
    require(cache::read(device, 101).empty(), "truncated pipeline cache is ignored");
}
static void check_gpu_scheduling(const std::string& device) {
    auto source = sphere_scene(scene_material_model::pbr);
    auto cam = test_camera(128, 96, 65);
    cam.sun_dir = unit_vector(vec3(-1, 2, 2));
    cam.use_sun = true;
    auto settings = raw_settings();
    std::vector<float> single, batched;
    auto render_batch = [&](const char* batch, std::vector<float>& output) {
#ifdef _WIN32
        _putenv_s("DUSK_GPU_BATCH_SAMPLES", batch);
#else
        setenv("DUSK_GPU_BATCH_SAMPLES", batch, 1);
#endif
        const auto start = std::chrono::steady_clock::now();
        dusk_gpu::render(dusk_gpu::compile_scene(source), cam, settings, device, nullptr, nullptr, {},
                         &output);
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    };
    const double single_ms = render_batch("1", single), batch_ms = render_batch("4", batched);
#ifdef _WIN32
    _putenv_s("DUSK_GPU_BATCH_SAMPLES", "");
#else
    unsetenv("DUSK_GPU_BATCH_SAMPLES");
#endif
    require(single == batched, "batching preserves every HDR pixel including the trailing sample");
    std::cout << "GPU scheduling (65 spp, includes setup): single " << single_ms << " ms, batched "
              << batch_ms << " ms\n";
    settings.adaptive_sampling = true;
    cam = test_camera(64, 64, 4096);
    cam.max_depth = 1;
    render_progress_state progress;
    bool converged = false;
    auto result = dusk_gpu::render(dusk_gpu::compile_scene({}), cam, settings, device, nullptr, &progress, {},
                                   nullptr, [&](const std::string& phase) {
                                       if (phase == "Adaptive sampling converged.") {
                                           converged = true;
                                       }
                                   });
    require(converged && result.pixels.size() == 64 * 64 * 3,
            "adaptive GPU render terminates after global convergence");
    require(progress.completed_tiles == progress.total_tiles, "adaptive completion finishes progress");
}
#endif

static void check_denoiser() {
#ifdef HAVE_OIDN
    constexpr int side = 96;
    std::vector<float> noisy(side * side * 3), albedo(noisy.size(), .5f), normal(noisy.size(), 0);
    for (size_t i = 0; i < noisy.size(); ++i) {
        noisy[i] = .4f + (float((i * 73 + 19) % 101) / 100 - .5f) * .35f;
        if (i % 3 == 2) {
            normal[i] = 1;
        }
    }
    auto filtered = noisy;
    require(denoise_hdr(filtered, albedo, normal, side, side, 1).empty(),
            "OIDN accepts final HDR and guide buffers");
    double raw_error = 0, filtered_error = 0;
    for (size_t i = 0; i < noisy.size(); ++i) {
        raw_error += std::pow(noisy[i] - .4f, 2);
        filtered_error += std::pow(filtered[i] - .4f, 2);
    }
    std::cout << "Denoiser MSE: " << raw_error / noisy.size() << " -> " << filtered_error / noisy.size()
              << '\n';
    require(filtered_error < raw_error * .25, "final denoising reduces known noise by at least 75 percent");
    auto disabled = noisy;
    require(denoise_hdr(disabled, albedo, normal, side, side, 0).empty() && disabled == noisy,
            "zero strength retains raw HDR");
    std::atomic<bool> cancel{true};
    require(denoise_hdr(disabled, albedo, normal, side, side, 1, &cancel).empty() && disabled == noisy,
            "cancelled denoising leaves input intact");
    require(!denoise_hdr(disabled, {}, normal, side, side, 1).empty() && disabled == noisy,
            "bad guides preserve the unfiltered image");
#endif
}

int main(int argc, char** argv) {
    check_denoiser();
    std::cout << std::unitbuf;
    try {
        const std::filesystem::path output = argc > 1 ? argv[1] : "gpu-validation.png";
#ifndef DUSK_ENABLE_VULKAN_RT
        auto source = sphere_scene(scene_material_model::lambert);
        auto cam = test_camera(8, 8, 4);
        std::string diagnostic;
        auto result = render_selected({render_backend::vulkan, "unavailable"}, source, nullptr, cam,
                                      raw_settings(), nullptr, nullptr, {}, diagnostic);
        require(result.pixels.size() == 8 * 8 * 3, "CPU-only fallback produces an image");
        require(diagnostic.find("using CPU") != std::string::npos, "CPU-only fallback is explained");
        require(enumerate_render_devices().devices.empty(), "CPU-only build exposes no GPU devices");
        std::cout << checks << " CPU-only backend checks passed\n";
        return 0;
#endif
        auto devices = enumerate_render_devices();
        for (const auto& device : devices.devices) {
            std::cout << device.name << " [" << device.id << "] "
                      << (device.compatible ? "hardware RT supported" : device.reason) << '\n';
        }
        auto compatible = std::find_if(devices.devices.begin(), devices.devices.end(),
                                       [](const auto& d) { return d.compatible; });
        if (compatible == devices.devices.end()) {
            std::cout << devices.diagnostic << '\n';
            return 77;
        }
#ifdef DUSK_ENABLE_VULKAN_RT
        std::string id = compatible->id;
        check_pipeline_cache();
        check_gpu_scheduling(id);
        if (argc > 2 && std::string(argv[2]) == "--scheduling") {
            return 0;
        }
        auto fixture_folder = std::filesystem::absolute(output).parent_path() / "gpu-fixtures";
        std::filesystem::create_directories(fixture_folder);
        check_graphs(id, fixture_folder);
        check_meshes(id, fixture_folder);
        check_transport(id);
#ifdef HAVE_OIDN
        {
            auto source = sphere_scene(scene_material_model::pbr);
            auto cam = test_camera(64, 64, 5);
            cam.use_sun = true;
            cam.sun_dir = unit_vector(vec3(1, 2, 3));
            cam.sun_radiance = colour(3, 3, 3);
            auto settings = raw_settings();
            auto raw =
                dusk_gpu::render(dusk_gpu::compile_scene(source), cam, settings, id, nullptr, nullptr, {});
            settings.use_denoiser = true;
            settings.denoiser_strength = 1;
            std::vector<std::string> phases;
            auto filtered =
                dusk_gpu::render(dusk_gpu::compile_scene(source), cam, settings, id, nullptr, nullptr, {},
                                 nullptr, [&](const std::string& phase) { phases.push_back(phase); });
            require(raw.pixels != filtered.pixels, "GPU final result contains denoised pixels");
            require(phases.size() >= 4 && phases[phases.size() - 2].find("Denoising") != std::string::npos &&
                        phases.back().find("Final image denoised") != std::string::npos,
                    "GPU tracing finishes before final denoising and completion");
        }
#endif

        renderer settings = raw_settings();
        std::vector<float> linear;
        camera cam = test_camera();
        scene empty;
        dusk_gpu::render(dusk_gpu::compile_scene(empty), cam, settings, id, nullptr, nullptr, {}, &linear);
        for (float value : linear) {
            near(value, 1, 1e-6, "empty TLAS returns the environment");
        }

        auto source = sphere_scene(scene_material_model::lambert);
        auto data = dusk_gpu::compile_scene(source);
        dusk_gpu::render(data, cam, settings, id, nullptr, nullptr, {}, &linear);
        near(channel_mean(linear, 0), .5, 1e-5, "Lambert furnace red");
        near(channel_mean(linear, 1), .2, 1e-5, "Lambert furnace green");
        near(channel_mean(linear, 2), .1, 1e-5, "Lambert furnace blue");

        // Emission uses graph RGB and intensity, without the PBR reflectance clamp.
        auto& material = source.materials[0];
        material.model = scene_material_model::diffuse_light;
        material.emission_intensity = 3;
        int root = material.graph.add(graph_kind::output, 0, 0),
            color = material.graph.add(graph_kind::vector, 0, 0);
        material.graph.find(color)->value = vec3(.25, .5, 2);
        material.graph.connect({color, 0, root, 0});
        dusk_gpu::render(dusk_gpu::compile_scene(source), cam, settings, id, nullptr, nullptr, {}, &linear);
        near(channel_mean(linear, 0), .75, 1e-6, "graph emission intensity");
        near(channel_mean(linear, 2), 6, 1e-6, "HDR graph emission is not clipped");

        // The same source and display pipeline, with enough samples to compare
        // Monte Carlo means without requiring identical CPU/GPU random streams.
        source = sphere_scene(scene_material_model::pbr);
        source.materials[0].metallic = .35;
        cam = test_camera(32, 24, 256);
        cam.use_sun = true;
        cam.sun_dir = unit_vector(vec3(-1, 2, 2));
        cam.sun_radiance = colour(2, 2, 2);
        cam.sun_angular_radius = 8;
        cam.sun_shadow_samples = 1;
        auto world = build_world_from_scene(source);
        build_emissive_surfaces(source, cam);
        auto cpu = settings.render(world, cam);
        auto gpu = dusk_gpu::render(dusk_gpu::compile_scene(source), cam, settings, id, nullptr, nullptr, {},
                                    &linear);
        double mean_error = 0;
        for (size_t i = 0; i < cpu.pixels.size(); ++i) {
            mean_error += std::abs(int(cpu.pixels[i]) - int(gpu.pixels[i]));
        }
        mean_error /= cpu.pixels.size();
        std::cout << "PBR CPU/GPU display MAE: " << mean_error << " / 255\n";
        require(mean_error < 4, "PBR image agrees with CPU reference");

        // A missing selected UUID must execute a real CPU render, with a visible reason.
        std::string diagnostic;
        bool notified = false;
        auto fallback =
            render_selected({render_backend::vulkan, "missing-device"}, source, nullptr, cam, settings,
                            nullptr, nullptr, {}, diagnostic, [&](const std::string& reason) {
                                notified = reason.find("using CPU") != std::string::npos;
                            });
        require(notified && diagnostic.find("using CPU") != std::string::npos,
                "GPU failure reports CPU fallback");
        require(fallback.pixels.size() == cpu.pixels.size(), "fallback produces a complete CPU result");

        std::atomic<bool> cancel{true};
        require(dusk_gpu::render(data, cam, settings, id, &cancel, nullptr, {}).pixels.empty(),
                "cancel before GPU initialization");
        auto unsupported = source;
        unsupported.materials[0].use_sss = true;
        unsupported.materials[0].sss_strength = .5;
        bool rejected = false;
        try {
            (void)dusk_gpu::compile_scene(unsupported);
        } catch (const std::exception&) {
            rejected = true;
        }
        require(rejected, "unsupported SSS is rejected explicitly");

        // A shareable chart through the actual hardware backend.
        source.objects.clear();
        source.materials.clear();
        for (int row = 0; row < 2; ++row) {
            for (int column = 0; column < 5; ++column) {
                scene_material m;
                m.model = scene_material_model::pbr;
                m.metallic = row;
                m.roughness = column * .25;
                m.base_color = row ? vec3(.8, .45, .15) : vec3(.55, .15, .08);
                scene_object sphere;
                sphere.radius = .42;
                sphere.center = point3((column - 2) * 1.05, row * .98, 0);
                sphere.material_index = int(source.materials.size());
                source.materials.push_back(m);
                source.objects.push_back(sphere);
            }
        }
        scene_material floor;
        floor.model = scene_material_model::pbr;
        floor.base_color = vec3(.35, .35, .35);
        floor.roughness = .8;
        scene_object ground;
        ground.type = scene_object_type::cube;
        ground.center = point3(0, -.48, 0);
        ground.scale = vec3(20, .1, 20);
        ground.material_index = int(source.materials.size());
        source.materials.push_back(floor);
        source.objects.push_back(ground);
        cam = test_camera(384, 216, 64);
        cam.lookfrom = point3(3, 2.7, 7);
        cam.lookat = point3(0, .45, 0);
        cam.vfov = 40;
        cam.background = colour(.12, .15, .2);
        cam.use_sun = true;
        cam.sun_dir = unit_vector(vec3(-2, 4, 3));
        cam.sun_radiance = colour(3, 2.8, 2.5);
        cam.sun_angular_radius = 5;
        cam.sun_shadow_samples = 1;
        auto start = std::chrono::steady_clock::now();
        gpu = dusk_gpu::render(dusk_gpu::compile_scene(source), cam, settings, id, nullptr, nullptr, {});
        std::cout << "GPU chart elapsed: "
                  << std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count()
                  << " seconds\n";
        require(stbi_write_png(output.string().c_str(), gpu.width, gpu.height, 3, gpu.pixels.data(),
                               gpu.width * 3) != 0,
                "write GPU chart");
        settings.use_denoiser = true;
        settings.denoiser_strength = 1;
        gpu = dusk_gpu::render(dusk_gpu::compile_scene(source), cam, settings, id, nullptr, nullptr, {});
        const auto denoised_path = output.parent_path() / "gpu-denoised.png";
        require(stbi_write_png(denoised_path.string().c_str(), gpu.width, gpu.height, 3, gpu.pixels.data(),
                               gpu.width * 3) != 0,
                "write denoised GPU chart");
#endif
        std::cout << checks << " GPU checks passed\n";
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
