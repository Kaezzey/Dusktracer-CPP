#ifndef MESH_LOADER_H
#define MESH_LOADER_H

#include <memory>
#include <string>
#include <functional>
#include <limits>
#include <iostream>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <sstream>

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

#include "triangle.h"
#include "hittable_list.h"

// Simple Z-up -> Y-up rotation (-90° around X):
// x' =  x
// y' =  z
// z' = -y
inline vec3 rotate_Zup_to_Yup(const vec3& v) {
    return vec3(
        v.x(),
        v.z(),
        -v.y()
    );
}

// -----------------------------------------------------------------------------
// Cached geometry representation
// -----------------------------------------------------------------------------
struct cached_triangle_geom {
    point3 p0, p1, p2;
    vec3   n0, n1, n2;
    vec3   t0, t1, t2;
    vec3   b0, b1, b2;
    double u0, v0, u1, v1, u2, v2;
    int    material_index;  // Assimp mesh->mMaterialIndex
};

struct cached_mesh_data {
    std::vector<cached_triangle_geom> triangles;
    std::vector<std::string>          material_names;  // scene->mMaterials[i] names

    point3 minp;
    point3 maxp;
    point3 center;
    double base_scale = 1.0;
    double user_scale = 1.0;
    double final_scale = 1.0;
    long long tri_count = 0;
};

// Build a cache key so we don't recompute geometry for the same (file, flags, scale)
inline std::string make_mesh_cache_key(const std::string& filepath,
                                       bool z_up,
                                       bool normalise_to_unit,
                                       double user_scale)
{
    std::ostringstream oss;
    oss.precision(15);
    oss << filepath << "|"
        << (z_up ? "ZUP" : "YUP") << "|"
        << (normalise_to_unit ? "NORM1" : "RAW") << "|"
        << user_scale;
    return oss.str();
}

// Import mesh (via Assimp), compute bounds, pivot, scale, and bake triangles
// into cached_mesh_data. This is done ONCE per unique cache key.
inline std::shared_ptr<cached_mesh_data> build_cached_mesh_data(
    const std::string& filepath,
    bool z_up,
    bool normalise_to_unit,
    double user_scale
)
{
    Assimp::Importer importer;

    const aiScene* scene = importer.ReadFile(
        filepath,
        aiProcess_Triangulate |
        aiProcess_JoinIdenticalVertices |
        aiProcess_GenSmoothNormals |
        aiProcess_CalcTangentSpace |
        aiProcess_GenUVCoords |
        aiProcess_ImproveCacheLocality |
        aiProcess_OptimizeMeshes
    );

    if (!scene || !scene->mRootNode || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE)) {
        throw std::runtime_error(
            "Assimp load failed for '" + filepath + "': " +
            importer.GetErrorString()
        );
    }

    auto data = std::make_shared<cached_mesh_data>();

    // Material names
    std::cout << "\nMaterials in FBX:\n";
    data->material_names.clear();
    data->material_names.reserve(scene->mNumMaterials);
    for (unsigned int i = 0; i < scene->mNumMaterials; ++i) {
        aiMaterial* m = scene->mMaterials[i];

        aiString name;
        m->Get(AI_MATKEY_NAME, name);

        std::cout << "  [" << i << "] " << name.C_Str() << "\n";
        data->material_names.emplace_back(name.C_Str());
    }

    // ----- PASS 1: compute bounds (after optional Z->Y rotation) -----
    point3 minp( 1e30, 1e30, 1e30 );
    point3 maxp(-1e30,-1e30,-1e30 );

    std::function<void(aiNode*)> computeBounds;
    computeBounds = [&](aiNode* node) {
        for (unsigned int m = 0; m < node->mNumMeshes; ++m) {
            aiMesh* mesh = scene->mMeshes[node->mMeshes[m]];
            for (unsigned int v = 0; v < mesh->mNumVertices; ++v) {
                vec3 p(mesh->mVertices[v].x,
                       mesh->mVertices[v].y,
                       mesh->mVertices[v].z);

                if (z_up) {
                    p = rotate_Zup_to_Yup(p);
                }

                minp = point3(
                    std::min(minp.x(), p.x()),
                    std::min(minp.y(), p.y()),
                    std::min(minp.z(), p.z())
                );

                maxp = point3(
                    std::max(maxp.x(), p.x()),
                    std::max(maxp.y(), p.y()),
                    std::max(maxp.z(), p.z())
                );
            }
        }

        for (unsigned int i = 0; i < node->mNumChildren; ++i)
            computeBounds(node->mChildren[i]);
    };

    computeBounds(scene->mRootNode);

    point3 center = 0.5 * (minp + maxp);
    vec3   extent = maxp - minp;

    double base_scale = 1.0;
    if (normalise_to_unit) {
        double max_extent = std::max({ extent.x(), extent.y(), extent.z() });
        if (max_extent <= 0.0) max_extent = 1.0;
        base_scale = 1.0 / max_extent;
    }
    double scale = base_scale * user_scale;

    // Pivot so feet are on y=0, centred in X/Z (same as before)
    double pivot_x = 0.5 * (minp.x() + maxp.x());
    double pivot_z = 0.5 * (minp.z() + maxp.z());
    double pivot_y = minp.y();  // bottom of model (feet)

    vec3 pivot(pivot_x, pivot_y, pivot_z);

    std::cout << "Loaded mesh '" << filepath << "' (cached geometry)\n";
    std::cout << "  min = (" << minp.x() << ", " << minp.y() << ", " << minp.z() << ")\n";
    std::cout << "  max = (" << maxp.x() << ", " << maxp.y() << ", " << maxp.z() << ")\n";
    std::cout << "  center = (" << center.x() << ", " << center.y() << ", " << center.z() << ")\n";
    std::cout << "  z_up = " << (z_up ? "true" : "false")
              << ", normalise_to_unit = " << (normalise_to_unit ? "true" : "false")
              << ", base_scale = " << base_scale
              << ", user_scale = " << user_scale
              << ", final_scale = " << scale << "\n";

    data->minp        = minp;
    data->maxp        = maxp;
    data->center      = center;
    data->base_scale  = base_scale;
    data->user_scale  = user_scale;
    data->final_scale = scale;

    // ----- PASS 2: bake all triangles into cached_triangle_geom -----
    data->triangles.clear();
    data->triangles.reserve(65536); // heuristic

    std::function<void(aiNode*)> processNode;
    processNode = [&](aiNode* node) {
        for (unsigned int m = 0; m < node->mNumMeshes; ++m) {
            aiMesh* mesh = scene->mMeshes[node->mMeshes[m]];

            int matIndex = (int)mesh->mMaterialIndex;

            for (unsigned int f = 0; f < mesh->mNumFaces; ++f) {
                const aiFace& face = mesh->mFaces[f];
                if (face.mNumIndices != 3) continue;

                int i0 = face.mIndices[0];
                int i1 = face.mIndices[1];
                int i2 = face.mIndices[2];

                // positions
                vec3 p0(mesh->mVertices[i0].x, mesh->mVertices[i0].y, mesh->mVertices[i0].z);
                vec3 p1(mesh->mVertices[i1].x, mesh->mVertices[i1].y, mesh->mVertices[i1].z);
                vec3 p2(mesh->mVertices[i2].x, mesh->mVertices[i2].y, mesh->mVertices[i2].z);

                if (z_up) {
                    p0 = rotate_Zup_to_Yup(p0);
                    p1 = rotate_Zup_to_Yup(p1);
                    p2 = rotate_Zup_to_Yup(p2);
                }

                p0 = (p0 - pivot) * scale;
                p1 = (p1 - pivot) * scale;
                p2 = (p2 - pivot) * scale;

                // Assimp cannot generate tangent frames for meshes without UVs.
                // Missing or degenerate normals also need a finite fallback.
                const vec3 face_normal = safe_unit_vector(cross(p1 - p0, p2 - p0));
                auto direction = [&](const aiVector3D* values, int index) {
                    if (!values) return vec3(0, 0, 0);
                    vec3 value(values[index].x, values[index].y, values[index].z);
                    return z_up ? rotate_Zup_to_Yup(value) : value;
                };
                const vec3 n0 = safe_unit_vector(direction(mesh->mNormals, i0), face_normal);
                const vec3 n1 = safe_unit_vector(direction(mesh->mNormals, i1), face_normal);
                const vec3 n2 = safe_unit_vector(direction(mesh->mNormals, i2), face_normal);

                // UVs
                double u0 = 0.0, v0u = 0.0;
                double u1 = 0.0, v1u = 0.0;
                double u2 = 0.0, v2u = 0.0;

                if (mesh->mTextureCoords[0]) {
                    u0 = mesh->mTextureCoords[0][i0].x;
                    v0u = mesh->mTextureCoords[0][i0].y;

                    u1 = mesh->mTextureCoords[0][i1].x;
                    v1u = mesh->mTextureCoords[0][i1].y;

                    u2 = mesh->mTextureCoords[0][i2].x;
                    v2u = mesh->mTextureCoords[0][i2].y;
                }

                auto tangent_frame = [&](int index, const vec3& normal) {
                    hit_record frame;
                    frame.normal = normal;
                    frame.set_tangent_frame(direction(mesh->mTangents, index),
                                            direction(mesh->mBitangents, index));
                    return frame;
                };
                const auto frame0 = tangent_frame(i0, n0);
                const auto frame1 = tangent_frame(i1, n1);
                const auto frame2 = tangent_frame(i2, n2);

                cached_triangle_geom tri;
                tri.p0 = point3(p0.x(), p0.y(), p0.z());
                tri.p1 = point3(p1.x(), p1.y(), p1.z());
                tri.p2 = point3(p2.x(), p2.y(), p2.z());

                tri.n0 = n0;
                tri.n1 = n1;
                tri.n2 = n2;

                tri.t0 = frame0.tangent;
                tri.t1 = frame1.tangent;
                tri.t2 = frame2.tangent;

                tri.b0 = frame0.bitangent;
                tri.b1 = frame1.bitangent;
                tri.b2 = frame2.bitangent;

                tri.u0 = u0;   tri.v0 = v0u;
                tri.u1 = u1;   tri.v1 = v1u;
                tri.u2 = u2;   tri.v2 = v2u;

                tri.material_index = matIndex;

                data->triangles.push_back(tri);
            }
        }

        for (unsigned int i = 0; i < node->mNumChildren; ++i)
            processNode(node->mChildren[i]);
    };

    processNode(scene->mRootNode);

    data->tri_count = (long long)data->triangles.size();
    std::cout << "  Triangles baked into cache: " << data->tri_count << "\n";

    return data;
}

// Global per-TU cache: file+flags+scale -> cached_mesh_data
inline std::shared_ptr<cached_mesh_data> get_cached_mesh_data(
    const std::string& filepath,
    bool z_up,
    bool normalise_to_unit,
    double user_scale
)
{
    static std::unordered_map<std::string, std::weak_ptr<cached_mesh_data>> cache;

    std::string key = make_mesh_cache_key(filepath, z_up, normalise_to_unit, user_scale);

    auto it = cache.find(key);
    if (it != cache.end()) {
        if (auto existing = it->second.lock()) {
            return existing;
        }
    }

    auto fresh = build_cached_mesh_data(filepath, z_up, normalise_to_unit, user_scale);
    cache[key] = fresh;
    return fresh;
}

// -----------------------------------------------------------------------------
// 1) Single-material version (now using cached geometry)
// -----------------------------------------------------------------------------
inline std::shared_ptr<hittable_list> load_mesh_as_triangles(
    const std::string& filepath,
    const std::shared_ptr<material>& mat,
    bool z_up = false,              // if true: treat source as Z-up and rotate to Y-up
    bool normalise_to_unit = true,  // if true: scale to fit within unit cube
    double user_scale = 1.0
) {
    auto cached = get_cached_mesh_data(filepath, z_up, normalise_to_unit, user_scale);

    auto world = std::make_shared<hittable_list>();

    for (const auto& tri : cached->triangles) {
        world->add(std::make_shared<triangle>(
            tri.p0, tri.p1, tri.p2,
            tri.n0, tri.n1, tri.n2,
            tri.u0, tri.v0,
            tri.u1, tri.v1,
            tri.u2, tri.v2,
            tri.t0, tri.t1, tri.t2,
            tri.b0, tri.b1, tri.b2,
            mat
        ));
    }

    std::cout << "load_mesh_as_triangles(single) '" << filepath
              << "' -> " << cached->tri_count << " triangles\n";

    return world;
}

// -----------------------------------------------------------------------------
// 2) Multi-material version: map FBX material name -> your material
//    Also uses cached geometry.
// -----------------------------------------------------------------------------
inline std::shared_ptr<hittable_list> load_mesh_as_triangles(
    const std::string& filepath,
    const std::unordered_map<std::string, std::shared_ptr<material>>& material_table,
    std::shared_ptr<material> default_mat,
    bool z_up = false,
    bool normalise_to_unit = true,
    double user_scale = 1.0
) {
    auto cached = get_cached_mesh_data(filepath, z_up, normalise_to_unit, user_scale);

    auto world = std::make_shared<hittable_list>();

    // Pre-resolve Assimp material index -> your material pointer once
    std::vector<std::shared_ptr<material>> slot_mats;
    slot_mats.resize(cached->material_names.size(), default_mat);

    for (size_t i = 0; i < cached->material_names.size(); ++i) {
        const std::string& name = cached->material_names[i];
        auto it = material_table.find(name);
        if (it != material_table.end()) {
            slot_mats[i] = it->second;
        } else {
            std::cout << "WARNING: no material mapping for '" << name
                      << "' (FBX slot " << i << "), using default.\n";
        }
    }

    for (const auto& tri : cached->triangles) {
        std::shared_ptr<material> useMat = default_mat;
        if (tri.material_index >= 0 &&
            tri.material_index < (int)slot_mats.size())
        {
            useMat = slot_mats[tri.material_index];
        }

        world->add(std::make_shared<triangle>(
            tri.p0, tri.p1, tri.p2,
            tri.n0, tri.n1, tri.n2,
            tri.u0, tri.v0,
            tri.u1, tri.v1,
            tri.u2, tri.v2,
            tri.t0, tri.t1, tri.t2,
            tri.b0, tri.b1, tri.b2,
            useMat
        ));
    }

    std::cout << "load_mesh_as_triangles(multi) '" << filepath
              << "' -> " << cached->tri_count << " triangles\n";

    return world;
}

#endif // MESH_LOADER_H
