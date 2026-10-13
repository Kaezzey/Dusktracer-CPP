#ifndef EYE_H
#define EYE_H

// Unreal's Eye shading model, shared by the CPU tracer and mirrored by the
// Vulkan tracer (pathtrace.comp: irisAt) and the viewport (raster_shaders.h).
//
// The eye mesh is the eyeball surface, as in game assets. The iris is a disc
// in UV space (centre and radius) whose mask fades to the sclera across the
// limbus. Inside it:
// - the base colour is looked up where the view ray, refracted by the cornea,
//   meets the iris plane Iris Distance below the surface (parallax refraction,
//   as Unreal's EyeRefraction material function does);
// - diffuse light follows Unreal's EyeBxDF: the iris is lit through its own
//   plane normal (the optical axis), times a caustic lobe that brightens the
//   side away from the light, as the cornea focuses light there.
// The sclera is plain diffuse. Both sit under a GGX cornea (the PBR lobes).

#include <algorithm>
#include <cmath>

#include "../vec3.h"

namespace eye_shading {

struct iris_params {
    double u = .5, v = .5;   // iris centre in UV space
    double radius = .15;     // UV units
    double limbus = .03;     // UV width of the soft edge
    double depth = .5;       // iris radii below the cornea at the centre
    double eta = 1.336;      // cornea / aqueous humour
};

struct iris_geometry {
    double u = 0, v = 0;  // base-colour lookup seen through the cornea
    double mask = 0;      // Iris Mask: 1 iris, 0 sclera
    vec3 normal;          // iris plane normal (the optical axis)
    vec3 caustic;         // concave caustic normal
};

// 1 inside the iris, 0 outside, smooth across the limbus.
inline double iris_mask(double distance, double radius, double limbus) {
    const double width = std::max(limbus, 1e-6);
    const double x = std::clamp((radius + .5 * width - distance) / width, 0.0, 1.0);
    return x * x * (3 - 2 * x);
}

// t, b, n: the surface tangent frame, n the smooth outward normal (t along
// increasing U, b along increasing V); view points to the viewer. Negative
// pins mean unconnected: Iris Mask then comes from the UV disc, and Iris
// Distance (iris radii) from a parabolic anterior chamber, depth (1 - r^2).
inline iris_geometry iris(const iris_params& p, double u, double v, const vec3& t, const vec3& b, const vec3& n,
                          const vec3& view, double mask_pin = -1, double depth_pin = -1) {
    iris_geometry g;
    const double du = u - p.u, dv = v - p.v, distance = std::sqrt(du * du + dv * dv);
    const double radius = std::max(p.radius, 1e-6), r = distance / radius;
    g.mask = mask_pin >= 0 ? std::min(mask_pin, 1.0) : iris_mask(distance, radius, p.limbus);
    const double depth = depth_pin >= 0 ? depth_pin : std::max(0.0, p.depth) * std::max(0.0, 1 - r * r);
    // Refract into the cornea in tangent space and follow the ray down to the
    // iris. Depth is in UV units here, so the parallax is independent of scale.
    const vec3 local(dot(view, t), dot(view, b), dot(view, n));
    const double eta = 1 / std::max(p.eta, 1.0), c = std::max(local.z(), 0.0);
    const double k = std::max(0.0, 1 - eta * eta * (1 - c * c));
    const vec3 refracted = -eta * local + (eta * c - std::sqrt(k)) * vec3(0, 0, 1);
    const double travel = depth * radius / std::max(-refracted.z(), .1);
    g.u = u + g.mask * refracted.x() * travel;
    g.v = v + g.mask * refracted.y() * travel;
    // The eyeball surface tilts away from the optical axis by about asin(r/2)
    // across the iris: the iris radius is about half the eyeball's.
    const vec3 radial = distance > 1e-9 ? unit_vector(du * t + dv * b) : vec3(0, 0, 0);
    const double s = .5 * std::min(r, 1.0);
    g.normal = unit_vector(std::sqrt(1 - s * s) * n - s * radial);
    // Unreal: lerp(IrisNormal, -N, IrisMask * IrisDistance) for concavity.
    const double concave = std::min(.45, .5 * g.mask * std::min(depth, 1.0));
    const vec3 caustic = (1 - concave) * g.normal - concave * n;
    g.caustic = caustic.length_squared() > 1e-12 ? unit_vector(caustic) : g.normal;
    return g;
}

// Unreal's iris term in place of N.L. Unreal's lobe reflects up to 4% more
// than it receives; 0.96 keeps the iris within unit albedo for path tracing.
inline double iris_cosine(const iris_geometry& g, const vec3& l) {
    const double nl = std::clamp(dot(g.normal, l), 0.0, 1.0);
    const double power = 12 + (1 - 12) * nl;
    const double caustic = .8 + .2 * (power + 1) * std::pow(std::clamp(dot(g.caustic, l), 0.0, 1.0), power);
    return .96 * nl * caustic;
}

// The diffuse cosine: the sclera follows the surface, the iris its own plane.
inline double diffuse_cosine(const iris_geometry& g, double nl, const vec3& l) {
    return g.mask > 0 ? nl + (iris_cosine(g, l) - nl) * g.mask : nl;
}

} // namespace eye_shading

#endif // EYE_H
