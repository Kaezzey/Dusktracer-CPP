#pragma once

namespace material_sphere_shaders {
inline constexpr const char* vertex = R"GLSL(#version 130
void main() {
    vec2 corner = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    gl_Position = vec4(corner * 2 - 1, 0, 1);
}
)GLSL";

// Intersect a perspective camera ray with the stationary unit sphere. Analytic
// normals and tangents keep the silhouette smooth at any preview resolution.
inline constexpr const char* fragment = R"GLSL(
uniform vec3 uPreviewOutward, uPreviewRight, uPreviewUp;
uniform float uPreviewHalfHeight, uPreviewSize;
void main() {
    // Store rows in the same top-down order as the CPU preview and ImGui images.
    vec2 screen = gl_FragCoord.xy / uPreviewSize * 2 - 1;
    screen.y = -screen.y;
    vec3 direction = -uPreviewOutward + uPreviewHalfHeight *
                     (screen.x * uPreviewRight + screen.y * uPreviewUp);
    float a = dot(direction,direction), b = dot(uEye,direction);
    float discriminant = b*b - a*(dot(uEye,uEye)-1);
    FragColor = vec4(displayColor(vec3(.028,.039,.052)),1);
    if (discriminant <= 0) return;
    float distance = (-b-sqrt(discriminant))/a;
    vWorld = uEye + distance*direction;
    vNormal = normalize(vWorld);
    vUV = vec2((atan(-vWorld.z,vWorld.x)+PI)/(2*PI), acos(clamp(-vNormal.y,-1,1))/PI);
    vTangent = safeNormal(vec3(vWorld.z,0,-vWorld.x),vec3(1,0,0));
    vBitangent = cross(vNormal,vTangent);
    vec4 backdrop = FragColor;
    shadeSurface();
    // Blend-mode alpha shows the backdrop through the sphere.
    FragColor = vec4(mix(backdrop.rgb,FragColor.rgb,clamp(FragColor.a,0,1)),1);
}
)GLSL";
} // namespace material_sphere_shaders
