static int run_project_checks() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    namespace fs = std::filesystem;
    using namespace dusk_project;
    const auto run = fs::current_path() /
                     ("run-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(run);
    fs::current_path(run);
    const auto original_cwd = fs::current_path();
    auto write = [](const fs::path& file, const std::string& contents) {
        fs::create_directories(file.parent_path());
        std::ofstream out(file, std::ios::binary);
        out << contents;
        check(bool(out), "write project fixture");
    };
    auto read = [](const fs::path& file) {
        std::ifstream in(file, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), {});
    };
    auto rejects = [&](auto operation, const char* message) {
        bool rejected = false;
        try {
            operation();
        } catch (const std::exception&) {
            rejected = true;
        }
        check(rejected, message);
    };
    const auto source = run / "source", first = run / "first", second = run / "second";
    write(source / "wood.ppm", std::string("P6\n2 2\n255\n") + std::string(12, char(127)));
    write(source / "part.mtl", "newmtl Paint\nKd 1 1 1\nmap_Kd wood.ppm\nnewmtl Glass\nKd 0.2 0.2 0.2\n");
    write(source / "part.obj", "mtllib part.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\nv 1 1 0\nusemtl Paint\nf 1 2 "
                               "3\nusemtl Glass\nf 2 4 3\n");
    auto doc = empty_document();
    doc.world.textures.push_back({"Wood | texture", (source / "wood.ppm").u8string()});
    scene_material m;
    m.name = "Paint | \"copper\"\ncoat";
    m.model = scene_material_model::pbr;
    m.base_color = vec3(.14, .24, .37);
    m.metallic = .63;
    m.roughness = .17;
    m.fuzz = .21;
    m.ior = 1.67;
    m.emission = vec3(1, 2, 3);
    m.emission_intensity = 4.3;
    m.dielectric_F0 = vec3(.06, .07, .08);
    m.use_sss = true;
    m.sss_strength = .34;
    m.sss_scale = 1.7;
    m.sss_model = 4;
    m.sss_samples = 7;
    m.sss_radius = 2.1;
    m.sss_profile=1; m.sss_radius_rgb=vec3(1,.372,.186);
    m.sss_eta = 1.33;
    m.sss_color_override_enabled = true;
    m.sss_color_override_color = vec3(.1, .2, .3);
    m.sss_shading = sss_shading_unreal_profile;
    m.sss_subsurface_color = vec3(.9, .3, .2); m.sss_opacity = .27; m.sss_transmission_distance = .013;
    m.sss_surface_albedo = vec3(.8, .5, .3); m.sss_mfp_color = vec3(1, .4, .1); m.sss_mfp_distance = 2.5;
    m.sss_units_per_metre = 100; m.sss_world_unit_scale = .25; m.sss_profile_tint = vec3(.9, .8, .7);
    m.sss_lobe_roughness0 = .6; m.sss_lobe_roughness1 = 1.4; m.sss_lobe_mix = .2;
    m.unreal_pbr = true;
    m.normal_strength = .75;
    m.alpha_double_sided = false;
    m.alpha_cutoff = .31;
    m.hair_specular = .65; m.hair_radial_roughness = .45; m.hair_backlit = .8;
    m.hair_cuticle_tilt = 3.5; m.hair_tangent = vec3(1, 0, 0); m.hair_coloring = 1;
    m.alpha_mode = alpha_clip;
    m.eye_specular = .42; m.eye_iris_u = .48; m.eye_iris_v = .53; m.eye_iris_radius = .26;
    m.eye_limbus_width = .05; m.eye_iris_depth = .7;
    m.albedo_tex = m.metallic_tex = m.roughness_tex = m.normal_tex = m.alpha_tex = 0;
    initialize_material_graph(m, doc.world.textures);
    int node = m.graph.add(graph_kind::texture_sample, -123.25f, 248.5f);
    m.graph.find(node)->texture_index = 0;
    m.graph.find(node)->texture_path = doc.world.textures[0].path;
    m.graph.find(node)->space = graph_space::srgb;
    m.graph.connect({node, 0, m.graph.output_id(), 0});
    m.graph.pan_x = 120.25f;
    m.graph.pan_y = -31.5f;
    m.graph.zoom = .875f;
    m.graph.next_id += 17;
    doc.world.materials.push_back(m);
    scene_mesh_asset mesh;
    mesh.name = "Part";
    mesh.file_path = (source / "part.obj").u8string();
    mesh.slot_names = {"Paint", "Glass"};
    mesh.slot_default_materials = {0, -1};
    doc.world.meshes.push_back(mesh);
    scene_object obj;
    obj.name = "Instance";
    obj.type = scene_object_type::mesh_instance;
    obj.mesh_index = 0;
    obj.center = vec3(.2, .3, .4);
    obj.translation = vec3(2, 3, 4);
    obj.rotation_deg = vec3(15, 30, 45);
    obj.scale = vec3(2, 1, 3);
    obj.radius = 1.23;
    obj.material_index = 0;
    obj.mesh_slot_materials = {-1, 0};
    doc.world.objects.push_back(obj);
    scene_light light;
    light.name = "Sun";
    light.type = scene_light_type::directional;
    light.radiance = vec3(4, 5, 6);
    light.direction = unit_vector(vec3(-1, -2, -3));
    light.angular_radius_deg = .53;
    doc.world.lights.push_back(light);
    light.type = scene_light_type::point;
    light.name = "Fill";
    light.position = vec3(1, 2, 3);
    light.range = 34;
    doc.world.lights.push_back(light);
    auto& c = doc.camera_settings;
    c.image_width = 854;
    c.image_height = 480;
    c.samples_per_pixel = 93;
    c.max_depth = 13;
    c.background = vec3(.03, .02, .01);
    c.defocus_angle = .4;
    c.focus_dist = 7.2;
    c.sampling_method = camera::RANDOM_SAMPLES;
    c.sun_shadow_samples = 3;
    c.enable_mnee = true;
    c.mnee_per_thread_budget = 23;
    c.mnee_newton_max_iters = 6;
    c.mnee_newton_tol = .0003;
    c.mnee_step_eps = .003;
    c.mnee_sun_samples = 7;
    c.mnee_gain_scale = 1.6;
    c.direct_light_samples = 3;
    c.enable_mis = false;
    auto& r = doc.render_settings;
    r.exposure = 1.7;
    r.use_denoiser = false;
    r.denoiser_strength = .73;
    r.progressive_denoise = true;
    r.gpu_firefly_filter = false;
    r.adaptive_sampling = false;
    r.adaptive_min_samples = 19;
    r.adaptive_check_interval = 23;
    r.adaptive_rel_threshold = .043;
    r.adaptive_abs_threshold = .00013;
    r.bucket_size = 96;
    r.bucket_order = 1;
    r.gpu_buckets = false;
    doc.viewport.position = vec3(2, 3, 5);
    doc.viewport.yaw = .41;
    doc.viewport.pitch = .27;
    doc.viewport.vfov = 57;
    doc.viewport.min_pitch = -1.2;
    doc.viewport.max_pitch = 1.3;
    doc.viewport_mode = 0;
    doc.movement_speed = 3.5f;
    doc.look_sensitivity = .004f;
    doc.match_render_resolution = false;
    doc.device.backend = render_backend::vulkan;
    doc.device.device_id = "Saved GPU identity";

    auto packed = save(first / "Scene.dusk", inspect(doc));
    auto loaded = load(first / "Scene.dusk");
    check(fingerprint(inspect(packed)) == fingerprint(inspect(loaded)),
          "roundtrip every authored scene, graph, camera and renderer field");
    check(loaded.world.materials[0].graph.next_id == m.graph.next_id, "preserve unused stable node IDs");
    check(loaded.camera_settings.emissive_surfaces.empty() && !loaded.world.meshes[0].mesh_bvh,
          "runtime resources are not serialized");
    check(read(first / "Scene.dusk").find(source.u8string()) == std::string::npos,
          "scene file contains no source absolute paths");
    check(missing_assets(loaded.world).empty(), "all packed asset references exist");
    check(fs::current_path() == original_cwd, "save and open never change the process directory");
    check(loaded.world.materials[0].graph.find(node)->texture_path == loaded.world.textures[0].path,
          "texture library and explicit graph path share the same packed file");
    auto before = read(first / "Scene.dusk");
    check(!loaded.render_settings.gpu_firefly_filter,"persist GPU firefly setting");
    check(loaded.world.materials[0].sss_profile==1 && (loaded.world.materials[0].sss_radius_rgb-m.sss_radius_rgb).length_squared()==0,"persist RGB skin scattering profile");
    {
        const auto& p = loaded.world.materials[0];
        auto same = [](const vec3& a, const vec3& b) { return (a - b).length_squared() == 0; };
        check(p.sss_shading == m.sss_shading && same(p.sss_subsurface_color, m.sss_subsurface_color) &&
              p.sss_opacity == m.sss_opacity && p.sss_transmission_distance == m.sss_transmission_distance &&
              same(p.sss_surface_albedo, m.sss_surface_albedo) && same(p.sss_mfp_color, m.sss_mfp_color) &&
              p.sss_mfp_distance == m.sss_mfp_distance && p.sss_units_per_metre == m.sss_units_per_metre &&
              same(p.sss_profile_tint, m.sss_profile_tint) && p.sss_lobe_roughness0 == m.sss_lobe_roughness0 &&
              p.sss_lobe_roughness1 == m.sss_lobe_roughness1 && p.sss_lobe_mix == m.sss_lobe_mix &&
              p.sss_color_from_base == m.sss_color_from_base && p.sss_world_unit_scale == m.sss_world_unit_scale,
              "persist Unreal Subsurface and Subsurface Profile settings");
    }
    {
        const auto& p = loaded.world.materials[0];
        check(p.alpha_mode == alpha_clip && p.eye_specular == .42 && p.eye_iris_u == .48 && p.eye_iris_v == .53 &&
              p.eye_iris_radius == .26 && p.eye_limbus_width == .05 && p.eye_iris_depth == .7,
              "persist alpha mode and eye settings");
    }
    auto legacy=before;
    legacy.replace(legacy.find("DUSK_SCENE 8"),12,"DUSK_SCENE 7");
    for(const char* record : {"\nalpha_mode ","\neye "})
        for(auto at=legacy.find(record);at!=std::string::npos;at=legacy.find(record))
            legacy.erase(at+1,legacy.find('\n',at+1)-at);
    write(first / "VersionSeven.dusk",legacy);
    {
        const auto seven = load(first / "VersionSeven.dusk").world.materials[0];
        check(seven.alpha_mode == alpha_blend && seven.alpha_cutoff == .31,
              "version-seven materials blend alpha and keep their cutoff for Clip");
        check(seven.eye_specular == .5 && seven.eye_iris_radius == .15 && seven.eye_iris_depth == .5,
              "version-seven materials load default eye settings");
    }
    legacy.replace(legacy.find("DUSK_SCENE 7"),12,"DUSK_SCENE 6");
    for(const char* record : {"\nbuckets ","\nhair "})
        for(auto at=legacy.find(record);at!=std::string::npos;at=legacy.find(record))
            legacy.erase(at+1,legacy.find('\n',at+1)-at);
    write(first / "VersionSix.dusk",legacy);
    {
        auto six=load(first / "VersionSix.dusk");
        check(six.render_settings.bucket_size==64 && six.render_settings.bucket_order==0 && six.render_settings.gpu_buckets,
              "version-six scenes render with default buckets");
        const auto& hair = six.world.materials[0];
        check(hair.hair_specular==.5 && hair.hair_radial_roughness==.3 && hair.hair_backlit==1 &&
              hair.hair_cuticle_tilt==2 && (hair.hair_tangent-vec3(0,1,0)).length_squared()==0 &&
              hair.hair_coloring==0,
              "version-six materials load default hair settings");
    }
    legacy.replace(legacy.find("DUSK_SCENE 6"),12,"DUSK_SCENE 5");
    for(auto at=legacy.find("sss_world_unit_scale ");at!=std::string::npos;at=legacy.find("sss_world_unit_scale "))
        legacy.erase(at,legacy.find('\n',at)-at+1);
    write(first / "VersionFive.dusk",legacy);
    check(load(first / "VersionFive.dusk").world.materials[0].sss_world_unit_scale==.1,
          "version-five profiles keep millimetre mean free paths");
    legacy.replace(legacy.find("DUSK_SCENE 5"),12,"DUSK_SCENE 4");
    for(auto at=legacy.find("sss_base_color ");at!=std::string::npos;at=legacy.find("sss_base_color "))
        legacy.erase(at,legacy.find('\n',at)-at+1);
    write(first / "VersionFour.dusk",legacy);
    check(!load(first / "VersionFour.dusk").world.materials[0].sss_color_from_base,
          "version-four Unreal Subsurface colours are not multiplied by base colour");
    legacy.replace(legacy.find("DUSK_SCENE 4"),12,"DUSK_SCENE 3");
    for(auto at=legacy.find("sss_unreal ");at!=std::string::npos;at=legacy.find("sss_unreal "))
        legacy.erase(at,legacy.find('\n',at)-at+1);
    write(first / "VersionThree.dusk",legacy);
    check(load(first / "VersionThree.dusk").world.materials[0].sss_shading==sss_shading_burley,
          "version-three scenes keep legacy Burley diffusion");
    legacy.replace(legacy.find("DUSK_SCENE 3"),12,"DUSK_SCENE 1");
    for(auto at=legacy.find("sss_profile ");at!=std::string::npos;at=legacy.find("sss_profile "))
        legacy.erase(at,legacy.find('\n',at)-at+1);
    auto version_two=legacy;
    version_two.replace(version_two.find("DUSK_SCENE 1"),12,"DUSK_SCENE 2");
    write(first / "VersionTwo.dusk",version_two);
    auto older=load(first / "VersionTwo.dusk");
    check(!older.render_settings.gpu_firefly_filter && (older.world.materials[0].sss_radius_rgb-vec3(1,1,1)).length_squared()==0,
          "version-two scenes retain GPU settings and neutral diffusion distances");
    auto filter_at=legacy.find("gpu_filter ");
    legacy.erase(filter_at,legacy.find('\n',filter_at)-filter_at+1);
    write(first / "Legacy.dusk",legacy);
    check(load(first / "Legacy.dusk").render_settings.gpu_firefly_filter,"version-one scenes load with firefly suppression enabled");
    check((load(first / "Legacy.dusk").world.materials[0].sss_radius_rgb-vec3(1,1,1)).length_squared()==0,"legacy scenes preserve neutral scattering distances");
    save(first / "Scene.dusk", inspect(loaded));
    check(read(first / "Scene.dusk") == before, "repeated save is deterministic");

    auto portable = save(second / "Copy.dusk", inspect(loaded));
    fs::rename(source, run / "source-away");
    fs::rename(first, run / "first-away");
    fs::rename(second, run / "moved");
    portable = load(run / "moved" / "Copy.dusk");
    check(missing_assets(portable.world).empty(),
          "Save As remains portable after both source directories move");
    Assimp::Importer importer;
    const auto* imported = importer.ReadFile(portable.world.meshes[0].file_path, aiProcess_Triangulate);
    check(imported && imported->mNumMeshes == 2,
          "portable OBJ retains both material groups through its MTL sidecar");
    bool paint = false, glass = false;
    if (imported) {
        for (unsigned i = 0; i < imported->mNumMaterials; ++i) {
            aiString name;
            imported->mMaterials[i]->Get(AI_MATKEY_NAME, name);
            paint |= std::string(name.C_Str()) == "Paint";
            glass |= std::string(name.C_Str()) == "Glass";
        }
    }
    check(paint && glass, "OBJ material names survive packing and folder relocation");
    const auto unicode_root = run / fs::u8path(u8"Studio-\u65e5\u672c");
    auto unicode_scene = save(unicode_root / "Scene.dusk", inspect(portable));
    Assimp::Importer unicode_importer;
    check(unicode_importer.ReadFile(unicode_scene.world.meshes[0].file_path, aiProcess_Triangulate) !=
              nullptr,
          "model decoder opens project folders with Unicode names");
    int image_w = 0, image_h = 0, channels = 0;
    auto* decoded = stbi_load(unicode_scene.world.textures[0].path.c_str(), &image_w, &image_h, &channels, 4);
    check(decoded && image_w == 2 && image_h == 2, "texture decoder opens Unicode project paths");
    stbi_image_free(decoded);
    render_result small_render{1, 1, {30, 40, 50}};
    check(write_ppm((unicode_root / "render.ppm").u8string(), small_render),
          "render output supports Unicode project folders");
    auto old_source = run / "source-away";
    write(run / "collision" / "wood.ppm", "P3\n1 1\n255\n0 0 0\n");
    auto a = import_asset(old_source / "wood.ppm", run / "collision-target");
    auto b = import_asset(run / "collision" / "wood.ppm", run / "collision-target");
    check(a != b && read(a) != read(b), "same-named assets never overwrite one another");
    check(import_asset(old_source / "wood.ppm", run / "collision-target") == a,
          "repeated import reuses identical content");
    check(import_asset(a, run / "collision-target") == a, "files already inside a project remain in place");
    write(run / "collision-target" / ".dusk-cache" / "ignored.ppm", "cache");
    write(run / "collision-target" / "Renders" / "ignored.ppm", "output");
    check(discover_assets(run / "collision-target").size() == 2,
          "asset discovery excludes caches and render output");

    const auto file = run / "moved" / "Copy.dusk";
    const auto original = read(file);
    auto corrupt = [&](const std::string& contents) {
        write(run / "broken.dusk", contents);
        rejects([&] { load(run / "broken.dusk"); }, "reject malformed scene document");
    };
    corrupt("DUSK_SCENE 99\n");
    corrupt(original.substr(0, original.size() / 2));
    corrupt(original + "junk");
    auto traversal = original;
    const auto at = traversal.find("textures/");
    check(at != std::string::npos, "fixture contains project-relative path");
    traversal.insert(at, "../");
    corrupt(traversal);
    auto bad = portable;
    bad.world.objects[0].material_index = 9999;
    rejects([&] { save(file, inspect(bad)); }, "reject dangling material reference on save");
    bad = portable;
    bad.camera_settings.background[0] = std::numeric_limits<double>::infinity();
    rejects([&] { save(file, inspect(bad)); }, "reject non-finite settings");
    bad = portable;
    bad.world.textures[0].path = (run / "missing-external.ppm").u8string();
    rejects([&] { save(file, inspect(bad)); },
            "missing external assets cannot silently create a nonportable save");
    check(read(file) == original, "failed saves leave previous scene intact");
#ifdef _WIN32
    HANDLE locked =
        CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    check(locked != INVALID_HANDLE_VALUE, "lock saved manifest against replacement");
    rejects([&] { save(file, inspect(portable)); }, "locked manifest reports atomic replacement failure");
    CloseHandle(locked);
    check(read(file) == original, "replacement failure preserves saved scene bytes");
#endif
    auto missing = portable;
    missing.world.textures[0].path = (file.parent_path() / "textures" / "missing.ppm").u8string();
    auto missing_saved = save(file.parent_path() / "Missing.dusk", inspect(missing));
    check(missing_assets(missing_saved.world).size() == 1,
          "missing local assets retain their identity for repair");
    auto derived = portable;
    derived.camera_settings.aspect_ratio = .7;
    derived.camera_settings.use_sun = true;
    derived.viewport.move_speed = 200;
    derived.camera_settings.lookfrom = vec3(9, 9, 9);
    check(fingerprint(inspect(derived)) == fingerprint(inspect(portable)),
          "derived camera data does not spuriously dirty a project");

    std::printf("Project archive and portability checks complete; starting editor checks.\n");
    check(glfwInit() != 0, "project integration GLFW init");
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    auto* window = glfwCreateWindow(800, 600, "Project tests", nullptr, nullptr);
    check(window != nullptr, "hidden project integration window");
    glfwMakeContextCurrent(window);
    glewInit();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    g_project_session_active = true;
    std::printf("Project GL context initialized.\n");
    check(OpenProjectFile(file), "production editor opens portable scene");
    check(g_scene.objects.size() == 1 && g_scene.lights.size() == 2 && g_gpu_meshes[0].vao != 0,
          "opening replaces scene and rebuilds GPU geometry");
    check(g_camera.image_width == 854 && g_editor_cam.vfov == 57 &&
              g_render_selection.device_id == "Saved GPU identity",
          "editor restores camera, resolution and GPU selection");
    check(g_scene.meshes[0].slot_names.size() >= 2, "mesh slots loaded from portable OBJ");
    int paint_slot = -1, glass_slot = -1;
    for (size_t i = 0; i < g_scene.meshes[0].slot_names.size(); ++i) {
        if (g_scene.meshes[0].slot_names[i] == "Paint") {
            paint_slot = int(i);
        }
        if (g_scene.meshes[0].slot_names[i] == "Glass") {
            glass_slot = int(i);
        }
    }
    check(paint_slot >= 0 && glass_slot >= 0 && g_scene.meshes[0].slot_default_materials[paint_slot] == 0 &&
              g_scene.objects[0].mesh_slot_materials[glass_slot] == 0,
          "shared defaults and instance overrides follow slot names");
    check(SaveProjectFile(file) && !ProjectHasChanges(), "saving records the exact clean editor state");
    g_scene.objects[0].rotation_deg[1] += 23;
    check(ProjectHasChanges(), "object transform edit marks project dirty");
    g_scene.objects[0].rotation_deg[1] -= 23;
    check(!ProjectHasChanges(), "reverting a transform returns to clean state");
    g_scene.materials[0].graph.pan_x += 3;
    check(ProjectHasChanges(), "shader graph layout is authored project state");
    check(SaveProjectFile(file), "save edited project");
    g_camera.samples_per_pixel++;
    check(ProjectHasChanges(), "render setting edits mark project dirty");
    g_camera.samples_per_pixel--;
    g_selected_object = 0;
    g_graph_states[0] = {};
    UndoManager::Instance().push(std::make_unique<LambdaAction>([] {}, [] {}, "Old scene edit"));
    const auto saved_scene = dusk_project::fingerprint(CurrentProjectView());
    check(!OpenProjectFile(run / "broken.dusk") &&
              dusk_project::fingerprint(CurrentProjectView()) == saved_scene,
          "failed open preserves current scene, camera and settings");
    project_ui::notice.clear();
    g_render_in_progress = true;
    check(!CreateProjectFile(run / "blocked" / "New.dusk") && !OpenProjectFile(file),
          "project switching is blocked during rendering");
    g_render_in_progress = false;
    std::promise<void> old_started, old_release, old_finished;
    auto release_old = old_release.get_future().share();
    auto old_finished_future = old_finished.get_future();
    auto stale_revision = ++g_preview_revision;
    g_material_preview_states[0].requested = stale_revision;
    g_editor_previews.request("M0", stale_revision, [&] {
        old_started.set_value();
        release_old.wait();
        old_finished.set_value();
        return preview_image{1, 1, {255, 0, 255, 255}};
    });
    check(old_started.get_future().wait_for(std::chrono::seconds(2)) == std::future_status::ready,
          "old project preview begins before switch");
    auto stale_texture_generation = raster_preview::texture_generation;
    check(CreateProjectFile(run / "empty" / "New.dusk"), "create independent project");
    check(g_scene.objects.empty() && g_scene.materials.empty() && g_scene.lights.empty() &&
              g_scene.meshes.empty(),
          "new project contains no previous or demo scene data");
    check(g_selected_object == -1 && g_graph_states.empty() && !UndoManager::Instance().can_undo() &&
              !g_rtHasImage,
          "switch clears old selection, graph interactions, undo and rendered image");
    old_release.set_value();
    check(old_finished_future.wait_for(std::chrono::seconds(2)) == std::future_status::ready,
          "project switch does not wait for active preview");
    g_scene.materials.push_back({});
    auto fresh_revision = ++g_preview_revision;
    g_material_preview_states[0].requested = fresh_revision;
    std::promise<void> preview_barrier;
    g_editor_previews.request("M999", ++g_preview_revision, [&] {
        preview_barrier.set_value();
        return preview_image{};
    });
    check(preview_barrier.get_future().wait_for(std::chrono::seconds(2)) == std::future_status::ready,
          "old preview result is queued before pumping new project previews");
    PumpEditorPreviews();
    check(g_material_thumb_cache.empty() && g_material_preview_states[0].displayed == 0,
          "late preview cannot paint a new project's reused material index");
    g_scene.materials.clear();
    check(raster_preview::texture_generation > stale_texture_generation &&
              raster_preview::queued_textures == 0,
          "project switch advances texture generation and resets upload accounting");
    g_dropped_files = {(old_source / "wood.ppm").u8string()};
    process_dropped_files();
    check(g_scene.textures.size() == 1 && g_scene.textures[0].path.find((run / "empty").u8string()) == 0,
          "dragged assets are copied into the active project");
    g_dropped_files = {(old_source / "wood.ppm").u8string()};
    process_dropped_files();
    check(g_scene.textures.size() == 1, "repeated drag avoids duplicate texture assets");
    check(!fs::exists(run / "materials.txt") && !fs::exists(run / "assets_manifest.txt"),
          "project sessions never write global legacy manifests");
    check(ProjectOutputPath("LastRender.ppm").find((run / "empty").u8string()) == 0,
          "renders go into the active project folder");
    RequestProjectAction(project_ui::action::close);
    ProcessProjectActions(window);
    check(project_ui::confirm_changes && !project_ui::exit_requested,
          "closing a dirty scene requires the in-app save decision");
    project_ui::confirm_changes = false;
    project_ui::pending = project_ui::action::none;
    write(run / "empty" / "added-in-explorer.ppm", "P3\n1 1\n255\n20 30 40\n");
    RequestProjectAction(project_ui::action::refresh);
    ProcessProjectActions(window);
    check(g_scene.textures.size() == 2 && g_scene.objects.empty(),
          "refresh discovers files added in Explorer without creating objects");
    g_dropped_files = {file.u8string()};
    process_dropped_files();
    ProcessProjectActions(window);
    check(project_ui::confirm_changes && project_ui::file.filename() == "New.dusk",
          "dropping a scene respects the unsaved-changes decision");
    project_ui::confirm_changes = false;
    project_ui::discard_confirmed = true;
    ProcessProjectActions(window);
    check(project_ui::file == file, "confirmed dropped scene opens on the next frame boundary");
    check(OpenProjectFile(file), "reopen prior project without new-project assets leaking");
    check(g_scene.objects.size() == 1 && g_scene.materials[0].name == m.name,
          "prior scene and material names restored");
    check(fs::current_path() == original_cwd, "editor project transitions preserve process directory");

    auto large = empty_document();
    large.world.objects.resize(10000);
    const auto started = std::chrono::steady_clock::now();
    uint64_t sink = 0;
    for (int i = 0; i < 20; ++i) {
        large.world.objects[0].radius += .01;
        sink ^= fingerprint(inspect(large));
    }
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count() / 20;
    std::printf("BENCH project dirty check: %.3f ms for 10,000 objects (%llu)\n", ms,
                static_cast<unsigned long long>(sink));
    ReleaseProjectMeshes(g_gpu_meshes);
    ClearProjectPreviews();
    StopEditorPreviews();
    StopRasterPreview();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    std::printf("%d project persistence and editor integration checks passed\n", checks);
    return 0;
}
