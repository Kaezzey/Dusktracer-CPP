# Dusktracer: Hybrid Ray Tracer + Interactive Editor

[![CMake](https://img.shields.io/badge/build-cmake-blue.svg)](https://cmake.org)  
[![License: MIT](https://img.shields.io/badge/license-MIT-green.svg)](LICENSE)

Dusktracer is a compact hybrid renderer built off Peter Shirley's teachings, featuring:
- CPU path tracing (GGX PBR, dielectric Fresnel, microfacet BRDFs)
- Optional Vulkan hardware ray tracing with GPU selection and CPU fallback ([backend and validation](src/gpu/README.md))
- [Editor layout, sphere previews and thumbnail caching](src/core/EDITOR_PREVIEWS.md).
- GPU raster preview for instant feedback (OpenGL, correct TRS)
- Interactive ImGui editor with gizmo, picking, and scene graph
- Assimp-powered import pipeline (OBJ/FBX + textures)

---

## Active Editor
<img width="2560" height="1398" alt="image" src="https://github.com/user-attachments/assets/b87b3cf7-e91c-4a48-943e-139c0af728b7" />
<img width="2560" height="1440" alt="CornellBoxHero" src="https://github.com/user-attachments/assets/1d30c475-d0f5-4bd1-8490-e4351e90b866" />
<img width="2559" height="1402" alt="Editor" src="https://github.com/user-attachments/assets/9c08ebba-d8ae-4c32-bd47-36482468dcb3" />
<img width="1920" height="1080" alt="CloudStrife" src="https://github.com/user-attachments/assets/1988fb52-275e-4238-be09-8364d7c86e5a" />

---

## Features
### Rendering
- GGX PBR with metallic/roughness workflow ([model, audit, and validation](docs/pbr-validation.md))
- Exact dielectric Fresnel and GGX visible-normal sampling
- Correct inverse-transpose normals under non-uniform scale  
- Unified transform wrapper for spheres, cubes, and meshes  
- BVH acceleration & AABB optimisations  
- Support for alpha in albedo textures (RGBA PNGs used as transparency source when no explicit alpha map is set)
- Alpha modes per material, as in Blender and Unreal: **Blend** (soft, fractional alpha, the default), **Clip** (all or nothing at a threshold) and **Opaque**, on CPU, GPU and the viewport
- Double-sided masked transparency for materials that use alpha
- Point-light direct illumination with inverse-square attenuation and shadow checks
- [Hair shading model](src/core/HAIR.md) for hair cards: Unreal-style pins on the Chiang et al. fibre BSDF (R, TT, TRT), on CPU, GPU and the raster viewport
- [Eye shading model](src/core/EYE.md): Unreal-style wet cornea, refracted iris and iris caustic, with Iris Mask and Iris Distance pins
- [Bucket rendering](src/core/BUCKET_RENDERING.md) on CPU and GPU, with Blender-style bucket marks and per-bucket denoising in the viewport
- Embree path UV interpolation fix to ensure correct texture sampling on hardware-accelerated geometry

### Editor Features
- [Project folders and complete `.dusk` scenes](src/core/PROJECTS.md): New/Open/Save/Save As, portable assets, and renderer state.
- [Unreal-style viewport navigation](src/core/EDITOR_VIEWPORT.md): captured right-drag fly with eased WASD, pan, orbit, zoom and F to frame; a saved dock layout, a status bar, and floating panels that stay above the editor
- Persistent material graph with texture drops, searchable nodes, editable wires, CPU shader evaluation, and asynchronous previews ([guide and performance checks](docs/material-graph.md))
- Content Drawer (ctrl + spacebar)
- GPU-based picking via RGB8 ID buffer  
- Unreal-style move, rotate and scale gizmos: finite pixel-sized handles, yellow hover/drag feedback, plane controls, snapping, world/local axes and undo ([controls](src/core/TRANSFORM_GIZMOS.md))
- Scene Hierarchy, Inspector, and Material Editor  
- Drag & drop mesh/texture import  
- Viewport selection + Delete key: click object in viewport to select and press Del (viewport-focused) to remove (undoable)
- Raster preview draws point-light icons for easy placement

### Raster Preview
- Modern OpenGL (VAO/VBO/EBO)  
- Perfect TRS parity with CPU ray tracer (Rz → Ry → Rx)  
- Per-mesh material slot binding  
- Robust raster picking and gizmo interaction

---

## What I Learned Building This
- Implementing physically-based BRDFs (GGX, Smith, Fresnel)  
- Building a material system with textured base/rough/metal/normal maps  
- Writing robust inverse + inverse-transpose transform math  
- Designing an editor → render-world translation layer  
- GPU picking pipelines, gizmo interaction, and editor UX  
- BVH/AABB performance tradeoffs and ray-offset fixes  
- End-to-end mesh import → normalization → GPU upload

---

## Technical Highlights
- CPU path tracer (multi-bounce, MIS-inspired sampling)
- Energy-conserving metallic/roughness shading
- Dielectric F0 control and correct Fresnel
- Tangent-space normal mapping with adjustable strength
- Stable transform dragging with frozen ray/plane anchors and continuous rotation
- Raster preview shares exact TRS math with ray tracer
- Reverse-mapping transforms for correct ray intersection
- Per-material alpha modes (Opaque, Clip, Blend) with automatic double-sided handling for masked materials

---

## How to test transparency and lights
1. Drag an RGBA PNG with transparent regions into the Textures window.
2. Assign it to a material's Albedo slot (or supply an explicit alpha map).
3. Apply that material to geometry (leaves/sprites/meshes).
4. Pick the material's **Alpha Mode**: Blend keeps soft edges soft, Clip cuts at the threshold.
5. Add point lights and position them in the viewport — they will appear as icons in the raster preview and light the ray-traced render.

Notes:
- Loader treats 0 = fully transparent, 255 = opaque.
- Blend is exact in the tracers (a surface with alpha 0.3 lets 70% through). The viewport draws the solid part first and blends the rest over it, farthest object first.
- If backface masking artifacts appear, double-sided masking is automatically enabled when alpha is present.

---
