#pragma once

#include "hittable.h"
#include <algorithm>
#include <cmath>

// Orbit a stationary unit sphere. Yaw is about world Y and pitch is about the
// camera's horizontal axis, so horizontal drags never introduce camera roll.
struct material_preview_camera {
    point3 eye;
    vec3 outward, right, up;
    double half_height;

    material_preview_camera(double yaw, double pitch, double zoom) {
        const double cy = std::cos(yaw), sy = std::sin(yaw);
        const double cp = std::cos(pitch), sp = std::sin(pitch);
        outward = vec3(cp * sy, sp, cp * cy);
        right = vec3(cy, 0, -sy);
        up = cross(outward, right);
        half_height = std::tan(35.0 * pi / 360.0);
        const double framing = .88 * std::clamp(zoom, .55, 1.1);
        const double distance = std::sqrt(1 + 1 / (framing * framing * half_height * half_height));
        eye = distance * outward;
    }

    bool surface(double screen_x, double screen_y, hit_record& hit, vec3& view) const {
        const vec3 direction = -outward + half_height * (screen_x * right + screen_y * up);
        const double a = direction.length_squared(), half_b = dot(eye, direction);
        const double discriminant = half_b * half_b - a * (eye.length_squared() - 1);
        if (discriminant <= 0) {
            return false;
        }
        const double distance = (-half_b - std::sqrt(discriminant)) / a;
        hit.p = eye + distance * direction;
        hit.normal = hit.geometric_normal = safe_unit_vector(hit.p);
        hit.front_face = true;
        hit.u = (std::atan2(-hit.p.z(), hit.p.x()) + pi) / (2 * pi);
        hit.v = std::acos(std::clamp(-hit.normal.y(), -1.0, 1.0)) / pi;
        const vec3 tangent(hit.p.z(), 0, -hit.p.x());
        hit.set_tangent_frame(tangent, cross(hit.normal, tangent));
        view = safe_unit_vector(eye - hit.p);
        return true;
    }
};
