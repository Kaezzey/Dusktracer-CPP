# Dusktracer — Hybrid Ray Tracer + Interactive Editor

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
<img width="2560" height="1395" alt="image" src="https://github.com/user-attachments/assets/f0226789-c9c9-4805-a88c-a86439ff9a09" />
<img width="2560" height="1440" alt="CornellBoxHero" src="https://github.com/user-attachments/assets/1d30c475-d0f5-4bd1-8490-e4351e90b866" />
<img width="2560" height="1395" alt="image" src="https://github.com/user-attachments/assets/f321a3e5-4f85-4e6c-8f18-1c0e86031269" />


---

## Features
### Rendering
- GGX PBR with metallic/roughness workflow ([model, audit, and validation](docs/pbr-validation.md))
- Exact dielectric Fresnel and GGX visible-normal sampling
- Correct inverse-transpose normals under non-uniform scale  
- Unified transform wrapper for spheres, cubes, and meshes  
- BVH acceleration & AABB optimisations  
- Support for alpha in albedo textures (RGBA PNGs used as transparency source when no explicit alpha map is set)
- Stochastic alpha testing for soft, Monte‑Carlo-correct transparent edges (alpha_cutoff retained as a fast-path)
- Double-sided masked transparency for materials that use alpha
- Point-light direct illumination with inverse-square attenuation and shadow checks
- Embree path UV interpolation fix to ensure correct texture sampling on hardware-accelerated geometry

### Editor Features
- Persistent material graph with texture drops, searchable nodes, editable wires, CPU shader evaluation, and asynchronous previews ([guide and performance checks](docs/material-graph.md))
- Content Drawer (ctrl + spacebar)
- GPU-based picking via RGB8 ID buffer  
- Screen-space gizmo: axis translation with closest-point ray math  
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
- Stable gizmo translation via line–ray closest points
- Raster preview shares exact TRS math with ray tracer
- Reverse-mapping transforms for correct ray intersection
- Per-material alpha masking with stochastic sampling and automatic double-sided handling for masked materials

---

## How to test transparency and lights
1. Drag an RGBA PNG with transparent regions into the Textures window.
2. Assign it to a material's Albedo slot (or supply an explicit alpha map).
3. Apply that material to geometry (leaves/sprites/meshes).
4. Render with higher SPP to see soft edges from stochastic alpha testing.
5. Add point lights and position them in the viewport — they will appear as icons in the raster preview and light the ray-traced render.

Notes:
- Loader treats 0 = fully transparent, 255 = opaque.
- alpha_cutoff remains available to cull near‑fully transparent pixels as a performance fast-path.
- If backface masking artifacts appear, double-sided masking is automatically enabled when alpha is present.

---
