static void check_firefly_reconstruction() {
    constexpr int side=9;
    std::vector<float> indirect(side*side*3,.1f), direct(indirect.size(),2.f), image(indirect.size());
    size_t center=(4*side+4)*3;
    indirect[center]=40; indirect[center+1]=20; indirect[center+2]=10;
    for(size_t i=0;i<image.size();++i) image[i]=direct[i]+indirect[i];
    auto original=image;
    dusk_fireflies::reconstruct(image,indirect,side,side,0);
    require(image==original,"disabled firefly reconstruction is exact");
    dusk_fireflies::reconstruct(image,indirect,side,side,1);
    near(dusk_fireflies::luminance(&image[center])-2,.1,2e-6,"isolated indirect firefly restored to neighbourhood energy");
    near((image[center]-2)/(image[center+1]-2),2,1e-5,"firefly suppression preserves colour ratios");
    require(image[0]==original[0] && image[center]>=2,"direct light never enters firefly filter");
    std::fill(indirect.begin(),indirect.end(),.1f);
    for(int y=2;y<=6;++y) for(int x=2;x<=6;++x) for(int c=0;c<3;++c) indirect[(y*side+x)*3+c]=100;
    image=indirect; original=image; dusk_fireflies::reconstruct(image,indirect,side,side,1);
    require(image==original,"dense bright caustic preserves every pixel including boundary");
}
#ifdef DUSK_ENABLE_VULKAN_RT
static void check_gpu_fireflies(const std::string& device,const std::filesystem::path& folder) {
    // Retain an unassisted high-variance fixture to test reconstruction itself.
    gpu_option("DUSK_GPU_NO_SPHERE_NEE","1");
    auto source=sphere_scene(scene_material_model::dielectric);
    source.materials[0].base_color=vec3(1,1,1); source.objects[0].radius=.65;
    source.objects[0].center=point3(0,0,0);
    scene_material floor; floor.model=scene_material_model::lambert; floor.base_color=vec3(.7,.65,.55);
    source.materials.push_back(floor);
    scene_object ground; ground.type=scene_object_type::cube; ground.center=point3(0,-1.1,0);
    ground.scale=vec3(10,.2,10); ground.material_index=1; source.objects.push_back(ground);
    auto cam=test_camera(192,144,256); cam.lookfrom=point3(2.7,2.4,4); cam.lookat=point3(0,-.35,0);
    cam.vfov=34; cam.background=vec3(); cam.use_sun=true; cam.sun_dir=vec3(0,1,0);
    cam.sun_radiance=vec3(2,2,2); cam.sun_angular_radius=.266; cam.max_depth=8;
    auto settings=raw_settings(); std::vector<float> raw_hdr,filtered_hdr;
    auto data=dusk_gpu::compile_scene(source);
    auto raw=dusk_gpu::render(data,cam,settings,device,nullptr,nullptr,{},&raw_hdr);
    settings.gpu_firefly_filter=true;
    auto clean=dusk_gpu::render(data,cam,settings,device,nullptr,nullptr,{},&filtered_hdr);
    require(raw_hdr==filtered_hdr,"firefly reconstruction leaves every raw transport sample unchanged");
    size_t spots=0, removed=0;
    for(int y=1;y<cam.image_height-1;++y) for(int x=1;x<cam.image_width-1;++x) {
        size_t p=(size_t(y)*cam.image_width+x)*3;
        int peak=std::max({raw.pixels[p],raw.pixels[p+1],raw.pixels[p+2]});
        if(peak>200) { ++spots; if(clean.pixels[p]+20<raw.pixels[p]) ++removed; }
    }
    std::cout<<"Sun/glass fireflies: "<<spots<<" bright pixels, "<<removed<<" isolated spikes suppressed\n";
    require(removed>=2 && removed>=spots*.6 && raw.pixels!=clean.pixels,"sunlight caustic fixture suppresses most isolated bright pixels");
    auto save=[&](const char* name,const render_result& image) {
        auto path=(folder/name).string();
        require(stbi_write_png(path.c_str(),image.width,image.height,3,image.pixels.data(),image.width*3)!=0,"save GPU firefly comparison");
    };
    save("sun-glass-fireflies-before.png",raw); save("sun-glass-fireflies-after.png",clean);
    // Exercise the compute-side preview filter at the completed sample count.
    gpu_option("DUSK_GPU_PREVIEW_MS","1");
    gpu_option("DUSK_GPU_SYNCHRONOUS","1");
    gpu_option("DUSK_GPU_BATCH_SAMPLES","4");
    std::atomic<bool> cancel{false}; render_progress_state progress; render_result preview;
    auto cancelled=dusk_gpu::render(data,cam,settings,device,&cancel,&progress,[&](const render_result& image) {
        if(progress.completed_tiles>=progress.total_tiles) { preview=image; cancel=true; }
    });
    gpu_option("DUSK_GPU_PREVIEW_MS","");
    gpu_option("DUSK_GPU_SYNCHRONOUS","");
    gpu_option("DUSK_GPU_BATCH_SAMPLES","");
    require(!preview.pixels.empty(),"filtered preview delivered at completed sample count");
    int error=0;
    for(size_t i=0;i<preview.pixels.size();++i) error=std::max(error,std::abs(int(preview.pixels[i])-clean.pixels[i]));
    require(error<=1 && cancelled.pixels==preview.pixels,"GPU preview firefly filter agrees with final reconstruction within one byte");
    save("sun-glass-filtered-preview.png",preview);
    // A broader source converges into a dense caustic footprint. Reconstruction
    // must retain that footprint rather than removing indirect lighting wholesale.
    cam.sun_angular_radius=8; cam.samples_per_pixel=2048;
    settings.gpu_firefly_filter=false;
    auto dense=dusk_gpu::render(data,cam,settings,device,nullptr,nullptr,{});
    settings.gpu_firefly_filter=true;
    auto dense_clean=dusk_gpu::render(data,cam,settings,device,nullptr,nullptr,{});
    size_t changed=0; double energy=0,clean_energy=0;
    for(size_t i=0;i<dense.pixels.size();++i) {
        changed+=std::abs(int(dense.pixels[i])-dense_clean.pixels[i])>1;
        energy+=dense.pixels[i]; clean_energy+=dense_clean.pixels[i];
    }
    std::cout<<"Dense caustic reconstruction: "<<changed<<" changed channels, "<<clean_energy/energy*100<<"% display energy retained\n";
    require(clean_energy>=energy*.98 && changed<dense.pixels.size()*.03,"converged caustic footprint is preserved");
    save("dense-caustic-before.png",dense); save("dense-caustic-after.png",dense_clean);
    gpu_option("DUSK_GPU_NO_SPHERE_NEE","");
    // The same isolated bright sun viewed through a specular-only glass chain
    // is a legitimate highlight and must not be reconstructed away.
    source.objects.pop_back(); source.materials[0].ior=1;
    cam=test_camera(8,8,8); cam.vfov=.02; cam.background=vec3(); cam.use_sun=true; cam.sun_dir=vec3(0,0,-1);
    cam.sun_radiance=vec3(1,1,1)*cam.sun_solid_angle();
    settings.gpu_firefly_filter=false;
    auto highlight=dusk_gpu::render(dusk_gpu::compile_scene(source),cam,settings,device,nullptr,nullptr,{});
    settings.gpu_firefly_filter=true;
    auto preserved=dusk_gpu::render(dusk_gpu::compile_scene(source),cam,settings,device,nullptr,nullptr,{});
    require(highlight.pixels==preserved.pixels,"camera-visible glass and sun highlights remain exact");
    // A quiet sun/glass scene must not stop at the old 16-sample minimum.
    settings.adaptive_sampling=true; cam.samples_per_pixel=128; std::vector<float> adaptive,reference;
    dusk_gpu::render(dusk_gpu::compile_scene(source),cam,settings,device,nullptr,nullptr,{},&adaptive);
    settings.adaptive_sampling=false;
    dusk_gpu::render(dusk_gpu::compile_scene(source),cam,settings,device,nullptr,nullptr,{},&reference);
    require(adaptive==reference,"sun/glass adaptive floor retains all samples below 256 spp");
}
#endif
