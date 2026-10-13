// Raster/path-tracer parity for the Unreal Subsurface and Subsurface Profile
// shading models. The raster's linear direct-lighting view is compared with
// CPU path tracing through the same pixel centres at max depth one, so both
// images contain exactly the direct light (including all subsurface paths).
namespace subsurface_showcase_entry { void run(); }
namespace subsurface_parity {
constexpr int size = 96;

static std::vector<float> raster_linear() {
    raster_preview::debug_view = 3;
    RenderRasterToTexture(size, size);
    glFinish();
    std::vector<float> rgba(size_t(size) * size * 4), rgb(size_t(size) * size * 3);
    glBindTexture(GL_TEXTURE_2D, g_rasterColorTex);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, rgba.data());
    check(glGetError() == GL_NO_ERROR, "linear raster readback");
    raster_preview::debug_view = 0;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            for (int c = 0; c < 3; ++c) {
                rgb[(size_t(y) * size + x) * 3 + c] = rgba[(size_t(size - 1 - y) * size + x) * 4 + c];
            }
        }
    }
    return rgb;
}

// Path-traced radiance at the raster's pixel centres; mask marks object hits.
static std::vector<float> traced(int samples, std::vector<char>& mask) {
    auto world = build_world_from_scene(g_scene);
    camera cam;
    cam.background = colour(0, 0, 0);
    cam.max_depth = 1;
    cam.direct_light_samples = 1;
    cam.sun_shadow_samples = 1;
    for (const auto& light : g_scene.lights) {
        if (light.type == scene_light_type::directional) {
            cam.use_sun = true;
            cam.sun_dir = unit_vector(-light.direction);
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
    build_emissive_surfaces(g_scene, cam);
    vec3 forward = unit_vector(g_editor_cam.forward);
    vec3 right = unit_vector(cross(forward, g_editor_cam.up)), up = cross(right, forward);
    double extent = std::tan(degrees_to_radians(g_editor_cam.vfov) / 2);
    std::vector<float> rgb(size_t(size) * size * 3);
    mask.assign(size_t(size) * size, 0);
    std::vector<std::thread> workers;
    std::atomic<int> next_row{0};
    for (unsigned t = 0; t < std::max(1u, std::thread::hardware_concurrency()); ++t) {
        workers.emplace_back([&] {
            for (int y = next_row++; y < size; y = next_row++) {
                for (int x = 0; x < size; ++x) {
                    vec3 direction = forward + ((x + .5) / size * 2 - 1) * extent * right +
                                     (1 - (y + .5) / size * 2) * extent * up;
                    ray primary(g_editor_cam.position, direction, .5);
                    hit_record hit;
                    size_t p = size_t(y) * size + x;
                    mask[p] = world.hit(primary, interval(.001, infinity), hit) &&
                              dynamic_cast<const pbr_material*>(hit.mat.get()) != nullptr;
                    // Box-filter the pixel, as the viewport's mipmapped textures do.
                    colour sum(0, 0, 0);
                    for (int s = 0; s < samples; ++s) {
                        vec3 jittered = forward + ((x + random_double()) / size * 2 - 1) * extent * right +
                                        (1 - (y + random_double()) / size * 2) * extent * up;
                        sum += cam.ray_colour(ray(g_editor_cam.position, jittered, .5), cam.max_depth, world);
                    }
                    for (int c = 0; c < 3; ++c) {
                        rgb[p * 3 + c] = float(sum[c] / samples);
                    }
                }
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    return rgb;
}

static void save_pair(const std::string& name, const std::vector<float>& raster, const std::vector<float>& path) {
    std::vector<unsigned char> pixels(size_t(size) * 2 * size * 3);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < 2 * size; ++x) {
            const auto& source = x < size ? raster : path;
            for (int c = 0; c < 3; ++c) {
                double v = std::max(0.f, source[(size_t(y) * size + x % size) * 3 + c]);
                v = v / (1 + v);
                v = v <= .0031308 ? 12.92 * v : 1.055 * std::pow(v, 1 / 2.4) - .055;
                pixels[(size_t(y) * 2 * size + x) * 3 + c] = (unsigned char)std::lround(std::clamp(v, 0., 1.) * 255);
            }
        }
    }
    stbi_flip_vertically_on_write(0);
    check(stbi_write_png(name.c_str(), 2 * size, size, 3, pixels.data(), 2 * size * 3) != 0,
          "save subsurface parity image");
}

// Accuracy is one minus the L1 difference relative to traced energy over the
// object interior, after a 3x3 box filter that suppresses Monte Carlo noise.
static double compare(const char* label, int samples, double minimum_accuracy) {
    InvalidateAllRasterMaterials();
    auto raster = raster_linear();
    raster = raster_linear(); // the first frame may still be building shadow maps
    std::vector<char> mask;
    auto path = traced(samples, mask);
    save_pair(std::string("parity-") + label + ".png", raster, path);
    {
        // Raw linear RGB pairs for offline inspection.
        std::ofstream raw(std::string("parity-") + label + ".bin", std::ios::binary);
        raw.write(reinterpret_cast<const char*>(raster.data()), std::streamsize(raster.size() * sizeof(float)));
        raw.write(reinterpret_cast<const char*>(path.data()), std::streamsize(path.size() * sizeof(float)));
    }
    // Two-pixel erosion excludes silhouettes, where tessellated raster spheres
    // and analytic traced spheres cover pixels differently.
    auto interior = [&](int x, int y) {
        if (x < 2 || y < 2 || x >= size - 2 || y >= size - 2) return false;
        for (int dy = -2; dy <= 2; ++dy) {
            for (int dx = -2; dx <= 2; ++dx) {
                if (!mask[size_t(y + dy) * size + x + dx]) return false;
            }
        }
        return true;
    };
    double difference = 0, energy = 0, raster_energy = 0, channel_ratio[3]{}, channel_energy[3]{};
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            if (!interior(x, y)) continue;
            for (int c = 0; c < 3; ++c) {
                double r = 0, p = 0;
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        size_t at = (size_t(y + dy) * size + x + dx) * 3 + c;
                        r += raster[at] / 9;
                        p += path[at] / 9;
                    }
                }
                difference += std::abs(r - p);
                energy += p;
                raster_energy += r;
                channel_ratio[c] += r;
                channel_energy[c] += p;
            }
        }
    }
    double accuracy = energy > 0 ? 1 - difference / energy : 0;
    std::printf("SSS parity %-28s accuracy %.1f%%  energy ratio %.3f  (R %.3f G %.3f B %.3f)\n", label,
                100 * accuracy, raster_energy / std::max(energy, 1e-12),
                channel_ratio[0] / std::max(channel_energy[0], 1e-12),
                channel_ratio[1] / std::max(channel_energy[1], 1e-12),
                channel_ratio[2] / std::max(channel_energy[2], 1e-12));
    check(energy > 0, "parity fixture receives light");
    check(accuracy >= minimum_accuracy, label);
    return accuracy;
}

static scene_material skin_material(int shading) {
    scene_material m;
    m.model = scene_material_model::pbr;
    m.base_color = vec3(.85, .62, .52);
    m.roughness = .45;
    m.use_sss = true;
    m.sss_model = SSS_DIPOLE_BURLEY;
    m.sss_shading = shading;
    m.sss_strength = 1;
    m.sss_eta = 1.4;
    // Unreal Subsurface
    m.sss_subsurface_color = vec3(.9, .35, .25);
    m.sss_opacity = .35;
    m.sss_transmission_distance = .6;
    // Unreal Subsurface Profile: default skin scaled up so red diffusion
    // (about 0.13) spans many shadow-map texels yet stays within the spheres.
    m.sss_units_per_metre = 45;
    return m;
}
static void sun(const vec3& toward_light, double strength) {
    scene_light light;
    light.type = scene_light_type::directional;
    light.direction = -unit_vector(toward_light);
    light.radiance = vec3(strength, strength, strength);
    g_scene.lights.push_back(light);
}
static void point(const point3& position, double strength) {
    scene_light light;
    light.type = scene_light_type::point;
    light.position = position;
    light.radiance = vec3(strength, strength, strength);
    light.range = 0;
    g_scene.lights.push_back(light);
}
static void sphere_fixture(int shading) {
    raster_reset();
    g_scene.materials.push_back(skin_material(shading));
    scene_object sphere;
    sphere.material_index = 0;
    sphere.radius = 1;
    g_scene.objects.push_back(sphere);
    g_editor_cam.vfov = 30;
    g_editor_cam.set_from_lookat(point3(0, 0, 4.5), point3());
}

// Viewport cost with a Subsurface Profile sphere filling a 720p frame.
static void timing() {
    sphere_fixture(sss_shading_unreal_profile);
    sun(vec3(-.7, .5, .5), 3);
    g_editor_cam.set_from_lookat(point3(0, 0, 2.2), point3());
    auto frame_ms = [] {
        for (int i = 0; i < 5; ++i) RenderRasterToTexture(1280, 720);
        glFinish();
        auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < 30; ++i) RenderRasterToTexture(1280, 720);
        glFinish();
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / 30;
    };
    double with = frame_ms();
    g_scene.materials[0].sss_units_per_metre = 1;
    InvalidateAllRasterMaterials();
    double real_scale = frame_ms();
    g_scene.materials[0].use_sss = false;
    InvalidateAllRasterMaterials();
    double without = frame_ms();
    std::printf("Raster 1280x720 frame: %.2f ms with Subsurface Profile, %.2f ms at real skin scale, %.2f ms without\n",
                with, real_scale, without);
}

static void run() {
    if (std::getenv("DUSK_SSS_SHOWCASE") || std::getenv("DUSK_SSS_PROJECT")) {
        ::subsurface_showcase_entry::run();
        return;
    }
    if (std::getenv("DUSK_SSS_TIMING")) {
        timing();
        return;
    }
    sphere_fixture(sss_shading_unreal_subsurface);
    sun(vec3(-.9, .35, -.6), 3);
    compare("unreal-subsurface-backlit", 512, .9);
    g_scene.lights.clear();
    sun(vec3(.8, .3, .5), 3);
    g_scene.materials[0].sss_opacity = 0;
    compare("unreal-subsurface-wrap", 512, .9);
    g_scene.lights.clear();
    point(point3(.3, .4, -3), 30);
    g_scene.materials[0].sss_opacity = .35;
    compare("unreal-subsurface-point", 512, .9);

    sphere_fixture(sss_shading_unreal_profile);
    sun(vec3(-.7, .5, .5), 3);
    compare("profile-sun", 512, .9);
    // Unreal default skin at real scale: millimetre diffusion on a metre sphere.
    g_scene.materials[0].sss_units_per_metre = 1;
    compare("profile-real-scale", 512, .9);
    g_scene.materials[0].sss_units_per_metre = 45;
    g_scene.lights.clear();
    point(point3(-2, 1.5, 2), 30);
    compare("profile-point", 512, .9);
    // Thin, ear-scale geometry transmits a visible red glow from behind.
    g_scene.objects[0].radius = .25;
    g_editor_cam.set_from_lookat(point3(0, 0, 1.2), point3());
    g_scene.lights.clear();
    sun(vec3(-.2, .3, -1), 3);
    compare("profile-backlit-thin", 1024, .9);
    g_scene.objects[0].radius = 1;
    g_editor_cam.set_from_lookat(point3(0, 0, 4.5), point3());
    g_scene.materials[0].sss_shading = sss_shading_burley;
    g_scene.materials[0].sss_radius = .1;
    g_scene.materials[0].sss_radius_rgb = vec3(1, .372, .186);
    g_scene.lights.clear();
    sun(vec3(-.7, .5, .5), 3);
    compare("legacy-burley-sun", 512, .9);

    // A cast shadow on a slab: diffusion carries light across the boundary.
    raster_reset();
    g_scene.materials.push_back(skin_material(sss_shading_unreal_profile));
    scene_material black;
    black.model = scene_material_model::lambert;
    black.base_color = vec3();
    g_scene.materials.push_back(black);
    scene_object slab;
    slab.type = scene_object_type::cube;
    slab.material_index = 0;
    slab.scale = vec3(6, 6, .4);
    g_scene.objects.push_back(slab);
    // The blocker sits above the camera frustum; its shadow edge lies at x=0.
    scene_object blocker = slab;
    blocker.material_index = 1;
    blocker.center = point3(-.32, 0, 3);
    blocker.scale = vec3(4, 6, .1);
    g_scene.objects.push_back(blocker);
    g_editor_cam.vfov = 40;
    g_editor_cam.set_from_lookat(point3(0, -2.2, 1.6), point3(0, .3, .2));
    sun(vec3(.6, 0, 1), 3);
    compare("profile-shadow-edge", 512, .9);
}
} // namespace subsurface_parity
#include "raster_subsurface_showcase.inl"
