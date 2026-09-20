#ifndef COLOUR_H
#define COLOUR_H

#include <iostream>
#include <cmath>

#include "vec3.h"

// Alias
using colour = vec3;

// Linear-light to sRGB display encoding (legacy function name retained).
inline double linear_to_gamma(double linear_component) {
    if (!std::isfinite(linear_component) || linear_component <= 0) return 0;
    return linear_component <= .0031308 ? 12.92*linear_component
        : 1.055*std::pow(linear_component,1/2.4)-.055;
}

// Simple clamp to [0.0, 0.999]
// Clamp to [0.0, 0.999], but handle NaN/Inf safely by returning 0.
inline double clamp01_999(double x) {
    if (!std::isfinite(x)) return 0.0;
    if (x < 0.0)   return 0.0;
    if (x > 0.999) return 0.999;
    return x;
}

// Write a colour to an output stream with gamma correction and [0,255] clamp
inline void write_colour(std::ostream& out, const colour& pixel_colour) {
    auto r = pixel_colour.x();
    auto g = pixel_colour.y();
    auto b = pixel_colour.z();

    // Sanitize components (replace NaN/Inf/negatives and clamp extremes),
    // then gamma-correct from linear space.
    r = linear_to_gamma(r);
    g = linear_to_gamma(g);
    b = linear_to_gamma(b);

    int ir = static_cast<int>(256 * clamp01_999(r));
    int ig = static_cast<int>(256 * clamp01_999(g));
    int ib = static_cast<int>(256 * clamp01_999(b));

    out << ir << ' ' << ig << ' ' << ib << '\n';
}

#endif // COLOUR_H