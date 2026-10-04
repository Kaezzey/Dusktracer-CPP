#ifdef DUSK_ENABLE_VULKAN_RT
static void check_gpu_subsurface(const std::string& device,const std::filesystem::path& folder) {
    auto source=sphere_scene(scene_material_model::pbr);
    auto& material=source.materials[0]; material.base_color=vec3(.7,.4,.2);
    material.use_sss=true; material.sss_model=SSS_DIPOLE_BURLEY; material.sss_strength=1;
    material.sss_radius=.5; material.sss_eta=1.3; material.roughness=.7;
    auto cam=test_camera(16,16,4096); cam.vfov=5; cam.max_depth=2;
    auto settings=raw_settings(); std::vector<float> hdr;
    auto render_hdr=[&] {
        return dusk_gpu::render(dusk_gpu::compile_scene(source),cam,settings,device,nullptr,nullptr,{},&hdr);
    };
    auto save=[&](const char* name,const render_result& image) {
        auto path=(folder/name).string();
        require(stbi_write_png(path.c_str(),image.width,image.height,3,image.pixels.data(),image.width*3)!=0,"save GPU subsurface comparison");
    };
    compare_render(source,cam,device,"GPU SSS indirect white furnace",3);
    render_hdr(); double furnace=channel_mean(hdr,0);
    require(furnace>.2 && furnace<1.01,"GPU SSS uses a bounded diffuse energy budget");
    for(double fraction:{.25,.5}) {
        material.sss_strength=fraction;
        compare_render(source,cam,device,"GPU SSS partial diffuse replacement",3);
    }
    material.sss_strength=1;
    cam.background=vec3(); cam.use_sun=true; cam.sun_dir=vec3(0,0,-1);
    cam.sun_radiance=vec3(1,1,1); cam.sun_angular_radius=10; cam.sun_shadow_samples=1;
    compare_render(source,cam,device,"GPU SSS backlit sphere",2);
    auto backlit=render_hdr(); double transmission=channel_mean(hdr,0);
    require(transmission>.002,"GPU SSS carries backlighting from a different surface point");
    material.sss_strength=0; render_hdr();
    near(channel_mean(hdr,0),0,1e-6,"ordinary opaque sphere has no backlight without SSS");
    material.sss_strength=1;
    scene_material black; black.model=scene_material_model::lambert; black.base_color=vec3();
    source.materials.push_back(black);
    scene_object blocker; blocker.type=scene_object_type::cube; blocker.center=point3(0,0,-2);
    blocker.scale=vec3(100,100,.1); blocker.material_index=1; source.objects.push_back(blocker);
    render_hdr(); near(channel_mean(hdr,0),0,1e-6,"external shadow blocker occludes GPU SSS entry lighting");
    source.objects.pop_back();
    // Reacquire after material vector insertion; tests also exercise record changes
    // in the retained device without rebuilding geometry.
    auto& sss_material=source.materials[0];
    sss_material.sss_color_override_enabled=true; sss_material.sss_color_override_color=vec3(.1,.8,.2);
    compare_render(source,cam,device,"GPU SSS override tint",2);
    render_hdr(); require(channel_mean(hdr,1)>channel_mean(hdr,0)*4,"SSS tint is independent of surface colour and applied once");
    sss_material.sss_color_override_enabled=false; sss_material.sss_eta=1;
    compare_render(source,cam,device,"GPU SSS unit IOR entry",2);
    sss_material.sss_eta=1.8; compare_render(source,cam,device,"GPU SSS higher IOR entry",2);
    source.objects[0].rotation_deg=vec3(17,29,11); source.objects[0].scale=vec3(1.3,.8,1.1);
    compare_render(source,cam,device,"GPU SSS transformed ellipsoid",2);
    auto neighbor=source.objects[0]; neighbor.center=point3(2.5,0,0); source.objects.push_back(neighbor);
    compare_render(source,cam,device,"GPU SSS same-material neighbouring instance",2);
    source.objects.pop_back(); source.objects[0].rotation_deg=vec3(); source.objects[0].scale=vec3(1,1,1);
    // Emissive-area MIS at the spatial entry must agree with continuation-only
    // sampling; otherwise SSS lights are either counted twice or lost.
    scene_material emitter; emitter.model=scene_material_model::diffuse_light;
    emitter.emission=vec3(2,2,2); source.materials.push_back(emitter);
    scene_object light; light.type=scene_object_type::cube; light.material_index=2;
    light.center=point3(0,0,-3); light.scale=vec3(4,4,.1); source.objects.push_back(light);
    cam.use_sun=false; cam.direct_light_samples=0;
    render_hdr(); double continuation=channel_mean(hdr,0);
    require(continuation>.002,"GPU SSS indirect paths reach emissive geometry");
    for(int samples:{1,4}) {
        cam.direct_light_samples=samples; render_hdr();
        near(channel_mean(hdr,0),continuation,.015,"SSS area-light MIS is invariant to direct sample count");
    }
    cam.enable_mis=false; cam.direct_light_samples=1; render_hdr();
    near(channel_mean(hdr,0),continuation,.015,"SSS light-only estimator does not double-count emitter hits");
    cam.enable_mis=true;
    compare_render(source,cam,device,"GPU SSS area-light transport",3);
    source.objects.pop_back(); cam.use_sun=true; cam.sun_dir=vec3(.4,.5,-1); cam.sun_dir=unit_vector(cam.sun_dir);
    cam.image_width=160; cam.image_height=160; cam.vfov=36; cam.samples_per_pixel=512; cam.max_depth=5;
    auto on=render_hdr(); save("subsurface-gpu-on.png",on);
    source.materials[0].sss_strength=0; auto off=render_hdr(); save("subsurface-gpu-off.png",off);
    require(on.pixels!=off.pixels,"GPU spatial subsurface visibly changes the material");
    source.materials[0].use_sss=false; auto disabled=render_hdr(); auto reference=hdr;
    source.materials[0].use_sss=true; source.materials[0].sss_strength=1; source.materials[0].sss_model=SSS_NONE;
    render_hdr(); require(hdr==reference,"None mode exactly disables SSS on GPU");
    source.materials[0].sss_model=SSS_DIPOLE_BURLEY; source.materials[0].metallic=1;
    render_hdr(); reference=hdr;
    source.materials[0].use_sss=false; render_hdr(); require(hdr==reference,"full conductor has no subsurface substrate");
    // Legacy model identifiers follow the CPU's normalized diffusion mapping.
    source.materials[0].metallic=0; source.materials[0].use_sss=true; cam.samples_per_pixel=16;
    for(int legacy:{1,2,3,4,5}) {
        source.materials[0].sss_model=legacy; render_hdr();
        if(legacy==1) reference=hdr; else require(hdr==reference,"legacy nonzero SSS IDs share Burley transport on GPU");
    }
    std::string diagnostic;
    source.materials[0].use_sss=true; source.materials[0].metallic=0;
    cam.image_width=8; cam.image_height=8; cam.samples_per_pixel=96;
    render_hdr(); reference=hdr;
    settings.adaptive_sampling=true; render_hdr();
    require(hdr==reference,"GPU spatial SSS retains all samples below its adaptive minimum");
    settings.adaptive_sampling=false;
    cam.image_width=8; cam.image_height=8; cam.samples_per_pixel=8;
    render_selected({render_backend::vulkan,device},source,nullptr,cam,settings,nullptr,nullptr,{},diagnostic);
    require(diagnostic.find("using CPU")==std::string::npos,"SSS editor render stays on selected GPU without fallback");
}
#endif
