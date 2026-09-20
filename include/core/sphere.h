#ifndef SPHERE_H
#define SPHERE_H

#include "hittable.h"

#include "packet.h"

class sphere : public hittable {

    public:
        ray centre;
        double radius;
        shared_ptr<material> mat;
        aabb bbox;

        //Stationary sphere constructor
        sphere(const point3& static_centre, double radius, shared_ptr<material> mat) : centre(static_centre, vec3(0, 0, 0), 0.0), radius(std::fmax(0, radius)), mat(mat) {

            auto rvec = vec3(radius, radius, radius);
            bbox = aabb(static_centre - rvec, static_centre + rvec);
        }

        //Moving sphere constructor
        sphere(const point3& centre0, const point3& centre1, double radius, shared_ptr<material> mat) : centre(centre0, (centre1 - centre0), 0.0), radius(std::fmax(0, radius)), mat(mat) {

            auto revec = vec3(radius, radius, radius);
            aabb box0(centre.at(0.0) - revec, centre.at(0.0) + revec);
            aabb box1(centre.at(1.0) - revec, centre.at(1.0) + revec);
            bbox = aabb(box0, box1);
        }

        bool hit(const ray& r, interval ray_t, hit_record& rec) const override {
            point3 current_centre = centre.at(r.time());
            vec3 oc = current_centre - r.origin();
            auto a = r.direction().length_squared();
            auto h = dot(r.direction(), oc);
            auto c = oc.length_squared() - radius * radius;
            auto discriminant = h * h - a * c;

            if (discriminant < 0) return false;
            auto sqrtd = std::sqrt(discriminant);

            auto root = (h - sqrtd) / a;
            if (!ray_t.surrounds(root)) {
                root = (h + sqrtd) / a;
                if (!ray_t.surrounds(root)) return false;
            }

            rec.t = root;
            rec.p = r.at(root);
            vec3 outward_normal = (rec.p - current_centre) / radius;
            rec.set_face_normal(r, outward_normal);
            
            // 1. GET UVs
            get_sphere_uv(outward_normal, rec.u, rec.v);

            // 2. Calculate a robust tangent basis for the sphere
            // Compute tangent from the final shading normal (rec.normal) so
            // that face-flips (set_face_normal) are handled consistently.
            vec3 n = rec.normal;

            // Prefer analytical longitude direction when valid
            vec3 t = vec3(n.z(), 0.0, -n.x());

            // If tangent is degenerate (poles), pick a stable perpendicular
            if (t.length_squared() < 1e-8) {
                vec3 up = (std::fabs(n.y()) < 0.999) ? vec3(0,1,0) : vec3(1,0,0);
                t = unit_vector(cross(up, n));
            } else {
                t = unit_vector(t);
            }

            rec.tangent = t;

            // 3. Bitangent (ensure orthogonality)
            rec.bitangent = unit_vector(cross(rec.normal, rec.tangent));

            rec.mat = mat;
            return true;
        }

        bool sample_surface(double u, double v, double time, hit_record& rec, double& pdf) const override {
            if (radius <= 0) return false;
            double y = 1-2*u, r = std::sqrt(std::max(0.0,1-y*y)), phi = 2*pi*v;
            rec.normal = rec.geometric_normal = vec3(r*std::cos(phi),y,r*std::sin(phi));
            rec.front_face = true; rec.p = centre.at(time)+radius*rec.normal; rec.mat = mat;
            get_sphere_uv(rec.normal,rec.u,rec.v);
            rec.set_tangent_frame(vec3(rec.normal.z(),0,-rec.normal.x()),vec3());
            pdf = surface_pdf(rec); return true;
        }
        double surface_pdf(const hit_record&) const override {
            return radius > 0 ? 1/(4*pi*radius*radius) : 0;
        }

        aabb bounding_box() const override {
            return bbox;
        }

        // Packetized hit: test up to 4 rays in `pkt`. Fills `out_recs` and
        // returns a bitmask of lanes that hit.
        inline unsigned int hit_packet(const RayPacket4& pkt, hit_record out_recs[4]) const {
            unsigned int mask = 0;
            point3 current_centre = centre.at(0.0); // time-varying sphere not supported in packet version
            for (int i = 0; i < 4; ++i) {
                if (!(pkt.active_mask & (1u << i))) continue;
                // Quick AABB test per lane
                if (!bbox.hit(pkt.r[i], interval(pkt.tmin[i], pkt.tmax[i]))) continue;

                vec3 oc = current_centre - pkt.r[i].origin();
                double a = pkt.r[i].direction().length_squared();
                double h = dot(pkt.r[i].direction(), oc);
                double c = oc.length_squared() - radius * radius;
                double disc = h*h - a*c;
                if (disc < 0.0) continue;
                double sqrtd = std::sqrt(disc);
                double root = (h - sqrtd) / a;
                if (!(interval(pkt.tmin[i], pkt.tmax[i]).surrounds(root))) {
                    root = (h + sqrtd) / a;
                    if (!(interval(pkt.tmin[i], pkt.tmax[i]).surrounds(root))) continue;
                }

                hit_record rec;
                rec.t = root;
                rec.p = pkt.r[i].at(root);
                vec3 outward_normal = (rec.p - current_centre) / radius;
                rec.set_face_normal(pkt.r[i], outward_normal);
                get_sphere_uv(outward_normal, rec.u, rec.v);

                // Tangent/bitangent similar to scalar path
                vec3 n = rec.normal;
                vec3 t = vec3(n.z(), 0.0, -n.x());
                if (t.length_squared() < 1e-8) {
                    vec3 up = (std::fabs(n.y()) < 0.999) ? vec3(0,1,0) : vec3(1,0,0);
                    t = unit_vector(cross(up, n));
                } else {
                    t = unit_vector(t);
                }
                rec.tangent = t;
                rec.bitangent = unit_vector(cross(rec.normal, rec.tangent));
                rec.mat = mat;

                out_recs[i] = rec;
                mask |= (1u << i);
            }
            return mask;
        }

    private:

        static void get_sphere_uv(const point3& p, double& u, double& v) {
        // p: a given point on the sphere of radius one, centered at the origin.
        // u: returned value [0,1] of angle around the Y axis from X=-1.
        // v: returned value [0,1] of angle from Y=-1 to Y=+1.
        //     <1 0 0> yields <0.50 0.50>       <-1  0  0> yields <0.00 0.50>
        //     <0 1 0> yields <0.50 1.00>       < 0 -1  0> yields <0.50 0.00>
        //     <0 0 1> yields <0.25 0.50>       < 0  0 -1> yields <0.75 0.50>

        auto theta = std::acos(-p.y());
        auto phi = std::atan2(-p.z(), p.x()) + pi;

        u = phi / (2*pi);
        v = theta / pi;
    }
        
};

#endif 