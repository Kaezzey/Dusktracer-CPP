// Offscreen tests of the actual OpenGL viewport, including image readback.
static std::vector<unsigned char> raster_image(int size = 256) {
    RenderRasterToTexture(size, size);
    glFinish();
    std::vector<unsigned char> pixels(size_t(size) * size * 4);
    glBindTexture(GL_TEXTURE_2D, g_rasterColorTex);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    check(glGetError() == GL_NO_ERROR, "raster passes leave no OpenGL error");
    return pixels;
}
static void raster_save(const char* name, const std::vector<unsigned char>& pixels, int size = 256) {
    stbi_flip_vertically_on_write(1);
    check(stbi_write_png(name, size, size, 4, pixels.data(), size * 4) != 0, "save raster validation image");
}
static void raster_reset() {
    g_scene = {};
    InvalidateAllRasterMaterials();
    g_show_gizmo = false;
    raster_preview::debug_view = 0;
    raster_preview::shadows_enabled = true;
    g_editor_cam.vfov = 45;
    g_editor_cam.set_from_lookat(point3(0, 0, 4), point3());
}
static void raster_wait_textures() {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    do {
        raster_image();
        bool ready = true;
        for (const auto& entry : raster_preview::textures) {
            ready &= entry.second.loaded;
        }
        if (ready) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    } while (std::chrono::steady_clock::now() < deadline);
    check(false, "asynchronous raster textures finish");
}
static void check_raster_graph() {
    raster_reset();
    scene_material material;
    material.model = scene_material_model::pbr;
    g_scene.materials.push_back(material);
    scene_object cube;
    cube.type = scene_object_type::cube;
    cube.material_index = 0;
    cube.scale = vec3(2, 2, .1);
    g_scene.objects.push_back(cube);
    raster_preview::debug_view = 1;
    auto& graph = g_scene.materials[0].graph;
    GLuint shader = g_rasterShader;
    for (auto kind :
         {graph_kind::add, graph_kind::subtract, graph_kind::multiply, graph_kind::divide, graph_kind::lerp,
          graph_kind::one_minus, graph_kind::saturate, graph_kind::power, graph_kind::reroute}) {
        graph = {};
        int output = graph.add(graph_kind::output, 0, 0), a = graph.add(graph_kind::vector, 0, 0);
        int b = graph.add(graph_kind::scalar, 0, 0), op = graph.add(kind, 0, 0);
        graph.find(a)->value = vec3(.2, .3, .4);
        graph.find(b)->value = vec3(.5, 0, 0);
        graph.connect({a, 0, op, 0});
        if (graph_inputs(kind) > 1) {
            graph.connect({b, 0, op, 1});
        }
        graph.connect({op, 0, output, 0});
        InvalidateRasterMaterial(0);
        auto pixels = raster_image();
        auto texture = compile_material_graph(
            graph, 0, [](const graph_node&, bool) -> std::shared_ptr<class texture> { return nullptr; });
        vec3 expected = texture->value(.5, .5, point3());
        for (int c = 0; c < 3; ++c) {
            int byte = int(std::lround(std::clamp(expected[c], 0., 1.) * 255));
            check(std::abs(int(pixels[(128 * 256 + 128) * 4 + c]) - byte) <= 1,
                  "raster graph arithmetic matches CPU evaluation");
        }
    }
    check(g_rasterShader == shader, "graph edits reuse the same linked raster shader");
    const unsigned char texels[] = {128, 64, 192, 96};
    check(stbi_write_png("material.png", 1, 1, 4, texels, 4) != 0, "write material fixture");
    g_scene.textures.push_back({"Fixture", "material.png"});
    graph = {};
    int output = graph.add(graph_kind::output, 0, 0), sample = graph.add(graph_kind::texture_sample, 0, 0);
    graph.find(sample)->texture_index = 0;
    for (int channel = 0; channel < 6; ++channel) {
        graph.connect({sample, channel, output, 0});
        InvalidateRasterMaterial(0);
        raster_wait_textures();
        auto pixels = raster_image();
        auto texture = compile_material_graph(graph, 0, [](const graph_node&, bool srgb) {
            return std::make_shared<image_texture>("material.png", srgb ? texture_sample_space::srgb_color
                                                                        : texture_sample_space::linear_data);
        });
        vec3 expected = texture->value(.5, .5, point3());
        // PBR alpha is tested separately with an opaque base; these channel checks
        // use an explicit opacity of one so texture alpha doesn't discard RGB.
        if (channel == 0) {
            int opacity = graph.add(graph_kind::scalar, 0, 0);
            graph.find(opacity)->value = vec3(1, 0, 0);
            graph.connect({opacity, 0, output, 4});
            InvalidateRasterMaterial(0);
            pixels = raster_image();
        }
        for (int c = 0; c < 3; ++c) {
            check(std::abs(int(pixels[(128 * 256 + 128) * 4 + c]) -
                           int(std::lround(std::clamp(expected[c], 0., 1.) * 255))) <= 1,
                  "raster sRGB, raw channels and masks match CPU graph");
        }
    }
    // Mutate opacity through the graph and verify that both visible surfaces disappear.
    int opacity = graph.incoming(output, 4)->from;
    graph.find(opacity)->value = vec3(0, 0, 0);
    InvalidateRasterMaterial(0);
    auto cutout = raster_image();
    check(cutout[(128 * 256 + 128) * 4] < 20, "opacity graph cuts out raster geometry");
}
static void check_raster_mesh_slots() {
    raster_reset();
    std::ofstream("slots.mtl") << "newmtl Left\nKd 1 0 0\nnewmtl Right\nKd 0 1 0\n";
    std::ofstream("slots.obj") << "mtllib slots.mtl\n"
                                  "v -1 0 0\nv 0 0 0\nv 0 2 0\nv -1 2 0\nv 1 0 0\nv 1 2 0\n"
                                  "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\n"
                                  "usemtl Left\nf 1/1 2/2 3/3\nf 1/1 3/3 4/4\n"
                                  "usemtl Right\nf 2/2 5/1 6/4\nf 2/2 6/4 3/3\n";
    auto loaded = load_assimp_mesh_as_gpu_mesh("slots.obj", false, true, 2);
    check(loaded.mesh.ranges.size() == 2, "raster importer keeps material draw ranges");
    g_gpu_meshes.push_back(loaded.mesh);
    scene_mesh_asset asset;
    asset.file_path = "slots.obj";
    asset.slot_names = loaded.material_slot_names;
    g_scene.meshes.push_back(asset);
    scene_material left;
    left.model = scene_material_model::pbr;
    left.base_color = vec3(.8, .1, .05);
    scene_material right = left;
    right.base_color = vec3(.05, .8, .1);
    g_scene.materials = {left, right};
    scene_object mesh;
    mesh.type = scene_object_type::mesh_instance;
    mesh.mesh_index = int(g_gpu_meshes.size() - 1);
    // Asset and GPU mesh indices coincide in the production scene.
    check(mesh.mesh_index == 0, "isolated raster mesh fixture index");
    mesh.material_index = 0;
    for (const auto& name : asset.slot_names) {
        mesh.mesh_slot_materials.push_back(name == "Right" ? 1 : 0);
    }
    g_scene.objects.push_back(mesh);
    g_editor_cam.set_from_lookat(point3(0, 1, 4), point3(0, 1, 0));
    raster_preview::debug_view = 1;
    auto image = raster_image();
    check(image[(128 * 256 + 90) * 4] > 180 && image[(128 * 256 + 166) * 4 + 1] > 180,
          "different mesh slots display their own assigned materials");
    raster_save("mesh-material-slots.png", image);
    const unsigned char normal[] = {204, 128, 230, 255};
    stbi_write_png("normal.png", 1, 1, 4, normal, 4);
    g_scene.textures.push_back({"Normal", "normal.png"});
    g_scene.materials[0].normal_tex = g_scene.materials[1].normal_tex = 0;
    InvalidateAllRasterMaterials();
    raster_wait_textures();
    raster_preview::debug_view = 2;
    image = raster_image();
    check(image[(128 * 256 + 90) * 4] > 165 && image[(128 * 256 + 166) * 4] < 90,
          "normal mapping retains mirrored UV tangent handedness");
    raster_save("mesh-normal-frames.png", image);
    auto world = build_world_from_scene(g_scene);
    hit_record hit;
    check(world.hit(ray(point3(-.5, 1, 4), vec3(0, 0, -1), 0), interval(.001, infinity), hit),
          "CPU material-slot mesh intersects");
    check(hit.tangent.x() > .9, "CPU and raster agree on increasing U direction");
    auto model_thumb = GenerateModelThumbnail(0);
    check(model_thumb != 0 && glGetError() == GL_NO_ERROR,
          "mesh thumbnails still use the dedicated studio shader");
    glDeleteTextures(1, &model_thumb);
    glDeleteVertexArrays(1, &loaded.mesh.vao);
    glDeleteBuffers(1, &loaded.mesh.vbo);
    glDeleteBuffers(1, &loaded.mesh.ebo);
    g_gpu_meshes.clear();
}
static int count_shadow_pixels(const std::vector<unsigned char>& lit,
                               const std::vector<unsigned char>& shadowed) {
    int count = 0;
    for (size_t p = 0; p < lit.size(); p += 4) {
        if (int(lit[p]) + lit[p + 1] + lit[p + 2] >
            int(shadowed[p]) + shadowed[p + 1] + shadowed[p + 2] + 30) {
            ++count;
        }
    }
    return count;
}
static void check_raster_shadows() {
    raster_reset();
    scene_material floor;
    floor.model = scene_material_model::pbr;
    floor.base_color = vec3(.65, .65, .65);
    floor.roughness = .8;
    scene_material copper = floor;
    copper.base_color = vec3(.75, .24, .09);
    copper.metallic = .8;
    copper.roughness = .3;
    g_scene.materials = {floor, copper};
    scene_object ground;
    ground.type = scene_object_type::cube;
    ground.material_index = 0;
    ground.center = point3(0, -.6, 0);
    ground.scale = vec3(6, .1, 6);
    scene_object ball;
    ball.material_index = 1;
    ball.center = point3(-.6, .2, 0);
    ball.radius = .75;
    scene_object box = ground;
    box.material_index = 1;
    box.center = point3(1, .35, -.8);
    box.scale = vec3(.75, 1.8, .6);
    box.rotation_deg = vec3(0, 30, 20);
    g_scene.objects = {ground, ball, box};
    scene_light sun;
    sun.type = scene_light_type::directional;
    sun.direction = vec3(-1, -2, -1);
    sun.radiance = vec3(4, 4, 4);
    g_scene.lights.push_back(sun);
    g_editor_cam.set_from_lookat(point3(4, 3, 6), point3(0, 0, 0));
    raster_preview::shadows_enabled = false;
    auto lit = raster_image();
    raster_preview::shadows_enabled = true;
    auto shadowed = raster_image();
    check(count_shadow_pixels(lit, shadowed) > 500, "directional shadow map casts visible object shadows");
    raster_save("sun-shadows.png", shadowed);
    auto updates = raster_preview::shadow_updates;
    raster_image();
    check(raster_preview::shadow_updates == updates, "unchanged viewport reuses shadow maps");
    g_editor_cam.set_from_lookat(point3(4.1, 3, 6), point3(0, 0, 0));
    raster_image();
    check(raster_preview::shadow_updates == updates, "camera movement reuses world-space shadow maps");
    g_scene.objects[1].translation = vec3(.1, 0, 0);
    raster_image();
    check(raster_preview::shadow_updates == updates + 1, "object movement refreshes shadow maps");
    g_scene.lights[0].type = scene_light_type::point;
    g_scene.lights[0].position = point3(-2, 4, 2);
    g_scene.lights[0].radiance = vec3(60, 60, 60);
    raster_preview::shadows_enabled = false;
    lit = raster_image();
    raster_preview::shadows_enabled = true;
    shadowed = raster_image();
    check(count_shadow_pixels(lit, shadowed) > 500, "point light cubemap casts visible object shadows");
    raster_save("point-shadows.png", shadowed);
    int false_shadow = 0;
    for (int y = 20; y < 50; ++y) {
        for (int x = 80; x < 170; ++x) {
            size_t pixel = size_t(y * 256 + x) * 4;
            if (int(lit[pixel]) - shadowed[pixel] > 3) {
                ++false_shadow;
            }
        }
    }
    check(false_shadow < 5, "point shadows do not produce self-shadow rings on the exposed floor");
    // Isolate the effect of a textured PBR surface from scene lighting.
    updates = raster_preview::shadow_updates;
    auto metallic = shadowed;
    g_scene.materials[1].metallic = 0;
    InvalidateRasterMaterial(1);
    auto dielectric = raster_image();
    check(dielectric != metallic, "metallic assignment changes raster lighting");
    g_scene.materials[1].roughness = .85;
    InvalidateRasterMaterial(1);
    check(raster_image() != dielectric, "roughness assignment changes raster lighting");
    check(raster_preview::shadow_updates == updates, "metal and roughness edits reuse shadow maps");
    auto shadow_depth = [] {
        constexpr size_t face_pixels = raster_preview::point_resolution*raster_preview::point_resolution;
        std::vector<float> depth(face_pixels*6);
        glBindTexture(GL_TEXTURE_CUBE_MAP,raster_preview::point_depth);
        for (int face = 0; face < 6; ++face)
            glGetTexImage(GL_TEXTURE_CUBE_MAP_POSITIVE_X+face,0,GL_DEPTH_COMPONENT,GL_FLOAT,depth.data()+face*face_pixels);
        return depth;
    };
    auto opaque_depth = shadow_depth();
    auto& graph = g_scene.materials[1].graph;
    int output = graph.add(graph_kind::output,0,0), opacity = graph.add(graph_kind::scalar,0,0);
    graph.find(opacity)->value = vec3(0,0,0); graph.connect({opacity,0,output,4});
    InvalidateRasterMaterial(1); raster_image();
    auto transparent_depth = shadow_depth();
    size_t removed = 0, added = 0;
    for (size_t i = 0; i < opaque_depth.size(); ++i) {
        removed += transparent_depth[i] > opaque_depth[i]+1e-5f;
        added += transparent_depth[i] < opaque_depth[i]-1e-5f;
    }
    check(removed > 100 && added == 0,"opacity edits remove shadow casters and invalidate cached cube depths");
}
static void raster_production_scene(const char* root) {
    raster_reset();
    const auto output = std::filesystem::current_path();
    const auto project = std::filesystem::absolute(root);
    std::filesystem::current_path(project);
    build_default_scene(g_scene);
    std::filesystem::current_path(output);
    for (auto& asset : g_scene.meshes) {
        asset.file_path = (project / asset.file_path).string();
    }
    for (auto& texture : g_scene.textures) {
        if (std::filesystem::path(texture.path).is_relative()) {
            texture.path = (project / texture.path).string();
        }
    }
    g_editor_cam.set_from_lookat(point3(.15, .1, 3.5), point3(.15, -.05, -1));
    raster_wait_textures();
    raster_save("production-raster.png", raster_image(1024), 1024);
    std::vector<double> times;
    const auto updates = raster_preview::shadow_updates;
    for (int i = 0; i < 30; ++i) {
        auto start = std::chrono::steady_clock::now();
        RenderRasterToTexture(1024, 1024);
        glFinish();
        times.push_back(
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    }
    check(raster_preview::shadow_updates == updates, "warm production frames reuse shadow maps");
    std::sort(times.begin(), times.end());
    double total = 0;
    for (double time : times) {
        total += time;
    }
    std::printf("RASTER warm 1024x1024: mean %.3f ms, p95 %.3f ms (30 completed GPU frames)\n",
                total / times.size(), times[28]);
}
static int run_raster_checks(const char* project) {
    check(glfwInit() != 0, "raster GLFW init");
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    auto* window = glfwCreateWindow(640, 640, "Raster checks", nullptr, nullptr);
    check(window != nullptr, "hidden raster context");
    glfwMakeContextCurrent(window);
    glewInit();
    while (glGetError() != GL_NO_ERROR) {
    }
    BuildRasterShader();
    BuildUnitSphereMesh();
    BuildUnitCubeMesh();
    GLint linked = GL_FALSE;
    glGetProgramiv(g_rasterShader, GL_LINK_STATUS, &linked);
    check(linked == GL_TRUE, "material raster shader compiles and links");
    glGetProgramiv(raster_preview::shadow_program.id, GL_LINK_STATUS, &linked);
    check(linked == GL_TRUE, "masked shadow shader compiles and links");
    check_raster_graph();
    check_raster_mesh_slots();
    check_raster_shadows();
    if (project) {
        raster_production_scene(project);
    }
    StopRasterPreview();
    glfwDestroyWindow(window);
    glfwTerminate();
    std::printf("%d raster material, transform and shadow checks passed\n", checks);
    return 0;
}
