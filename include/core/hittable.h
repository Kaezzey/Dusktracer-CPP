#ifndef HITTABLE_H
#define HITTABLE_H\

#include "aabb.h"

class material;

class hit_record{

    public:
        point3 p;
        vec3 normal;
        shared_ptr<material> mat;
        double t = 0;
        double u = 0;
        double v = 0;
        bool front_face = true;
        vec3 geometric_normal;

        vec3 tangent;
        vec3 bitangent;

        inline void set_face_normal(const ray& r, const vec3& outward_normal) {
            front_face = dot(r.direction(), outward_normal) < 0;
            normal = geometric_normal = front_face ? outward_normal : -outward_normal;
        }
        vec3 geometry_normal() const {
            return geometric_normal.near_zero() ? normal : geometric_normal;
        }
        void set_shading_normal(const vec3& outward_normal) {
            normal = safe_unit_vector(front_face ? outward_normal : -outward_normal,geometry_normal());
            if (dot(normal,geometry_normal()) < 0) normal = -normal;
        }
        void set_tangent_frame(const vec3& t, const vec3& b) {
            vec3 axis = std::abs(normal.z()) < .999 ? vec3(0,0,1) : vec3(0,1,0);
            tangent = safe_unit_vector(t-dot(t,normal)*normal,safe_unit_vector(cross(axis,normal)));
            bitangent = cross(normal,tangent);
            if (dot(bitangent,b) < 0) bitangent = -bitangent;
        }
};

class hittable{

    public:
        virtual ~hittable() = default;
        virtual bool hit(const ray& r, interval ray_t, hit_record& rec) const = 0;
        virtual aabb bounding_box() const = 0;
        // Optional area sampling. Records use outward normals and authored UVs.
        virtual bool sample_surface(double, double, double, hit_record&, double&) const { return false; }
        virtual double surface_pdf(const hit_record&) const { return 0; }
};


#endif
