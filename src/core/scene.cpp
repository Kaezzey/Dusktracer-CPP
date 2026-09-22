#include <unordered_map>
#include <cstring>
#include <cctype>

#include "../../include/core/dusktracer.h"          // colour, texture, materials, point3, etc.
#include "../../include/core/scene.h"
#include "../../include/core/camera.h"              // for camera::emissive_surface
#include "../../include/core/hittable_list.h"
#include "../../include/core/sphere.h"
#include "../../include/core/materials/material.h"
#include "../../include/core/mesh_loader.h"
#include "../../include/core/BVH.h"
#include "../../include/core/triangle.h"
#include "../../include/core/transform.h"
#include "../../include/core/quad.h"               // <-- your quad/box implementation

// ------------------------------------------------------------
// Case-insensitive extension check
// ------------------------------------------------------------
static bool has_extension_ci(const std::string& path, const char* ext)
{
    const size_t lenp = path.size();
    const size_t lene = std::strlen(ext);
    if (lenp < lene) return false;
    const size_t off = lenp - lene;
    for (size_t i = 0; i < lene; ++i) {
        char c1 = (char)std::tolower(path[off + i]);
        char c2 = (char)std::tolower(ext[i]);
        if (c1 != c2) return false;
    }
    return true;
}

// ------------------------------------------------------------
// Texture cache: map file path + sample-space -> shared_ptr<texture>
// ------------------------------------------------------------
static std::shared_ptr<texture> load_texture_path(const std::string& path, texture_sample_space space) {
    // Preview and scene compilation use independent caches; immutable decoded
    // pixels are shared by the sRGB and linear wrappers in each cache.
    thread_local std::unordered_map<std::string,std::weak_ptr<texture>> textures;
    thread_local std::unordered_map<std::string,std::weak_ptr<rtw_image>> images;
    std::string key = path + (space == texture_sample_space::srgb_color ? "|srgb" : "|linear");
    if (auto texture = textures[key].lock()) return texture;
    auto image = images[path].lock();
    if (!image) { image = std::make_shared<rtw_image>(path.c_str()); images[path] = image; }
    auto result = std::make_shared<image_texture>(std::move(image),space);
    textures[key] = result;
    return result;
}
// ------------------------------------------------------------
// Helper: lightweight default materials (shared once)
// ------------------------------------------------------------
static std::shared_ptr<material> get_default_grey_mat()
{
    static std::shared_ptr<material> mat =
        std::make_shared<lambertian>(
            std::make_shared<solid_colour>(colour(0.5, 0.5, 0.5))
        );
    return mat;
}

static std::shared_ptr<material> get_default_magenta_mat()
{
    static std::shared_ptr<material> mat =
        std::make_shared<lambertian>(
            std::make_shared<solid_colour>(colour(1.0, 0.0, 1.0))
        );
    return mat;
}

// ------------------------------------------------------------
// Helper: build one concrete material from a scene_material
// using scene.texture indices
// ------------------------------------------------------------
std::shared_ptr<material> build_rt_material(const scene& scn,
                                            const scene_material& m, const material_texture_loader& loader)
{
    auto make_base_colour = [&](const vec3& c) {
        return std::make_shared<solid_colour>(
            colour(c.x(), c.y(), c.z())
        );
    };

    auto make_grey = [&](double v) {
        return std::make_shared<solid_colour>(
            colour(v, v, v)
        );
    };

    auto load_path = [&](const std::string& path, texture_sample_space space) {
        return loader ? loader(path, space) : load_texture_path(path, space);
    };
    auto load_index = [&](int index, texture_sample_space space) -> std::shared_ptr<texture> {
        return index < 0 || index >= int(scn.textures.size()) ? nullptr : load_path(scn.textures[index].path, space);
    };
    auto graph_loader = [&](const graph_node& node, bool srgb) -> std::shared_ptr<texture> {
        auto space = srgb ? texture_sample_space::srgb_color : texture_sample_space::linear_data;
        return node.texture_path.empty() ? load_index(node.texture_index, space) : load_path(node.texture_path, space);
    };
    auto input_texture = [&](int slot, int legacy, texture_sample_space space) {
        if (!m.graph.nodes.empty()) return compile_material_graph(m.graph, slot, graph_loader);
        return load_index(legacy, space);
    };

    switch (m.model) {

    case scene_material_model::lambert:
    {
        auto tex = input_texture(0, m.albedo_tex, texture_sample_space::srgb_color);
        std::shared_ptr<texture> base_tex =
            tex ? tex : make_base_colour(m.base_color);

        return std::make_shared<lambertian>(base_tex);
    }

    case scene_material_model::metal:
    {
        auto tex = input_texture(0, m.albedo_tex, texture_sample_space::srgb_color);
        std::shared_ptr<texture> base_tex =
            tex ? tex : make_base_colour(m.base_color);

        return std::make_shared<metal>(base_tex, m.fuzz);
    }

    case scene_material_model::dielectric:
    {
        auto tex = input_texture(0, m.albedo_tex, texture_sample_space::srgb_color);
        if (tex) {
            return std::make_shared<dielectric>(m.ior, tex);
        }
        return std::make_shared<dielectric>(
            m.ior,
            colour(m.base_color.x(),
                   m.base_color.y(),
                   m.base_color.z())
        );
    }

    case scene_material_model::diffuse_light:
    {
        // Use emission colour if set; otherwise base_color as emissive.
        // Apply emission_intensity as a scalar multiplier so colour + intensity
        // are independently controllable from the editor.
        vec3 base_emit = (m.emission.x() != 0.0 ||
                          m.emission.y() != 0.0 ||
                          m.emission.z() != 0.0)
                         ? m.emission
                         : m.base_color;

        vec3 emit_col = base_emit * (float)m.emission_intensity;

        auto tex = input_texture(0, m.albedo_tex, texture_sample_space::srgb_color);
        std::shared_ptr<texture> emit_tex =
            tex ? tex : make_base_colour(emit_col);

        return std::make_shared<diffuse_light>(emit_tex,tex ? m.emission_intensity : 1.0);
    }

    case scene_material_model::pbr:
    {
        // Base albedo
        auto base_tex_loaded = input_texture(0, m.albedo_tex, texture_sample_space::srgb_color);
        std::shared_ptr<texture> base_tex =
            base_tex_loaded ? base_tex_loaded : make_base_colour(m.base_color);

        // Metallic scalar -> greyscale texture if no texture bound
        auto metal_tex_loaded = input_texture(1, m.metallic_tex, texture_sample_space::linear_data);
        std::shared_ptr<texture> metallic_tex =
            metal_tex_loaded ? metal_tex_loaded : make_grey(m.metallic);

        // Roughness scalar -> greyscale texture if no texture bound
        auto rough_tex_loaded = input_texture(2, m.roughness_tex, texture_sample_space::linear_data);
        std::shared_ptr<texture> roughness_tex =
            rough_tex_loaded ? rough_tex_loaded : make_grey(m.roughness);

        // Normal map (tangent-space)
        std::shared_ptr<texture> normal_tex =
            input_texture(3, m.normal_tex, texture_sample_space::linear_data); // can be nullptr

        // Alpha map (optional)
        std::shared_ptr<texture> alpha_tex =
            input_texture(4, m.alpha_tex, texture_sample_space::linear_data); // can be nullptr

        // Force double-sided masking if either an explicit alpha map is
        // provided or the albedo texture contains an alpha channel.
        bool effective_double_sided = m.alpha_double_sided;
        if (alpha_tex || !m.graph.nodes.empty()) effective_double_sided = true;
        else {
            // If base_tex is an image_texture, query whether it has alpha.
            if (auto img_tex = std::dynamic_pointer_cast<image_texture>(base_tex)) {
                if (img_tex->has_alpha()) effective_double_sided = true;
            }
        }

        auto mat = std::make_shared<pbr_material>(
            base_tex,
            metallic_tex,
            roughness_tex,
            normal_tex,
            m.normal_strength,
            colour(m.dielectric_F0.x(),
                   m.dielectric_F0.y(),
                   m.dielectric_F0.z()),
            alpha_tex,
            effective_double_sided,
            m.alpha_cutoff
        );

        // Propagate subsurface scattering (SSS) settings from editor material
        // Only apply SSS if use_sss is enabled
        mat->sss_strength = m.use_sss ? m.sss_strength : 0.0;
        mat->sss_scale = m.sss_scale;
        // Extended dipole/diffusion params
        mat->sss_model = static_cast<SSSModel>(m.sss_model);
        mat->sss_samples = m.sss_samples;
        mat->sss_radius = m.sss_radius;
        mat->sss_eta = m.sss_eta;
        mat->sss_color_override = m.sss_color_override_enabled;
        mat->sss_color_override_col = colour(
            (float)m.sss_color_override_color.x(),
            (float)m.sss_color_override_color.y(),
            (float)m.sss_color_override_color.z()
        );
        // Unreal PBR packing flag
        mat->use_unreal_pbr = m.graph.nodes.empty() && m.unreal_pbr;

        return mat;
    }

    case scene_material_model::isotropic:
    {
        auto tex = input_texture(0, m.albedo_tex, texture_sample_space::srgb_color);
        std::shared_ptr<texture> base_tex =
            tex ? tex : make_base_colour(m.base_color);
        return std::make_shared<isotropic>(base_tex);
    }
    }

    // Fallback if model enum is invalid
    return get_default_grey_mat();
}

// ------------------------------------------------------------
// Convert editor scene into runtime hittables for the renderer.
// Uses per-slot mesh materials via mesh_loader's multi-material
// overload.
// ------------------------------------------------------------
hittable_list build_world_from_scene(const scene& scn)
{
    hittable_list world;

    // Rough reservation to avoid repeated reallocs:
    // spheres + cubes + each mesh instance will at least add one transform.
    world.objects.reserve(scn.objects.size() * 2);

    // Cache concrete materials so multiple objects can share them
    std::vector<std::shared_ptr<material>> built_materials(scn.materials.size());

    auto get_material = [&](int idx) -> std::shared_ptr<material> {
        if (idx < 0 || idx >= static_cast<int>(scn.materials.size()))
            return get_default_grey_mat();

        if (!built_materials[idx])
            built_materials[idx] = build_rt_material(scn, scn.materials[idx]);

        return built_materials[idx];
    };

    // --------------------------------------------------------
    // 1) Simple primitives: spheres + cubes
    // --------------------------------------------------------
    for (const auto& obj : scn.objects) {

        if (obj.type == scene_object_type::sphere) {
            auto mat = get_material(obj.material_index);

            vec3 scale = obj.scale;
            for (int axis = 0; axis < 3; ++axis) if (scale[axis] == 0) scale[axis] = 1;
            scale *= obj.radius;

            vec3 translation = obj.translation + vec3(obj.center.x(), obj.center.y(), obj.center.z());

            auto unit_sphere = std::make_shared<sphere>(point3(0,0,0), 1.0, mat);

            auto inst = std::make_shared<transform>(
                unit_sphere,
                translation,
                obj.rotation_deg,
                scale
            );

            world.add(inst);
        }
        else if (obj.type == scene_object_type::cube) {
            auto mat = get_material(obj.material_index);

            // Build a unit cube as a box from [-0.5, -0.5, -0.5] to [0.5, 0.5, 0.5]
            auto unit_cube = box(
                point3(-0.5, -0.5, -0.5),
                point3( 0.5,  0.5,  0.5),
                mat
            );

            // Preserve nonuniform and mirrored local scale.
            vec3 scl = obj.scale;
            if (scl.x() == 0.0) scl = vec3(1.0, scl.y(), scl.z());
            if (scl.y() == 0.0) scl = vec3(scl.x(), 1.0, scl.z());
            if (scl.z() == 0.0) scl = vec3(scl.x(), scl.y(), 1.0);

            // Combine center + translation for instance transform
            vec3 translation = obj.translation + vec3(obj.center.x(), obj.center.y(), obj.center.z());

            auto inst = std::make_shared<transform>(
                unit_cube,
                translation,
                obj.rotation_deg,
                scl
            );

            world.add(inst);
        }
    }

    // --------------------------------------------------------
    // 2) Mesh instances with per-slot materials (Legs/Chest/…)
    // --------------------------------------------------------
    for (const auto& obj : scn.objects) {
        if (obj.type != scene_object_type::mesh_instance)
            continue;

        if (obj.mesh_index < 0 ||
            obj.mesh_index >= static_cast<int>(scn.meshes.size()))
            continue;

        const scene_mesh_asset& asset = scn.meshes[obj.mesh_index];

        // Build mapping: FBX material name ("Legs", "Chest", ... )
        // -> runtime material.
        std::unordered_map<std::string, std::shared_ptr<material>> mat_table;
        mat_table.reserve(asset.slot_names.size());

        size_t num_slots = asset.slot_names.size();
        if (obj.mesh_slot_materials.size() < num_slots)
            num_slots = obj.mesh_slot_materials.size();

        for (size_t s = 0; s < num_slots; ++s) {
            int scene_mat_idx = obj.mesh_slot_materials[s];
            if (scene_mat_idx < 0 ||
                scene_mat_idx >= static_cast<int>(scn.materials.size()))
                continue;

            const std::string& fbx_mat_name = asset.slot_names[s];

            mat_table[fbx_mat_name] = get_material(scene_mat_idx);
        }

        // Default fallback material for any FBX material name not in the table.
        std::shared_ptr<material> default_mat;
        if (obj.material_index >= 0 &&
            obj.material_index < static_cast<int>(scn.materials.size())) {
            default_mat = get_material(obj.material_index);
        } else {
            default_mat = get_default_magenta_mat(); // magenta debug
        }

        const bool z_up = has_extension_ci(asset.file_path, ".fbx");

        // Diagnostic log: print mesh instance / asset info so we can debug
        // cases where raster shows the mesh but the raytracer skips it.
        std::printf("[scene] Loading mesh_instance '%s' -> mesh_index=%d, file='%s'\n",
                    obj.name.c_str(), obj.mesh_index, asset.file_path.c_str());

        // Multi-material mesh load
        std::shared_ptr<hittable_list> tri_list =
            load_mesh_as_triangles(
                asset.file_path,
                mat_table,
                default_mat,
                z_up,
                /*normalise_to_unit=*/true,
                /*user_scale=*/2.0
            );

        if (!tri_list) {
            std::fprintf(stderr, "[scene] load_mesh_as_triangles returned null for '%s'\n", asset.file_path.c_str());
        }

        if (!tri_list || tri_list->objects.empty())
            continue;

        // Build an acceleration structure for this instance's triangles.
        // Prefer `triangle_mesh`, which will use an Embree-backed accel when
        // compiled with Embree support (HAVE_EMBREE). If Embree is not
        // available, `triangle_mesh` falls back to a BVH.
        auto tri_mesh = std::make_shared<triangle_mesh>(tri_list->objects);

        // Apply the shared local-scale, rotation, translation convention
        vec3 scl = obj.scale;
            if (scl.x() == 0.0) scl = vec3(1.0, scl.y(), scl.z());
            if (scl.y() == 0.0) scl = vec3(scl.x(), 1.0, scl.z());
            if (scl.z() == 0.0) scl = vec3(scl.x(), scl.y(), 1.0);

            vec3 translation = obj.translation + vec3(obj.center.x(), obj.center.y(), obj.center.z());

            auto inst = std::make_shared<transform>(
                tri_mesh,
                translation,
                obj.rotation_deg,  // degrees
                scl
            );

        world.add(inst);
    }

    // If there are no objects, just return the empty world.
    if (world.objects.empty()) return world;

    // Wrap the scene objects in a top-level BVH to accelerate ray traversal.
    // We return a hittable_list containing a single BVH node so the rest of
    // the renderer (which expects a hittable_list) can remain unchanged.
    auto top_bvh = std::make_shared<bvh_node>(world.objects, 0, world.objects.size());
    hittable_list wrapped(top_bvh);
    return wrapped;
}

// ------------------------------------------------------------
// Collect emissive surfaces from the scene
// ------------------------------------------------------------
void build_emissive_surfaces(const scene& scn, camera& cam)
{
    std::vector<camera::emissive_surface> surfaces;
    auto add = [&](std::shared_ptr<hittable> geometry) {
        hit_record sample; double pdf;
        if (!geometry->sample_surface(.37,.63,0,sample,pdf) || pdf <= 0) return;
        camera::emissive_surface light;
        light.geometry = std::move(geometry); light.area = 1/pdf;
        light.emission = sample.mat->emitted(sample.u,sample.v,sample.p);
        surfaces.push_back(std::move(light));
    };
    for (const auto& obj : scn.objects) {
        if (obj.material_index < 0 || obj.material_index >= (int)scn.materials.size()) continue;
        const auto& description = scn.materials[obj.material_index];
        if (description.model != scene_material_model::diffuse_light) continue;
        auto mat = build_rt_material(scn,description);
        vec3 translation = obj.translation+obj.center;
        if (obj.type == scene_object_type::sphere) {
            if (obj.radius <= 0) continue;
            vec3 scale = obj.scale;
            for (int axis = 0; axis < 3; ++axis) if (scale[axis] == 0) scale[axis] = 1;
            scale *= obj.radius;
            add(std::make_shared<transform>(std::make_shared<sphere>(point3(),1,mat),
                translation,obj.rotation_deg,scale));
        } else if (obj.type == scene_object_type::cube) {
            vec3 scale = obj.scale;
            for (int i = 0; i < 3; ++i) if (scale[i] == 0) scale[i] = 1;
            auto faces = box(point3(-.5,-.5,-.5),point3(.5,.5,.5),mat);
            for (const auto& face : faces->objects)
                add(std::make_shared<transform>(face,translation,obj.rotation_deg,scale));
        }
        // Emissive mesh triangles remain reachable through BSDF sampling. Do
        // not invent a center-point distribution for unsampleable geometry.
    }
    cam.set_emissive_surfaces(surfaces);
}
