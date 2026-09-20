#ifndef QUAD_H
#define QUAD_H

#include "hittable.h"
#include "hittable_list.h"

class quad : public hittable {

  public:
    quad(const point3& Q, const vec3& u, const vec3& v, shared_ptr<material> mat)
      : Q(Q), u(u), v(v), mat(mat)
    {
        auto n = cross(u, v);
        normal = unit_vector(n);
        D = dot(normal, Q);
        w = n / dot(n,n);

        set_bounding_box();
    }

    virtual void set_bounding_box() {

        //compute four vertex bounding box
        auto bbox_diagonal1 = aabb(Q, Q + u + v);
        auto bbox_diagonal2 = aabb(Q + u, Q + v);
        bbox = aabb(bbox_diagonal1, bbox_diagonal2);
    }

    aabb bounding_box() const override { return bbox; }

    bool hit(const ray& r, interval ray_t, hit_record& rec) const override {

        auto denom = dot(normal, r.direction());

        //no hit if the ray is parallel to the plane.
        if (std::fabs(denom) < 1e-8)
            return false;

        //return false if the hit point parameter t is outside the ray interval.
        auto t = (D - dot(normal, r.origin())) / denom;
        if (!ray_t.contains(t))
            return false;

        auto intersection = r.at(t);
        vec3 planar_hitpt_vector = intersection - Q;
        auto alpha = dot(w, cross(planar_hitpt_vector, v));
        auto beta = dot(w, cross(u, planar_hitpt_vector));

        if (!is_interior(alpha, beta, rec))
            return false;

        rec.t = t;
        rec.p = r.at(t);
        rec.set_face_normal(r, normal);

        // These are the texture coords a, b they compute
        rec.u = alpha;   // or rec.u = a;
        rec.v = beta;    // or rec.v = b;

        // Build TBN
        vec3 T = u;
        vec3 B = v;
        vec3 N = rec.normal;

        // Orthonormalise: Gram–Schmidt to avoid skewed basis
        T = unit_vector(T - dot(T, N) * N);
        B = unit_vector(cross(N, T));   // right-handed basis

        rec.tangent   = T;
        rec.bitangent = B;

        rec.mat = mat;
        return true;
    }

    bool sample_surface(double a, double b, double, hit_record& rec, double& pdf) const override {
        rec.p = Q+a*u+b*v; rec.u = a; rec.v = b; rec.mat = mat;
        rec.normal = rec.geometric_normal = normal; rec.front_face = true;
        rec.set_tangent_frame(u,v); pdf = surface_pdf(rec); return pdf > 0;
    }
    double surface_pdf(const hit_record&) const override {
        double area = cross(u,v).length(); return area > 0 ? 1/area : 0;
    }

    virtual bool is_interior(double a, double b, hit_record& rec) const {
        interval unit_interval = interval(0, 1);
        // Given the hit point in plane coordinates, return false if it is outside the
        // primitive, otherwise set the hit record UV coordinates and return true.

        if (!unit_interval.contains(a) || !unit_interval.contains(b))
            return false;

        rec.u = a;
        rec.v = b;
        return true;
    }

  private:
    point3 Q;
    vec3 u, v;
    vec3 w;
    shared_ptr<material> mat;
    aabb bbox;
    vec3 normal;
    double D;
};

inline shared_ptr<hittable_list> box(const point3& a, const point3& b, shared_ptr<material> mat)
{
    // Returns the 3D box (six sides) that contains the two opposite vertices a & b.

    auto sides = make_shared<hittable_list>();

    // Construct the two opposite vertices with the minimum and maximum coordinates.
    auto min = point3(std::fmin(a.x(),b.x()), std::fmin(a.y(),b.y()), std::fmin(a.z(),b.z()));
    auto max = point3(std::fmax(a.x(),b.x()), std::fmax(a.y(),b.y()), std::fmax(a.z(),b.z()));

    auto dx = vec3(max.x() - min.x(), 0, 0);
    auto dy = vec3(0, max.y() - min.y(), 0);
    auto dz = vec3(0, 0, max.z() - min.z());

    sides->add(make_shared<quad>(point3(min.x(), min.y(), max.z()),  dx,  dy, mat)); // front
    sides->add(make_shared<quad>(point3(max.x(), min.y(), max.z()), -dz,  dy, mat)); // right
    sides->add(make_shared<quad>(point3(max.x(), min.y(), min.z()), -dx,  dy, mat)); // back
    sides->add(make_shared<quad>(point3(min.x(), min.y(), min.z()),  dz,  dy, mat)); // left
    sides->add(make_shared<quad>(point3(min.x(), max.y(), max.z()),  dx, -dz, mat)); // top
    sides->add(make_shared<quad>(point3(min.x(), min.y(), min.z()),  dx,  dz, mat)); // bottom

    return sides;
}

#endif