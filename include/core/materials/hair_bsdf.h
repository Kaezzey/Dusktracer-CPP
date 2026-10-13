#pragma once

#include "../dusktracer.h"

#include <algorithm>
#include <array>
#include <cmath>

// Fibre scattering of Chiang, Bitterli, Tappan and Burley (2016), "A Practical
// and Controllable Hair and Fur Model for Production Path Tracing", following
// pbrt-v3's HairBSDF. It is the model behind Cycles' Principled Hair BSDF and
// the physically based counterpart of Unreal's Hair shading model.
//
// The local frame has x along the fibre (root to tip) and y, z spanning its
// normal plane. Lobe p = 0 is R (the white primary highlight), 1 is TT (light
// through the fibre: the soft backlit glow), 2 is TRT (the coloured secondary
// highlight) and 3 sums the remaining internal bounces. f() returns the
// scattering function including its projected-area factor, which is the
// renderers' "BSDF times cosine": it integrates to one over the sphere for a
// fibre without absorption and unit lobe weights.
//
// pathtrace.comp (hairF/hairPDF/sampleHair) mirrors this file in FP32.
namespace hair {
constexpr int p_max = 3;
constexpr double tau = 6.283185307179586476925;
constexpr double half_pi = 1.570796326794896619231;

struct parameters {
    double h = 0;               // offset across the fibre width, -1..1
    double eta = 1.55;          // cuticle index of refraction
    colour sigma_a;             // absorption per fibre diameter
    double beta_m = .3;         // longitudinal roughness
    double beta_n = .3;         // azimuthal (radial) roughness
    double alpha = 2;           // cuticle scale tilt in degrees
    double r_weight = 1;        // primary highlight scale (Unreal Specular x 2)
    double tt_weight = 1;       // transmission scale for every event, including sampling
};

inline double square(double x) { return x * x; }
inline double safe_sqrt(double x) { return std::sqrt(std::max(0.0, x)); }
inline double safe_asin(double x) { return std::asin(std::clamp(x, -1.0, 1.0)); }

// Modified Bessel function of the first kind, order zero.
inline double bessel_i0(double x) {
    double value = 0, x2i = 1, factorial = 1, four = 1;
    for (int i = 0; i < 10; ++i) {
        if (i > 1) factorial *= i;
        value += x2i / (four * square(factorial));
        x2i *= x * x;
        four *= 4;
    }
    return value;
}
inline double log_bessel_i0(double x) {
    if (x > 12) return x + .5 * (-std::log(tau) + std::log(1 / x) + 1 / (8 * x));
    return std::log(bessel_i0(x));
}
// Longitudinal scattering (d'Eon et al. 2011), stable for small variance.
inline double longitudinal(double cos_i, double cos_o, double sin_i, double sin_o, double v) {
    double a = cos_i * cos_o / v, b = sin_i * sin_o / v;
    return v <= .1 ? std::exp(log_bessel_i0(a) - b - 1 / v + .6931 + std::log(1 / (2 * v)))
                   : std::exp(-b) * bessel_i0(a) / (std::sinh(1 / v) * 2 * v);
}
// Unpolarized Fresnel reflectance entering a dielectric of relative IOR eta.
inline double fresnel(double cos_i, double eta) {
    cos_i = std::clamp(cos_i, -1.0, 1.0);
    double eta_i = 1, eta_t = eta;
    if (cos_i < 0) {
        std::swap(eta_i, eta_t);
        cos_i = -cos_i;
    }
    double sin_t = eta_i / eta_t * safe_sqrt(1 - cos_i * cos_i);
    if (sin_t >= 1) return 1;
    double cos_t = safe_sqrt(1 - sin_t * sin_t);
    double parallel = (eta_t * cos_i - eta_i * cos_t) / (eta_t * cos_i + eta_i * cos_t);
    double perpendicular = (eta_i * cos_i - eta_t * cos_t) / (eta_i * cos_i + eta_t * cos_t);
    return .5 * (parallel * parallel + perpendicular * perpendicular);
}
inline double logistic(double x, double s) {
    x = std::abs(x);
    return std::exp(-x / s) / (s * square(1 + std::exp(-x / s)));
}
inline double logistic_cdf(double x, double s) { return 1 / (1 + std::exp(-x / s)); }
inline double trimmed_logistic(double x, double s, double a, double b) {
    return logistic(x, s) / (logistic_cdf(b, s) - logistic_cdf(a, s));
}
inline double sample_trimmed_logistic(double u, double s, double a, double b) {
    double k = logistic_cdf(b, s) - logistic_cdf(a, s);
    double x = -s * std::log(1 / (u * k + logistic_cdf(a, s)) - 1);
    return std::clamp(x, a, b);
}
inline double phi(int p, double gamma_o, double gamma_t) { return 2 * p * gamma_t - 2 * gamma_o + p * pi; }
inline double azimuthal(double angle, int p, double s, double gamma_o, double gamma_t) {
    double d = angle - phi(p, gamma_o, gamma_t);
    d = std::remainder(d, tau);
    return trimmed_logistic(d, s, -pi, pi);
}
// Colour-to-absorption fit from Chiang et al. so Base Color is the fibre's
// multiple-scattering colour, as artists expect from Unreal and Cycles.
inline colour absorption_from_colour(const colour& c, double beta_n) {
    double scale = 5.969 - .215 * beta_n + 2.532 * square(beta_n) - 10.73 * std::pow(beta_n, 3) +
                   5.574 * std::pow(beta_n, 4) + .245 * std::pow(beta_n, 5);
    colour sigma;
    for (int k = 0; k < 3; ++k) {
        double value = std::isfinite(c[k]) ? std::clamp(c[k], 1e-4, 1.0) : 1;
        sigma[k] = square(std::log(value) / scale);
    }
    return sigma;
}

// Unreal's convention: Base Color is one fibre's colour. A ray through the
// fibre centre (path length 2 in this model) transmits sqrt(colour), and a TRT
// path transmits the colour itself.
inline colour absorption_from_fibre_colour(const colour& c) {
    colour sigma;
    for (int k = 0; k < 3; ++k) sigma[k] = -std::log(std::isfinite(c[k]) ? std::clamp(c[k], 1e-4, 1.0) : 1) / 4;
    return sigma;
}

class bsdf {
public:
    explicit bsdf(const parameters& p) : h(std::clamp(p.h, -1.0, 1.0)), eta(std::max(1.0001, p.eta)), sigma_a(p.sigma_a) {
        // Below about 0.02 the lobes are narrower than double precision resolves.
        double beta_m = std::clamp(p.beta_m, .02, 1.0), beta_n = std::clamp(p.beta_n, .02, 1.0);
        gamma_o = safe_asin(h);
        v[0] = square(.726 * beta_m + .812 * square(beta_m) + 3.7 * std::pow(beta_m, 20));
        v[1] = .25 * v[0];
        v[2] = 4 * v[0];
        v[3] = v[2];
        s = std::sqrt(pi / 8) * (.265 * beta_n + 1.194 * square(beta_n) + 5.372 * std::pow(beta_n, 22));
        sin_2k[0] = std::sin(p.alpha * pi / 180);
        cos_2k[0] = safe_sqrt(1 - square(sin_2k[0]));
        for (int i = 1; i < 3; ++i) {
            sin_2k[i] = 2 * cos_2k[i - 1] * sin_2k[i - 1];
            cos_2k[i] = square(cos_2k[i - 1]) - square(sin_2k[i - 1]);
        }
        weights = {std::max(0.0, p.r_weight), std::max(0.0, p.tt_weight), 1.0, 1.0};
    }

    // tt_scale additionally scales TT in this evaluation only; the hair
    // material uses it for Backlit, which dims direct light through strands
    // without removing the light that crosses hair between bounces.
    colour f(const vec3& wo, const vec3& wi, double tt_scale = 1) const {
        angles a(wo, wi);
        fibre t = transmission(a.sin_o, a.cos_o);
        auto ap = attenuation(a.cos_o, t.absorbed);
        ap[1] *= std::max(0.0, tt_scale);
        double angle = a.phi_i - a.phi_o;
        colour sum(0, 0, 0);
        for (int p = 0; p < p_max; ++p) {
            double sin_op, cos_op;
            tilt(p, a.sin_o, a.cos_o, sin_op, cos_op);
            sum += ap[p] * (longitudinal(a.cos_i, cos_op, a.sin_i, sin_op, v[p]) *
                            azimuthal(angle, p, s, gamma_o, t.gamma_t));
        }
        sum += ap[p_max] * (longitudinal(a.cos_i, a.cos_o, a.sin_i, a.sin_o, v[p_max]) / tau);
        for (int k = 0; k < 3; ++k) sum[k] = std::isfinite(sum[k]) ? std::max(0.0, sum[k]) : 0;
        return sum;
    }

    double pdf(const vec3& wo, const vec3& wi) const {
        angles a(wo, wi);
        fibre t = transmission(a.sin_o, a.cos_o);
        auto lobe = lobe_pdf(a.cos_o, t.absorbed);
        double angle = a.phi_i - a.phi_o, density = 0;
        for (int p = 0; p < p_max; ++p) {
            double sin_op, cos_op;
            tilt(p, a.sin_o, a.cos_o, sin_op, cos_op);
            density += lobe[p] * longitudinal(a.cos_i, cos_op, a.sin_i, sin_op, v[p]) *
                       azimuthal(angle, p, s, gamma_o, t.gamma_t);
        }
        density += lobe[p_max] * longitudinal(a.cos_i, a.cos_o, a.sin_i, a.sin_o, v[p_max]) / tau;
        return std::isfinite(density) ? std::max(0.0, density) : 0;
    }

    // Samples a lobe by its attenuation, then the longitudinal and azimuthal
    // terms exactly; u holds four uniform variates.
    bool sample(const vec3& wo, const double u[4], vec3& wi, colour& weight, double& density) const {
        double sin_o = std::clamp(wo.x(), -1.0, 1.0), cos_o = safe_sqrt(1 - sin_o * sin_o);
        double phi_o = std::atan2(wo.z(), wo.y());
        fibre t = transmission(sin_o, cos_o);
        auto lobe = lobe_pdf(cos_o, t.absorbed);
        int p = 0;
        double pick = u[0];
        for (; p < p_max; ++p) {
            if (pick < lobe[p]) break;
            pick -= lobe[p];
        }
        double sin_op = sin_o, cos_op = cos_o;
        if (p < p_max) tilt(p, sin_o, cos_o, sin_op, cos_op);
        double um = std::max(u[2], 1e-5);
        double cos_theta = 1 + v[p] * std::log(um + (1 - um) * std::exp(-2 / v[p]));
        double sin_theta = safe_sqrt(1 - square(cos_theta)), cos_phi = std::cos(tau * u[3]);
        double sin_i = std::clamp(-cos_theta * sin_op + sin_theta * cos_phi * cos_op, -1.0, 1.0);
        double cos_i = safe_sqrt(1 - sin_i * sin_i);
        double angle = p < p_max ? phi(p, gamma_o, t.gamma_t) + sample_trimmed_logistic(u[1], s, -pi, pi) : tau * u[1];
        double phi_i = phi_o + angle;
        wi = vec3(sin_i, cos_i * std::cos(phi_i), cos_i * std::sin(phi_i));
        density = pdf(wo, wi);
        if (!(density > 0)) return false;
        weight = f(wo, wi) / density;
        return true;
    }

private:
    struct angles {
        double sin_o, cos_o, phi_o, sin_i, cos_i, phi_i;
        angles(const vec3& wo, const vec3& wi) {
            sin_o = std::clamp(wo.x(), -1.0, 1.0);
            cos_o = safe_sqrt(1 - sin_o * sin_o);
            phi_o = std::atan2(wo.z(), wo.y());
            sin_i = std::clamp(wi.x(), -1.0, 1.0);
            cos_i = safe_sqrt(1 - sin_i * sin_i);
            phi_i = std::atan2(wi.z(), wi.y());
        }
    };
    struct fibre {
        colour absorbed;
        double gamma_t;
    };
    // Refraction into the fibre and absorption along one internal path.
    fibre transmission(double sin_o, double cos_o) const {
        double sin_t = sin_o / eta, cos_t = safe_sqrt(1 - square(sin_t));
        double etap = safe_sqrt(eta * eta - square(sin_o)) / std::max(cos_o, 1e-8);
        double sin_gamma_t = h / etap, cos_gamma_t = safe_sqrt(1 - square(sin_gamma_t));
        fibre result;
        result.gamma_t = safe_asin(sin_gamma_t);
        double length = 2 * cos_gamma_t / std::max(cos_t, 1e-8);
        for (int k = 0; k < 3; ++k) result.absorbed[k] = std::exp(-sigma_a[k] * length);
        return result;
    }
    std::array<colour, p_max + 1> attenuation(double cos_o, const colour& transmitted) const {
        std::array<colour, p_max + 1> ap;
        double cos_gamma_o = safe_sqrt(1 - h * h);
        double r = fresnel(cos_o * cos_gamma_o, eta);
        ap[0] = colour(r, r, r);
        ap[1] = square(1 - r) * transmitted;
        for (int p = 2; p < p_max; ++p) ap[p] = ap[p - 1] * transmitted * r;
        for (int k = 0; k < 3; ++k) {
            double denominator = 1 - transmitted[k] * r;
            ap[p_max][k] = denominator > 1e-12 ? ap[p_max - 1][k] * r * transmitted[k] / denominator : 0;
        }
        for (int p = 0; p <= p_max; ++p) ap[p] *= weights[p];
        return ap;
    }
    std::array<double, p_max + 1> lobe_pdf(double cos_o, const colour& transmitted) const {
        auto ap = attenuation(cos_o, transmitted);
        std::array<double, p_max + 1> result{};
        double sum = 0;
        for (int p = 0; p <= p_max; ++p) sum += result[p] = .2126 * ap[p].x() + .7152 * ap[p].y() + .0722 * ap[p].z();
        for (auto& value : result) value = sum > 0 ? value / sum : 1.0 / (p_max + 1);
        return result;
    }
    // Rotates the outgoing elevation by the cuticle tilt for each lobe: R
    // shifts by -2 alpha, TT by alpha and TRT by 4 alpha.
    void tilt(int p, double sin_o, double cos_o, double& sin_op, double& cos_op) const {
        if (p == 0) {
            sin_op = sin_o * cos_2k[1] - cos_o * sin_2k[1];
            cos_op = cos_o * cos_2k[1] + sin_o * sin_2k[1];
        } else if (p == 1) {
            sin_op = sin_o * cos_2k[0] + cos_o * sin_2k[0];
            cos_op = cos_o * cos_2k[0] - sin_o * sin_2k[0];
        } else if (p == 2) {
            sin_op = sin_o * cos_2k[2] + cos_o * sin_2k[2];
            cos_op = cos_o * cos_2k[2] - sin_o * sin_2k[2];
        } else {
            sin_op = sin_o;
            cos_op = cos_o;
        }
        cos_op = std::abs(cos_op);
    }

    double h, eta, gamma_o, s;
    colour sigma_a;
    std::array<double, p_max + 1> v{}, weights{};
    std::array<double, 3> sin_2k{}, cos_2k{};
};
} // namespace hair
