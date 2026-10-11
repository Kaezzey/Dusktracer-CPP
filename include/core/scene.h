#ifndef DUSK_SCENE_H
#define DUSK_SCENE_H

#include <string>
#include <vector>
#include <memory>
#include <functional>

#include "vec3.h"  // for vec3 / point3
#include "material_graph.h"

// Forward declarations
class material;
class texture;
enum class texture_sample_space;
using material_texture_loader = std::function<std::shared_ptr<texture>(const std::string&, texture_sample_space)>;
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
    // Spatial Burley diffusion, shared by CPU and GPU.
    // sss_strength replaces part of the diffuse substrate; sss_radius is a
    // world-space diffusion distance. Tint defaults to the evaluated albedo.
    bool   use_sss      = false;                      // Enable subsurface scattering
    double sss_strength = 0.0;
    double sss_scale    = 1.0;
    // Legacy nonzero model IDs all select normalized Burley diffusion.
    int    sss_model    = 1;    // 0=None, nonzero=Burley
    int    sss_samples  = 4;
    double sss_radius   = 1.0;
    // Relative RGB diffusion distances. Skin scatters red farther than blue.
    vec3 sss_radius_rgb = vec3(1,1,1);
    int sss_profile = 0; // 0 neutral, 1 skin, 2 custom (editor preset only)
    double sss_eta      = 1.3;
    bool   sss_color_override_enabled = false;
    vec3   sss_color_override_color = vec3(1.0, 1.0, 1.0);
    // Shading model used when SSS is enabled and sss_model is nonzero.
    int sss_shading = 0; // sss_shading_model
    // Unreal "Subsurface": wrap/back-scatter transmission with this colour.
    // Opacity follows Unreal: 0 wraps light fully around, 1 follows the normal.
    // Light reaching back faces through the object is attenuated by
    // exp(-thickness/transmission distance), with distance in scene units.
    vec3   sss_subsurface_color = vec3(1, 1, 1);
    // Multiplies the subsurface colour by the (textured) base colour, as when
    // an Unreal material wires its colour map into Subsurface Color.
    bool   sss_color_from_base = true;
    double sss_opacity = .5;
    double sss_transmission_distance = .5;
    // Unreal "Subsurface Profile" (Burley parameterization). Distances are
    // mean free paths in millimetres, converted with scene units per metre.
    vec3   sss_surface_albedo = vec3(.91058, .338275, .2);
    vec3   sss_mfp_color = vec3(1, .198863, .0782621);
    double sss_mfp_mm = 1.2;
    double sss_units_per_metre = 1;
    vec3   sss_profile_tint = vec3(1, 1, 1);
    // Dual specular: lobe roughness = roughness * scale; mix weights lobe 1.
    double sss_lobe_roughness0 = .75;
    double sss_lobe_roughness1 = 1.3;
    double sss_lobe_mix = .85;
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

enum sss_shading_model {
    sss_shading_burley = 0,          // legacy spatial Burley diffusion
    sss_shading_unreal_subsurface = 1,
    sss_shading_unreal_profile = 2,
};

// Backend-neutral subsurface description. CPU, Vulkan and the raster viewport
// all consume this, so the two Unreal models cannot drift between backends.
struct subsurface_lowered {
    enum kind { none, diffusion, transmission } mode = none;
    // Diffusion: fraction of the diffuse substrate replaced by Burley
    // diffusion with world-space shape distances, entry IOR and a tint that
    // is either an override or the base colour times tint_scale.
    double strength = 0;
    vec3 distances = vec3(1, 1, 1);
    double eta = 1.3;
    bool tint_override = false;
    vec3 tint = vec3(1, 1, 1);
    vec3 tint_scale = vec3(1, 1, 1);
    // Specular lobe roughness scales and the weight of lobe 1.
    double lobe_scale0 = 1, lobe_scale1 = 1, lobe_mix = 0;
    // Transmission: Unreal Subsurface colour (optionally times base colour),
    // opacity and extinction length.
    vec3 color = vec3(0, 0, 0);
    bool color_from_base = false;
    double opacity = 1, transmission_distance = 1;
};
subsurface_lowered lower_subsurface(const scene_material_parameters& material);

// Convert an editor-facing scene_material into a runtime material*
// using the scene's texture list (scene.textures[...] indices).
std::shared_ptr<material> build_rt_material(const scene& scn,
                                            const scene_material& desc,
                                            const material_texture_loader& loader = {});

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
// Local nonuniform scale, then right-handed Euler X/Y/Z rotation, then
// center + translation. Spheres additionally multiply local scale by radius.
// Mesh slot bindings override material_index, which is the instance fallback.
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

    // Sun angular radius (degrees). Legacy zero uses a 0.266-degree disc
    double angular_radius_deg = 0.266;

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
