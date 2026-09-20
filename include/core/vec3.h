#ifndef VEC3_H
#define VEC3_H

#include <cmath>
#if defined(__SSE__)
#include <xmmintrin.h>
#endif       
#include <iostream>   
// Forward-declare RNG helpers to avoid pulling in `dusktracer.h` here
// (including dusktracer.h from here caused a circular include with colour.h).
double random_double();
double random_double(double min, double max);

class vec3 {
    public:
        double e[3];

        //constructor initializer list for e0,1,2
        vec3(double e0 = 0, double e1 = 0, double e2 = 0) : e{e0, e1, e2} {}

        //getter functions e.g v.x() easier than v.e[0]
        double x() const { return e[0]; }
        double y() const { return e[1]; }
        double z() const { return e[2]; }

        //operator overloads for -, [], +=, *=, /=
        vec3 operator-() const { return vec3(-e[0], -e[1], -e[2]); }
        double operator[](int i) const { return e[i]; }
        double& operator[](int i) { return e[i]; }

        //using & to return reference to current object to allow chaining
        vec3& operator+=(const vec3& v){
            e[0] += v.e[0];
            e[1] += v.e[1];
            e[2] += v.e[2];
            return *this;
        }

        vec3& operator*=(double t){
            e[0] *= t;
            e[1] *= t;
            e[2] *= t;
            return *this;
        }

        vec3& operator/=(double t){
            return *this *= 1/t;
        }

        //length functions
        double length() const {
            return std::sqrt(length_squared());
        }

        double length_squared() const {
            return e[0]*e[0] + e[1]*e[1] + e[2]*e[2];
        }

        bool near_zero() const {
            //return true if the vector is close to zero in all dimensions
            const auto s = 1e-8;
            return (fabs(e[0]) < s) && (fabs(e[1]) < s) && (fabs(e[2]) < s);
        }

        static vec3 random() {
        return vec3(random_double(), random_double(), random_double());
        }

        static vec3 random(double min, double max) {
            return vec3(random_double(min,max), random_double(min,max), random_double(min,max));
        }
};

//type alias for vec3
using point3 = vec3;   //3D point

//vec3 utility functions
inline std::ostream& operator<<(std::ostream& out, const vec3& v) {
    return out << v.e[0] << ' ' << v.e[1] << ' ' << v.e[2];
}

//+ operator
inline vec3 operator+(const vec3& u, const vec3& v){
    return vec3(u.e[0] + v.e[0], u.e[1] + v.e[1], u.e[2] + v.e[2]);
}

//- operator
inline vec3 operator-(const vec3& u, const vec3& v){
    return vec3(u.e[0] - v.e[0], u.e[1] - v.e[1], u.e[2] - v.e[2]);
}

//*.* operator overloads for vec3
//   vec3 * vec3 (component-wise multiplication)
//   double * vec3
//   vec3 * double

inline vec3 operator*(const vec3& u, const vec3& v){
    return vec3(u.e[0] * v.e[0], u.e[1] * v.e[1], u.e[2] * v.e[2]);
}

inline vec3 operator*(double t, const vec3& v){
    return vec3(t*v.e[0], t*v.e[1], t*v.e[2]);
}

inline vec3 operator*(const vec3& v, double t){
    return t * v;
}

inline vec3 operator/(const vec3& v, double t){
    return (1/t) * v;
}

//dot product and cross product functions
inline double dot(const vec3& u, const vec3& v){
    return u.e[0] * v.e[0]
         + u.e[1] * v.e[1]
         + u.e[2] * v.e[2];
}

inline vec3 cross(const vec3& u, const vec3& v){
    return vec3(u.e[1] * v.e[2] - u.e[2] * v.e[1],
                u.e[2] * v.e[0] - u.e[0] * v.e[2],
                u.e[0] * v.e[1] - u.e[1] * v.e[0]);
}

//unit vector function
inline vec3 unit_vector(const vec3& v){
    return v / v.length();
}

inline vec3 safe_unit_vector(const vec3& v, const vec3& fallback = vec3(0,0,1)) {
    double length2 = v.length_squared();
    return length2 > 1e-20 && std::isfinite(length2) ? v / std::sqrt(length2) : fallback;
}

// Faster approximate normalization path using SSE on floats.
// Converts to float, normalizes with SSE, converts back to double.
inline vec3 unit_vector_fast(const vec3& v) {
#if defined(__SSE__)
    float fv[4];
    fv[0] = (float)v.x(); fv[1] = (float)v.y(); fv[2] = (float)v.z(); fv[3] = 0.0f;
    __m128 m = _mm_loadu_ps(fv);
    __m128 sq = _mm_mul_ps(m, m);
    // horizontal add to get sum of squares
    __m128 t = _mm_hadd_ps(sq, sq);
    t = _mm_hadd_ps(t, t);
    float sum = _mm_cvtss_f32(t);
    if (sum <= 0.0f) return v;
    float invlen = 1.0f / sqrtf(sum);
    __m128 inv = _mm_set1_ps(invlen);
    __m128 n = _mm_mul_ps(m, inv);
    _mm_storeu_ps(fv, n);
    return vec3((double)fv[0], (double)fv[1], (double)fv[2]);
#else
    return unit_vector(v);
#endif
}

inline vec3 random_in_unit_disk() {
    while (true) {
        auto p = vec3(random_double(-1,1), random_double(-1,1), 0);
        if (p.length_squared() < 1)
            return p;
    }
}

inline vec3 random_unit_vector() {
    while (true) {
        auto p = vec3::random(-1,1);
        auto lensq = p.length_squared();
        if (1e-160 < lensq && lensq <= 1)
            return p / sqrt(lensq);
    }
}

inline vec3 random_on_hemisphere(const vec3& normal) {
    vec3 on_unit_sphere = random_unit_vector();

    //same hemisphere as normal
    if (dot(on_unit_sphere, normal) > 0.0) 
        return on_unit_sphere;
    else
        return -on_unit_sphere;
}

inline vec3 reflect(const vec3& v, const vec3& n) {
    return v - 2 * dot(v, n) * n;
}

// VEC3_H
#endif
