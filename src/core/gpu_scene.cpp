#include "core/dusktracer.h"
#include "core/gpu_scene.h"
#include "core/mesh_loader.h"
#include "core/transform.h"
#include "core/sphere.h"
#include <unordered_map>
#include <cstring>
#include <filesystem>
#include <stdexcept>

namespace dusk_gpu {
namespace {
f4 pack(const vec3& v, float w = 0) {
    for (int c = 0; c < 3; ++c) {
        if (!std::isfinite(v[c]) || std::abs(v[c]) > 1e20) {
            throw std::runtime_error("GPU scene contains a non-finite or out-of-range value.");
        }
    }
    return {float(v.x()), float(v.y()), float(v.z()), w};
}
vec3 unpack(const f4& v) {
    return {v.x, v.y, v.z};
}
vec3 vector_transform(const instance& i, const vec3& p) {
    return {dot(unpack(i.rows[0]), p), dot(unpack(i.rows[1]), p), dot(unpack(i.rows[2]), p)};
}
struct compiler {
    const scene& source;
    const std::atomic<bool>* cancel;
    snapshot out;
    std::unordered_map<std::string, int> texture_ids;
    void check_cancelled() const {
        if (cancel && cancel->load()) {
            throw std::runtime_error("GPU scene preparation cancelled.");
        }
    }
    int texture_id(const graph_node& node) {
        check_cancelled();
        std::string path = node.texture_path;
        if (path.empty() && node.texture_index >= 0 && node.texture_index < int(source.textures.size())) {
            path = source.textures[node.texture_index].path;
        }
        if (path.empty()) {
            return -1; // unbound texture node uses CPU's magenta fallback
        }
        auto found = texture_ids.find(path);
        if (found != texture_ids.end()) {
            return found->second;
        }
        rtw_image image(path.c_str());
        if (image.width() <= 0 || image.height() <= 0) {
            throw std::runtime_error("Cannot load GPU texture: " + path);
        }
        size_t words = size_t(image.width()) * image.height() * (image.is_hdr() ? 4 : 1);
        if (words + out.texels.size() > size_t(INT32_MAX)) {
            throw std::runtime_error("GPU texture storage exceeds the upload limit.");
        }
        int id = int(out.textures.size());
        texture_ids[path] = id;
        out.textures.push_back({{int(out.texels.size()), image.width(), image.height(),
                                 image.channels() | (image.is_hdr() ? 256 : 0)}});
        out.texels.reserve(out.texels.size() + words);
        for (int y = 0; y < image.height(); ++y) {
            check_cancelled();
            for (int x = 0; x < image.width(); ++x) {
                float rgba[4];
                int ch = image.channels();
                for (int c = 0; c < 3; ++c) {
                    rgba[c] = float(image.channel_value(x, y, ch <= 2 ? 0 : c));
                }
                rgba[3] = image.has_alpha() ? float(image.channel_value(x, y, ch == 2 ? 1 : 3)) : 1;
                if (image.is_hdr()) {
                    uint32_t bits[4];
                    std::memcpy(bits, rgba, sizeof(bits));
                    out.texels.insert(out.texels.end(), bits, bits + 4);
                } else {
                    uint32_t bits = 0;
                    for (int c = 0; c < 4; ++c) {
                        bits |= uint32_t(std::lround(std::clamp(rgba[c], 0.f, 1.f) * 255)) << (c * 8);
                    }
                    out.texels.push_back(bits);
                }
            }
        }
        return id;
    }
    void material(const scene_material& m) {
        if (m.model == scene_material_model::pbr && m.use_sss && m.sss_strength > 0) {
            throw std::runtime_error("Material '" + m.name +
                                     "' uses subsurface scattering, which currently requires CPU rendering.");
        }
        out.materials.push_back(compile_material(
            source, m, [&](const graph_node& node) { return texture_id(node); }, out.instructions,
            [&](int id) { return out.textures[id].data.w & 255; }));
    }
    void triangle(const cached_triangle_geom& t) {
        auto append_vertex = [&](const point3& position, const vec3& normal, const vec3& tangent,
                                 const vec3& bitangent, double u, double v) {
            hit_record frame;
            frame.normal = safe_unit_vector(normal);
            frame.set_tangent_frame(tangent, bitangent);
            out.vertices.push_back({pack(position),
                                    pack(normal),
                                    pack(frame.tangent),
                                    pack(frame.bitangent),
                                    {float(u), float(v), 0, 0}});
        };
        append_vertex(t.p0, t.n0, t.t0, t.b0, t.u0, t.v0);
        append_vertex(t.p1, t.n1, t.t1, t.b1, t.u1, t.v1);
        append_vertex(t.p2, t.n2, t.t2, t.b2, t.u2, t.v2);
        out.slots.push_back(std::max(0, t.material_index));
    }
    void cube() {
        point3 q[6] = {{-.5, -.5, .5},  {.5, -.5, .5}, {.5, -.5, -.5},
                       {-.5, -.5, -.5}, {-.5, .5, .5}, {-.5, -.5, -.5}};
        vec3 u[6] = {{1, 0, 0}, {0, 0, -1}, {-1, 0, 0}, {0, 0, 1}, {1, 0, 0}, {1, 0, 0}};
        vec3 v[6] = {{0, 1, 0}, {0, 1, 0}, {0, 1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}};
        for (int f = 0; f < 6; ++f) {
            for (int half = 0; half < 2; ++half) {
                cached_triangle_geom t{};
                vec3 n = unit_vector(cross(u[f], v[f]));
                t.p0 = q[f];
                t.p1 = q[f] + u[f] + (half ? v[f] : vec3());
                t.p2 = q[f] + v[f] + (half ? vec3() : u[f]);
                t.n0 = t.n1 = t.n2 = n;
                t.t0 = t.t1 = t.t2 = u[f];
                t.b0 = t.b1 = t.b2 = v[f];
                t.u1 = 1;
                t.v1 = half ? 1 : 0;
                t.u2 = half ? 0 : 1;
                t.v2 = 1;
                triangle(t);
            }
        }
    }
    snapshot build() {
        // Compile only materials actually bound by an instance. Remapping below
        // ensures an unused unsupported library material cannot prevent a render.
        std::unordered_map<int, int> compiled;
        auto mat = [&](int source_index, bool mesh = false) {
            int key = source_index >= 0 && source_index < int(source.materials.size()) ? source_index
                                                                                       : (mesh ? -2 : -1);
            auto found = compiled.find(key);
            if (found != compiled.end()) {
                return found->second;
            }
            int id = int(out.materials.size());
            compiled[key] = id;
            if (key >= 0) {
                material(source.materials[key]);
            } else {
                scene_material fallback;
                fallback.base_color = key == -2 ? vec3(1, 0, 1) : vec3(.5, .5, .5);
                material(fallback);
            }
            return id;
        };
        out.geometries.push_back({{0, 0, 1, 1}}); // shared analytic unit sphere
        out.geometries.push_back({{0, 0, 12, 0}});
        cube();
        std::unordered_map<int, std::pair<int, std::shared_ptr<cached_mesh_data>>> meshes;
        auto dummy = std::make_shared<sphere>(point3(), 1, nullptr);
        for (const auto& object : source.objects) {
            check_cancelled();
            instance inst{};
            inst.data.y = int(out.material_map.size());
            inst.data.z = -1;
            vec3 scale = object.scale;
            for (int c = 0; c < 3; ++c) {
                if (scale[c] == 0) {
                    scale[c] = 1;
                }
            }
            if (object.type == scene_object_type::sphere) {
                if (object.radius <= 0) {
                    throw std::runtime_error("GPU sphere radius must be positive.");
                }
                scale *= object.radius;
                inst.data.x = 0;
                out.material_map.push_back(mat(object.material_index));
            } else if (object.type == scene_object_type::cube) {
                inst.data.x = 1;
                out.material_map.push_back(mat(object.material_index));
            } else {
                if (object.mesh_index < 0 || object.mesh_index >= int(source.meshes.size())) {
                    continue;
                }
                const auto& asset = source.meshes[object.mesh_index];
                auto entry = meshes.find(object.mesh_index);
                if (entry == meshes.end()) {
                    auto ext = std::filesystem::path(asset.file_path).extension().string();
                    std::transform(ext.begin(), ext.end(), ext.begin(),
                                   [](unsigned char c) { return char(std::tolower(c)); });
                    // Local importer, not the editor's mutable static mesh cache.
                    auto mesh = build_cached_mesh_data(asset.file_path, ext == ".fbx", true, 2);
                    int id = int(out.geometries.size());
                    out.geometries.push_back(
                        {{int(out.vertices.size()), int(out.slots.size()), int(mesh->triangles.size()), 0}});
                    for (const auto& t : mesh->triangles) {
                        triangle(t);
                    }
                    entry = meshes.emplace(object.mesh_index, std::make_pair(id, std::move(mesh))).first;
                }
                inst.data.x = entry->second.first;
                for (const auto& name : entry->second.second->material_names) {
                    int material = object.material_index;
                    for (size_t s = 0; s < asset.slot_names.size() && s < object.mesh_slot_materials.size();
                         ++s) {
                        if (name == asset.slot_names[s] && object.mesh_slot_materials[s] >= 0 &&
                            object.mesh_slot_materials[s] < int(source.materials.size())) {
                            material = object.mesh_slot_materials[s];
                        }
                    }
                    out.material_map.push_back(mat(material, true));
                }
                if (entry->second.second->material_names.empty()) {
                    out.material_map.push_back(mat(object.material_index, true));
                }
            }
            transform trs(dummy, object.translation + object.center, object.rotation_deg, scale);
            float rows[12];
            trs.affine_rows(rows);
            std::memcpy(inst.rows, rows, sizeof(rows));
            const auto& geom = out.geometries[inst.data.x];
            if (geom.data.z == 0) {
                continue;
            }
            int instance_id = int(out.instances.size());
            inst.data.z = int(out.lights.size());
            for (int p = 0; p < geom.data.z; ++p) {
                int mi = out.material_map[inst.data.y + (geom.data.w ? 0 : out.slots[geom.data.y + p])];
                if (int(out.materials[mi].base.w) != int(scene_material_model::diffuse_light)) {
                    continue;
                }
                double area;
                if (geom.data.w) {
                    // Ellipsoid area estimate only chooses a light; the shader uses
                    // the exact surface Jacobian for both sampling and MIS PDFs.
                    constexpr double power = 1.6075;
                    area = 4 * pi *
                           std::pow((std::pow(std::abs(scale.x() * scale.y()), power) +
                                     std::pow(std::abs(scale.x() * scale.z()), power) +
                                     std::pow(std::abs(scale.y() * scale.z()), power)) /
                                        3,
                                    1 / power);
                } else {
                    int v = geom.data.x + 3 * p;
                    auto e1 = vector_transform(inst, unpack(out.vertices[v + 1].position) -
                                                         unpack(out.vertices[v].position));
                    auto e2 = vector_transform(inst, unpack(out.vertices[v + 2].position) -
                                                         unpack(out.vertices[v].position));
                    area = .5 * cross(e1, e2).length();
                }
                if (area <= 1e-12) {
                    continue;
                }
                const auto& m = out.materials[mi];
                double power =
                    std::max(.001, double(m.emission.w) *
                                       (.2126 * m.emission.x + .7152 * m.emission.y + .0722 * m.emission.z));
                out.lights.push_back({{instance_id, p, 0, 0}, {float(area * power), 0, float(area), 0}});
            }
            inst.data.w = int(out.lights.size()) - inst.data.z;
            out.instances.push_back(inst);
        }
        double sum = 0;
        for (const auto& l : out.lights) {
            sum += l.distribution.x;
        }
        double cdf = 0;
        for (auto& l : out.lights) {
            l.distribution.y = float(l.distribution.x / sum);
            cdf += l.distribution.y;
            l.distribution.x = float(cdf);
        }
        if (!out.lights.empty()) {
            out.lights.back().distribution.x = 1;
        }
        if (out.instances.size() > 0xffffff) {
            throw std::runtime_error("GPU instance index limit exceeded.");
        }
        return std::move(out);
    }
};
} // namespace
snapshot compile_scene(const scene& source, const std::atomic<bool>* cancel) {
    return compiler{source, cancel}.build();
}
} // namespace dusk_gpu
