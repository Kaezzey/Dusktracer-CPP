// Alpha modes and the Eye shading model (materials/eye.h). Included by
// pbr_tests.cpp, which provides require(), near(), sequence() and surface().

// A black texture with constant alpha, and one whose colour is its UV.
struct constant_alpha_texture : texture {
    double alpha;
    explicit constant_alpha_texture(double a) : alpha(a) {}
    colour value(double, double, const point3&) const override { return colour(0, 0, 0); }
    double alpha_at(double, double, const point3&) const override { return alpha; }
};
struct uv_texture : texture {
    colour value(double u, double v, const point3&) const override { return colour(u, v, .5); }
};

static void check_alpha_modes() {
    near(alpha_coverage(.3, 0, .5), 1, 0, "Opaque ignores alpha");
    near(alpha_coverage(.3, 1, .5), 0, 0, "Clip drops alpha below the threshold");
    near(alpha_coverage(.7, 1, .5), 1, 0, "Clip keeps alpha at or above the threshold fully");
    near(alpha_coverage(.3, 2, .5), .3, 0, "Blend keeps alpha fractional, whatever the threshold");

    // A black card with 30% alpha in front of a white sky. Blend shows 70% of
    // the sky, as Cycles does; Clip shows all of it or none.
    camera cam;
    cam.background = colour(1, 1, 1);
    cam.use_sun = false;
    const struct { int mode; double cutoff, expected; const char* message; } cases[] = {
        {2, .5, .7, "a Blend card lets 1 - alpha through"},
        {1, .5, 1, "a Clip card below its threshold disappears"},
        {1, .2, 0, "a Clip card above its threshold is solid"},
        {0, .5, 0, "an Opaque card ignores its alpha"},
    };
    for (const auto& c : cases) {
        auto card = std::make_shared<pbr_material>(std::make_shared<constant_alpha_texture>(.3),
            std::make_shared<solid_colour>(colour(0, 0, 0)), std::make_shared<solid_colour>(colour(.5, .5, .5)),
            nullptr, 1.0, colour(0, 0, 0), nullptr, true, c.cutoff);
        card->alpha_mode = c.mode;
        hittable_list world;
        world.add(std::make_shared<quad>(point3(-1, -1, 0), vec3(2, 0, 0), vec3(0, 2, 0), card));
        double seen = 0;
        const int count = 20000;
        for (int i = 0; i < count; ++i) seen += cam.ray_colour(ray(point3(0, 0, 1), vec3(0, 0, -1), 0), 4, world).x();
        near(seen / count, c.expected, .02, c.message);
    }

    // A surface half a millimetre behind a transparent one (an eyeball under
    // its occlusion shell) is seen, and shadows, rather than being skipped.
    {
        auto clear = std::make_shared<pbr_material>(std::make_shared<constant_alpha_texture>(0),
            std::make_shared<solid_colour>(colour(0, 0, 0)), std::make_shared<solid_colour>(colour(.5, .5, .5)),
            nullptr, 1.0, colour(0, 0, 0));
        hittable_list world;
        world.add(std::make_shared<quad>(point3(-1, -1, 0), vec3(2, 0, 0), vec3(0, 2, 0), clear));
        world.add(std::make_shared<quad>(point3(-1, -1, -.0005), vec3(2, 0, 0), vec3(0, 2, 0),
                                         std::make_shared<diffuse_light>(colour(.2, .4, .6))));
        const ray through(point3(0, 0, 1), vec3(0, 0, -1), 0);
        near(cam.ray_colour(through, 4, world).z(), .6, 1e-9, "a surface just behind a transparent one is seen");
        near(cam.compute_transmittance(through, 2, world), 0, 0, "a surface just behind a transparent one shadows");
    }

    // Hair cards honour the same modes.
    hair_material strands;
    strands.base_tex = std::make_shared<constant_alpha_texture>(.3);
    hit_record hit = surface();
    strands.alpha_mode = 2;
    near(strands.opacity_at(hit), .3, 1e-12, "Blend hair cards keep fractional alpha");
    strands.alpha_mode = 1;
    near(strands.opacity_at(hit), 0, 0, "Clip hair cards drop alpha below the threshold");
}

static void check_eye() {
    using namespace eye_shading;
    const vec3 t(1, 0, 0), b(0, 1, 0), n(0, 0, 1);
    iris_params p;
    p.radius = .2; p.limbus = .02; p.depth = .5; p.eta = 1.336;
    // The mask is the iris disc, soft across the limbus.
    near(iris(p, .5, .5, t, b, n, n).mask, 1, 1e-12, "the iris centre is iris");
    near(iris(p, .9, .5, t, b, n, n).mask, 0, 1e-12, "outside the iris is sclera");
    const double edge = iris(p, .7, .5, t, b, n, n).mask;
    require(edge > .3 && edge < .7, "the limbus blends iris into sclera");
    // Looking straight in, nothing shifts; from the side, the iris is seen
    // through the cornea at a point away from the viewer, refracted toward
    // the normal (less than straight-line parallax), and more for a deeper iris.
    const auto straight = iris(p, .5, .5, t, b, n, n);
    near(straight.u, .5, 1e-12, "a head-on view sees the iris beneath it");
    const vec3 side = unit_vector(vec3(.7, 0, .7));
    const double shift = iris(p, .5, .5, t, b, n, side).u - .5;
    iris_params unrefracted = p;
    unrefracted.eta = 1;
    iris_params deeper = p;
    deeper.depth = 1;
    require(shift < 0, "the iris is seen at a point away from the viewer");
    require(iris(p, .5, .5, t, b, n, side).v == .5, "a view across U shifts only U");
    require(-shift < .5 - iris(unrefracted, .5, .5, t, b, n, side).u, "the cornea refracts toward the normal");
    require(iris(deeper, .5, .5, t, b, n, side).u - .5 < shift, "a deeper iris shifts further");
    near(iris(p, .9, .5, t, b, n, side).u, .9, 1e-12, "the sclera is not refracted");

    // Unreal's iris lighting: light from +U reaches the far side of the iris
    // through the cornea, and the near side lies in the iris's own shadow.
    const vec3 light = unit_vector(vec3(.8, 0, .6));
    const auto far = iris(p, .4, .5, t, b, n, n), near_side = iris(p, .6, .5, t, b, n, n);
    require(iris_cosine(far, light) > dot(n, light), "the iris lights up on the side away from the light");
    require(iris_cosine(near_side, light) < dot(n, light), "the iris darkens on the side facing the light");

    // White furnace: cornea and iris (or sclera) never reflect more than they
    // receive, and the sampler integrates the same BSDF as quadrature.
    hittable_list empty;
    double worst = 0;
    for (double u : {.45, .62, .9}) for (double nv : {.3, 1.}) {
        pbr_material mat(colour(1, 1, 1), 0, .3);
        mat.eye = true;
        mat.iris = p;
        auto hit = surface();
        hit.u = u; hit.v = .5;
        const vec3 view(std::sqrt(1 - nv * nv), 0, nv);
        const ray incoming(view, -view, 0);
        colour sampled(0, 0, 0);
        const int count = 65536;
        for (int i = 0; i < count; ++i) {
            bsdf_sample sample;
            if (!mat.sample_surface(incoming, hit, sequence(i, 2), sequence(i, 3), sequence(i, 5), sample)) continue;
            sampled += sample.weight / count;
            if (i < 32)
                near(sample.weight.x() * sample.pdf,
                     mat.shade_direct(hit, view, sample.scattered.direction(), colour(1, 1, 1), empty).x(), 1e-9,
                     "eye sample weight is the BSDF over its density");
        }
        colour integrated(0, 0, 0);
        const int nz = 256, nphi = 512;
        for (int z = 0; z < nz; ++z) for (int k = 0; k < nphi; ++k) {
            const double cosine = (z + .5) / nz, phi = 2 * pi * (k + .5) / nphi, r = std::sqrt(1 - cosine * cosine);
            integrated += mat.shade_direct(hit, view, vec3(r * std::cos(phi), r * std::sin(phi), cosine),
                                           colour(1, 1, 1), empty);
        }
        integrated *= 2 * pi / (nz * nphi);
        near(sampled.x(), integrated.x(), .01, "eye sampling agrees with quadrature");
        worst = std::max(worst, integrated.x());
    }
    std::cout << "Eye white furnace: max reflectance " << worst << '\n';
    require(worst <= 1.003, "the eye never reflects more light than it receives");

    // The material looks the base colour up through the cornea: a texture
    // whose red is U reads less red at the iris centre seen from +U.
    pbr_material textured(std::make_shared<uv_texture>(), 0, .3, nullptr);
    textured.eye = true;
    textured.iris = p;
    auto hit = surface();
    hit.u = .5; hit.v = .5;
    const colour head_on = textured.shade_direct(hit, n, n, colour(1, 1, 1), empty);
    const colour oblique = textured.shade_direct(hit, side, n, colour(1, 1, 1), empty);
    near(head_on.x() / head_on.y(), 1, 1e-6, "head on, the iris centre reads its own texel");
    require(oblique.x() / oblique.y() < .95, "from the side, the iris texel comes from away from the viewer");
}
