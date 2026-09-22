#ifndef TRANSFORM_H
#define TRANSFORM_H

#include <memory>
#include <cmath>
#include "hittable.h"
#include "aabb.h"
#include "affine_transform.h"

class transform : public hittable {
  public:
    transform(std::shared_ptr<hittable> p, const vec3& translate, const vec3& rotate_deg,
              const vec3& scale = vec3(1.0, 1.0, 1.0))
        : ptr(std::move(p)), t(translate) {
        make_trs_linear(rotate_deg, scale, M, M_inv);
        const double sx = scale.x() == 0 ? 1 : scale.x();
        const double sy = scale.y() == 0 ? 1 : scale.y();
        const double sz = scale.z() == 0 ? 1 : scale.z();
        area_determinant = std::abs(sx * sy * sz);

        aabb box_local = ptr->bounding_box();

        vec3 minp(infinity, infinity, infinity);
        vec3 maxp(-infinity, -infinity, -infinity);

        for (int i = 0; i < 2; ++i) {
            for (int j = 0; j < 2; ++j) {
                for (int k = 0; k < 2; ++k) {
                    double x = i ? box_local.x.max : box_local.x.min;
                    double y = j ? box_local.y.max : box_local.y.min;
                    double z = k ? box_local.z.max : box_local.z.min;

                    vec3 p_local(x, y, z);
                    vec3 p_world = apply(M, p_local) + t;

                    minp = vmin(minp, p_world);
                    maxp = vmax(maxp, p_world);
                }
            }
        }

        bbox_world = aabb(minp, maxp);
    }

    //----------------------------------------------------------------------
    // hit()
    //----------------------------------------------------------------------
    bool hit(const ray& r_in, interval t_range, hit_record& rec) const override {
        // Ray → local
        vec3 origin_local = apply(M_inv, r_in.origin() - t);
        vec3 direction_local = apply(M_inv, r_in.direction());

        ray r_local(origin_local, direction_local, r_in.time());

        if (!ptr->hit(r_local, t_range, rec)) {
            return false;
        }

        // Point → world
        rec.p = apply(M, rec.p) + t;

        // Preserve the original outward orientation. Re-facing an already
        // face-forward normal incorrectly classifies every exit as an entry.
        vec3 outward_n = rec.front_face ? rec.normal : -rec.normal;
        vec3 outward_g = rec.front_face ? rec.geometry_normal() : -rec.geometry_normal();
        vec3 t_world = apply(M, rec.tangent), b_world = apply(M, rec.bitangent);
        rec.set_face_normal(r_in, safe_unit_vector(apply_transpose(M_inv, outward_g)));
        rec.set_shading_normal(safe_unit_vector(apply_transpose(M_inv, outward_n)));
        rec.set_tangent_frame(t_world, b_world);

        return true;
    }

    bool sample_surface(double u, double v, double time, hit_record& rec, double& pdf) const override {
        if (!ptr->sample_surface(u, v, time, rec, pdf)) {
            return false;
        }
        vec3 g = apply_transpose(M_inv, rec.geometry_normal());
        double jacobian = area_determinant * g.length();
        if (jacobian <= 0) {
            return false;
        }
        rec.p = apply(M, rec.p) + t;
        vec3 tangent = apply(M, rec.tangent), bitangent = apply(M, rec.bitangent);
        rec.normal = safe_unit_vector(apply_transpose(M_inv, rec.normal));
        rec.geometric_normal = safe_unit_vector(g);
        rec.set_tangent_frame(tangent, bitangent);
        pdf /= jacobian;
        return true;
    }
    double surface_pdf(const hit_record& rec) const override {
        hit_record local = rec;
        local.p = apply(M_inv, rec.p - t);
        local.geometric_normal = safe_unit_vector(apply_transpose(M, rec.geometry_normal()));
        double jacobian = area_determinant * apply_transpose(M_inv, local.geometric_normal).length();
        return jacobian > 0 ? ptr->surface_pdf(local) / jacobian : 0;
    }

    aabb bounding_box() const override {
        return bbox_world;
    }

    // Backend-neutral affine export: retain exactly the CPU/editor convention.
    void affine_rows(float rows[12]) const {
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                rows[i * 4 + j] = float(M[i][j]);
            }
            rows[i * 4 + 3] = float(t[i]);
        }
    }

  private:
    std::shared_ptr<hittable> ptr;
    vec3 t;
    double area_determinant = 1;
    double M[3][3];
    double M_inv[3][3];
    aabb bbox_world;

    static vec3 apply(const double M[3][3], const vec3& v) {
        return vec3(M[0][0] * v.x() + M[0][1] * v.y() + M[0][2] * v.z(),
                    M[1][0] * v.x() + M[1][1] * v.y() + M[1][2] * v.z(),
                    M[2][0] * v.x() + M[2][1] * v.y() + M[2][2] * v.z());
    }

    // Multiply by transpose: out = M^T * v
    static vec3 apply_transpose(const double M[3][3], const vec3& v) {
        return vec3(M[0][0] * v.x() + M[1][0] * v.y() + M[2][0] * v.z(),
                    M[0][1] * v.x() + M[1][1] * v.y() + M[2][1] * v.z(),
                    M[0][2] * v.x() + M[1][2] * v.y() + M[2][2] * v.z());
    }

    static vec3 vmin(const vec3& a, const vec3& b) {
        return vec3(fmin(a.x(), b.x()), fmin(a.y(), b.y()), fmin(a.z(), b.z()));
    }

    static vec3 vmax(const vec3& a, const vec3& b) {
        return vec3(fmax(a.x(), b.x()), fmax(a.y(), b.y()), fmax(a.z(), b.z()));
    }
};

#endif // TRANSFORM_H
