#pragma once

#include "material.h"
#include "hair_bsdf.h"

#include <cstdint>
#include <cstring>

// Unreal-style Hair shading model on the Chiang et al. fibre BSDF. Inputs
// follow Unreal's Hair pins; textures (from the material graph) override the
// scalar fallbacks. The strand direction is the Tangent input in tangent
// space: Unreal's default (0,1,0) runs along a hair card's V texture axis.
// A card does not record where across a strand a ray landed, so each hit
// picks that offset pseudo-randomly from its position. Averaged over samples
// this is the fibre's far-field response, which is what makes a card read as
// many soft, shiny strands rather than one shaded sheet.
class hair_material : public material {
public:
    shared_ptr<texture> base_tex, specular_tex, roughness_tex, tangent_tex, alpha_tex, radial_tex, backlit_tex;
    colour base_color = colour(.55, .4, .25);
    double specular = .5;          // primary highlight; .5 is physical, as in Unreal
    double roughness = .35;        // longitudinal roughness
    double radial_roughness = .3;  // azimuthal roughness
    double backlit = 1;            // direct light through strands (Unreal Backlit)
    double ior = 1.55;
    double cuticle_tilt = 2;       // degrees
    double alpha_cutoff = .5;
    int alpha_mode = 2;            // alpha_coverage
    vec3 tangent = vec3(0, 1, 0);
    int coloring = 0;              // scene_material_parameters::hair_coloring

    struct fibre {
        hair::bsdf bsdf;
        vec3 x, y, z;
        vec3 local(const vec3& w) const { return vec3(dot(w, x), dot(w, y), dot(w, z)); }
        vec3 world(const vec3& w) const { return w.x() * x + w.y() * y + w.z() * z; }
    };

    colour albedo(const hit_record& rec) const override {
        return base_tex ? pbr::reflectance(base_tex->value(rec.u, rec.v, rec.p)) : pbr::reflectance(base_color);
    }
    // Hair cards are always two-sided.
    double opacity_at(const hit_record& rec) const override {
        if (alpha_mode == 0) return 1;
        double a = alpha_tex ? alpha_tex->mask_alpha_at(rec.u, rec.v, rec.p)
                             : base_tex ? base_tex->alpha_at(rec.u, rec.v, rec.p) : 1;
        return alpha_coverage(a, alpha_mode, alpha_cutoff);
    }
    bool is_masked_transparent(const hit_record& rec) const override {
        double opacity = opacity_at(rec);
        return opacity <= 0 || (opacity < 1 && random_double() >= opacity);
    }

    // Offset across the fibre in [-1, 1], fixed for a hit point so light
    // sampling, BSDF sampling and their MIS densities use one fibre.
    static double fibre_offset(const point3& p) {
        uint64_t key = 0x9e3779b97f4a7c15ull;
        for (int k = 0; k < 3; ++k) {
            double coordinate = p[k] + 0.0; // folds -0 into +0
            uint64_t bits;
            std::memcpy(&bits, &coordinate, sizeof(bits));
            key ^= bits + 0x9e3779b97f4a7c15ull + (key << 6) + (key >> 2);
        }
        key ^= key >> 30;
        key *= 0xbf58476d1ce4e5b9ull;
        key ^= key >> 27;
        key *= 0x94d049bb133111ebull;
        key ^= key >> 31;
        return 2 * double(key >> 11) / 9007199254740992.0 - 1;
    }

    // Unreal's Backlit dims the glow of lights behind the hair. Only direct
    // light uses it: as an absorption on every bounce it would darken dense
    // hair, because light reaches the inside of a hairstyle through strands.
    double backlit_at(const hit_record& rec) const {
        return std::max(0.0, backlit_tex ? backlit_tex->value(rec.u, rec.v, rec.p).x() : backlit);
    }
    fibre prepare(const hit_record& rec) const {
        auto scalar = [&](const shared_ptr<texture>& tex, double fallback) {
            return tex ? tex->value(rec.u, rec.v, rec.p).x() : fallback;
        };
        hair::parameters p;
        p.h = fibre_offset(rec.p);
        p.eta = ior;
        p.beta_m = clamp01(scalar(roughness_tex, roughness));
        p.beta_n = clamp01(scalar(radial_tex, radial_roughness));
        p.sigma_a = coloring == 1 ? hair::absorption_from_colour(albedo(rec), p.beta_n)
                                  : hair::absorption_from_fibre_colour(albedo(rec));
        p.alpha = cuticle_tilt;
        p.r_weight = 2 * clamp01(scalar(specular_tex, specular));
        vec3 n = safe_unit_vector(rec.normal, rec.geometry_normal());
        vec3 t = tangent_tex ? tangent_tex->value(rec.u, rec.v, rec.p) : tangent;
        vec3 strand = t.x() * rec.tangent + t.y() * rec.bitangent + t.z() * n;
        if (strand.length_squared() < 1e-16) strand = pbr::frame(n).b;
        vec3 x = safe_unit_vector(strand, pbr::frame(n).b);
        vec3 z = safe_unit_vector(n - dot(n, x) * x, pbr::frame(x).t);
        return {hair::bsdf(p), x, cross(z, x), z};
    }

    colour shade_direct(const hit_record& rec, const vec3& view, const vec3& light, const colour& radiance,
                        const hittable&, double visibility = 1) const override {
        if (opacity_at(rec) <= 0) return colour(0, 0, 0);
        auto f = prepare(rec);
        return f.bsdf.f(f.local(safe_unit_vector(view)), f.local(safe_unit_vector(light)), backlit_at(rec)) * radiance *
               visibility;
    }
    double bsdf_pdf(const hit_record& rec, const vec3& view, const vec3& light) const override {
        if (opacity_at(rec) <= 0) return 0;
        auto f = prepare(rec);
        return f.bsdf.pdf(f.local(safe_unit_vector(view)), f.local(safe_unit_vector(light)));
    }
    // Deterministic variates support sampler validation.
    bool sample_fibre(const ray& incoming, const hit_record& rec, const double u[4], bsdf_sample& sample) const {
        sample = {};
        auto f = prepare(rec);
        vec3 wi;
        const vec3 wo = f.local(safe_unit_vector(-incoming.direction()));
        if (!f.bsdf.sample(wo, u, wi, sample.weight, sample.pdf)) return false;
        const double backlit = backlit_at(rec);
        if (backlit != 1) {
            const colour full = f.bsdf.f(wo, wi), direct = f.bsdf.f(wo, wi, backlit);
            for (int c = 0; c < 3; ++c) sample.direct_scale[c] = full[c] > 0 ? direct[c] / full[c] : 0;
        }
        sample.scattered = ray(rec.p, f.world(wi), incoming.time());
        return true;
    }
    bool sample_bsdf(const ray& incoming, const hit_record& rec, bsdf_sample& sample) const override {
        double opacity = opacity_at(rec);
        if (opacity <= 0 || (opacity < 1 && random_double() >= opacity)) {
            vec3 direction = safe_unit_vector(incoming.direction());
            sample.scattered = ray(rec.p + direction * .001, direction, incoming.time());
            sample.weight = colour(1, 1, 1);
            sample.delta = true;
            sample.passthrough = true;
            return true;
        }
        const double u[4] = {random_double(), random_double(), random_double(), random_double()};
        return sample_fibre(incoming, rec, u, sample);
    }
    bool scatter(const ray& incoming, const hit_record& rec, colour& attenuation, ray& scattered) const override {
        bsdf_sample sample;
        if (!sample_bsdf(incoming, rec, sample)) return false;
        attenuation = sample.weight;
        scattered = sample.scattered;
        return true;
    }
};
