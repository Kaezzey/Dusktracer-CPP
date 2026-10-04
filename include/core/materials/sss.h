#ifndef SSS_H
#define SSS_H

#include "../vec3.h"
#include <cmath>
#include <algorithm>

namespace sss {
constexpr double profile_pi = 3.14159265358979323846;
// Normalized Burley diffusion: integral 2*pi*r*Rd(r) dr = albedo.
// d is a world-space diffusion distance, not a brightness multiplier.
inline double burley_Rd(double r, double albedo, double d) {
    d = std::max(1e-6, d); r = std::max(1e-12, r);
    return std::clamp(albedo, 0.0, 1.0) *
        (std::exp(-r/d) + std::exp(-r/(3*d))) / (8*profile_pi*d*r);
}
inline double burley_pdf_area(double r, double d) { return burley_Rd(r, 1, d); }
inline double sample_burley_radius(double d, double u) {
    d = std::max(1e-6, d); u = std::clamp(u, 0.0, 1.0-1e-12);
    // Radial density is a 1/4 : 3/4 mixture of exponentials.
    return u < .25 ? -d*std::log1p(-4*u) : -3*d*std::log1p(-(u-.25)/.75);
}
inline double burley_cdf(double r, double d) {
    d = std::max(1e-6, d);
    return 1-.25*std::exp(-r/d)-.75*std::exp(-r/(3*d));
}
// Exact dielectric entry transmission, normalized over the incident hemisphere.
inline double entry_transmission(double cosine, double eta) {
    cosine = std::clamp(cosine, 0.0, 1.0);
    eta = std::clamp(eta, 1.0, 4.0);
    if (eta == 1) return 1;
    double transmitted = std::sqrt(1-(1-cosine*cosine)/(eta*eta));
    double rp = (eta*cosine-transmitted)/(eta*cosine+transmitted);
    double rs = (cosine-eta*transmitted)/(cosine+eta*transmitted);
    return 1-.5*(rp*rp+rs*rs);
}
inline double entry_directional(double cosine, double eta) {
    if (cosine <= 0) return 0;
    eta = std::clamp(eta, 1.0, 4.0);
    thread_local double cached_eta = -1, normalization = 1;
    if (cached_eta != eta) {
        normalization = 0;
        for (int i = 0; i < 128; ++i) {
            double mu = (i+.5)/128;
            normalization += 2*mu*entry_transmission(mu, eta)/128;
        }
        cached_eta = eta;
    }
    return entry_transmission(cosine, eta)/(normalization*profile_pi);
}
} // namespace sss
#endif
