#pragma once

// Isotropic GGX with perceptual roughness alpha = roughness^2. Evaluation,
// sampling and PDFs share these definitions (Heitz 2018; PBRT 4e, chapter 9).
namespace pbr {
inline double distribution(double nh, double alpha) {
    if (nh <= 0 || alpha <= 0) return 0;
    double a2 = alpha*alpha;
    double d = (1-nh*nh) + a2*nh*nh;
    return a2/(pi*d*d);
}
inline double smith_root(double cosine, double alpha) {
    return std::sqrt(alpha*alpha + (1-alpha*alpha)*cosine*cosine);
}
inline double masking(double cosine, double alpha) {
    return cosine > 0 ? 2*cosine/(cosine+smith_root(cosine,alpha)) : 0;
}
inline double visibility(double nv, double nl, double alpha) {
    return nv > 0 && nl > 0 ? .5/(nl*smith_root(nv,alpha)+nv*smith_root(nl,alpha)) : 0;
}
inline vec3 visible_normal(const vec3& view, double alpha, double u, double v) {
    vec3 stretched = safe_unit_vector(vec3(alpha*view.x(),alpha*view.y(),view.z()));
    vec3 t = safe_unit_vector(vec3(-stretched.y(),stretched.x(),0),vec3(1,0,0));
    vec3 b = cross(stretched,t);
    double r = std::sqrt(u), phi = 2*pi*v;
    double x = r*std::cos(phi), y = r*std::sin(phi), s = .5*(1+stretched.z());
    y = (1-s)*std::sqrt(std::max(0.0,1-x*x))+s*y;
    vec3 n = x*t+y*b+std::sqrt(std::max(0.0,1-x*x-y*y))*stretched;
    return safe_unit_vector(vec3(alpha*n.x(),alpha*n.y(),std::max(0.0,n.z())));
}
struct frame {
    vec3 n, t, b;
    explicit frame(const vec3& normal) : n(normal) {
        vec3 axis = std::abs(n.z()) < .999 ? vec3(0,0,1) : vec3(0,1,0);
        t = safe_unit_vector(cross(axis,n)); b = cross(n,t);
    }
    vec3 local(const vec3& v) const { return vec3(dot(v,t),dot(v,b),dot(v,n)); }
    vec3 world(const vec3& v) const { return v.x()*t+v.y()*b+v.z()*n; }
};
inline colour reflectance(const colour& c) {
    colour result;
    for (int i = 0; i < 3; ++i) result[i] = std::isfinite(c[i]) ? std::clamp(c[i],0.0,1.0) : 0;
    return result;
}
}
