#pragma once
#include "vec3.h"
#include <cmath>

// Right-handed Euler XYZ rotation: local scale, then X/Y/Z rotation, then translation.
// Both renderers and the editor use this row-major linear transform.
inline void make_trs_linear(const vec3& degrees, const vec3& scale, double forward[3][3],
                            double inverse[3][3]) {
    constexpr double radians = 0.017453292519943295;
    const double cx = std::cos(degrees.x() * radians), sx = std::sin(degrees.x() * radians);
    const double cy = std::cos(degrees.y() * radians), sy = std::sin(degrees.y() * radians);
    const double cz = std::cos(degrees.z() * radians), sz = std::sin(degrees.z() * radians);
    const double rotation[3][3] = {{cz * cy, cz * sy * sx - sz * cx, cz * sy * cx + sz * sx},
                                   {sz * cy, sz * sy * sx + cz * cx, sz * sy * cx - cz * sx},
                                   {-sy, cy * sx, cy * cx}};
    for (int row = 0; row < 3; ++row) {
        const double row_scale = scale[row] == 0 ? 1 : scale[row];
        for (int col = 0; col < 3; ++col) {
            const double col_scale = scale[col] == 0 ? 1 : scale[col];
            forward[row][col] = rotation[row][col] * col_scale;
            inverse[row][col] = rotation[col][row] / row_scale;
        }
    }
}

inline void make_trs_column_major(const vec3& translation, const vec3& degrees, const vec3& scale,
                                  float matrix[16]) {
    double linear[3][3], inverse[3][3];
    make_trs_linear(degrees, scale, linear, inverse);
    for (int col = 0; col < 3; ++col) {
        for (int row = 0; row < 3; ++row) {
            matrix[col * 4 + row] = float(linear[row][col]);
        }
        matrix[col * 4 + 3] = 0;
    }
    matrix[12] = float(translation.x());
    matrix[13] = float(translation.y());
    matrix[14] = float(translation.z());
    matrix[15] = 1;
}
