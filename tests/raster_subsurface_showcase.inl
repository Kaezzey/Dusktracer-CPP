// Visual comparison of both Unreal models in an editor-like scene: a normal-
// mapped sphere on a floor beside a wall, under a high sun. Run the raster
// checks with DUSK_SSS_SHOWCASE set to the folder holding skin_0001_*_1k
// textures; images are written to the working folder.
namespace subsurface_showcase {
// Side-lit: a red wall and a sun from above and behind put the terminator
// across the middle of the view, as in the editor scene that showed artefacts.
static void scene_fixture(int shading, const std::string& models, bool side_lit = false) {
    raster_reset();
    g_scene.textures.push_back({"Skin color", models + "/skin_0001_color_1k.jpg"});
    g_scene.textures.push_back({"Skin normal", models + "/skin_0001_normal_opengl_1k.png"});
    g_scene.textures.push_back({"Skin roughness", models + "/skin_0001_roughness_1k.jpg"});
    scene_material skin;
    skin.model = scene_material_model::pbr;
    skin.albedo_tex = 0;
    skin.normal_tex = 1;
    skin.roughness_tex = 2;
    skin.use_sss = true;
    skin.sss_model = SSS_DIPOLE_BURLEY;
    skin.sss_shading = shading;
    skin.sss_strength = 1;
    g_scene.materials.push_back(skin);
    scene_material floor;
    floor.model = scene_material_model::lambert;
    floor.base_color = vec3(.75, .75, .75);
    g_scene.materials.push_back(floor);
    scene_material wall = floor;
    wall.base_color = vec3(.45, .45, .45);
    g_scene.materials.push_back(wall);
    scene_object sphere;
    sphere.material_index = 0;
    sphere.radius = 1;
    sphere.center = point3(0, 1, 0);
    g_scene.objects.push_back(sphere);
    scene_object slab;
    slab.type = scene_object_type::cube;
    slab.material_index = 1;
    slab.center = point3(0, -.1, 0);
    slab.scale = vec3(5, .2, 5);
    g_scene.objects.push_back(slab);
    scene_object side = slab;
    side.material_index = 2;
    side.center = point3(-2.6, 1.4, 0);
    side.scale = vec3(.2, 3, 5);
    g_scene.objects.push_back(side);
    if (side_lit) {
        scene_material red = floor;
        red.base_color = vec3(.7, .08, .06);
        g_scene.materials.push_back(red);
        g_scene.objects.back().material_index = 3;
        scene_object back = slab;
        back.material_index = 2;
        back.center = point3(0, 1.4, -2.6);
        back.scale = vec3(5, 3, .2);
        g_scene.objects.push_back(back);
    }
    scene_light sun;
    sun.type = scene_light_type::directional;
    sun.direction = -unit_vector(side_lit ? vec3(.35, .8, -.6) : vec3(.25, 1, .45));
    sun.radiance = vec3(3, 3, 3);
    g_scene.lights.push_back(sun);
    g_editor_cam.vfov = 40;
    g_editor_cam.set_from_lookat(point3(.9, 1.5, 3.6), point3(0, 1, 0));
}

static void render(const char* name, const std::string& device) {
    InvalidateAllRasterMaterials();
    raster_wait_textures();
    auto pixels = raster_image(640);
    raster_save((std::string("showcase-raster-") + name + ".png").c_str(), pixels, 640);
#ifdef DUSK_ENABLE_VULKAN_RT
    if (device.empty()) return;
    camera cam;
    to_shirley_camera(g_editor_cam, cam);
    cam.image_width = cam.image_height = 640;
    cam.samples_per_pixel = 256;
    cam.max_depth = 8;
    cam.background = colour(0, 0, 0);
    cam.use_sun = true;
    cam.sun_dir = -g_scene.lights[0].direction;
    cam.sun_radiance = g_scene.lights[0].radiance;
    cam.sun_angular_radius = g_scene.lights[0].angular_radius_deg;
    cam.sun_shadow_samples = 1;
    renderer settings;
    settings.use_denoiser = false;
    settings.adaptive_sampling = false;
    auto image = dusk_gpu::render(dusk_gpu::compile_scene(g_scene), cam, settings, device, nullptr, nullptr, {});
    stbi_flip_vertically_on_write(0);
    check(stbi_write_png((std::string("showcase-traced-") + name + ".png").c_str(), image.width, image.height, 3,
                         image.pixels.data(), image.width * 3) != 0, "save traced showcase");
#endif
}

static void run() {
    std::string device;
    for (const auto& candidate : enumerate_render_devices().devices) {
        if (candidate.compatible) { device = candidate.id; break; }
    }
    const char* models = std::getenv("DUSK_SSS_SHOWCASE");
    scene_fixture(sss_shading_unreal_subsurface, models);
    render("unreal-subsurface", device);
    subsurface_parity::compare("showcase-unreal-direct", 256, 0);
    scene_fixture(sss_shading_unreal_profile, models);
    subsurface_parity::compare("showcase-profile-direct", 256, 0);
    render("profile", device);
    g_scene.materials[0].use_sss = false;
    render("no-sss", device);
    scene_fixture(sss_shading_unreal_profile, models, true);
    g_scene.materials[0].sss_units_per_metre = 30;
    render("side-profile", device);
    subsurface_parity::compare("showcase-side-profile-direct", 256, 0);
    scene_fixture(sss_shading_unreal_subsurface, models, true);
    render("side-unreal", device);
    subsurface_parity::compare("showcase-side-unreal-direct", 256, 0);
}
// Renders a saved project through the viewport and the GPU tracer, using its
// viewport camera, lights and Sky / Background colour, as the editor does.
static void render_project(const char* file) {
    std::string device;
    for (const auto& candidate : enumerate_render_devices().devices) {
        if (candidate.compatible) { device = candidate.id; break; }
    }
    auto doc = dusk_project::load(file);
    raster_reset();
    g_scene = doc.world;
    g_editor_cam = doc.viewport;
    g_camera.background = doc.camera_settings.background;
    g_renderer.exposure = doc.render_settings.exposure;
    InvalidateAllRasterMaterials();
    raster_wait_textures();
    auto pixels = raster_image(720);
    raster_save("project-raster.png", pixels, 720);
#ifdef DUSK_ENABLE_VULKAN_RT
    if (device.empty()) return;
    camera cam = doc.camera_settings;
    to_shirley_camera(doc.viewport, cam);
    cam.image_width = cam.image_height = 720;
    cam.aspect_ratio = 1;
    cam.use_sun = false;
    cam.point_lights.clear();
    for (const auto& light : doc.world.lights) {
        if (light.type == scene_light_type::directional) {
            cam.use_sun = true;
            cam.sun_dir = -light.direction;
            cam.sun_radiance = light.radiance;
            cam.sun_angular_radius = light.angular_radius_deg;
        }
    }
    renderer settings = doc.render_settings;
    settings.use_denoiser = false;
    settings.adaptive_sampling = false;
    auto trace = [&](const char* name) {
        auto image = dusk_gpu::render(dusk_gpu::compile_scene(g_scene), cam, settings, device, nullptr, nullptr, {});
        stbi_flip_vertically_on_write(0);
        check(stbi_write_png(name, image.width, image.height, 3, image.pixels.data(), image.width * 3) != 0,
              "save traced project");
    };
    trace("project-traced.png");
    // Optional breakdown: the same render without normal maps, then without SSS.
    if (std::getenv("DUSK_SSS_BREAKDOWN")) {
        auto saved = g_scene.materials;
        for (auto& m : g_scene.materials) {
            m.normal_tex = -1;
            m.graph.links.erase(std::remove_if(m.graph.links.begin(), m.graph.links.end(),
                [&](const graph_link& l) { return l.to == m.graph.output_id() && l.input == 3; }), m.graph.links.end());
        }
        trace("project-traced-no-normal.png");
        g_scene.materials = saved;
        for (auto& m : g_scene.materials) m.use_sss = false;
        trace("project-traced-no-sss.png");
        g_scene.materials = saved;
        for (auto& m : g_scene.materials) m.sss_mfp_color = vec3(1, .5, .3);
        trace("project-traced-softer-colour.png");
        g_scene.materials = saved;
    }
#endif
}
} // namespace subsurface_showcase
namespace subsurface_showcase_entry {
void run() {
    if (const char* project = std::getenv("DUSK_SSS_PROJECT")) subsurface_showcase::render_project(project);
    else subsurface_showcase::run();
}
}
