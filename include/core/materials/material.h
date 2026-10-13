#ifndef MATERIAL_H
#define MATERIAL_H

#include "../hittable.h"
#include "texture.h"
#include "sss.h"
#include "pbr_bsdf.h"
#include "eye.h"
#include <cstdio>
#include <limits>
#include <vector>

// ISPC specular helper (defined in src/core). Forward declaration to avoid
// pulling source headers into this public include.
void ispc_compute_specular(const float* Nx, const float* Ny, const float* Nz,
                           const float* Vx, const float* Vy, const float* Vz,
                           const float* Lx, const float* Ly, const float* Lz,
                           const float* F0r, const float* F0g, const float* F0b,
                           const float* alpha, int count,
                           float* out_r, float* out_g, float* out_b);

inline vec3 refract(const vec3& uv, const vec3& n, double etai_over_etat) {
    double cos_theta = fmin(dot(-uv, n), 1.0);
    vec3 r_out_perp = etai_over_etat * (uv + cos_theta * n);
    vec3 r_out_parallel = -sqrt(fmax(0.0, 1.0 - r_out_perp.length_squared())) * n;
    return r_out_perp + r_out_parallel;
}

inline double clamp01(double x) {
    return x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x);
}

// Coverage of a surface with alpha a under scene_alpha_mode: 0 opaque,
// 1 clip (all or nothing at the cutoff), 2 blend (fractional, as Cycles).
inline double alpha_coverage(double a, int mode, double cutoff) {
    if (mode == 0) return 1;
    a = clamp01(a);
    return mode == 1 ? (a < cutoff ? 0.0 : 1.0) : a;
}

inline double pow5(double x) {
    double x2 = x * x;
    return x2 * x2 * x;
}

inline vec3 schlick_fresnel(double cosTheta, const vec3& F0) {
    cosTheta = clamp01(cosTheta);
    double x  = 1.0 - cosTheta;
    return F0 + (vec3(1.0,1.0,1.0) - F0) * pow5(x);
}

// Perceptual roughness is shared by direct and indirect evaluation.
inline double perceptual_to_alpha(double roughness) {
    roughness = clamp01(roughness);
    return roughness * roughness;
}

// Cosine-weighted hemisphere sampling around +Z
inline vec3 sample_cosine_hemisphere(double xi1, double xi2) {
    double r = sqrt(xi1);
    double theta = 2.0 * pi * xi2;

    double x = r * cos(theta);
    double y = r * sin(theta);
    double z = sqrt(std::max(0.0, 1.0 - xi1));

    return vec3(x, y, z);
}

inline double luminance(const colour& c) {
    return clamp01(0.2126*c.x() + 0.7152*c.y() + 0.0722*c.z());
}

struct bsdf_sample {
    ray scattered;
    colour weight;
    double pdf = 0;
    bool delta = false;
    bool passthrough = false;
    bool subsurface = false;
    // The continuation gathers indirect light only (Unreal Subsurface ambient
    // lobe): emitters and the sun it reaches are counted by light sampling.
    bool indirect_only = false;
    // Scales emitters and the sun if the continuation reaches them directly,
    // for materials whose direct response differs from the light they carry
    // between bounces (hair Backlit). Light sampling uses the same response,
    // so MIS stays consistent.
    colour direct_scale = colour(1, 1, 1);
};

class material {
  public:
    virtual ~material() = default;

    //emission colour, default black
    virtual colour emitted(double u, double v, const point3& p) const {
        return colour(0,0,0);
    }

    virtual bool scatter(
        const ray& r_in, const hit_record& rec, colour& attenuation, ray& scattered
    ) const {
        return false;
    }

    // The integrator needs the event type as well as f*cos/pdf: delta and
    // opacity-continuation events cannot be assigned a continuous BSDF PDF.
    virtual bool sample_bsdf(const ray& incoming, const hit_record& rec, bsdf_sample& sample) const {
        if (!scatter(incoming,rec,sample.weight,sample.scattered)) return false;
        sample.delta = is_specular();
        sample.pdf = sample.delta ? 0 : bsdf_pdf(rec,-unit_vector(incoming.direction()),sample.scattered.direction());
        return true;
    }

    // By default materials are not purely specular. Override in materials
    // that are fully specular (mirror/dielectric) so renderer can skip
    // direct-diffuse lighting (e.g. lambert terms) for them.
        virtual bool is_specular() const { return false; }

    // Identify dielectric materials explicitly (glass, water, etc.)
    // Default false; overridden by dielectric.
    virtual bool is_dielectric() const { return false; }
    virtual double dielectric_ior() const { return 1; }
    virtual colour transmission_tint(const hit_record&) const { return colour(1,1,1); }

    // Direct-shading hook for direct lights. Default is
    // Lambertian: albedo * Li * (NdotL / pi). V is the view direction
    // (pointing out from the surface).
    // `vis` is the deterministic visibility/transmittance of the light
    // toward the shading point (in [0,1]). Materials should multiply their
    // surface BRDF contribution by `vis` but may perform independent
    // visibility checks for subsurface/external contributions.
    virtual colour shade_direct(const hit_record& rec, const vec3& V, const vec3& Ldir, const colour& Li, const hittable& world, double vis = 1.0) const {
        double NdotL = std::max(0.0, dot(rec.normal, unit_vector(Ldir)));
        if (NdotL <= 0.0) return colour(0,0,0);
        colour base = albedo(rec);
        return base * Li * (NdotL / pi) * (float)vis;
    }

    // Optional subsurface shading hook. Default implementation returns
    // zero so materials that don't implement SSS don't contribute.
    virtual colour shade_sss(const hit_record& /*rec*/, const vec3& /*V*/, const vec3& /*Ldir*/, const colour& /*Li*/, const hittable& /*world*/, double /*vis*/ = 1.0) const {
        return colour(0,0,0);
    }

    virtual bool sample_subsurface(const hit_record&, const hittable&, double,
                                  double, double, double, double, hit_record&, colour&) const { return false; }
    virtual double subsurface_directional(double) const { return 0; }
    // Extinction length for light crossing this material's own body to reach
    // back-facing directions; zero when the material has no such transport.
    virtual double body_transmission_distance() const { return 0; }
    // Direct-light-only Unreal Subsurface transmission; it has no BSDF
    // sampling counterpart, so light sampling counts it without MIS.
    virtual colour shade_transmission(const hit_record&, const vec3&, const vec3&, const colour&,
                                      double = 1) const { return colour(0,0,0); }

    virtual double bsdf_pdf(const hit_record& rec, const vec3& /*V*/, const vec3& Ldir) const {
        double NdotL = std::max(0.0, dot(rec.normal, unit_vector(Ldir)));
        return NdotL / pi;
    }

    //diffuse "base color" hook (used later for direct lighting)
    virtual colour albedo(const hit_record& rec) const {
        return colour(1,1,1);
    }

    // Alpha/mask test: return true if this material wants the current
    // hit to be treated as transparent (i.e. discarded) based on opacity.
    virtual bool is_masked_transparent(const hit_record& rec) const { return false; }

    // Deterministic opacity query in [0,1]. Default is fully opaque (1.0).
    // This is used for visibility/shadow queries where we need a stable
    // alpha value rather than a stochastic discard.
    virtual double opacity_at(const hit_record& rec) const { (void)rec; return 1.0; }
};

class lambertian : public material {
  public:
    lambertian(const colour& albedo) : tex(make_shared<solid_colour>(albedo)) {}
    lambertian(shared_ptr<texture> tex) : tex(tex) {}

    bool scatter(const ray& r_in, const hit_record& rec, colour& attenuation, ray& scattered)
    const override {
        auto scatter_direction = rec.normal + random_unit_vector();

        if (scatter_direction.near_zero())
            scatter_direction = rec.normal;

        scattered = ray(rec.p, scatter_direction, r_in.time());
        attenuation = tex->value(rec.u, rec.v, rec.p);
        return true;
    }

    colour albedo(const hit_record& rec) const override {
        return tex->value(rec.u, rec.v, rec.p);
    } 

  private:
    shared_ptr<texture> tex;
};

class metal : public material {
  public:
    // Existing "flat colour" ctor still works
    metal(const colour& albedo, double fuzz)
        : tex(make_shared<solid_colour>(albedo)),
          fuzz(fuzz < 1 ? fuzz : 1) {}

    // New: textured metal
    metal(shared_ptr<texture> tex, double fuzz)
        : tex(std::move(tex)),
          fuzz(fuzz < 1 ? fuzz : 1) {}

    bool scatter(const ray& r_in, const hit_record& rec,
                 colour& attenuation, ray& scattered) const override
    {
        vec3 reflected = reflect(r_in.direction(), rec.normal);
        reflected = unit_vector(reflected) + (fuzz * random_unit_vector());
        scattered = ray(rec.p, reflected, r_in.time());

        // Sample from the texture instead of a flat colour
        attenuation = tex->value(rec.u, rec.v, rec.p);

        return (dot(scattered.direction(), rec.normal) > 0);
    }

    bool is_specular() const override { return true; }
    colour shade_direct(const hit_record& rec, const vec3& V, const vec3& Ldir, const colour& Li, const hittable& /*world*/, double vis = 1.0) const override {
        return colour(0,0,0);
    }

    colour albedo(const hit_record& rec) const override {
        return tex->value(rec.u, rec.v, rec.p);
    }

  private:
    shared_ptr<texture> tex;
    double fuzz;
};

class dielectric : public material {
  public:
    // Default: clear glass (white attenuation)
    dielectric(double refraction_index)
        : refraction_index(refraction_index),
          tex(make_shared<solid_colour>(colour(1.0, 1.0, 1.0))) {}

    // Coloured glass with flat colour
    dielectric(double refraction_index, const colour& tint)
        : refraction_index(refraction_index),
          tex(make_shared<solid_colour>(tint)) {}

    // Textured “stained” glass
    dielectric(double refraction_index, shared_ptr<texture> tex)
        : refraction_index(refraction_index),
          tex(std::move(tex)) {}

    bool scatter(const ray& r_in, const hit_record& rec,
                 colour& attenuation, ray& scattered) const override
    {
        // Physically-based dielectric (clear glass by default)
        // Determine incident/transmitted indices
        const double ior = refraction_index;
        double etai = 1.0;
        double etat = ior;
        if (!rec.front_face) std::swap(etai, etat);
        double etai_over_etat = etai / etat;

        vec3 unit_direction = unit_vector(r_in.direction());
        double cos_theta = std::fmin(dot(-unit_direction, rec.normal), 1.0);
        double sin_theta = std::sqrt(std::max(0.0, 1.0 - cos_theta * cos_theta));

        // Fresnel reflectance (probability of reflection)
        double reflect_prob = reflectance(cos_theta, etai, etat);

        vec3 direction;
        bool reflected = false;

        // Total internal reflection check
        if (etai_over_etat * sin_theta > 1.0) {
            direction = reflect(unit_direction, rec.normal);
            reflected = true;
        } else {
            if (random_double() < reflect_prob) {
                direction = reflect(unit_direction, rec.normal);
                reflected = true;
            } else {
                direction = refract(unit_direction, rec.normal, etai_over_etat);
            }
        }

        // Offset ray origin slightly to avoid self-intersections.
        // Use the same bias used elsewhere (0.001) to be consistent and
        // avoid the reflected ray immediately re-hitting the same surface.
        const double eps = 1e-3;
        vec3 n = rec.normal;
        // Ensure direction is unit-length for the dot test
        vec3 dir_norm = unit_vector(direction);
        vec3 origin = rec.p + ((dot(dir_norm, n) > 0.0) ? (eps * n) : (-eps * n));

        // Reflections stay untinted. Refracted paths can carry stained-glass colour.
        attenuation = reflected ? colour(1.0, 1.0, 1.0)
                                : tex->value(rec.u, rec.v, rec.p) * (etai_over_etat*etai_over_etat);

        // normalize outgoing direction to avoid non-normalized rays later
        scattered = ray(origin, dir_norm, r_in.time());
        return true;
    }
    
    bool is_specular() const override { return true; }
    bool is_dielectric() const override { return true; }
    double dielectric_ior() const override { return refraction_index; }
    colour transmission_tint(const hit_record& rec) const override { return tex->value(rec.u,rec.v,rec.p); }

    // A perfect dielectric is a delta BSDF. Analytic-light highlights require
    // a rough dielectric model; inventing a finite GGX lobe double-counts energy.
    colour shade_direct(const hit_record&, const vec3&, const vec3&, const colour&,
                        const hittable&, double = 1) const override { return colour(0,0,0); }

    colour albedo(const hit_record& rec) const override {
        return colour(1.0, 1.0, 1.0);
    }

  private:
    double refraction_index;
    shared_ptr<texture> tex;

    // Exact unpolarized dielectric Fresnel, including total internal reflection.
    static double reflectance(double cosine, double etai, double etat) {
        cosine = std::clamp(cosine,0.0,1.0);
        double sin2 = (etai/etat)*(etai/etat)*(1-cosine*cosine);
        if (sin2 >= 1) return 1;
        double transmitted = std::sqrt(1-sin2);
        double parallel = (etat*cosine-etai*transmitted)/(etat*cosine+etai*transmitted);
        double perpendicular = (etai*cosine-etat*transmitted)/(etai*cosine+etat*transmitted);
        return .5*(parallel*parallel+perpendicular*perpendicular);
    }

};

class diffuse_light : public material {
  public:
    diffuse_light(shared_ptr<texture> tex, double intensity = 1) : tex(tex), intensity(intensity) {}
    diffuse_light(const colour& emit) : tex(make_shared<solid_colour>(emit)) {}

    colour emitted(double u, double v, const point3& p) const override {
        return tex->value(u, v, p)*intensity;
    }

    colour albedo(const hit_record& rec) const override {
        return tex->value(0, 0, 0);
    }

  private:
    shared_ptr<texture> tex;
    double intensity = 1;
};

class isotropic : public material {
  public:
    isotropic(const colour& albedo) : tex(make_shared<solid_colour>(albedo)) {}
    isotropic(shared_ptr<texture> tex) : tex(tex) {}

    bool scatter(const ray& r_in, const hit_record& rec, colour& attenuation, ray& scattered)
    const override {
        scattered = ray(rec.p, random_unit_vector(), r_in.time());
        attenuation = tex->value(rec.u, rec.v, rec.p);
        return true;
    }

    double bsdf_pdf(const hit_record&, const vec3&, const vec3&) const override { return 1/(4*pi); }
    colour shade_direct(const hit_record& rec, const vec3&, const vec3&, const colour& radiance,
                        const hittable&, double vis = 1) const override { return albedo(rec)*radiance*(vis/(4*pi)); }

    colour albedo(const hit_record& rec) const override {
        return tex->value(rec.u, rec.v, rec.p);
    }

  private:
    shared_ptr<texture> tex;
};

// ----------------------------
// PBR material
// ----------------------------

// SSS model choices
enum SSSModel {
    SSS_NONE = 0,
    SSS_SINGLE_SCATTER = 1,
    SSS_MULTI_SINGLE_SCATTER = 2,
    SSS_DIPOLE_BURLEY = 3,
    SSS_SKIN = 4,      // Legacy alias for Burley diffusion
    SSS_FOLIAGE = 5    // Legacy alias for Burley diffusion
};

class pbr_material : public material {
public:
    // Base color, metallic, roughness, specular F0, normal map
    shared_ptr<texture> base_tex;
    shared_ptr<texture> metallic_tex;   // greyscale in [0,1]
    shared_ptr<texture> roughness_tex;  // greyscale in [0,1]
    shared_ptr<texture> normal_tex;     // tangent-space normal map
    double normal_strength;

    // Optional alpha mask texture (single-channel or image alpha)
    shared_ptr<texture> alpha_tex;
    bool alpha_double_sided = true;
    double alpha_cutoff = 0.5;
    int alpha_mode = 2; // alpha_coverage

    // Eye shading model (eye.h): the base colour is seen through the cornea
    // inside the iris, which is lit by Unreal's iris term. The cornea's F0 is
    // 0.08 x Specular; metallic and subsurface scattering are unused.
    bool eye = false;
    eye_shading::iris_params iris;
    double eye_specular = .5;
    shared_ptr<texture> specular_tex, iris_mask_tex, iris_depth_tex;

    // Dielectric F0 when metallic = 0 (0.04 is common)
    colour  dielectric_F0;

    // Spatial diffusion replaces this fraction of the diffuse substrate.
    // Old scale/sample fields are retained for project-file compatibility;
    // radius controls distance and pixel samples control convergence.
    double sss_strength = 0.0;
    double sss_scale = 1.0;
    // Dipole/diffusion parameters
    SSSModel sss_model = SSS_SINGLE_SCATTER;
    int      sss_samples = 4;        // legacy project field
    double   sss_radius  = 1.0;      // mean free path / diffusion radius
    vec3 sss_radius_rgb = vec3(1,1,1);
    double   sss_eta     = 1.3;      // entry interface IOR
    bool     sss_color_override = false;
    colour   sss_color_override_col = colour(1.0, 1.0, 1.0);
    // Diffusion tint multiplier (Unreal profile Tint) and dual specular lobes.
    colour   sss_tint_scale = colour(1, 1, 1);
    double   lobe_scale0 = 1, lobe_scale1 = 1, lobe_mix = 0;
    // Unreal "Subsurface" transmission lobe; see sss::unreal_transmission.
    bool     sss_transmission = false;
    colour   sss_transmission_color = colour(0, 0, 0);
    bool     sss_transmission_base = false; // multiply the colour by the base colour
    double   sss_opacity = 1, sss_transmission_distance = 1;
    // If true, sample combined textures using Unreal-style channel packing
    // (green = roughness, blue = metallic)
    bool     use_unreal_pbr = false;

public:
    // (A) Constant base/metal/rough with optional normal map
    pbr_material(
        const colour& base_color,
        double metallic,
        double roughness,
        shared_ptr<texture> normal_map = nullptr,
        double normal_strength_in = 1.0,
        const colour& dielectric_specular = colour(0.04, 0.04, 0.04)
    )
        : dielectric_F0(dielectric_specular),
          base_tex(make_shared<solid_colour>(base_color)),
          metallic_tex(make_shared<solid_colour>(colour(clamp01(metallic), clamp01(metallic), clamp01(metallic)))),
          roughness_tex(make_shared<solid_colour>(colour(clamp01(roughness), clamp01(roughness), clamp01(roughness)))),
          normal_tex(normal_map),
          normal_strength(normal_strength_in)
    {}


    // (B) Textured base + scalar metallic/rough + normal map
    pbr_material(
        shared_ptr<texture> base_color,
        double metallic,
        double roughness,
        shared_ptr<texture> normal_map,
        double normal_strength_in = 1.0,
        const colour& dielectric_specular = colour(0.04, 0.04, 0.04)
    )
        : dielectric_F0(dielectric_specular),
          base_tex(base_color),
          metallic_tex(make_shared<solid_colour>(colour(clamp01(metallic), clamp01(metallic), clamp01(metallic)))),
          roughness_tex(make_shared<solid_colour>(colour(clamp01(roughness), clamp01(roughness), clamp01(roughness)))),
          normal_tex(normal_map),
          normal_strength(normal_strength_in)
    {}


    // (C) Fully textured PBR: base, metallic, roughness, normal
    pbr_material(
        shared_ptr<texture> base_color,
        shared_ptr<texture> metallic,
        shared_ptr<texture> roughness,
        shared_ptr<texture> normal_map,
        double normal_strength_in = 1.0,
        const colour& dielectric_specular = colour(0.04, 0.04, 0.04),
        shared_ptr<texture> alpha_map = nullptr,
        bool alpha_double_sided_in = true,
        double alpha_cutoff_in = 0.5
    )
        : dielectric_F0(dielectric_specular),
          base_tex(base_color),
          metallic_tex(metallic),
          roughness_tex(roughness),
          normal_tex(normal_map),
          normal_strength(normal_strength_in),
          alpha_tex(alpha_map),
          alpha_double_sided(alpha_double_sided_in),
          alpha_cutoff(alpha_cutoff_in)
    {}

  private:
    static bool is_scalar_fallback_texture(const shared_ptr<texture>& tex) {
        return tex && dynamic_cast<const solid_colour*>(tex.get()) != nullptr;
    }

    double specular_sampling_probability(const colour& F0, double metallic) const {
        if (metallic >= 1.0) return 1.0;
        double f0_peak = std::max(F0.x(), std::max(F0.y(), F0.z()));
        return std::clamp(f0_peak, 0.05, 0.98);
    }

    void sample_surface_params(const hit_record& rec,
                               colour& baseColor,
                               double& metallic,
                               double& rough) const
    {
        baseColor = base_tex ? base_tex->value(rec.u, rec.v, rec.p)
                             : colour(1,1,1);

        metallic = 0.0;
        rough = 0.5;

        if (use_unreal_pbr) {
            const bool has_metallic_texture =
                metallic_tex && !is_scalar_fallback_texture(metallic_tex);
            const bool has_roughness_texture =
                roughness_tex && !is_scalar_fallback_texture(roughness_tex);

            // Start from scalar fallbacks so materials without authored maps still work.
            if (metallic_tex) {
                metallic = clamp01(metallic_tex->value(rec.u, rec.v, rec.p).x());
            }
            if (roughness_tex) {
                rough = clamp01(roughness_tex->value(rec.u, rec.v, rec.p).x());
            }

            // Accept a packed map from either slot. This matches common editor usage where
            // artists may place the ORM/MR texture in only one of the fields.
            shared_ptr<texture> packed_tex = has_metallic_texture ? metallic_tex
                                                                  : (has_roughness_texture ? roughness_tex : nullptr);
            if (packed_tex) {
                colour mr = packed_tex->value(rec.u, rec.v, rec.p);
                rough = clamp01(mr.y());
                metallic = clamp01(mr.z());
            }

            // If dedicated authored grayscale maps are supplied separately, let them
            // override the corresponding packed channels while leaving scalar fallbacks alone.
            if (has_roughness_texture && roughness_tex.get() != packed_tex.get()) {
                rough = clamp01(roughness_tex->value(rec.u, rec.v, rec.p).x());
            }
            if (has_metallic_texture && metallic_tex.get() != packed_tex.get()) {
                metallic = clamp01(metallic_tex->value(rec.u, rec.v, rec.p).x());
            }
            return;
        }

        if (metallic_tex) {
            // Separate metal maps are data textures; for grayscale authoring the
            // value is replicated across channels, so sample the first channel.
            metallic = clamp01(metallic_tex->value(rec.u, rec.v, rec.p).x());
        }
        if (roughness_tex) {
            rough = clamp01(roughness_tex->value(rec.u, rec.v, rec.p).x());
        }
    }

  public:
    virtual colour albedo(const hit_record& rec) const override {
        return base_tex ? base_tex->value(rec.u, rec.v, rec.p)
                        : colour(1,1,1);
    }

  private:
    struct surface {
        colour base, f0, transmission, ambient;
        // n: shading normal (normal-mapped); interpolated: the vertex normal
        // before normal mapping; geometry: the flat triangle, for ray sides.
        vec3 n, geometry, interpolated;
        double metallic, alpha, alpha1, alpha_mean, lobe_mix, specular_probability,
               subsurface_fraction, ambient_probability, opacity;
        bool smooth;
        eye_shading::iris_geometry iris; // mask 0 except on eyes
    };
    surface prepare_surface(const hit_record& rec, const vec3& view) const {
        surface s; double rough;
        sample_surface_params(rec,s.base,s.metallic,rough);
        s.base = pbr::reflectance(s.base);
        s.f0 = (1-s.metallic)*pbr::reflectance(dielectric_F0)+s.metallic*s.base;
        s.lobe_mix = std::clamp(lobe_mix,0.0,1.0);
        if (s.lobe_mix > 0) {
            // Unreal dual specular: both lobes scale one roughness; masking
            // uses their mixed roughness.
            double r0 = std::clamp(clamp01(rough)*lobe_scale0,.02,1.0), r1 = std::clamp(clamp01(rough)*lobe_scale1,.02,1.0);
            double mean = r0+(r1-r0)*s.lobe_mix;
            s.alpha = r0*r0; s.alpha1 = r1*r1; s.alpha_mean = mean*mean;
        } else s.alpha = s.alpha1 = s.alpha_mean = perceptual_to_alpha(rough);
        s.smooth = s.alpha < 1e-4;
        s.specular_probability = specular_sampling_probability(s.f0,s.metallic);
        s.subsurface_fraction = sss_model == SSS_NONE ? 0 : clamp01(sss_strength);
        s.transmission = sss_transmission ? pbr::reflectance(sss_transmission_color) : colour(0,0,0);
        if (sss_transmission_base) s.transmission = s.transmission*s.base;
        s.ambient = unreal_ambient(s.transmission,s.base,s.metallic);
        double ambient_weight = luminance(s.ambient);
        s.ambient_probability = ambient_weight/(1+ambient_weight);
        s.opacity = clamp01(sss_opacity);
        s.geometry = safe_unit_vector(rec.geometry_normal());
        s.n = s.interpolated = safe_unit_vector(rec.normal,s.geometry);
        if (normal_tex) {
            hit_record frame = rec; frame.normal = s.n; frame.set_tangent_frame(rec.tangent,rec.bitangent);
            colour texel = normal_tex->value(rec.u,rec.v,rec.p);
            vec3 tangent((2*texel.x()-1)*normal_strength,(2*texel.y()-1)*normal_strength,2*texel.z()-1);
            vec3 mapped = safe_unit_vector(tangent.x()*frame.tangent+tangent.y()*frame.bitangent+tangent.z()*s.n,s.n);
            if (dot(mapped,s.geometry) > 0) s.n = mapped;
        }
        if (eye) prepare_eye(rec,view,s);
        s.n = face_view(s.n,s.geometry,view);
        return s;
    }
    void prepare_eye(const hit_record& rec, const vec3& view, surface& s) const {
        hit_record frame = rec; frame.normal = s.interpolated; frame.set_tangent_frame(rec.tangent,rec.bitangent);
        auto pin = [&](const shared_ptr<texture>& t) { return t ? t->value(rec.u,rec.v,rec.p).x() : -1.0; };
        s.iris = eye_shading::iris(iris,rec.u,rec.v,frame.tangent,frame.bitangent,s.interpolated,view,
                                   pin(iris_mask_tex),pin(iris_depth_tex));
        if (s.iris.mask > 0 && base_tex) s.base = pbr::reflectance(base_tex->value(s.iris.u,s.iris.v,rec.p));
        const double specular = specular_tex ? std::max(0.0,specular_tex->value(rec.u,rec.v,rec.p).x()) : eye_specular;
        s.metallic = 0;
        s.f0 = colour(1,1,1)*clamp01(.08*specular);
        s.specular_probability = specular_sampling_probability(s.f0,0);
    }
    // Normal maps tilt some shading normals away from the viewer near
    // silhouettes, where they would see no light at all and render black.
    // Bend those toward the viewer so shading is never more grazing than the
    // geometry; normals already facing the viewer are unchanged.
    static vec3 face_view(const vec3& n, const vec3& g, const vec3& v) {
        double limit = std::min(.9*dot(g,v),.1), facing = dot(n,v);
        if (limit <= 0 || facing >= limit) return n;
        vec3 side = safe_unit_vector(n-facing*v,safe_unit_vector(g-dot(g,v)*v,g));
        return safe_unit_vector(limit*v+std::sqrt(1-limit*limit)*side,g);
    }
    // Light reaches the smooth surface the vertex normals describe: gating it
    // on the flat triangle would cut the terminator along triangle edges.
    static bool lit(const surface& s, const vec3& v, const vec3& l) {
        return dot(s.geometry,v) > 0 && dot(s.interpolated,l) > 0 && dot(s.n,v) > 0 && dot(s.n,l) > 0;
    }
    // Sampled directions must also leave the flat triangle, or the ray would
    // start inside the mesh; light sampling alone covers the rest.
    static bool above_surface(const surface& s, const vec3& v, const vec3& l) {
        return lit(s,v,l) && dot(s.geometry,l) > 0;
    }
    static bool viewable(const surface& s, const vec3& v) { return dot(s.geometry,v) > 0 && dot(s.n,v) > 0; }
    static double specular_distribution(const surface& s, double nh) {
        double d = pbr::distribution(nh,s.alpha);
        return s.lobe_mix > 0 ? d+(pbr::distribution(nh,s.alpha1)-d)*s.lobe_mix : d;
    }
    static double specular_pdf(const surface& s, double nh, double nv) {
        double p = pbr::distribution(nh,s.alpha)*pbr::masking(nv,s.alpha);
        if (s.lobe_mix > 0) p += (pbr::distribution(nh,s.alpha1)*pbr::masking(nv,s.alpha1)-p)*s.lobe_mix;
        return p/(4*nv);
    }
    // Surface lobes (diffuse substrate and GGX) times cosine.
    static colour evaluate_cosine(const surface& s, const vec3& v, const vec3& l) {
        if (!lit(s,v,l)) return colour(0,0,0);
        double nv = std::clamp(dot(s.n,v),0.0,1.0), nl = std::clamp(dot(s.n,l),0.0,1.0);
        // A diffuse substrate loses light on both entry and exit through its
        // dielectric interface. Do not add a full Lambert lobe on top of GGX.
        colour transmission_v = colour(1,1,1)-schlick_fresnel(nv,s.f0);
        colour transmission_l = colour(1,1,1)-schlick_fresnel(nl,s.f0);
        colour diffuse = s.base*((1-s.metallic)*(1-s.subsurface_fraction)/pi)*transmission_v*transmission_l;
        colour specular(0,0,0);
        if (!s.smooth) {
            vec3 h = safe_unit_vector(v+l,s.n);
            double d = specular_distribution(s,std::clamp(dot(s.n,h),0.0,1.0));
            specular = schlick_fresnel(dot(v,h),s.f0)*(d*pbr::visibility(nv,nl,s.alpha_mean));
        }
        // An eye's iris replaces N.L with Unreal's iris term (eye.h).
        return (diffuse*eye_shading::diffuse_cosine(s.iris,nl,l)+specular*nl)*terminator_softening(s,l);
    }
    // Chiang, Li and Burley (2019), "Taming the Shadow Terminator": a normal
    // or bump map keeps lighting surfaces up to the terminator and then cuts
    // it off. This fades it as the smooth surface turns away from the light,
    // as Cycles does; without a normal map it is exactly one.
    static double terminator_softening(const surface& s, const vec3& l) {
        double nl = dot(s.n,l), ng = dot(s.n,s.interpolated);
        if (nl <= 0 || ng <= 0) return 0;
        double g = std::min(1.0,dot(s.interpolated,l)/(nl*ng));
        return g*(1+g*(1-g));
    }
    // Density of the surface lobes. The ambient lobe only gathers indirect
    // light, so light sampling never competes with it.
    static double surface_pdf(const surface& s, const vec3& v, const vec3& l) {
        if (!above_surface(s,v,l)) return 0;
        double nv = dot(s.n,v), nl = dot(s.n,l);
        double pdf = (1-s.specular_probability)*(1-s.subsurface_fraction)*nl/pi;
        if (!s.smooth) {
            vec3 h = safe_unit_vector(v+l,s.n);
            pdf += s.specular_probability*specular_pdf(s,std::clamp(dot(s.n,h),0.0,1.0),nv);
        }
        return pdf*(1-s.ambient_probability);
    }

  public:
    // Unreal treats Subsurface Color as extra diffuse for indirect light. A
    // path tracer bounces repeatedly, so the sum stays within unit albedo.
    static colour unreal_ambient(const colour& subsurface, const colour& base, double metallic) {
        colour ambient;
        for (int c = 0; c < 3; ++c) ambient[c] = std::clamp(std::min(subsurface[c],1-(1-metallic)*base[c]),0.0,1.0);
        return ambient;
    }
    // Exposed deterministic variates support reproducible sampler validation.
    bool sample_surface(const ray& incoming, const hit_record& rec, double lobe, double u, double v, bsdf_sample& sample) const {
        sample = {};
        vec3 view = safe_unit_vector(-incoming.direction());
        surface s = prepare_surface(rec,view);
        if (!viewable(s,view)) return false;
        double subsurface_probability = (1-s.specular_probability)*s.subsurface_fraction;
        if (subsurface_probability > 0 && lobe >= 1-subsurface_probability) {
            sample.subsurface = true;
            sample.weight = (colour(1,1,1)-schlick_fresnel(dot(view,s.n),s.f0)) *
                ((1-s.metallic)*s.subsurface_fraction/subsurface_probability);
            return true;
        }
        double ambient = s.ambient_probability, specular = (1-ambient)*s.specular_probability;
        pbr::frame frame(s.n);
        vec3 direction;
        if (ambient > 0 && lobe >= 1-ambient) {
            // Indirect-only Lambert lobe: direct light reaches this material
            // through the Unreal transmission term instead.
            direction = frame.world(sample_cosine_hemisphere(u,v));
            if (!above_surface(s,view,direction)) return false;
            sample.weight = s.ambient/ambient;
            sample.pdf = ambient*dot(s.n,direction)/pi;
            sample.indirect_only = true;
            sample.scattered = ray(rec.p,direction,incoming.time());
            return true;
        }
        if (lobe < specular) {
            if (s.smooth) {
                direction = reflect(-view,s.n);
                if (!above_surface(s,view,direction)) return false;
                sample.delta = true; sample.pdf = 0;
                sample.weight = schlick_fresnel(dot(view,s.n),s.f0)/specular;
                sample.scattered = ray(rec.p,direction,incoming.time());
                return true;
            }
            double alpha = s.lobe_mix > 0 && lobe < specular*s.lobe_mix ? s.alpha1 : s.alpha;
            vec3 h = frame.world(pbr::visible_normal(frame.local(view),alpha,u,v));
            direction = reflect(-view,h);
        } else direction = frame.world(sample_cosine_hemisphere(u,v));
        // Samples below the real surface are absorption events. Redirecting or
        // retrying them would change the distribution without changing its PDF.
        if (!above_surface(s,view,direction)) return false;
        sample.pdf = surface_pdf(s,view,direction);
        if (sample.pdf <= 0) return false;
        sample.weight = evaluate_cosine(s,view,direction)/sample.pdf;
        sample.scattered = ray(rec.p,direction,incoming.time());
        return true;
    }
    bool sample_bsdf(const ray& incoming, const hit_record& rec, bsdf_sample& sample) const override {
        double opacity = opacity_at(rec);
        if (opacity <= 0 || (opacity < 1 && random_double() >= opacity)) {
            vec3 direction = safe_unit_vector(incoming.direction());
            sample.scattered = ray(rec.p+direction*.001,direction,incoming.time());
            sample.weight = colour(1,1,1); sample.delta = true; sample.passthrough = true;
            return true;
        }
        return sample_surface(incoming,rec,random_double(),random_double(),random_double(),sample);
    }
    bool scatter(const ray& incoming, const hit_record& rec, colour& attenuation, ray& scattered) const override {
        bsdf_sample sample;
        if (!sample_bsdf(incoming,rec,sample) || sample.subsurface) return false;
        attenuation = sample.weight; scattered = sample.scattered; return true;
    }
    colour shade_direct(const hit_record& rec, const vec3& view, const vec3& light, const colour& radiance,
                        const hittable&, double visibility = 1) const override {
        if (opacity_at(rec) <= 0) return colour(0,0,0);
        vec3 v = safe_unit_vector(view), l = safe_unit_vector(light); auto s = prepare_surface(rec,v);
        return evaluate_cosine(s,v,l)*radiance*visibility;
    }
    // Unreal Subsurface transmission for a direct light on either side. It
    // already includes its geometry term. Callers pass ordinary visibility
    // for lights in front of the geometric surface and the body
    // transmittance for lights behind it.
    colour shade_transmission(const hit_record& rec, const vec3& view, const vec3& light, const colour& radiance,
                              double visibility = 1) const override {
        if (!sss_transmission || opacity_at(rec) <= 0) return colour(0,0,0);
        vec3 v = safe_unit_vector(view), l = safe_unit_vector(light); auto s = prepare_surface(rec,v);
        if (!viewable(s,v)) return colour(0,0,0);
        vec3 h = safe_unit_vector(v+l,s.n);
        return s.transmission*radiance*(sss::unreal_transmission(dot(s.n,h),-dot(l,v),s.opacity)*visibility);
    }
    double bsdf_pdf(const hit_record& rec, const vec3& view, const vec3& light) const override {
        if (opacity_at(rec) <= 0) return 0;
        vec3 v = safe_unit_vector(view);
        return surface_pdf(prepare_surface(rec,v),v,safe_unit_vector(light));
    }
    double body_transmission_distance() const override {
        return sss_transmission ? std::max(1e-6,sss_transmission_distance) : 0;
    }

    // Spatial BSSRDF transport replaces part of the diffuse substrate. Legacy
    // nonzero model IDs map to normalized Burley diffusion.
    double subsurface_distance() const { return std::max(1e-6, sss_radius); }
    double subsurface_directional(double cosine) const override {
        return sss::entry_directional(cosine, sss_eta);
    }
    bool sample_subsurface(const hit_record& out, const hittable& world, double time,
                           double axis_u, double radius_u, double phi_u, double pick_u,
                           hit_record& entry, colour& weight) const override {
        if (!out.object || sss_strength <= 0 || sss_model == SSS_NONE) return false;
        vec3 distances = subsurface_distance()*sss_radius_rgb;
        for (int c=0;c<3;++c) distances[c]=std::max(1e-6,distances[c]);
        double extent = 30*std::max({distances.x(),distances.y(),distances.z()});
        pbr::frame frame(safe_unit_vector(out.geometry_normal()));
        vec3 axes[3] = {frame.t, frame.b, frame.n};
        const double probabilities[3] = {.25, .25, .5};
        int axis = axis_u < .5 ? 2 : axis_u < .75 ? 0 : 1;
        // Remap the axis variate to an independent uniform spectral proposal.
        double channel_u = axis == 2 ? 2*axis_u : axis == 0 ? 4*(axis_u-.5) : 4*(axis_u-.75);
        double d = distances[std::clamp(int(3*channel_u),0,2)];
        double r = sss::sample_burley_radius(d, radius_u);
        if (r >= extent) return false; // Tiny tail is absorption, never renormalized.
        double phi = 2*pi*phi_u;
        point3 center = out.p + r*(std::cos(phi)*axes[(axis+1)%3] + std::sin(phi)*axes[(axis+2)%3]);
        auto intersections = [&](int a, const point3& c, std::vector<hit_record>* hits) {
            vec3 delta = c-out.p;
            double projected2 = delta.length_squared()-std::pow(dot(delta,axes[a]),2);
            if (projected2 >= extent*extent) return 0;
            double half = std::sqrt(std::max(0.0,extent*extent-projected2));
            ray probe(c-half*axes[a], axes[a], time);
            double minimum = 1e-6;
            int found = 0;
            hit_record hit;
            for (int step = 0; step < 256 && world.hit(probe,interval(minimum,2*half),hit); ++step) {
                minimum = hit.t+1e-6;
                if (hit.object != out.object || hit.mat.get() != this) continue;
                vec3 outward = hit.front_face ? hit.geometry_normal() : -hit.geometry_normal();
                if (!hit.front_face) hit.normal = -hit.normal;
                hit.geometric_normal = outward;
                hit.front_face = true;
                if (opacity_at(hit) <= 0) continue;
                ++found;
                if (hits) hits->push_back(hit);
            }
            return found;
        };
        std::vector<hit_record> candidates;
        int found = intersections(axis,center,&candidates);
        if (!found) return false;
        entry = candidates[std::min(found-1,int(std::clamp(pick_u,0.0,1.0)*found))];
        vec3 delta = entry.p-out.p;
        double pdf = 0;
        // Include intersection selection for every projection axis; ignoring it
        // biases thin and curved objects.
        for (int a = 0; a < 3; ++a) {
            point3 projected = entry.p-dot(delta,axes[a])*axes[a];
            int n = a == axis ? found : intersections(a,projected,nullptr);
            double projected_r = (projected-out.p).length();
            if (n) {
                double mixture = 0;
                for (int c=0;c<3;++c) mixture += sss::burley_pdf_area(projected_r,distances[c])/3;
                pdf += probabilities[a]*mixture*std::abs(dot(entry.geometry_normal(),axes[a]))/n;
            }
        }
        if (pdf <= 0 || !std::isfinite(pdf)) return false;
        colour tint = sss_color_override ? pbr::reflectance(sss_color_override_col)
            : pbr::reflectance(albedo(out))*pbr::reflectance(sss_tint_scale);
        colour profile;
        for (int c=0;c<3;++c) profile[c]=sss::burley_Rd(delta.length(),1,distances[c]);
        weight = tint*profile*(opacity_at(entry)/pdf);
        return true;
    }

    bool is_masked_transparent(const hit_record& rec) const override {
        double opacity = opacity_at(rec);
        return opacity <= 0 || (opacity < 1 && random_double() >= opacity);
    }
    double opacity_at(const hit_record& rec) const override {
        if (alpha_mode == 0 || (!alpha_double_sided && !rec.front_face)) return 1;
        double a = alpha_tex ? alpha_tex->mask_alpha_at(rec.u,rec.v,rec.p)
            : base_tex ? base_tex->alpha_at(rec.u,rec.v,rec.p) : 1;
        return alpha_coverage(a,alpha_mode,alpha_cutoff);
    }
};

#endif // MATERIAL_H
