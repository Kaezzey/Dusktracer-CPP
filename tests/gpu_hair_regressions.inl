// The Vulkan hair BSDF (pathtrace.comp) against the CPU reference. Included by
// gpu_tests.cpp after its helpers.
#ifdef DUSK_ENABLE_VULKAN_RT
static void check_gpu_hair(const std::string& device, const std::filesystem::path& folder) {
    auto source = sphere_scene(scene_material_model::hair);
    auto& strands = source.materials[0];
    strands.base_color = vec3(.75, .5, .3);
    strands.roughness = .4;
    strands.hair_radial_roughness = .35;
    auto cam = test_camera(16, 16, 1024);
    cam.vfov = .001;
    cam.max_depth = 4;
    cam.background = colour(.4, .45, .5);
    cam.use_sun = true;
    cam.sun_radiance = colour(3, 3, 3);
    cam.sun_angular_radius = 2;
    cam.direct_light_samples = 1;
    auto world = build_world_from_scene(source);
    build_emissive_surfaces(source, cam);
    // Front, oblique and grazing views exercise R, TT/TRT and the tilt.
    const point3 targets[] = {point3(0, 0, 0), point3(.55, .35, 0), point3(-.2, .93, 0)};
    for (const auto& target : targets) {
        for (const auto& sun : {vec3(-1, 1.5, 1), vec3(1, .2, -.4)}) {
            cam.lookat = target;
            cam.sun_dir = unit_vector(sun);
            colour reference(0, 0, 0);
            const int count = 65536;
            // Jitter within a pixel footprint, as the camera does: each hit
            // samples a different position across the fibre.
            for (int i = 0; i < count; ++i) {
                const vec3 jitter(random_double() - .5, random_double() - .5, random_double() - .5);
                const vec3 direction = unit_vector(target + 1e-4 * jitter - cam.lookfrom);
                reference += cam.ray_colour(ray(cam.lookfrom, direction, 0), cam.max_depth, world) / count;
            }
            std::vector<float> linear;
            dusk_gpu::render(dusk_gpu::compile_scene(source), cam, raw_settings(), device, nullptr, nullptr, {},
                             &linear);
            std::cout << "GPU hair (" << target.x() << ',' << target.y() << "): " << channel_mean(linear, 0) << ' '
                      << channel_mean(linear, 1) << ' ' << channel_mean(linear, 2) << " CPU: " << reference << '\n';
            for (int c = 0; c < 3; ++c)
                near(channel_mean(linear, c), reference[c], .006 + .03 * reference[c],
                     "GPU hair fibres match the CPU BSDF");
        }
    }
    // Each lobe separately, under direct sunlight only, so a parity failure
    // names the lobe that drifted.
    cam.lookat = point3(.3, .2, 0);
    cam.sun_dir = unit_vector(vec3(-1, 1.5, 1));
    cam.background = colour(0, 0, 0);
    cam.max_depth = 1;
    for (int lobe = 0; lobe < 4; ++lobe) {
        auto variant = source;
        auto& m = variant.materials[0];
        if (lobe == 0) m.hair_backlit = 0;          // R, TRT and residual
        if (lobe == 1) m.hair_specular = 0;         // TT, TRT and residual
        if (lobe == 2) m.hair_cuticle_tilt = 6;     // stronger highlight shifts
        if (lobe == 3) m.hair_coloring = 1;         // volume (Cycles) absorption
        auto lobe_world = build_world_from_scene(variant);
        colour reference(0, 0, 0);
        const int count = 65536;
        for (int i = 0; i < count; ++i) {
            const vec3 jitter(random_double() - .5, random_double() - .5, random_double() - .5);
            reference += cam.ray_colour(ray(cam.lookfrom, unit_vector(cam.lookat + 1e-4 * jitter - cam.lookfrom), 0),
                                        cam.max_depth, lobe_world) / count;
        }
        std::vector<float> linear;
        dusk_gpu::render(dusk_gpu::compile_scene(variant), cam, raw_settings(), device, nullptr, nullptr, {}, &linear);
        for (int c = 0; c < 3; ++c)
            near(channel_mean(linear, c), reference[c], .002 + .03 * reference[c],
                 lobe == 0 ? "GPU hair R lobe matches the CPU" : lobe == 1 ? "GPU hair TT lobe matches the CPU"
                 : lobe == 2 ? "GPU hair cuticle tilt matches the CPU" : "GPU hair volume colouring matches the CPU");
    }
    cam.background = colour(.4, .45, .5);
    cam.max_depth = 4;
    // The Specular pin reaches the GPU through the graph.
    auto pinned = source;
    auto& graph = pinned.materials[0].graph;
    initialize_material_graph(pinned.materials[0], {});
    int specular = graph.add(graph_kind::scalar, 0, 0);
    graph.find(specular)->value = vec3(0, 0, 0);
    require(graph.connect({specular, 0, graph.output_id(), 1}), "connect hair Specular pin");
    cam.lookat = point3(0, 0, 0);
    cam.sun_dir = unit_vector(vec3(-1, 1.5, 1));
    std::vector<float> plain, dull;
    dusk_gpu::render(dusk_gpu::compile_scene(source), cam, raw_settings(), device, nullptr, nullptr, {}, &plain);
    dusk_gpu::render(dusk_gpu::compile_scene(pinned), cam, raw_settings(), device, nullptr, nullptr, {}, &dull);
    require(channel_mean(dull, 2) < channel_mean(plain, 2), "a zero Specular pin removes the GPU hair highlight");

    // Visual fixture: strands run pole to pole, so highlights band across them.
    // Back light from above and behind shows the Backlit (TT) rim.
    auto view = test_camera(256, 256, 256);
    view.lookfrom = point3(0, .4, 4.2);
    view.lookat = point3(0, 0, 0);
    view.vfov = 32;
    view.background = colour(.03, .03, .035);
    view.use_sun = true;
    view.sun_dir = unit_vector(vec3(-.6, .9, .8));
    view.sun_radiance = colour(3, 3, 3);
    view.sun_angular_radius = 1;
    view.max_depth = 8;
    view.point_lights.push_back({point3(.4, 1.6, -2.4), colour(18, 16, 14), 20});
    auto blond = sphere_scene(scene_material_model::hair);
    blond.materials[0].base_color = vec3(.82, .62, .38);
    blond.materials[0].roughness = .3;
    auto denoised = raw_settings();
    denoised.use_denoiser = true;
    auto image = dusk_gpu::render(dusk_gpu::compile_scene(blond), view, denoised, device, nullptr, nullptr, {});
    const auto path = (folder / "hair-sphere.png").string();
    require(stbi_write_png(path.c_str(), image.width, image.height, 3, image.pixels.data(), image.width * 3) != 0,
            "save GPU hair fixture");
}
#endif
