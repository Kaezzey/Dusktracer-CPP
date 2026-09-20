#include "core/dusktracer.h"
#include "core/materials/material.h"
#include "core/camera.h"
#include "core/renderer.h"
#include "core/scene.h"
#include "core/quad.h"
#include "core/sphere.h"
#include "core/transform.h"
#include "core/triangle.h"
#include "core/embree_accel.h"
#include <filesystem>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "external/stb_image_write.h"

static int checks = 0;
static void require(bool pass, const char* message) {
    ++checks;
    if (!pass) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
static void near(double a, double b, double tolerance, const char* message) {
    if (std::abs(a-b) > tolerance) std::cerr << message << ": " << a << " versus " << b << '\n';
    require(std::isfinite(a) && std::abs(a-b) <= tolerance,message);
}
static double sequence(int i, int base) {
    double value = 0, place = 1;
    for (++i; i; i /= base) { place /= base; value += (i%base)*place; }
    return value;
}
static hit_record surface() {
    hit_record hit;
    hit.normal = hit.geometric_normal = vec3(0,0,1);
    hit.tangent = vec3(1,0,0); hit.bitangent = vec3(0,1,0);
    return hit;
}
static void check_bsdf() {
    hittable_list empty; auto hit = surface();
    near(pbr::distribution(1,.5),4/pi,1e-12,"GGX distribution at normal incidence");
    near(pbr::masking(1,.3),1,1e-12,"Smith masking at normal incidence");
    near(pbr::visibility(1,1,.3),.25,1e-12,"Smith correlated visibility at normal incidence");
    near(schlick_fresnel(1,colour(.04,.04,.04)).x(),.04,1e-12,"dielectric F0");
    near(schlick_fresnel(0,colour(.04,.04,.04)).x(),1,1e-12,"Fresnel grazing limit");
    near(linear_to_gamma(.0031308),.040449936,1e-9,"sRGB linear segment");
    near(linear_to_gamma(.18),.4613561295,1e-8,"sRGB middle grey");
    {
        // Independent textbook D*G*F/(4*cos_v*cos_l), evaluated in double
        // precision rather than through the production visibility helpers.
        double alpha = .7*.7;
        vec3 v(.8,0,.6), l(0,.6,.8), h = unit_vector(v+l);
        double a2 = alpha*alpha, d = a2/(pi*std::pow(h.z()*h.z()*(a2-1)+1,2));
        auto lambda = [=](double cosine) { return .5*(std::sqrt(1+a2*(1-cosine*cosine)/(cosine*cosine))-1); };
        double geometry = 1/(1+lambda(v.z())+lambda(l.z()));
        colour f0(.8,.5,.2), fresnel = f0+(colour(1,1,1)-f0)*std::pow(1-dot(v,h),5);
        auto expected = fresnel*(d*geometry/(4*v.z()));
        pbr_material conductor(f0,1,.7);
        auto actual = conductor.shade_direct(hit,v,l,colour(1,1,1),empty);
        for (int c = 0; c < 3; ++c) near(actual[c],expected[c],1e-12,"GGX conductor matches the independent Cook-Torrance equation");
    }
    double worst_energy = 0, worst_integral_error = 0;
    for (double metal : {0.,.5,1.}) for (double rough : {.35,.7,1.}) for (double nv : {.25,1.}) {
        pbr_material mat(colour(1,1,1),metal,rough);
        vec3 view(std::sqrt(1-nv*nv),0,nv); ray incoming(view,-view,0);
        colour sampled; double accepted = 0;
        const int count = 65536;
        for (int i = 0; i < count; ++i) {
            bsdf_sample sample;
            if (!mat.sample_surface(incoming,hit,sequence(i,2),sequence(i,3),sequence(i,5),sample)) continue;
            sampled += sample.weight; accepted += 1;
            if (i < 32) {
                double pdf = mat.bsdf_pdf(hit,view,sample.scattered.direction());
                near(sample.pdf,pdf,1e-9,"sampler and evaluator PDFs agree");
                auto direct = mat.shade_direct(hit,view,sample.scattered.direction(),colour(1,1,1),empty);
                near(sample.weight.x()*pdf,direct.x(),1e-9,"sample weight equals full BSDF times cosine over mixture PDF");
            }
        }
        sampled /= count;
        // Independent uniform-solid-angle midpoint quadrature, not the sampler.
        colour integrated; double integrated_pdf = 0;
        const int nz = 256, nphi = 512;
        for (int z = 0; z < nz; ++z) for (int p = 0; p < nphi; ++p) {
            double cosine = (z+.5)/nz, phi = 2*pi*(p+.5)/nphi;
            double r = std::sqrt(1-cosine*cosine); vec3 light(r*std::cos(phi),r*std::sin(phi),cosine);
            integrated += mat.shade_direct(hit,view,light,colour(1,1,1),empty);
            integrated_pdf += mat.bsdf_pdf(hit,view,light);
        }
        integrated *= 2*pi/(nz*nphi); integrated_pdf *= 2*pi/(nz*nphi);
        near(sampled.x(),integrated.x(),.008,"indirect estimator agrees with independent BRDF integration");
        near(accepted/count,integrated_pdf,.007,"PDF mass matches sampler acceptance including rejected directions");
        require(sampled.x() >= 0 && sampled.x() <= 1.003,"white furnace does not create energy");
        worst_energy = std::max(worst_energy,sampled.x());
        worst_integral_error = std::max(worst_integral_error,std::abs(sampled.x()-integrated.x()));
        vec3 light = safe_unit_vector(vec3(-.3,.7,.8));
        double forward = mat.shade_direct(hit,view,light,colour(1,1,1),empty).x()/dot(hit.normal,light);
        double reverse = mat.shade_direct(hit,light,view,colour(1,1,1),empty).x()/dot(hit.normal,view);
        near(forward,reverse,1e-12,"BRDF is reciprocal");
    }
    pbr_material mirror(colour(.8,.5,.2),1,0); bsdf_sample sample;
    require(mirror.sample_surface(ray(vec3(0,0,1),vec3(0,0,-1),0),hit,.1,.2,.3,sample),"smooth metal samples a reflection");
    require(sample.delta && sample.pdf == 0,"zero roughness is a discrete mirror event");
    near(sample.weight.x(),.8,1e-12,"mirror throughput is Fresnel reflectance");
    near(mirror.shade_direct(hit,vec3(0,0,1),vec3(0,0,1),colour(1,1,1),empty).x(),0,0,"delta reflection has no fake finite direct lobe");
    pbr_material normal_mapped(colour(.7,.4,.2),.2,.5,std::make_shared<solid_colour>(colour(.8,.65,.9)));
    hit.tangent = vec3(3,0,2); hit.bitangent = vec3(2,4,1);
    vec3 view = safe_unit_vector(vec3(.1,.2,1)), light = safe_unit_vector(vec3(.4,.1,1));
    for (int i = 0; i < 1024; ++i) {
        bsdf_sample mapped;
        if (normal_mapped.sample_surface(ray(view,-view,0),hit,sequence(i,2),sequence(i,3),sequence(i,5),mapped)) {
            require(dot(mapped.scattered.direction(),hit.geometry_normal()) > 0,"normal mapping never redirects a sample below real geometry");
            near(mapped.weight.x()*mapped.pdf,normal_mapped.shade_direct(hit,view,mapped.scattered.direction(),colour(1,1,1),empty).x(),1e-9,"normal-map sampling uses the same orthonormal frame and BSDF");
        }
    }
    auto black = normal_mapped.shade_direct(hit,view,vec3(0,0,-1),colour(1,1,1),empty);
    require(black.near_zero(),"light below geometry cannot leak through a normal map");
    std::cout << "FURNACE max reflectance " << worst_energy << ", max quadrature error " << worst_integral_error << '\n';
}

static void check_geometry() {
    auto mat = std::make_shared<dielectric>(1.5);
    auto ball = std::make_shared<sphere>(point3(),1,mat);
    transform instance(ball,vec3(),vec3(20,30,40),vec3(2,1,.5));
    hit_record hit;
    require(instance.hit(ray(point3(),vec3(1,0,0),0),interval(.001,infinity),hit) && !hit.front_face,"transformed glass preserves inside-to-outside intersections");
    require(instance.hit(ray(point3(4,0,0),vec3(-1,0,0),0),interval(.001,infinity),hit) && hit.front_face,"transformed glass preserves entering intersections");
    auto tri = std::make_shared<triangle>(point3(-1,-1,0),point3(1,-1,0),point3(0,1,0),0,0,1,0,.5,1,mat);
    tri->n0 = tri->n1 = tri->n2 = safe_unit_vector(vec3(.8,0,1));
    tri->t0 = tri->t1 = tri->t2 = vec3(1,0,0);
    tri->b0 = tri->b1 = tri->b2 = vec3(0,-1,0);
    transform mirrored(tri,vec3(),vec3(),vec3(-2,1,1));
    require(mirrored.hit(ray(point3(0,0,2),vec3(0,0,-1),0),interval(.001,infinity),hit),"mirrored smooth triangle intersects");
    near(dot(hit.tangent,hit.normal),0,1e-12,"transformed tangent is orthogonal");
    near(dot(hit.bitangent,vec3(0,-1,0)),1,1e-12,"mirrored UV bitangent orientation is preserved");
    near(hit.geometric_normal.z(),1,1e-12,"geometric normal stays distinct from the smooth shading normal");
#ifdef HAVE_EMBREE
    triangle_mesh mesh; mesh.triangles.push_back(tri);
    embree_triangle_accel accel(mesh); hit_record cpu, embree;
    ray test(point3(0,0,2),vec3(0,0,-1),0);
    require(tri->hit(test,interval(.001,infinity),cpu) && accel.hit(test,interval(.001,infinity),embree),"CPU and Embree intersect the same smooth triangle");
    near((cpu.normal-embree.normal).length(),0,1e-6,"CPU and Embree shading normals agree");
    near((cpu.geometric_normal-embree.geometric_normal).length(),0,1e-6,"CPU and Embree geometric normals agree");
    near((cpu.bitangent-embree.bitangent).length(),0,1e-6,"CPU and Embree preserve the same tangent handedness");
    std::array<ray,4> rays = {test,ray(point3(0,0,-2),vec3(0,0,1),0),test,test};
    std::array<hit_record,4> packet;
    require(accel.hit_packet(rays,interval(.001,infinity),packet),"Embree packet intersects the fixture");
    for (int i = 0; i < 4; ++i) {
        require(tri->hit(rays[i],interval(.001,infinity),cpu) && cpu.front_face == packet[i].front_face,"packet and scalar hits agree on front/back orientation");
        near((cpu.normal-packet[i].normal).length(),0,1e-6,"packet and scalar shading frames agree");
    }
#endif
    double pdf;
    require(instance.sample_surface(.2,.3,0,hit,pdf),"transformed surface samples exist");
    near(pdf,instance.surface_pdf(hit),1e-12,"nonuniform transform preserves the area sampling Jacobian");
}

static double mean_trace(camera& cam, const hittable& world, int count = 80000) {
    double value = 0;
    for (int i = 0; i < count; ++i) value += cam.ray_colour(ray(point3(0,0,1),vec3(0,0,-1),0),1,world).x();
    return value/count;
}
static void check_transport() {
    auto mat = std::make_shared<pbr_material>(colour(.7,.7,.7),.25,.5);
    auto floor = std::make_shared<quad>(point3(-10,-10,0),vec3(20,0,0),vec3(0,20,0),mat);
    auto light_mat = std::make_shared<diffuse_light>(colour(2,2,2));
    auto light = std::make_shared<quad>(point3(-2,-2,2),vec3(4,0,0),vec3(0,4,0),light_mat);
    hittable_list world; world.add(floor); world.add(light);
    camera cam; cam.enable_mnee = false; cam.background = colour();
    cam.set_emissive_surfaces({{light,16,colour(2,2,2)}});
    double reference = 0; auto hit = surface();
    for (int x = 0; x < 256; ++x) for (int y = 0; y < 256; ++y) {
        vec3 delta(-2+4*(x+.5)/256,-2+4*(y+.5)/256,2);
        double d2 = delta.length_squared(); vec3 dir = delta/std::sqrt(d2);
        reference += mat->shade_direct(hit,vec3(0,0,1),dir,colour(2,2,2),world).x()*dir.z()/d2;
    }
    reference *= 16./(256*256);
    for (int samples : {0,1,4}) {
        cam.direct_light_samples = samples; cam.enable_mis = true;
        double measured = mean_trace(cam,world);
        std::cout << "AREA samples " << samples << ": " << measured << " reference " << reference << '\n';
        near(measured,reference,.012,"area-light NEE and BSDF hits agree without double counting");
    }
    cam.enable_mis = false; cam.direct_light_samples = 1;
    near(mean_trace(cam,world),reference,.012,"light-only estimator remains correct when MIS is disabled");
    world.clear(); world.add(floor); cam.set_emissive_surfaces({});
    cam.use_sun = true; cam.sun_dir = vec3(0,0,1); cam.sun_radiance = colour(2,2,2); cam.sun_angular_radius = 30;
    double sun_reference = 0, omega = cam.sun_solid_angle();
    for (int i = 0; i < 65536; ++i) {
        double z = 1-sequence(i,2)*omega/(2*pi), phi = 2*pi*sequence(i,3), r = std::sqrt(1-z*z);
        sun_reference += mat->shade_direct(hit,vec3(0,0,1),vec3(r*std::cos(phi),r*std::sin(phi),z),colour(2,2,2),world).x();
    }
    sun_reference /= 65536;
    for (int samples : {0,1,4}) {
        cam.enable_mis = true; cam.direct_light_samples = samples;
        near(mean_trace(cam,world),sun_reference,.012,"sun has consistent radiometry and MIS for every sample count");
    }
    // Transparent null events do not consume the path's scattering-depth budget.
    auto cutout = std::make_shared<pbr_material>(colour(1,1,1),0,.5);
    cutout->alpha_tex = std::make_shared<solid_colour>(colour(.4,.4,.4));
    world.clear();
    for (int i = 0; i < 20; ++i) world.add(std::make_shared<quad>(point3(-2,-2,-i*.02),vec3(4,0,0),vec3(0,4,0),cutout));
    cam.use_sun = false; cam.background = colour(1,1,1);
    near(cam.ray_colour(ray(point3(0,0,1),vec3(0,0,-1),0),1,world).x(),1,1e-12,"cutout continuation does not burn bounce depth");
    near(cam.compute_transmittance(ray(point3(0,0,1),vec3(0,0,-1),0),5,world),1,1e-12,"shadow cutoffs agree with camera cutoffs");
    cutout->alpha_tex = std::make_shared<solid_colour>(colour(.75,.75,.75));
    world.clear(); world.add(std::make_shared<quad>(point3(-2,-2,0),vec3(4,0,0),vec3(0,4,0),cutout));
    near(cam.compute_transmittance(ray(point3(0,0,1),vec3(0,0,-1),0),5,world),.25,1e-12,"partial-alpha shadows use deterministic transmittance");
    cam.background = colour(); cam.point_lights = {{point3(0,0,2),colour(4,4,4),0}};
    double opaque_direct = cutout->shade_direct(hit,vec3(0,0,1),vec3(0,0,1),colour(1,1,1),world).x();
    near(mean_trace(cam,world,40000),.75*opaque_direct,.005,"partial opacity weights direct light exactly once");
    cutout->alpha_double_sided = false; hit.front_face = false;
    near(cutout->opacity_at(hit),1,0,"one-sided alpha convention matches shadows and scattering");
    scene scene; scene_material glow; glow.model = scene_material_model::diffuse_light; glow.emission_intensity = 3;
    initialize_material_graph(glow,{}); int color = glow.graph.add(graph_kind::vector,0,0);
    glow.graph.find(color)->value = vec3(.2,.4,.6); glow.graph.connect({color,0,glow.graph.output_id(),0});
    near(build_rt_material(scene,glow)->emitted(0,0,{}).y(),1.2,1e-12,"emission intensity applies to shader graphs");
    scene.materials.push_back(glow); scene_object cube; cube.type = scene_object_type::cube; cube.material_index = 0;
    cube.scale = vec3(2,3,4); cube.rotation_deg = vec3(12,34,56); scene.objects.push_back(cube);
    build_emissive_surfaces(scene,cam);
    require(cam.emissive_surfaces.size() == 6,"emissive cube samples its real transformed faces");
    scene_object sphere; sphere.type = scene_object_type::sphere; sphere.material_index = 0;
    sphere.center = point3(3,4,5); sphere.scale = vec3(2,2,2); sphere.radius = .5; scene.objects.push_back(sphere);
    build_emissive_surfaces(scene,cam); double sphere_pdf; hit_record sphere_sample;
    require(cam.emissive_surfaces.back().geometry->sample_surface(.2,.7,0,sphere_sample,sphere_pdf),"emissive sphere samples its surface");
    near((sphere_sample.p-sphere.center).length(),1,1e-12,"sphere light sample lies on the sphere rather than at its center");
    near(sphere_pdf,1/(4*pi),1e-12,"sphere light area PDF matches transformed radius");
}

static void render_chart(const std::string& path) {
    hittable_list world;
    world.add(std::make_shared<quad>(point3(-8,-1,-6),vec3(16,0,0),vec3(0,0,12),std::make_shared<lambertian>(colour(.3,.3,.3))));
    for (int row = 0; row < 2; ++row) for (int i = 0; i < 5; ++i) {
        double roughness = i*.25;
        auto mat = std::make_shared<pbr_material>(row ? colour(.8,.45,.15) : colour(.55,.15,.08),double(row),roughness);
        world.add(std::make_shared<sphere>(point3(-3.2+1.6*i,-.2,1.5-2.3*row),.7,mat));
    }
    camera cam; cam.image_width = 384; cam.aspect_ratio = 16./9; cam.samples_per_pixel = 64; cam.max_depth = 5;
    cam.lookfrom = point3(6,6,10); cam.lookat = point3(0,0,0); cam.vfov = 40;
    cam.background = colour(.3,.3,.3); cam.use_sun = true; cam.sun_dir = safe_unit_vector(vec3(-.5,1,1));
    cam.sun_radiance = colour(3,3,3); cam.sun_angular_radius = 8; cam.direct_light_samples = 1;
    renderer engine; engine.use_denoiser = false; engine.adaptive_sampling = false;
    auto image = engine.render(world,cam);
    require(stbi_write_png(path.c_str(),image.width,image.height,3,image.pixels.data(),image.width*3) != 0,"production renderer saves the roughness/metalness chart");
}
int main(int argc, char** argv) {
    check_bsdf(); check_geometry(); check_transport();
    if (argc > 1) render_chart(argv[1]);
    std::cout << checks << " PBR checks passed\n";
}
