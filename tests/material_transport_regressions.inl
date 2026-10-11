// Independent transport regressions for sunlight and spatial diffusion.
static double trace_mean(const camera& cam, const hittable& world, const ray& r, int depth, int n) {
    double sum = 0;
    for (int i = 0; i < n; ++i) sum += cam.ray_colour(r,depth,world).x();
    return sum/n;
}
static void check_dielectric_sun() {
    camera sun; sun.background = colour(); sun.use_sun = true;
    sun.sun_dir = vec3(0,0,1); sun.sun_radiance = colour(1,1,1);
    sun.sun_angular_radius = 0;
    require(sun.sun_pdf(vec3(0,0,1)) > 0,"legacy zero-radius sunlight has a finite radiance disc");
    double physical_omega = 4*pi*std::pow(std::sin(degrees_to_radians(.266)/2),2);
    near(sun.sun_solid_angle(),physical_omega,1e-14,"legacy sun uses the physical angular radius");
    auto glass = std::make_shared<dielectric>(1.0);
    hittable_list world;
    world.add(std::make_shared<sphere>(point3(0,0,2),.5,glass));
    ray toward_sun(point3(),vec3(0,0,1),.4);
    near(sun.ray_colour(toward_sun,4,world).x(),1/physical_omega,1e-8,
         "sunlight reaches the camera through dielectric entry and exit");
    sun.enable_mnee = true; sun.mnee_has_sphere = true;
    sun.mnee_sphere_center = point3(0,0,2); sun.mnee_sphere_radius = .5;
    sun.caustics.insert({point3(),vec3(0,0,-1),colour(1000,1000,1000)});
    near(sun.ray_colour(toward_sun,4,world).x(),1/physical_omega,1e-8,
         "stored experimental settings cannot add light to normal transport");

    // A distant emissive sphere and an environment disc subtend the same cone.
    // Their radiance must agree after a diffuse -> glass -> glass light path.
    sun.sun_angular_radius = 20; sun.enable_mnee = false;
    glass = std::make_shared<dielectric>(1.5);
    world.clear();
    world.add(std::make_shared<quad>(point3(-5,-5,0),vec3(10,0,0),vec3(0,10,0),
                                   std::make_shared<lambertian>(colour(.8,.8,.8))));
    world.add(std::make_shared<sphere>(point3(0,0,1),.5,glass));
    ray receiver(point3(0,0,.1),vec3(0,0,-1),.4);
    sun.sun_shadow_samples = 1;
    double sunlight = trace_mean(sun,world,receiver,5,160000);
    camera area = sun; area.use_sun = false;
    double distance = 100000, radius = distance*std::sin(degrees_to_radians(20));
    auto emitter = std::make_shared<sphere>(point3(0,0,distance),radius,
        std::make_shared<diffuse_light>(colour(1,1,1)/sun.sun_solid_angle()));
    world.add(emitter);
    double emissive = trace_mean(area,world,receiver,5,160000);
    std::cout << "GLASS caustic sun " << sunlight << " emissive " << emissive << '\n';
    require(sunlight > .1,"a sunlight caustic arrives through the refracting sphere");
    near(sunlight,emissive,.025,"sun and equal-radiance emissive discs agree through glass");
    sun.enable_mnee = true; sun.mnee_has_sphere = true;
    world.objects.pop_back();
    near(trace_mean(sun,world,receiver,5,80000),sunlight,.03,
         "old MNEE and photon settings do not boost diffuse caustic receivers");
    sun.sun_spheres.push_back({point3(0,0,1),.5,1.5});
    near(trace_mean(sun,world,receiver,5,24000),sunlight,.025,
         "sphere manifold sampling agrees with independent backward path tracing");
    double targeted=trace_mean(sun,world,receiver,5,24000);
    sun.sun_shadow_samples=4;
    near(trace_mean(sun,world,receiver,5,12000),targeted,.015,"sphere caustic energy is invariant to sun sample count");
    // Check the solid-angle focusing factor against a numerical 3D map,
    // independently of the analytic angular derivative and root solver.
    for(double ratio:{1.05,1.3,1.7,3.0}) {
        sun_sphere::lens lens{point3(0,0,ratio),1,1.5};
        sun_sphere::map mapping(ratio,1.5);
        for(double theta:{.02,.1,.2}) {
            auto c=sun_sphere::connect(lens,point3(),vec3(0,0,1),vec3(1,0,0),theta);
            near(dot(c.outgoing,vec3(1,0,0)),std::sin(mapping.angle(theta)),1e-9,"spherical Snell mapping matches analytic angle");
            double eps=1e-5;
            auto a=sun_sphere::connect(lens,point3(),vec3(0,0,1),vec3(1,0,0),theta+eps);
            auto b=sun_sphere::connect(lens,point3(),vec3(0,0,1),vec3(std::cos(eps),std::sin(eps),0),theta);
            double measured=std::abs(dot(c.outgoing,cross((a.outgoing-c.outgoing)/eps,(b.outgoing-c.outgoing)/eps)))/std::sin(theta);
            near(measured,c.jacobian,1e-3,"sphere solid-angle Jacobian matches independent finite differences");
            std::array<double,3> roots;
            int found=mapping.roots(mapping.angle(theta),roots);
            require(found>0,"sphere solver finds transmitted manifold branches");
            bool recovered=false;
            for(int i=0;i<found;++i) {near(mapping.angle(roots[i]),mapping.angle(theta),1e-7,"each sphere branch reaches the requested light direction"); recovered|=std::abs(roots[i]-theta)<1e-6;}
            require(recovered,"sphere solver recovers the known branch even across caustic folds");
        }
    }
    std::array<double,3> folded;
    require(sun_sphere::map(1.1,1.5).roots(.01,folded)==3,"sphere caustic solver enumerates all three folded branches");
}
static void check_subsurface_transport() {
    // Independent midpoint quadrature of the profile and its radial moments.
    double mass = 0, mean = 0;
    const int steps = 200000; const double d = .3, dr = 40*d/steps;
    for (int i = 0; i < steps; ++i) {
        double r = (i+.5)*dr, density = 2*pi*r*sss::burley_Rd(r,.7,d);
        mass += density*dr; mean += r*density*dr;
    }
    near(mass,.7,2e-6,"Burley spatial profile integrates to its albedo");
    near(mean/.7,2.5*d,3e-5,"Burley radial mean follows the independent analytic moment");
    double sampled_mean = 0;
    for (int i = 0; i < steps; ++i) sampled_mean += sss::sample_burley_radius(d,(i+.5)/steps);
    near(sampled_mean/steps,2.5*d,1e-5,"Burley sampler has the correct radial distribution");
    for (double eta : {1.0,1.3,1.8,2.5}) {
        double integral = 0;
        for (int i = 0; i < 10000; ++i) {
            double mu = (i+.5)/10000;
            integral += 2*pi*mu*sss::entry_directional(mu,eta)/10000;
        }
        near(integral,1,2e-5,"subsurface entry direction integrates to unit energy");
    }
    require(std::abs(sss::entry_directional(.1,1)-sss::entry_directional(.1,1.5)) > .05,
            "SSS IOR changes the angular entry response");
    auto mat = std::make_shared<pbr_material>(colour(.7,.4,.2),0,.7);
    mat->sss_model = SSS_DIPOLE_BURLEY; mat->sss_radius = .05;
    auto plane = std::make_shared<quad>(point3(-50,-50,0),vec3(100,0,0),vec3(0,100,0),mat);
    hittable_list world; world.add(plane);
    camera cam; cam.background = colour(1,1,1);
    ray view(point3(0,0,1),vec3(0,0,-1),.6);
    double baseline = trace_mean(cam,world,view,1,60000);
    for (double strength : {.25,.5,1.0}) {
        mat->sss_strength = strength;
        double energy = trace_mean(cam,world,view,1,80000);
        std::cout << "SSS furnace strength " << strength << " radiance " << energy << '\n';
        require(energy > .3 && energy < 1.01,"SSS uses the diffuse energy budget in the white furnace");
        require(std::abs(energy-baseline) < .12,"SSS strength blends transport instead of adding a full lobe");
    }
    hit_record hit; require(plane->hit(view,interval(.001,infinity),hit),"SSS plane fixture intersects");
    hit_record entry; colour weight;
    require(mat->sample_subsurface(hit,world,.6,.1,.2,.3,.4,entry,weight),"SSS samples a spatial entry point");
    require((entry.p-hit.p).length() > .001,"SSS transports light between distinct surface points");
    require(entry.object == hit.object && entry.front_face,"entry remains on the original object with outward normal");
    near(weight.x()/weight.y(),.7/.4,1e-9,"scatter tint is applied once, without a second luminance factor");
    // A separate object sharing the same material must never become an entry.
    auto neighbor = std::make_shared<quad>(point3(-50,-50,.1),vec3(100,0,0),vec3(0,100,0),mat);
    world.add(neighbor);
    require(mat->sample_subsurface(hit,world,.6,.1,.2,.3,.99,entry,weight) && entry.object == plane.get(),
            "diffusion cannot jump to a neighbor that shares its material");
    mat->sss_strength = 0;
    bsdf_sample sample;
    require(mat->sample_surface(view,hit,.99,.3,.4,sample) && !sample.subsurface,
            "disabled SSS preserves ordinary BSDF sampling");
    mat->sss_strength = 1; mat->metallic_tex = std::make_shared<solid_colour>(colour(1,1,1));
    require(mat->sample_surface(view,hit,.99,.3,.4,sample) && !sample.subsurface,
            "a conductor has no subsurface substrate");

    // SSS must use complementary MIS at the sampled entry, including when
    // only continuation rays can reach an emissive surface.
    mat->metallic_tex = std::make_shared<solid_colour>(colour(0,0,0));
    world.clear(); world.add(plane);
    auto area_light = std::make_shared<quad>(point3(-2,-2,2),vec3(4,0,0),vec3(0,4,0),
                                           std::make_shared<diffuse_light>(colour(2,2,2)));
    world.add(area_light); cam.background = colour();
    cam.set_emissive_surfaces({{area_light,16,colour(2,2,2)}});
    cam.direct_light_samples = 0;
    double sss_reference = trace_mean(cam,world,view,1,100000);
    for (int count : {1,4}) {
        cam.direct_light_samples = count;
        near(trace_mean(cam,world,view,1,100000),sss_reference,.015,
             "SSS area-light MIS agrees with pure continuation sampling");
    }
    cam.enable_mis = false; cam.direct_light_samples = 1;
    near(trace_mean(cam,world,view,1,100000),sss_reference,.015,
         "SSS light-only estimates do not duplicate emitter hits");
    cam.enable_mis = true; cam.set_emissive_surfaces({});

    // Moving and transformed objects retain their own scattering boundary.
    mat->sss_radius = .08;
    auto moving = std::make_shared<sphere>(point3(),point3(4,0,0),1,mat);
    auto instance = std::make_shared<transform>(moving,vec3(1,0,0),vec3(0,0,15),vec3(2,1,1));
    world.clear(); world.add(instance);
    ray moving_view(point3(1+8*.8,0,3),vec3(0,0,-1),.8);
    // Pick the moving center through the exact instance transformation.
    float affine[12]; instance->affine_rows(affine);
    point3 moving_center(affine[0]*3.2+affine[3],affine[4]*3.2+affine[7],affine[8]*3.2+affine[11]);
    moving_view = ray(moving_center+vec3(0,0,3),vec3(0,0,-1),.8);
    require(instance->hit(moving_view,interval(.001,infinity),hit),"moving SSS instance intersects at shutter time");
    require(mat->sample_subsurface(hit,world,.8,.1,.2,.3,.4,entry,weight) && entry.object == instance.get(),
            "subsurface probes preserve ray time and transformed object identity");

    // An opaque object lit from behind must still scatter light from its far side.
    mat = std::make_shared<pbr_material>(colour(.7,.4,.2),0,.7);
    mat->sss_strength = 1; mat->sss_radius = .5; mat->sss_model = SSS_DIPOLE_BURLEY;
    world.clear(); world.add(std::make_shared<sphere>(point3(),1,mat));
    cam.background = colour(); cam.use_sun = true; cam.sun_dir = vec3(0,0,-1);
    cam.sun_radiance = colour(1,1,1); cam.sun_angular_radius = 10; cam.sun_shadow_samples = 1;
    ray sphere_view(point3(0,0,3),vec3(0,0,-1),.6);
    double transmitted = trace_mean(cam,world,sphere_view,1,80000);
    require(transmitted > .002,"backlit SSS checks visibility at the far-side entry, not at the camera hit");
    world.add(std::make_shared<quad>(point3(-100,-100,-2),vec3(200,0,0),vec3(0,200,0),
                                  std::make_shared<lambertian>(colour(0,0,0))));
    near(trace_mean(cam,world,sphere_view,1,30000),0,1e-12,"external occluders block SSS at the entry point");
    world.objects.pop_back(); cam.use_sun = false; cam.background = colour(1,1,1);
    require(trace_mean(cam,world,sphere_view,2,30000) > .1,"SSS supports indirect environment lighting");
}

static void render_material_transport_samples(const std::filesystem::path& folder) {
    renderer engine; engine.use_denoiser = false; engine.adaptive_sampling = false;
    camera cam; cam.image_width = 256; cam.image_height = 192;
    cam.samples_per_pixel = 2048; cam.max_depth = 8;
    cam.lookfrom = point3(2.7,2.4,4); cam.lookat = point3(0,-.35,0); cam.vfov = 34;
    cam.background = colour(); cam.use_sun = true; cam.sun_dir = vec3(0,1,0);
    cam.sun_radiance = colour(2,2,2); cam.sun_angular_radius = 12; cam.sun_shadow_samples = 1;
    hittable_list world;
    world.add(std::make_shared<quad>(point3(-10,-1,-10),vec3(0,0,20),vec3(20,0,0),
                                   std::make_shared<lambertian>(colour(.7,.7,.7))));
    world.add(std::make_shared<sphere>(point3(),.65,std::make_shared<dielectric>(1.5)));
    auto save = [&](const char* name) {
        auto image = engine.render(world,cam);
        auto path = (folder/name).string();
        require(stbi_write_png(path.c_str(),image.width,image.height,3,image.pixels.data(),image.width*3) != 0,
                "material transport comparison image saves");
    };
    save("glass-sun.png");
    double distance = 100000, radius = distance*std::sin(degrees_to_radians(cam.sun_angular_radius));
    world.add(std::make_shared<sphere>(point3(0,distance,0),radius,
        std::make_shared<diffuse_light>(cam.sun_radiance/cam.sun_solid_angle())));
    cam.use_sun = false;
    save("glass-emissive.png");

    world.clear();
    auto skin = std::make_shared<pbr_material>(colour(.7,.3,.15),0,.6);
    skin->sss_radius = .25; skin->sss_model = SSS_DIPOLE_BURLEY;
    world.add(std::make_shared<sphere>(point3(),1,skin));
    cam.samples_per_pixel = 512; cam.max_depth = 5; cam.lookfrom = point3(0,0,4);
    cam.lookat = point3(); cam.vfov = 36; cam.background = colour(.02,.02,.02);
    cam.use_sun = true; cam.sun_dir = safe_unit_vector(vec3(.4,.5,-1));
    cam.sun_radiance = colour(3,3,3); cam.sun_angular_radius = 12;
    save("subsurface-off.png");
    skin->sss_strength = 1;
    save("subsurface-on.png");
}

// Unreal Subsurface transmission and Subsurface Profile dual specular.
static std::shared_ptr<pbr_material> unreal_subsurface_material() {
    auto mat = std::make_shared<pbr_material>(colour(.6,.4,.3),0,.45);
    mat->sss_transmission = true; mat->sss_transmission_color = colour(.9,.35,.25);
    mat->sss_opacity = .35; mat->sss_transmission_distance = .5;
    return mat;
}
static void check_unreal_subsurface_transport() {
    near(sss::unreal_transmission(1,1,0),1/(2*pi)+(1-1/(2*pi))*3,1e-12,"opacity zero in-scatter follows Unreal's lerp");
    near(sss::unreal_transmission(.5,-1,1),.5/(2*pi),1e-12,"opacity one back-scatter follows the half vector");

    // The one-sample lobe mixture must be unbiased: E[weight] equals the
    // integral of the surface lobes plus the ambient-only Lambert lobe.
    auto mat = unreal_subsurface_material();
    mat->lobe_scale0 = .75; mat->lobe_scale1 = 1.3; mat->lobe_mix = .85;
    hittable_list empty; auto hit = surface();
    ray view(point3(.3,.2,1),vec3(-.3,-.2,-1),.5);
    vec3 v = unit_vector(vec3(.3,.2,1));
    colour quadrature(0,0,0);
    const int rings = 600, segments = 1200;
    for (int i = 0; i < rings; ++i) for (int j = 0; j < segments; ++j) {
        double theta = (i+.5)*pi/rings, phi = (j+.5)*2*pi/segments;
        vec3 l(std::sin(theta)*std::cos(phi),std::sin(theta)*std::sin(phi),std::cos(theta));
        quadrature += mat->shade_direct(hit,v,l,colour(1,1,1),empty)*(std::sin(theta)*(pi/rings)*(2*pi/segments));
    }
    colour ambient = pbr_material::unreal_ambient(colour(.9,.35,.25),colour(.6,.4,.3),0);
    near(ambient.x(),.4,1e-12,"ambient subsurface colour stays within unit diffuse albedo");
    near(ambient.z(),.25,1e-12,"ambient subsurface colour is unchanged below unit albedo");
    colour estimate(0,0,0); const int samples = 400000; int indirect = 0;
    for (int i = 0; i < samples; ++i) {
        bsdf_sample sample;
        if (mat->sample_surface(view,hit,sequence(i,2),sequence(i,3),sequence(i,5),sample)) {
            estimate += sample.weight; indirect += sample.indirect_only;
        }
    }
    estimate /= samples;
    for (int c = 0; c < 3; ++c) near(estimate[c]/(quadrature[c]+ambient[c]),1,.01,"Unreal ambient and dual specular sampling is unbiased");
    require(indirect > samples/20,"ambient samples gather indirect light only");
    near(mat->bsdf_pdf(hit,v,vec3(0,0,-1)),0,1e-12,"transmission is never BSDF sampled");
    require(mat->shade_transmission(hit,v,vec3(0,0,-1),colour(1,1,1)).x() > 0,"transmission reaches back-facing lights");

    // A backlit sphere: light crosses its full diameter. NEE uses the closed form.
    mat = unreal_subsurface_material();
    hittable_list world; world.add(std::make_shared<sphere>(point3(),1,mat));
    // Off-axis, so the half vector of Unreal's term is well defined.
    vec3 toward_sun = safe_unit_vector(vec3(.25,.1,-1));
    camera cam; cam.background = colour(); cam.use_sun = true; cam.sun_dir = toward_sun;
    cam.sun_radiance = colour(1,1,1); cam.sun_angular_radius = .01; cam.sun_shadow_samples = 1;
    ray center(point3(0,0,3),vec3(0,0,-1),.5);
    hit_record front; require(world.hit(center,interval(.001,infinity),front),"backlit sphere fixture intersects");
    double chord = -2*dot(front.p-point3(),toward_sun);
    colour lobe = mat->shade_transmission(front,vec3(0,0,1),toward_sun,colour(1,1,1));
    double inscatter = std::pow(-dot(toward_sun,vec3(0,0,1)),12)*(3+(.1-3)*.35);
    require(lobe.x() > .9*inscatter*.9,"in-scatter dominates when looking toward the light");
    double expected = lobe.x()*std::exp(-chord/.5);
    near(trace_mean(cam,world,center,1,20000),expected,expected*.02,
         "backlit Unreal Subsurface transmits through its thickness");
    mat->sss_transmission_distance = 1;
    require(trace_mean(cam,world,center,1,2000) > expected*2,"a longer transmission distance passes more light");
    mat->sss_transmission_distance = .5;

    // Transmission is counted once, by light sampling: estimates do not depend
    // on the number of light samples or on MIS.
    cam.sun_angular_radius = 15; cam.sun_dir = safe_unit_vector(vec3(.3,.2,-1));
    ray oblique(point3(.2,.1,3),vec3(-.05,-.03,-1),.5);
    double reference = trace_mean(cam,world,oblique,1,200000);
    require(reference > .01,"oblique backlight transmits through the body");
    cam.direct_light_samples = 4; cam.sun_shadow_samples = 4;
    near(trace_mean(cam,world,oblique,1,100000),reference,reference*.03,
         "Unreal transmission is invariant to the light sample count");
    cam.enable_mis = false; cam.direct_light_samples = 1; cam.sun_shadow_samples = 1;
    near(trace_mean(cam,world,oblique,1,200000),reference,reference*.03,
         "Unreal transmission does not depend on MIS");
    cam.enable_mis = true;
    world.add(std::make_shared<quad>(point3(-100,-100,-2),vec3(200,0,0),vec3(0,200,0),
                                  std::make_shared<lambertian>(colour(0,0,0))));
    near(trace_mean(cam,world,center,1,20000),0,1e-12,"external blockers stop transmitted light");
    world.objects.pop_back();
    // Indirect light sees at most unit albedo, so bounces cannot amplify it.
    cam.use_sun = false; cam.background = colour(1,1,1);
    double furnace = trace_mean(cam,world,center,1,100000);
    std::cout << "Unreal Subsurface white furnace " << furnace << '\n';
    require(furnace > .4 && furnace < 1.01,"Unreal Subsurface ambient keeps unit albedo in a white furnace");
    cam.use_sun = true; cam.background = colour();
    mat->sss_transmission = false;
    near(trace_mean(cam,world,center,1,4000),0,1e-12,"without the Unreal model, a backlit opaque sphere stays dark");
}
