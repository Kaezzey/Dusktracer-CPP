// Coarse smooth-shaded meshes must light and shadow as their smooth surface,
// not as facets (the shadow terminator problem). Included by pbr_tests.cpp.

// A UV sphere of radius one with per-vertex normals: smooth (the sphere
// normal) or flat (each triangle's own normal).
static hittable_list faceted_sphere(int rings, int segments, bool smooth, std::shared_ptr<material> mat) {
    hittable_list list;
    auto point = [&](int i, int j) {
        const double theta = pi * i / rings, phi = 2 * pi * j / segments;
        return vec3(std::sin(theta) * std::cos(phi), std::cos(theta), std::sin(theta) * std::sin(phi));
    };
    auto add = [&](const point3& a, const point3& b, const point3& c) {
        const vec3 face = cross(b - a, c - a);
        if (face.length_squared() < 1e-12) return;
        const vec3 flat = unit_vector(face) * (dot(face, a + b + c) > 0 ? 1.0 : -1.0);
        list.add(std::make_shared<triangle>(a, b, c, smooth ? a : flat, smooth ? b : flat, smooth ? c : flat, 0., 0.,
                                            1., 0., 0., 1., vec3(1, 0, 0), vec3(0, 1, 0), mat));
    };
    for (int i = 0; i < rings; ++i)
        for (int j = 0; j < segments; ++j) {
            add(point(i, j), point(i + 1, j), point(i + 1, j + 1));
            add(point(i, j), point(i + 1, j + 1), point(i, j + 1));
        }
    return list;
}

static void check_shadow_terminator() {
    // The smooth-surface offset is zero on flat triangles and lifts points
    // above the facets of a curved one.
    const point3 a(1, 0, 0), b(0, 1, 0), c(0, 0, 1);
    const vec3 flat = unit_vector(vec3(1, 1, 1));
    const point3 centre = (a + b + c) / 3;
    require(smooth_surface_offset(centre, a, b, c, flat, flat, flat, 1. / 3, 1. / 3, 1. / 3).near_zero(),
            "flat triangles have no smooth-surface offset");
    const vec3 lift = smooth_surface_offset(centre, a, b, c, a, b, c, 1. / 3, 1. / 3, 1. / 3);
    require(dot(lift, flat) > 0 && lift.length() < .5, "curved triangles lift onto their smooth surface");

    // Sun 12 degrees off +x, so the visible terminator crosses facets rather
    // than following the vertex column at x = 0. Where the smooth normal faces
    // the sun but the flat triangle does not, the point was black (a sawtooth
    // terminator); it must now be lit.
    auto diffuse = std::make_shared<pbr_material>(colour(.8, .8, .8), 0, 1);
    camera cam;
    cam.background = colour(0, 0, 0);
    cam.use_sun = true;
    cam.sun_dir = vec3(std::cos(pi / 15), 0, -std::sin(pi / 15));
    cam.sun_radiance = colour(3, 3, 3);
    cam.sun_angular_radius = 0;
    cam.direct_light_samples = 1;
    for (bool smooth : {true, false}) {
        auto world = faceted_sphere(6, 12, smooth, diffuse);
        int band = 0, dark = 0;
        double worst = 1e9;
        for (int y = 0; y < 40; ++y)
            for (int x = 0; x < 40; ++x) {
                ray r(point3(-.45 + .9 * x / 39, -.8 + 1.6 * y / 39, -3), vec3(0, 0, 1), 0);
                hit_record rec;
                if (!world.hit(r, interval(.001, infinity), rec)) continue;
                if (dot(rec.normal, cam.sun_dir) < .1 || dot(rec.geometry_normal(), cam.sun_dir) >= 0) continue;
                ++band;
                const double radiance = cam.ray_colour(r, 1, world).x();
                worst = std::min(worst, radiance / dot(rec.normal, cam.sun_dir));
                dark += radiance <= 0;
            }
        if (smooth) {
            std::cout << "Shadow terminator: " << band << " smooth-lit points facing away from their triangle, "
                      << dark << " black, worst radiance/cosine " << worst << '\n';
            require(band > 20 && dark == 0, "smooth-shaded facets are lit where their smooth surface faces the light");
        } else {
            require(band == 0, "flat-shaded triangles keep their geometric terminator");
        }
    }
}
