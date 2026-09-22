# Raster material viewport

The Raster view evaluates assigned materials directly on object UVs. Imported
meshes keep their per-slot bindings, smooth normals, and tangent frames through
the same geometry importer as ray tracing. Spheres and cubes use the CPU
renderer's UV conventions. The normal matrix is the inverse transpose, so
nonuniform and mirrored transforms preserve the shading frame.

`gpu_material.cpp` lowers the reachable material graph for both raster and Vulkan.
Raster uses one linked GLSL 1.30 program and a small float instruction texture
per material. Editing values uploads bytecode; it never recompiles the viewport
shader. Moving nodes or adding disconnected nodes schedules no material work.
Supported inputs include base color, metalness, roughness, tangent normals,
opacity, emission, UV tiling and all current arithmetic nodes. Color samples
are decoded from sRGB; numeric texture channels stay linear unless explicitly
set to sRGB, following the CPU graph rules.

Textures decode on a bounded, joined worker. The GL thread uploads at most one
completed image per frame, creates mipmaps, and shares the texture between
materials. Ordinary images use the disposable preview disk cache at up to
2048 pixels per side. HDR uploads preserve linear float values. Resident
textures have a 256 MiB soft budget: unused images are evicted first; textures
required by the current scene stay resident. Final rendering still uses the
original images. This is independent of the Content Drawer and sphere-preview
workers.

Lighting uses the existing GGX distribution, Smith masking and Schlick Fresnel
for direct illumination, plus a neutral studio fill. One directional light has
a fitted 2048-square filtered depth map. The first point light (or first
emissive primitive when there are no point lights) has a filtered 512-square
cubemap. Receiver-plane comparisons use the actual cubemap texel centers to
avoid self-shadow rings. Geometry/opacity/light-position changes invalidate
shadow maps; camera navigation and scalar color/metalness/roughness changes
reuse them. Other point lights illuminate without shadow maps, up to eight
point/emissive lights in total.

This remains an interactive preview: studio fill and emissive center-light
approximations are not global illumination. Glass is a tinted reflective
preview; refraction, traced reflections, subsurface scattering and caustics
require Render. Alpha uses a stable cutout at max(authored cutoff, 0.5), rather
than path-traced stochastic transparency. Filtered, reduced viewport textures
need not match nearest-sampled full-resolution final pixels. Materials are
limited to 12 distinct textures and 32 live graph values; an unsupported graph
shows magenta and reports the reason in the viewport instead of displaying a
misleading partial material.

All object transforms now use local scale followed by right-handed Euler
X/Y/Z rotation, then translation. `affine_transform.h` is shared by the editor
and CPU transform wrapper, and Vulkan instances export that same CPU matrix.
Spheres honor all three scale components in every backend. Ellipsoid emitter
sampling uses the exact affine area Jacobian for its PDF and MIS weights.
The previous transform transposed rotation and scaled world axes; previously
authored rotations may therefore need adjustment to the corrected convention.

`raster_materials_and_shadows` creates a hidden real OpenGL context and checks
CPU/GL graph arithmetic, texture channels/color spaces, alpha, imported material
slots, mirrored UV normal mapping, directional/point shadows, acne and shadow
cache invalidation. It writes diagnostic images in `build/raster-validation`.
The PBR tests sweep each rotation axis with unequal scale and verify axis
lengths, orthogonality, intersection distances and raster/CPU matrix agreement.
GPU tests additionally compare rotated ellipsoid UVs with CPU renders.

To capture the supplied model scene and profile the warmed viewport without
modifying project assets:

```powershell
& .\build\Release\DuskGraphIntegrationTests.exe build/raster-validation --raster $PWD.Path
```
