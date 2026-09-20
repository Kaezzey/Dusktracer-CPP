#ifndef DUSK_SCENE_H
#define DUSK_SCENE_H

#include <string>
#include <vector>
#include <memory>

#include "vec3.h"  // for vec3 / point3
#include "material_graph.h"

// Forward declarations
class material;
class hittable;
class hittable_list;

// Forward declare scene so we can reference it in build_rt_material.
struct scene;

// ----------------------------------------
// Material “model” types the editor can pick
// ----------------------------------------
enum class scene_material_model {
    lambert,
    metal,
    dielectric,
    diffuse_light,
    isotropic,
    pbr          // uses your pbr_material class
};

// ----------------------------------------
// Texture asset owned by the scene
// ----------------------------------------
// The editor will manage this list (add/remove, rename, etc.);
// materials just hold indices into this array.
struct scene_texture {
    std::string name;  // UI/display name, e.g. "Rust_Albedo"
    std::string path;  // file path on disk, e.g. "textures/rust_albedo.png"
};

// ----------------------------------------
// Editor-facing material description
// ----------------------------------------
struct scene_material_parameters {
    std::string          name;
    scene_material_model model = scene_material_model::lambert;

    // --- Shared scalar parameters ---
    vec3   base_color       = vec3(0.8, 0.8, 0.8);    // lambert/metal/pbr/etc.
    double metallic         = 0.0;                    // [0,1] PBR
    double roughness        = 0.5;                    // [0,1] PBR
    double fuzz             = 0.0;                    // metal fuzz
    double ior              = 1.5;                    // dielectric IOR
    vec3   emission         = vec3(0, 0, 0);          // diffuse_light color
    // Sub-surface scattering (thin/translucent approximation)
    // - sss_strength: [0..1] amount of subsurface transmission
    // - sss_scale: mean free path / scattering scale (larger -> more transmission)
    // Note: the SSS tint uses the material albedo (base_color).
    bool   use_sss      = false;                      // Enable subsurface scattering
    double sss_strength = 0.0;
    double sss_scale    = 1.0;
    // Extended dipole/diffusion parameters (editor-facing). sss_model maps
    // directly to runtime SSSModel values.
    int    sss_model    = 1;    // 0=none,1=single,2=multi-single,3=dipole,4=skin,5=foliage
    int    sss_samples  = 4;
    double sss_radius   = 1.0;
    double sss_eta      = 1.3;
    bool   sss_color_override_enabled = false;
    vec3   sss_color_override_color = vec3(1.0, 1.0, 1.0);
    // When true, interpret packed metal/roughness textures using Unreal-style channels
    // (G = roughness, B = metallic) when a combined texture is used.
    bool   unreal_pbr = false;
    double emission_intensity = 1.0;                  // intensity multiplier for diffuse_light
    vec3   dielectric_F0    = vec3(0.04, 0.04, 0.04); // PBR dielectric F0
    double normal_strength  = 1.0;                    // PBR normal strength

    // --- Texture bindings (indices into scene.textures) ---
    // -1 = no texture; renderer should fall back to scalar params above.
    int albedo_tex    = -1;   // base color map
    int metallic_tex  = -1;   // greyscale in [0,1]
    int roughness_tex = -1;   // greyscale in [0,1]
    int normal_tex    = -1;   // tangent-space normal map
    // Optional alpha/opacity mask (single-channel or image alpha)
    int alpha_tex     = -1;   // alpha mask index into scene.textures
    bool alpha_double_sided = true;
    double alpha_cutoff = 0.5;
};

struct scene_material : scene_material_parameters {
    material_graph graph;
};

// Convert an editor-facing scene_material into a runtime material*
// using the scene's texture list (scene.textures[...] indices).
std::shared_ptr<material> build_rt_material(const scene& scn,
                                            const scene_material& desc);

// ----------------------------------------
// Geometry types
// ----------------------------------------
enum class scene_object_type {
    sphere,
    cube,          // NEW: box made from your quad/box
    mesh_instance  // mesh placed in the world with a transform
};

// ----------------------------------------
// Editor-facing object description
// ----------------------------------------
//
// Conventions:
// - sphere:
//     center, radius, material_index
//
// - cube:
//     center       = cube centre in world space
//     scale.x      = uniform scale multiplier (side length; 1 = unit cube)
//     rotation_deg = Euler rotation (degrees)
//     material_index used
//
// - mesh_instance:
//     mesh_index, translation, rotation_deg, scale.x (uniform),
//     mesh_slot_materials for per-slot overrides,
//     material_index used as a fallback.
//
struct scene_object {
    std::string       name;
    scene_object_type type = scene_object_type::sphere;

    int   material_index = -1;

    // Sphere / cube common “anchor”
    point3 center       = point3(0, 0, 0);
    double radius       = 0.5;           // used only for spheres

    // Mesh instance specific
    int   mesh_index    = -1;
    vec3  translation   = vec3(0, 0, 0);
    vec3  rotation_deg  = vec3(0, 0, 0);
    vec3  scale         = vec3(1, 1, 1);

    // Per-slot mesh materials (same order as scene_mesh_asset::slot_names)
    std::vector<int> mesh_slot_materials;
};

// ----------------------------------------
// Mesh asset: result of importing an FBX/OBJ
// ----------------------------------------
// This is *not* an instance; it's the shared geometry data.
struct scene_mesh_asset {
    std::string name;
    std::string file_path;
    std::string thumbnail_path; // optional preview image for Assets UI

    std::shared_ptr<hittable> mesh_bvh;  // optional: per-mesh BVH cache

    std::vector<std::string> slot_names;
    std::vector<int>         slot_default_materials;
    double                   approx_radius = 1.0; // approximate bounding sphere radius (for thumbnails)
};

// ----------------------------------------
// Lights
// ----------------------------------------
enum class scene_light_type {
    directional,
    point,
};

struct scene_light {
    std::string      name;
    scene_light_type type = scene_light_type::point;

    // Radiance / intensity (radiance for directional, intensity for point)
    vec3   radiance  = vec3(1.0, 1.0, 1.0);

    // Directional light: direction *from light toward scene*, normalized
    vec3   direction = vec3(-1, -1, -1);

    // Directional light angular radius (degrees). 0 = delta directional
    double angular_radius_deg = 0.0;

    // Point light: world position + simple range
    point3 position  = point3(0, 5, 0);
    double range     = 10.0;   // falloff / influence radius
};

// ----------------------------------------
// Whole scene: what the editor owns
// ----------------------------------------
struct scene {
    std::vector<scene_texture>    textures;   // texture library

    std::vector<scene_material>   materials;
    std::vector<scene_object>     objects;
    std::vector<scene_mesh_asset> meshes;     // mesh assets (FBX/OBJ etc.)
    std::vector<scene_light>      lights;     // lights in the scene
};

// Convert editor scene into runtime hittables for the renderer.
hittable_list build_world_from_scene(const scene& scn);

// Forward-declare camera to access emissive_surface type
class camera;

// Collect emissive surfaces from the scene and populate camera's emissive light database
void build_emissive_surfaces(const scene& scn, camera& cam);

#endif // DUSK_SCENE_H
