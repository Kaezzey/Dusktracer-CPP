#ifndef MATERIAL_H
#define MATERIAL_H

#include "../hittable.h"
#include "texture.h"
#include "sss.h"
#include "pbr_bsdf.h"
#include <cstdio>
#include <limits>

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
    SSS_SKIN = 4,      // Skin-like surface scattering / transmission
    SSS_FOLIAGE = 5    // Thin transmission model for leaves and petals
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

    // Dielectric F0 when metallic = 0 (0.04 is common)
    colour  dielectric_F0;

    // Simple thin-subsurface approximation parameters
    // sss_strength: [0,1] how much light is transmitted/scattered through the thin material
    // sss_scale: user-facing scale controlling amount of transmission (mean free path)
    // The SSS tint now uses the material albedo (baseColor) so no explicit
    // sss tint colour is required.
    double sss_strength = 0.0;
    double sss_scale = 1.0;
    // Dipole/diffusion parameters
    SSSModel sss_model = SSS_SINGLE_SCATTER;
    int      sss_samples = 4;        // number of exit-point samples for dipole
    double   sss_radius  = 1.0;      // mean free path / diffusion radius
    double   sss_eta     = 1.3;      // relative index (not yet used)
    bool     sss_color_override = false;
    colour   sss_color_override_col = colour(1.0, 1.0, 1.0);
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
        colour base, f0;
        vec3 n, geometry;
        double metallic, alpha, specular_probability;
        bool smooth;
    };
    surface prepare_surface(const hit_record& rec) const {
        surface s; double rough;
        sample_surface_params(rec,s.base,s.metallic,rough);
        s.base = pbr::reflectance(s.base);
        s.f0 = (1-s.metallic)*pbr::reflectance(dielectric_F0)+s.metallic*s.base;
        s.alpha = perceptual_to_alpha(rough); s.smooth = s.alpha < 1e-4;
        s.specular_probability = specular_sampling_probability(s.f0,s.metallic);
        s.geometry = safe_unit_vector(rec.geometry_normal());
        s.n = safe_unit_vector(rec.normal,s.geometry);
        if (normal_tex) {
            hit_record frame = rec; frame.normal = s.n; frame.set_tangent_frame(rec.tangent,rec.bitangent);
            colour texel = normal_tex->value(rec.u,rec.v,rec.p);
            vec3 tangent((2*texel.x()-1)*normal_strength,(2*texel.y()-1)*normal_strength,2*texel.z()-1);
            vec3 mapped = safe_unit_vector(tangent.x()*frame.tangent+tangent.y()*frame.bitangent+tangent.z()*s.n,s.n);
            if (dot(mapped,s.geometry) > 0) s.n = mapped;
        }
        return s;
    }
    static bool above_surface(const surface& s, const vec3& v, const vec3& l) {
        return dot(s.geometry,v) > 0 && dot(s.geometry,l) > 0 && dot(s.n,v) > 0 && dot(s.n,l) > 0;
    }
    static colour evaluate_surface(const surface& s, const vec3& v, const vec3& l) {
        if (!above_surface(s,v,l)) return colour(0,0,0);
        double nv = std::clamp(dot(s.n,v),0.0,1.0), nl = std::clamp(dot(s.n,l),0.0,1.0);
        // A diffuse substrate loses light on both entry and exit through its
        // dielectric interface. Do not add a full Lambert lobe on top of GGX.
        colour transmission_v = colour(1,1,1)-schlick_fresnel(nv,s.f0);
        colour transmission_l = colour(1,1,1)-schlick_fresnel(nl,s.f0);
        colour f = s.base*((1-s.metallic)/pi)*transmission_v*transmission_l;
        if (!s.smooth) {
            vec3 h = safe_unit_vector(v+l,s.n);
            double d = pbr::distribution(std::clamp(dot(s.n,h),0.0,1.0),s.alpha);
            f += schlick_fresnel(dot(v,h),s.f0)*(d*pbr::visibility(nv,nl,s.alpha));
        }
        return f;
    }
    static double surface_pdf(const surface& s, const vec3& v, const vec3& l) {
        if (!above_surface(s,v,l)) return 0;
        double nv = dot(s.n,v), nl = dot(s.n,l);
        double pdf = (1-s.specular_probability)*nl/pi;
        if (!s.smooth) {
            vec3 h = safe_unit_vector(v+l,s.n);
            pdf += s.specular_probability*pbr::distribution(std::clamp(dot(s.n,h),0.0,1.0),s.alpha)
                *pbr::masking(nv,s.alpha)/(4*nv);
        }
        return pdf;
    }

  public:
    // Exposed deterministic variates support reproducible sampler validation.
    bool sample_surface(const ray& incoming, const hit_record& rec, double lobe, double u, double v, bsdf_sample& sample) const {
        sample = {};
        surface s = prepare_surface(rec);
        vec3 view = safe_unit_vector(-incoming.direction());
        if (dot(s.geometry,view) <= 0 || dot(s.n,view) <= 0) return false;
        pbr::frame frame(s.n);
        vec3 direction;
        if (lobe < s.specular_probability) {
            if (s.smooth) {
                direction = reflect(-view,s.n);
                if (!above_surface(s,view,direction)) return false;
                sample.delta = true; sample.pdf = 0;
                sample.weight = schlick_fresnel(dot(view,s.n),s.f0)/s.specular_probability;
                sample.scattered = ray(rec.p,direction,incoming.time());
                return true;
            }
            vec3 h = frame.world(pbr::visible_normal(frame.local(view),s.alpha,u,v));
            direction = reflect(-view,h);
        } else direction = frame.world(sample_cosine_hemisphere(u,v));
        // Samples below the real surface are absorption events. Redirecting or
        // retrying them would change the distribution without changing its PDF.
        if (!above_surface(s,view,direction)) return false;
        sample.pdf = surface_pdf(s,view,direction);
        if (sample.pdf <= 0) return false;
        sample.weight = evaluate_surface(s,view,direction)*(dot(s.n,direction)/sample.pdf);
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
        if (!sample_bsdf(incoming,rec,sample)) return false;
        attenuation = sample.weight; scattered = sample.scattered; return true;
    }
    colour shade_direct(const hit_record& rec, const vec3& view, const vec3& light, const colour& radiance,
                        const hittable&, double visibility = 1) const override {
        if (opacity_at(rec) <= 0) return colour(0,0,0);
        auto s = prepare_surface(rec); vec3 v = safe_unit_vector(view), l = safe_unit_vector(light);
        return evaluate_surface(s,v,l)*radiance*(std::max(0.0,dot(s.n,l))*visibility);
    }
    double bsdf_pdf(const hit_record& rec, const vec3& view, const vec3& light) const override {
        if (opacity_at(rec) <= 0) return 0;
        return surface_pdf(prepare_surface(rec),safe_unit_vector(view),safe_unit_vector(light));
    }

    // pbr_material: SSS-only shading hook (called by renderer after BRDF)
    colour shade_sss(const hit_record& rec, const vec3& V, const vec3& Ldir, const colour& Li, const hittable& world, double vis = 1.0) const override {
        if (sss_strength <= 0.0 || sss_model == SSS_NONE) return colour(0,0,0);

        double alpha_sss = 1.0;
        if (alpha_tex) alpha_sss = alpha_tex->mask_alpha_at(rec.u, rec.v, rec.p);
        else if (base_tex) alpha_sss = base_tex->alpha_at(rec.u, rec.v, rec.p);
        if ((alpha_double_sided || rec.front_face) && (alpha_sss <= 0.0 || alpha_sss < alpha_cutoff)) {
            return colour(0,0,0);
        }

        vec3 L = unit_vector(Ldir);
        vec3 Vn = unit_vector(V);
        double NdotL_geom = dot(rec.normal, L);
        double NdotV_geom = std::max(0.0, dot(rec.normal, Vn));
        colour sss_tint = sss_color_override ? sss_color_override_col : albedo(rec);

        colour baseColor_dummy(1,1,1);
        double metallic = 0.0;
        double rough_dummy = 0.5;
        sample_surface_params(rec, baseColor_dummy, metallic, rough_dummy);

        double strength = sss_strength * (1.0 - metallic);
        strength *= vis; // Opacity is sampled once by the integrator.
        if (strength <= 0.0) return colour(0,0,0);

        auto transmission_tint = [&](double optical_depth, const colour& channel_bias) {
            optical_depth = std::max(0.0, optical_depth);
            return colour(
                std::pow(std::clamp(sss_tint.x(), 0.02, 0.999), optical_depth * channel_bias.x()),
                std::pow(std::clamp(sss_tint.y(), 0.02, 0.999), optical_depth * channel_bias.y()),
                std::pow(std::clamp(sss_tint.z(), 0.02, 0.999), optical_depth * channel_bias.z())
            );
        };

        auto estimate_thickness = [&](double fallback, double& thickness_ws) {
            thickness_ws = fallback;
            vec3 inside_dir = -L;
            ray into_ray(rec.p + inside_dir * 0.001, inside_dir, 0.0);
            hit_record exit_rec;
            double max_distance = std::max(0.05, sss_radius * 6.0);
            if (!world.hit(into_ray, interval(0.001, max_distance), exit_rec)) return false;
            thickness_ws = std::max(0.0, (exit_rec.p - rec.p).length());
            return thickness_ws > 1e-6;
        };

        if (sss_model == SSS_FOLIAGE) {
            double thickness_ws = std::max(0.05, sss_radius);
            estimate_thickness(thickness_ws, thickness_ws);
            double optical_depth = thickness_ws / std::max(0.05, sss_scale);
            colour trans_color = transmission_tint(optical_depth, colour(0.75, 1.0, 1.25));

            double lambert = clamp01(NdotL_geom);
            double wrapped = std::clamp((NdotL_geom + 0.35) / 1.35, 0.0, 1.0);
            double wrap_gain = std::max(0.0, wrapped - lambert);
            double view_through = clamp01(dot(-Vn, L));
            double back_scatter = clamp01(-NdotL_geom) * (0.35 + 0.65 * view_through);
            double scatter_term = wrap_gain * 0.35 + back_scatter;
            return trans_color * Li * (float)(strength * scatter_term);
        }

        if (sss_model == SSS_SKIN) {
            double thickness_ws = std::max(0.05, sss_radius);
            estimate_thickness(thickness_ws, thickness_ws);
            double optical_depth = thickness_ws / std::max(0.05, sss_scale);
            colour back_color = transmission_tint(optical_depth, colour(0.45, 1.0, 2.0));

            double lambert = clamp01(NdotL_geom);
            double wrapped = std::clamp((NdotL_geom + 0.45) / 1.45, 0.0, 1.0);
            double forward_scatter = std::max(0.0, wrapped - lambert);
            double back_scatter = clamp01(-NdotL_geom) * NdotV_geom;
            colour scatter_color =
                sss_tint * (float)(forward_scatter * 0.75) +
                back_color * (float)(back_scatter * 0.5);
            return scatter_color * Li * (float)strength;
        }

        if (sss_model == SSS_DIPOLE_BURLEY) {
            double NdotL = clamp01(NdotL_geom);
            if (NdotL <= 0.0) return colour(0,0,0);

            colour sss_acc(0,0,0);
            int ns = std::max(1, sss_samples);
            vec3 T = rec.tangent;
            vec3 B = rec.bitangent;
            for (int si = 0; si < ns; ++si) {
                double u1 = random_double();
                double u2 = random_double();
                double r = sss::sample_r_exponential(sss_radius, u1);
                double theta = 2.0 * pi * u2;
                double dx = r * std::cos(theta);
                double dy = r * std::sin(theta);

                point3 exit_probe = rec.p + T * (float)dx + B * (float)dy;
                point3 ray_origin = exit_probe + rec.normal * (float)(sss_radius * 0.5);
                ray probe_ray(ray_origin, -rec.normal, 0.0);
                hit_record exit_rec;
                if (!world.hit(probe_ray, interval(0.001, sss_radius * 2.0), exit_rec)) continue;

                double scatter_dist = (exit_rec.p - rec.p).length();
                double trans = std::exp(-scatter_dist / std::max(1e-6, sss_radius));
                double Rd = sss::burley_Rd(r, std::max(0.1, luminance(sss_tint)), sss_radius);
                double pdf_area = sss::pdf_area_from_radius(r, sss_radius);
                if (pdf_area <= 1e-9) continue;

                double NdotV_exit = std::max(0.0, dot(exit_rec.normal, Vn));
                if (NdotV_exit <= 0.0) continue;

                colour contrib = sss_tint * (float)(strength * trans * Rd / pdf_area)
                                 * Li * (float)NdotL * (float)NdotV_exit;
                sss_acc += contrib;
            }
            return sss_acc * (float)(1.0 / std::max(1, sss_samples));
        }

        double thickness_ws = std::max(0.05, sss_radius);
        estimate_thickness(thickness_ws, thickness_ws);
        double optical_depth = thickness_ws / std::max(0.05, sss_scale);
        colour trans_color = transmission_tint(optical_depth, colour(0.65, 1.0, 1.4));
        double wrapped = std::clamp((NdotL_geom + 0.3) / 1.3, 0.0, 1.0);
        double back_scatter = clamp01(-NdotL_geom);
        double scatter_term = (sss_model == SSS_MULTI_SINGLE_SCATTER)
            ? (wrapped * 0.55 + back_scatter * 0.85)
            : (wrapped * 0.35 + back_scatter * 0.75);
        return trans_color * Li * (float)(strength * scatter_term);
    }

    bool is_masked_transparent(const hit_record& rec) const override {
        double opacity = opacity_at(rec);
        return opacity <= 0 || (opacity < 1 && random_double() >= opacity);
    }
    double opacity_at(const hit_record& rec) const override {
        if (!alpha_double_sided && !rec.front_face) return 1;
        double a = alpha_tex ? alpha_tex->mask_alpha_at(rec.u,rec.v,rec.p)
            : base_tex ? base_tex->alpha_at(rec.u,rec.v,rec.p) : 1;
        return a < alpha_cutoff ? 0 : clamp01(a);
    }
};

#endif // MATERIAL_H
