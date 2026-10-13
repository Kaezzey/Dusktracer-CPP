// The Vulkan eye shading model and alpha modes (pathtrace.comp) against the
// CPU reference. Included by gpu_tests.cpp after its helpers.
#ifdef DUSK_ENABLE_VULKAN_RT
// Mean CPU radiance over a pixel footprint around the camera's target.
static colour cpu_footprint(camera& cam, const hittable& world, int count = 65536) {
    colour reference(0, 0, 0);
    for (int i = 0; i < count; ++i) {
        const vec3 jitter(random_double() - .5, random_double() - .5, random_double() - .5);
        const vec3 direction = unit_vector(cam.lookat + 1e-4 * jitter - cam.lookfrom);
        reference += cam.ray_colour(ray(cam.lookfrom, direction, 0), cam.max_depth, world) / count;
    }
    return reference;
}

static void check_gpu_eye(const std::string& device) {
    // An eye on a sphere whose base colour is its UV, so the refracted iris
    // lookup shows up in the colour. The iris faces the camera (U 0.25).
    auto source = sphere_scene(scene_material_model::eye);
    auto& eye = source.materials[0];
    eye.roughness = .2;
    eye.ior = 1.336;
    eye.eye_iris_u = .25;
    eye.eye_iris_v = .5;
    eye.eye_iris_radius = .12;
    eye.eye_iris_depth = .6;
    initialize_material_graph(eye, {});
    int uv = eye.graph.add(graph_kind::texcoord, 0, 0), rgb = eye.graph.add(graph_kind::combine, 0, 0);
    require(eye.graph.connect({uv, 1, rgb, 0}) && eye.graph.connect({uv, 2, rgb, 1}) &&
                eye.graph.connect({rgb, 0, eye.graph.output_id(), 0}),
            "connect eye Base Color");
    auto cam = test_camera(16, 16, 1024);
    cam.vfov = .001;
    cam.max_depth = 4;
    cam.background = colour(.3, .32, .35);
    cam.use_sun = true;
    cam.sun_radiance = colour(3, 3, 3);
    cam.sun_angular_radius = 2;
    cam.direct_light_samples = 1;
    auto world = build_world_from_scene(source);
    // Head on, through the cornea from the side, and the sclera.
    for (const auto& target : {point3(0, 0, 0), point3(.32, .12, 0), point3(-.1, .62, 0)}) {
        for (const auto& sun : {vec3(1, .5, 1), vec3(-1, .3, .2)}) {
            cam.lookat = target;
            cam.sun_dir = unit_vector(sun);
            const colour reference = cpu_footprint(cam, world);
            std::vector<float> linear;
            dusk_gpu::render(dusk_gpu::compile_scene(source), cam, raw_settings(), device, nullptr, nullptr, {},
                             &linear);
            std::cout << "GPU eye (" << target.x() << ',' << target.y() << "): " << channel_mean(linear, 0) << ' '
                      << channel_mean(linear, 1) << ' ' << channel_mean(linear, 2) << " CPU: " << reference << '\n';
            for (int c = 0; c < 3; ++c)
                near(channel_mean(linear, c), reference[c], .006 + .03 * reference[c],
                     "GPU eye shading matches the CPU");
        }
    }
    // The Iris Mask pin replaces the UV disc: zero makes the whole eye sclera.
    auto masked = source;
    auto& graph = masked.materials[0].graph;
    int zero = graph.add(graph_kind::scalar, 0, 0);
    graph.find(zero)->value = vec3(0, 0, 0);
    require(graph.connect({zero, 0, graph.output_id(), 5}), "connect Iris Mask");
    cam.lookat = point3(.32, .12, 0);
    cam.sun_dir = unit_vector(vec3(1, .5, 1));
    auto masked_world = build_world_from_scene(masked);
    const colour sclera = cpu_footprint(cam, masked_world);
    std::vector<float> linear;
    dusk_gpu::render(dusk_gpu::compile_scene(masked), cam, raw_settings(), device, nullptr, nullptr, {}, &linear);
    for (int c = 0; c < 3; ++c)
        near(channel_mean(linear, c), sclera[c], .006 + .03 * sclera[c], "GPU Iris Mask pin matches the CPU");
}

static void check_gpu_alpha_modes(const std::string& device) {
    // A sphere with 35% opacity in front of a white sky, in each mode.
    auto source = sphere_scene(scene_material_model::pbr);
    auto& m = source.materials[0];
    initialize_material_graph(m, {});
    int opacity = m.graph.add(graph_kind::scalar, 0, 0);
    m.graph.find(opacity)->value = vec3(.35, 0, 0);
    require(m.graph.connect({opacity, 0, m.graph.output_id(), 4}), "connect Opacity");
    auto cam = test_camera(16, 16, 2048);
    cam.vfov = .001;
    cam.max_depth = 6;
    const struct { int mode; double cutoff; const char* message; } cases[] = {
        {alpha_blend, .5, "GPU Blend alpha matches the CPU"},
        {alpha_clip, .5, "GPU Clip removes alpha below its threshold"},
        {alpha_clip, .3, "GPU Clip keeps alpha above its threshold solid"},
        {alpha_opaque, .5, "GPU Opaque ignores alpha"},
    };
    double blended = 0, clipped = 0, solid = 0;
    for (const auto& c : cases) {
        m.alpha_mode = c.mode;
        m.alpha_cutoff = c.cutoff;
        auto world = build_world_from_scene(source);
        const colour reference = cpu_footprint(cam, world, 32768);
        std::vector<float> linear;
        dusk_gpu::render(dusk_gpu::compile_scene(source), cam, raw_settings(), device, nullptr, nullptr, {}, &linear);
        for (int k = 0; k < 3; ++k) near(channel_mean(linear, k), reference[k], .006 + .03 * reference[k], c.message);
        (c.mode == alpha_blend ? blended : c.cutoff == .5 && c.mode == alpha_clip ? clipped : solid) =
            channel_mean(linear, 2);
    }
    near(clipped, 1, 1e-4, "a clipped-away sphere shows the sky unchanged");
    require(solid < blended && blended < clipped, "Blend lies between solid and clipped away");

    // A surface half a millimetre behind a transparent one (an eyeball under
    // its occlusion shell) is seen rather than skipped.
    scene layered;
    scene_material veil, glow;
    veil.model = scene_material_model::pbr;
    veil.dielectric_F0 = vec3(0, 0, 0);
    initialize_material_graph(veil, {});
    int none = veil.graph.add(graph_kind::scalar, 0, 0);
    veil.graph.find(none)->value = vec3(0, 0, 0);
    require(veil.graph.connect({none, 0, veil.graph.output_id(), 4}), "connect veil Opacity");
    glow.model = scene_material_model::diffuse_light;
    glow.emission = vec3(.2, .4, .6);
    layered.materials = {veil, glow};
    scene_object front, back;
    front.type = back.type = scene_object_type::cube;
    front.scale = back.scale = vec3(2, 2, .0001);
    front.material_index = 0;
    back.material_index = 1;
    back.center = point3(0, 0, -.0006);
    layered.objects = {front, back};
    cam.lookat = point3(0, 0, 0);
    std::vector<float> linear;
    dusk_gpu::render(dusk_gpu::compile_scene(layered), cam, raw_settings(), device, nullptr, nullptr, {}, &linear);
    near(channel_mean(linear, 2), .6, 1e-4, "GPU sees a surface just behind a transparent one");
}

// Camera and shadow rays crossing many transparent surfaces, which the GPU
// handles in one traversal each (pathtrace.comp: traceSurface, visibility):
// a stack of 30%-opaque cards (two faces each) and two 50%-opaque spheres
// (two crossings each) over a floor in sunlight.
static void check_gpu_stacked_alpha(const std::string& device) {
    scene stack;
    scene_material floor, card, ball;
    floor.model = card.model = ball.model = scene_material_model::pbr;
    floor.base_color = vec3(.8, .8, .8);
    floor.roughness = .8;
    card.base_color = vec3(.2, .5, .8);
    ball.base_color = vec3(.9, .4, .2);
    auto translucent = [](scene_material& m, double alpha) {
        initialize_material_graph(m, {});
        int opacity = m.graph.add(graph_kind::scalar, 0, 0);
        m.graph.find(opacity)->value = vec3(alpha, 0, 0);
        require(m.graph.connect({opacity, 0, m.graph.output_id(), 4}), "connect stack Opacity");
    };
    translucent(card, .3);
    translucent(ball, .5);
    stack.materials = {floor, card, ball};
    scene_object ground;
    ground.type = scene_object_type::cube;
    ground.scale = vec3(8, .02, 8);
    ground.material_index = 0;
    stack.objects.push_back(ground);
    for (int i = 0; i < 5; ++i) {
        scene_object layer;
        layer.type = scene_object_type::cube;
        layer.center = point3(0, .3 + .2 * i, 0);
        layer.scale = vec3(3, .01, 3);
        layer.material_index = 1;
        stack.objects.push_back(layer);
    }
    for (double x : {-.4, .4}) {
        scene_object sphere;
        sphere.center = point3(x, 1.6, .3);
        sphere.radius = .3;
        sphere.material_index = 2;
        stack.objects.push_back(sphere);
    }
    auto cam = test_camera(16, 16, 2048);
    cam.lookfrom = point3(0, 3.2, 2.6);
    cam.vfov = .001;
    cam.max_depth = 6;
    cam.background = colour(.2, .25, .3);
    cam.use_sun = true;
    cam.sun_dir = unit_vector(vec3(.1, 1, .25));
    cam.sun_radiance = colour(3, 3, 3);
    cam.sun_angular_radius = 2;
    cam.sun_shadow_samples = 4;
    auto world = build_world_from_scene(stack);
    // Through the cards to the floor, through a sphere, and the floor in the spheres' shadow.
    for (const auto& target : {point3(0, 0, 0), point3(.4, 1.6, .3), point3(-.25, 0, -.1)}) {
        cam.lookat = target;
        const colour reference = cpu_footprint(cam, world, 65536);
        std::vector<float> linear;
        dusk_gpu::render(dusk_gpu::compile_scene(stack), cam, raw_settings(), device, nullptr, nullptr, {}, &linear);
        std::cout << "GPU stacked alpha (" << target.x() << ',' << target.y() << "): " << channel_mean(linear, 0)
                  << ' ' << channel_mean(linear, 1) << ' ' << channel_mean(linear, 2) << " CPU: " << reference << '\n';
        for (int c = 0; c < 3; ++c)
            near(channel_mean(linear, c), reference[c], .006 + .03 * reference[c],
                 "GPU rays through stacked transparent surfaces match the CPU");
    }
}
#endif
