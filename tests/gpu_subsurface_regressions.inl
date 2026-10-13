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
    material.sss_radius_rgb=vec3(1,.372,.186);
    compare_render(source,cam,device,"GPU RGB skin diffusion backlighting",3);
    render_hdr();
    require(channel_mean(hdr,0)>channel_mean(hdr,2)*5,"skin transports red farther than blue through the sphere");
    cam.background=vec3(1,1,1); cam.use_sun=false;
    compare_render(source,cam,device,"GPU RGB diffusion white furnace",3);
    render_hdr();
    for(int channel=0;channel<3;++channel) require(channel_mean(hdr,channel)<1.01,"each RGB skin diffusion channel has a bounded energy budget");
    cam.background=vec3(); cam.use_sun=true;
    material.sss_radius_rgb=vec3(1,1,1);
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
static void check_gpu_skin_shadow(const std::string& device,const std::filesystem::path& folder) {
    scene source;
    scene_material skin; skin.model=scene_material_model::pbr; skin.base_color=vec3(.7,.7,.7);
    skin.use_sss=true; skin.sss_strength=1; skin.sss_radius=.15; skin.roughness=.8;
    source.materials.push_back(skin);
    scene_material black; black.model=scene_material_model::lambert; black.base_color=vec3(); source.materials.push_back(black);
    scene_object slab; slab.type=scene_object_type::cube; slab.material_index=0; slab.scale=vec3(5,5,.3); source.objects.push_back(slab);
    scene_object blocker=slab; blocker.material_index=1; blocker.center=point3(-1.25,0,2); blocker.scale=vec3(2.5,5,.1); source.objects.push_back(blocker);
    auto cam=test_camera(128,96,1024); cam.lookfrom=point3(0,0,1); cam.lookat=point3(); cam.vfov=60; cam.max_depth=2;
    cam.background=vec3(); cam.use_sun=true; cam.sun_dir=vec3(0,0,1); cam.sun_radiance=vec3(2,2,2); cam.sun_angular_radius=.266; cam.sun_shadow_samples=1;
    auto settings=raw_settings(); std::vector<float> hdr;
    auto run=[&] {return dusk_gpu::render(dusk_gpu::compile_scene(source),cam,settings,device,nullptr,nullptr,{},&hdr);};
    auto save=[&](const char* name,const render_result& image) {
        auto file=(folder/name).string(); require(stbi_write_png(file.c_str(),image.width,image.height,3,image.pixels.data(),image.width*3)!=0,"save skin shadow-boundary comparison");
    };
    auto neutral=run(); save("skin-shadow-neutral.png",neutral);
    source.materials[0].sss_profile=1; source.materials[0].sss_radius_rgb=vec3(1,.372,.186);
    auto image=run(); save("skin-shadow-profile.png",image);
    double red=0,green=0,blue=0;
    // The blocker covers the left half; integrate a strip just inside shadow.
    for(int y=32;y<64;++y) for(int x=48;x<60;++x) {size_t p=(y*128+x)*3;red+=hdr[p];green+=hdr[p+1];blue+=hdr[p+2];}
    std::cout<<"Skin shadow strip RGB: "<<red<<", "<<green<<", "<<blue<<'\n';
    require(red>green*1.3 && green>blue*1.3 && red>1,"skin diffusion carries a warm tail across an external shadow boundary");
    source.materials[0].sss_strength=0; run();
    double opaque=0;
    for(int y=32;y<64;++y) for(int x=48;x<60;++x) opaque+=hdr[(y*128+x)*3];
    require(opaque<red*.05,"the skin shadow transition comes from spatial scattering");
    cam.image_width=32; cam.image_height=24; cam.samples_per_pixel=8192;
    source.materials[0].sss_strength=1;
    compare_render(source,cam,device,"CPU/GPU RGB skin shadow boundary",3);
}
// Unreal Subsurface and Subsurface Profile must match the CPU tracer.
static void check_gpu_unreal_subsurface(const std::string& device,const std::filesystem::path& folder) {
    auto source=sphere_scene(scene_material_model::pbr);
    auto& material=source.materials[0]; material.base_color=vec3(.6,.45,.4); material.roughness=.45;
    material.use_sss=true; material.sss_model=SSS_DIPOLE_BURLEY; material.sss_shading=sss_shading_unreal_subsurface;
    material.sss_subsurface_color=vec3(.9,.35,.25); material.sss_opacity=.35; material.sss_transmission_distance=.6;
    auto cam=test_camera(24,24,2048); cam.vfov=34; cam.max_depth=3; cam.background=vec3();
    cam.use_sun=true; cam.sun_dir=safe_unit_vector(vec3(-.6,.3,-1)); cam.sun_radiance=vec3(2,2,2);
    cam.sun_angular_radius=5; cam.sun_shadow_samples=1;
    auto settings=raw_settings(); std::vector<float> hdr;
    auto render_hdr=[&] {
        return dusk_gpu::render(dusk_gpu::compile_scene(source),cam,settings,device,nullptr,nullptr,{},&hdr);
    };
    compare_render(source,cam,device,"GPU Unreal Subsurface backlit transmission",2);
    source.materials[0].sss_opacity=0;
    compare_render(source,cam,device,"GPU Unreal Subsurface opacity-zero wrap",2);
    source.materials[0].sss_opacity=.35;
    // Transmission is counted once, by light sampling, whatever the sample count.
    cam.max_depth=1; render_hdr(); double reference=channel_mean(hdr,0);
    require(reference>.005,"GPU backlight transmits through the body");
    cam.direct_light_samples=4; cam.sun_shadow_samples=4; render_hdr();
    near(channel_mean(hdr,0),reference,reference*.03,"GPU Unreal transmission is invariant to light samples");
    cam.direct_light_samples=1; cam.sun_shadow_samples=1; cam.max_depth=3;
    // Indirect light sees at most unit albedo in a white furnace.
    cam.use_sun=false; cam.background=vec3(1,1,1); render_hdr();
    require(channel_mean(hdr,0)<1.01 && channel_mean(hdr,1)<1.01,"GPU Unreal Subsurface ambient keeps unit albedo");
    compare_render(source,cam,device,"GPU Unreal Subsurface white furnace",2);
    cam.use_sun=true; cam.background=vec3();
    scene_material black; black.model=scene_material_model::lambert; black.base_color=vec3();
    source.materials.push_back(black);
    scene_object blocker; blocker.type=scene_object_type::cube; blocker.center=point3(0,0,-2.5);
    blocker.scale=vec3(100,100,.1); blocker.material_index=1; source.objects.push_back(blocker);
    cam.max_depth=1; render_hdr(); double blocked=channel_mean(hdr,0);
    source.objects.pop_back(); render_hdr();
    require(blocked<channel_mean(hdr,0)*.5,"an external blocker removes transmitted sunlight on the GPU");
    cam.max_depth=3; source.materials.pop_back();
    cam.use_sun=false;
    camera::point_light point; point.position=point3(.4,.3,-3); point.radiance=colour(20,20,20);
    cam.point_lights.push_back(point);
    compare_render(source,cam,device,"GPU Unreal Subsurface point-light transmission",2);
    render_hdr(); std::cout<<"Unreal point-light transmission mean red "<<channel_mean(hdr,0)<<'\n';
    require(channel_mean(hdr,0)>.005,"GPU point light transmits through the body");
    cam.point_lights.clear(); cam.use_sun=true;
    auto unreal=render_hdr(); auto save=[&](const char* name,const render_result& image) {
        auto path=(folder/name).string();
        require(stbi_write_png(path.c_str(),image.width,image.height,3,image.pixels.data(),image.width*3)!=0,"save GPU Unreal subsurface fixture");
    };
    save("unreal-subsurface-gpu.png",unreal);

    auto& profile=source.materials[0];
    profile.sss_shading=sss_shading_unreal_profile; profile.sss_strength=1; profile.sss_units_per_metre=45;
    profile.sss_eta=1.4;
    cam.sun_dir=safe_unit_vector(vec3(-.7,.5,.5));
    compare_render(source,cam,device,"GPU Subsurface Profile skin diffusion",3);
    save("subsurface-profile-gpu.png",render_hdr());
    // Dual specular alone, without diffusion, also matches.
    profile.sss_strength=0; profile.roughness=.3;
    compare_render(source,cam,device,"GPU Subsurface Profile dual specular",2);
    profile.sss_lobe_mix=0; render_hdr(); auto single=hdr;
    profile.use_sss=false; render_hdr();
    require(hdr==single,"zero lobe mix reproduces the ordinary specular lobe exactly");
}
#endif
