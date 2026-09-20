#ifndef TRANSFORM_H
#define TRANSFORM_H

#include <memory>
#include <cmath>
#include "hittable.h"
#include "aabb.h"

// Convert degrees → radians
inline double deg2rad(double d) {
    return d * 0.017453292519943295; // π/180
}

class transform : public hittable {
public:
    transform(std::shared_ptr<hittable> p,
              const vec3& translate,
              const vec3& rotate_deg,  // *** editor uses DEGREES ***
              const vec3& scale = vec3(1.0, 1.0, 1.0))
        : ptr(std::move(p)), t(translate)
    {
        //----------------------------------------------------------------------
        // 1. Build rotation matrices from DEGREES
        //----------------------------------------------------------------------

        // Build rotation angles (degrees from editor are used directly).
        double rx = deg2rad(rotate_deg.x());
        double ry = deg2rad(rotate_deg.y());
        double rz = deg2rad(rotate_deg.z());

        double cx = std::cos(rx), sx = std::sin(rx);
        double cy = std::cos(ry), sy = std::sin(ry);
        double cz = std::cos(rz), sz = std::sin(rz);

        double Rx[3][3], Ry[3][3], Rz[3][3];
        double Rtemp[3][3];
        double R[3][3];

        // Rot X
        Rx[0][0] = 1;  Rx[0][1] = 0;   Rx[0][2] = 0;
        Rx[1][0] = 0;  Rx[1][1] = cx;  Rx[1][2] = -sx;
        Rx[2][0] = 0;  Rx[2][1] = sx;  Rx[2][2] = cx;

        // Rot Y
        Ry[0][0] = cy; Ry[0][1] = 0;   Ry[0][2] = sy;
        Ry[1][0] = 0;  Ry[1][1] = 1;   Ry[1][2] = 0;
        Ry[2][0] = -sy;Ry[2][1] = 0;   Ry[2][2] = cy;

        // Rot Z
        Rz[0][0] = cz; Rz[0][1] = -sz; Rz[0][2] = 0;
        Rz[1][0] = sz; Rz[1][1] =  cz; Rz[1][2] = 0;
        Rz[2][0] = 0;  Rz[2][1] =  0;  Rz[2][2] = 1;

        // Combine R = Rz * Ry * Rx
        mat_mul(Ry, Rx, Rtemp);
        mat_mul(Rz, Rtemp, R);

        //----------------------------------------------------------------------
        // 2. Build M and M_inv (forward + inverse transform)
        //----------------------------------------------------------------------

        double scale_x = scale.x(); if (scale_x == 0.0) scale_x = 1.0;
        double scale_y = scale.y(); if (scale_y == 0.0) scale_y = 1.0;
        double scale_z = scale.z(); if (scale_z == 0.0) scale_z = 1.0;

        double inv_scale_x = 1.0 / scale_x;
        double inv_scale_y = 1.0 / scale_y;
        double inv_scale_z = 1.0 / scale_z;
        area_determinant = std::abs(scale_x*scale_y*scale_z);

        // The raster `make_model_trs` constructs a column-major matrix where
        // the linear part is effectively (for element m_{row,col}):
        //   m_{row,col} = R[col][row] * scale_row
        // To match raster and RT conventions, build our row-major M so that
        // apply(M, v) produces identical rotated+scaled results. Therefore:
        //   M[row][col] = R[col][row] * scale_row
        for (int i = 0; i < 3; ++i) {
            double scale_i = (i == 0) ? scale_x : (i == 1) ? scale_y : scale_z;
            for (int j = 0; j < 3; ++j) {
                M[i][j] = R[j][i] * scale_i;
            }
        }

        // For M = S * R^T, the inverse is M_inv = R * S^{-1} so:
        //   M_inv[i][j] = R[i][j] * inv_scale_j
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                double inv_scale_j = (j == 0) ? inv_scale_x : (j == 1) ? inv_scale_y : inv_scale_z;
                M_inv[i][j] = R[i][j] * inv_scale_j;
            }
        }

        //----------------------------------------------------------------------
        // 3. Build transformed bounding box
        //----------------------------------------------------------------------

        aabb box_local = ptr->bounding_box();

        vec3 minp(  infinity,  infinity,  infinity);
        vec3 maxp( -infinity, -infinity, -infinity);

        for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 2; ++j)
        for (int k = 0; k < 2; ++k)
        {
            double x = i ? box_local.x.max : box_local.x.min;
            double y = j ? box_local.y.max : box_local.y.min;
            double z = k ? box_local.z.max : box_local.z.min;

            vec3 p_local(x,y,z);
            vec3 p_world = apply(M, p_local) + t;

            minp = vmin(minp, p_world);
            maxp = vmax(maxp, p_world);
        }

        bbox_world = aabb(minp, maxp);
    }

    //----------------------------------------------------------------------
    // hit()
    //----------------------------------------------------------------------
    bool hit(const ray& r_in, interval t_range, hit_record& rec) const override {
        // Ray → local
        vec3 origin_local    = apply(M_inv, r_in.origin() - t);
        vec3 direction_local = apply(M_inv, r_in.direction());

        ray r_local(origin_local, direction_local, r_in.time());

        if (!ptr->hit(r_local, t_range, rec))
            return false;

        // Point → world
        rec.p = apply(M, rec.p) + t;

        // Preserve the original outward orientation. Re-facing an already
        // face-forward normal incorrectly classifies every exit as an entry.
        vec3 outward_n = rec.front_face ? rec.normal : -rec.normal;
        vec3 outward_g = rec.front_face ? rec.geometry_normal() : -rec.geometry_normal();
        vec3 t_world = apply(M,rec.tangent), b_world = apply(M,rec.bitangent);
        rec.set_face_normal(r_in,safe_unit_vector(apply_transpose(M_inv,outward_g)));
        rec.set_shading_normal(safe_unit_vector(apply_transpose(M_inv,outward_n)));
        rec.set_tangent_frame(t_world,b_world);

        return true;
    }

    bool sample_surface(double u, double v, double time, hit_record& rec, double& pdf) const override {
        if (!ptr->sample_surface(u,v,time,rec,pdf)) return false;
        vec3 g = apply_transpose(M_inv,rec.geometry_normal());
        double jacobian = area_determinant*g.length();
        if (jacobian <= 0) return false;
        rec.p = apply(M,rec.p)+t;
        vec3 tangent = apply(M,rec.tangent), bitangent = apply(M,rec.bitangent);
        rec.normal = safe_unit_vector(apply_transpose(M_inv,rec.normal));
        rec.geometric_normal = safe_unit_vector(g); rec.set_tangent_frame(tangent,bitangent);
        pdf /= jacobian; return true;
    }
    double surface_pdf(const hit_record& rec) const override {
        hit_record local = rec;
        local.p = apply(M_inv,rec.p-t);
        local.geometric_normal = safe_unit_vector(apply_transpose(M,rec.geometry_normal()));
        double jacobian = area_determinant*apply_transpose(M_inv,local.geometric_normal).length();
        return jacobian > 0 ? ptr->surface_pdf(local)/jacobian : 0;
    }

    aabb bounding_box() const override {
        return bbox_world;
    }

private:
    std::shared_ptr<hittable> ptr;
    vec3 t;
    double area_determinant = 1;
    double M[3][3];
    double M_inv[3][3];
    aabb bbox_world;

    // A * B = out
    static void mat_mul(const double A[3][3], const double B[3][3],
                        double out[3][3])
    {
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) {
                out[i][j] = 0.0;
                for (int k = 0; k < 3; ++k)
                    out[i][j] += A[i][k] * B[k][j];
            }
    }

    static vec3 apply(const double M[3][3], const vec3& v) {
        return vec3(
            M[0][0]*v.x() + M[0][1]*v.y() + M[0][2]*v.z(),
            M[1][0]*v.x() + M[1][1]*v.y() + M[1][2]*v.z(),
            M[2][0]*v.x() + M[2][1]*v.y() + M[2][2]*v.z()
        );
    }

    // Multiply by transpose: out = M^T * v
    static vec3 apply_transpose(const double M[3][3], const vec3& v) {
        return vec3(
            M[0][0]*v.x() + M[1][0]*v.y() + M[2][0]*v.z(),
            M[0][1]*v.x() + M[1][1]*v.y() + M[2][1]*v.z(),
            M[0][2]*v.x() + M[1][2]*v.y() + M[2][2]*v.z()
        );
    }

    static vec3 vmin(const vec3& a, const vec3& b) {
        return vec3(fmin(a.x(), b.x()),
                    fmin(a.y(), b.y()),
                    fmin(a.z(), b.z()));
    }

    static vec3 vmax(const vec3& a, const vec3& b) {
        return vec3(fmax(a.x(), b.x()),
                    fmax(a.y(), b.y()),
                    fmax(a.z(), b.z()));
    }
};

#endif // TRANSFORM_H
