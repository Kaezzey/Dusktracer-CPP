// Included in the hardware tests after the shared scene and camera fixtures.
static void gpu_option(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    if (*value) setenv(name, value, 1); else unsetenv(name);
#endif
}
static void check_gpu_cache(const std::string& device) {
    auto source = sphere_scene(scene_material_model::pbr);
    source.objects[0].type = scene_object_type::cube;
    auto data = dusk_gpu::compile_scene(source);
    auto cam = test_camera(24, 24, 8);
    cam.use_sun = true;
    auto settings = raw_settings();
    dusk_gpu::render_statistics stats;
    std::vector<float> first, actual;
    auto run = [&](std::vector<float>& output) {
        dusk_gpu::render(data,cam,settings,device,nullptr,nullptr,{},&output,{},&stats);
    };
    auto matches_cold = [&] {
        const auto warm = actual;
        dusk_gpu::clear_cache();
        run(actual);
        require(actual == warm, "cached render matches a completely fresh device and scene");
    };
    dusk_gpu::clear_cache();
    run(first);
    require(!stats.reused_device && stats.blas_builds > 0 && stats.tlas_builds == 1,
            "cold render builds device and acceleration structures");
    run(actual);
    require(first == actual && stats.reused_device && stats.reused_pipeline && !stats.uploaded_bytes &&
                !stats.blas_builds && !stats.tlas_builds,
            "identical render reuses all scene resources and resets accumulation");
    cam.lookfrom += vec3(.2,.1,0);
    run(actual);
    require(stats.uploaded_bytes == 208 && !stats.blas_builds && !stats.tlas_builds,
            "camera edits upload only camera configuration");
    matches_cold();
    data.materials[0].base.x = .85f;
    run(actual);
    require(actual != first && stats.uploaded_bytes == data.materials.size()*sizeof(dusk_gpu::material_record) &&
                !stats.blas_builds && !stats.tlas_builds, "material edits retain geometry and acceleration structures");
    matches_cold();
    data.instances[0].rows[0].w += .3f;
    run(actual);
    require(stats.blas_builds == 0 && stats.tlas_builds == 1, "instance transforms retain mesh BLASes");
    matches_cold();
    data.vertices[0].position.x += .1f;
    run(actual);
    require(stats.blas_builds > 0 && stats.tlas_builds == 1, "geometry edits rebuild acceleration structures");
    matches_cold();
    data.materials[0].alpha.z = 0;
    run(actual);
    require(stats.blas_builds == 0 && stats.tlas_builds == 1,
            "opacity classification changes refresh instance masks without rebuilding geometry");
    matches_cold();
    cam.image_width = 31;
    cam.image_height = 19;
    run(actual);
    require(actual.size() == 31*19*3 && !stats.blas_builds && !stats.tlas_builds,
            "resolution changes resize outputs without rebuilding the scene");
    matches_cold();
}
static void check_shadow_specialisation(const std::string& device) {
    auto source = sphere_scene(scene_material_model::pbr);
    source.objects[0].type = scene_object_type::cube;
    source.objects[0].scale = vec3(4,.1,4);
    source.objects[0].translation = vec3(0,-1,0);
    scene_object sphere;
    sphere.material_index = 0;
    sphere.radius = .5;
    source.objects.push_back(sphere);
    auto cam = test_camera(64,48,32);
    cam.lookfrom = vec3(2,2,5);
    cam.use_sun = true;
    cam.sun_dir = unit_vector(vec3(-1,2,1));
    cam.vfov = 35;
    auto settings = raw_settings();
    for (bool transparent : {false,true}) {
        if (transparent) {
            auto material = source.materials[0];
            initialize_material_graph(material,source.textures);
            int scalar = material.graph.add(graph_kind::scalar,0,0);
            material.graph.find(scalar)->value = vec3(.35,0,0);
            material.graph.connect({scalar,0,material.graph.output_id(),4});
            material.alpha_cutoff = 0;
            source.materials.push_back(material);
            source.objects.back().material_index = 1;
        }
        std::vector<float> reference, specialised;
        gpu_option("DUSK_GPU_REFERENCE","1");
        dusk_gpu::render(dusk_gpu::compile_scene(source),cam,settings,device,nullptr,nullptr,{},&reference);
        gpu_option("DUSK_GPU_REFERENCE","");
        dusk_gpu::render(dusk_gpu::compile_scene(source),cam,settings,device,nullptr,nullptr,{},&specialised);
        require(reference == specialised, transparent ? "mixed opaque/partial shadows retain exact HDR transport"
                                                     : "opaque early-out shadows retain exact HDR transport");
    }
}
static void check_compact_preview(const std::string& device) {
    auto source = sphere_scene(scene_material_model::diffuse_light);
    source.materials[0].emission = vec3(.3,.8,2);
    auto cam = test_camera(32,32,1);
    auto settings = raw_settings();
    settings.exposure = 1.7;
    const auto expected = dusk_gpu::render(dusk_gpu::compile_scene(source),cam,settings,device,nullptr,nullptr,{});
    cam.samples_per_pixel = 10000000;
    std::atomic<bool> cancel{false};
    dusk_gpu::render_statistics stats;
    render_result preview;
    gpu_option("DUSK_GPU_PREVIEW_MS","1");
    const auto partial = dusk_gpu::render(dusk_gpu::compile_scene(source),cam,settings,device,&cancel,nullptr,
        [&](const render_result& image) { preview = image; cancel = true; },nullptr,{},&stats);
    gpu_option("DUSK_GPU_PREVIEW_MS","");
    require(!preview.pixels.empty() && partial.pixels == preview.pixels, "compact preview supports cancellation");
    int max_error = 0;
    for (size_t i = 0; i < preview.pixels.size(); ++i) max_error = std::max(max_error,std::abs(int(preview.pixels[i])-expected.pixels[i]));
    require(max_error <= 1, "GPU preview tone mapping matches final CPU resolve within one display level");
    require(stats.preview_readback_bytes == 2*32*32*4 && stats.final_readback_bytes == 0,
            "progressive and cancelled results transfer four bytes per pixel without HDR guides");
}
static void check_material_optimisation() {
    auto source = sphere_scene(scene_material_model::pbr);
    auto& m = source.materials[0];
    initialize_material_graph(m, source.textures);
    auto& graph = m.graph;
    int scalar = graph.add(graph_kind::scalar,0,0);
    graph.find(scalar)->value = vec3(.8,0,0);
    int last = scalar;
    for (int i = 0; i < 24; ++i) {
        int op = graph.add(graph_kind::multiply,0,0);
        graph.find(op)->defaults[1] = .99;
        graph.connect({last,0,op,0}); last = op;
    }
    graph.connect({last,0,graph.output_id(),1});
    graph.connect({last,0,graph.output_id(),2});
    auto data = dusk_gpu::compile_scene(source);
    const auto& material = data.materials[0];
    require(material.programs[1].y == 1 && material.programs[2].x == material.programs[1].x &&
                data.instructions.size() == 1, "constant branches fold and identical output programs share bytecode");
    require(material.alpha.z == 1, "constant PBR surface is proven opaque");
    graph.connect({scalar,0,graph.output_id(),4});
    data = dusk_gpu::compile_scene(source);
    require(data.materials[0].alpha.z == 0, "partial opacity never enters the opaque shadow path");
}
static void benchmark_gpu_optimisations(const std::string& device) {
    auto source = sphere_scene(scene_material_model::pbr);
    auto cam = test_camera(512,384,128);
    cam.use_sun = true;
    cam.sun_dir = unit_vector(vec3(-1,2,2));
    cam.max_depth = 8;
    auto settings = raw_settings();
    const auto data = dusk_gpu::compile_scene(source);
    std::vector<float> reference, result;
    auto run = [&](const char* label, const char* sync, const char* batch, std::vector<float>& output) {
        gpu_option("DUSK_GPU_SYNCHRONOUS",sync);
        gpu_option("DUSK_GPU_BATCH_SAMPLES",batch);
        dusk_gpu::render(data,cam,settings,device,nullptr,nullptr,{}); // warm shaders and clocks
        double sum = 0;
        dusk_gpu::render_statistics stats;
        for (int i = 0; i < 3; ++i) {
            auto start = std::chrono::steady_clock::now();
            dusk_gpu::render(data,cam,settings,device,nullptr,nullptr,{},&output,{},&stats);
            sum += std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        }
        std::printf("GPU BENCH %s: %.2f ms, %llu submissions, %llu bytes uploaded (512x384, 128 spp, 3 warm runs)\n",
                    label,sum/3,static_cast<unsigned long long>(stats.submissions),static_cast<unsigned long long>(stats.uploaded_bytes));
    };
    gpu_option("DUSK_GPU_TILE_GROUP","1");
    run("one tile/four samples","","4",reference);
    gpu_option("DUSK_GPU_TILE_GROUP","4");
    run("four tiles/four samples","","4",result);
    require(reference==result,"tile grouping alone preserves every HDR pixel");
    gpu_option("DUSK_GPU_TILE_GROUP","1");
    run("one tile/queued/adaptive","","",reference);
    gpu_option("DUSK_GPU_TILE_GROUP","4");
    run("four tiles/queued/adaptive","","",result);
    require(reference == result,"queued/adaptive scheduling preserves every HDR pixel across multiple tiles");
    gpu_option("DUSK_GPU_SYNCHRONOUS","");
    gpu_option("DUSK_GPU_BATCH_SAMPLES","");
    cam.image_width=513; cam.image_height=289; cam.samples_per_pixel=7;
    gpu_option("DUSK_GPU_TILE_GROUP","1");
    dusk_gpu::render(data,cam,settings,device,nullptr,nullptr,{},&reference);
    gpu_option("DUSK_GPU_TILE_GROUP","4");
    render_progress_state progress;
    dusk_gpu::render(data,cam,settings,device,nullptr,&progress,{},&result);
    require(reference==result,"grouped tiles preserve trailing pixels and partial sample batches");
    require(progress.completed_tiles==progress.total_tiles,"grouped dispatch counts all completed tile samples");
    gpu_option("DUSK_GPU_TILE_GROUP","");
}
