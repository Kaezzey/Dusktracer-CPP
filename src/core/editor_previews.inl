#include "core/preview_worker.h"
#include "core/preview_cache.h"
#include <iomanip>
#include <deque>

static constexpr int MATERIAL_THUMB_SIZE = 128;
static std::unordered_map<int, GLuint> g_material_thumb_cache;
static preview_worker g_editor_previews;
static preview_worker g_texture_previews;
static preview_worker g_sphere_previews;
struct preview_state {
    uint64_t requested = 0, displayed = 0;
    bool dirty = true;
    int size = 0, requested_size = 0;
    std::chrono::steady_clock::time_point submitted{};
};
static std::unordered_map<int, preview_state> g_material_preview_states;
static std::unordered_map<std::string, uint64_t> g_texture_preview_requests;
static uint64_t g_preview_revision = 0;
struct sphere_preview_state {
    preview_state image;
    GLuint texture = 0;
    float yaw = 0, pitch = 0, zoom = 1;
};
static std::unordered_map<int, sphere_preview_state> g_sphere_preview_states;

static std::shared_ptr<texture> LoadPreviewTexture(const std::string& path, texture_sample_space space) {
    // An eight-image LRU keeps graph edits fast with bounded retained memory.
    struct entry {
        std::string stamp;
        std::shared_ptr<rtw_image> image;
    };
    thread_local std::deque<entry> decoded;
    const auto stamp = preview_cache::file_stamp(path);
    std::shared_ptr<rtw_image> image;
    for (auto it = decoded.begin(); it != decoded.end(); ++it) {
        if (it->stamp == stamp) {
            image = it->image;
            decoded.erase(it);
            break;
        }
    }
    if (!image) {
        if (stbi_is_hdr(path.c_str())) {
            image = std::make_shared<rtw_image>(path.c_str());
        } else {
            auto data = preview_cache::texture(path);
            if (data.image.pixels.empty()) {
                return std::make_shared<solid_colour>(colour(0, 1, 1));
            }
            image = std::make_shared<rtw_image>(data.image.width, data.image.height, data.source_channels,
                                                data.image.pixels);
        }
    }
    decoded.push_front({stamp, image});
    if (decoded.size() > 8) {
        decoded.pop_back();
    }
    return std::make_shared<image_texture>(image, space);
}

static uint64_t MaterialPreviewStamp(const scene& assets, const scene_material& m) {
    std::ostringstream key;
    key << std::setprecision(17) << int(m.model) << '|' << m.base_color << '|' << m.metallic << '|'
        << m.roughness << '|' << m.fuzz << '|' << m.ior << '|' << m.emission << '|' << m.emission_intensity
        << '|' << m.dielectric_F0 << '|' << m.normal_strength << '|' << m.alpha_cutoff << '|'
        << m.alpha_double_sided << '|' << m.unreal_pbr << '|' << m.use_sss << '|' << m.sss_strength << '|'
        << m.sss_scale << '|' << m.sss_model << '|' << m.sss_samples << '|' << m.sss_radius << '|'
        << m.sss_eta << '|' << m.sss_color_override_enabled << '|' << m.sss_color_override_color << '|'
        << graph_surface_key(m.graph);
    auto texture_stamp = [&](int index, const std::string& path = {}) {
        if (!path.empty()) {
            key << '|' << preview_cache::file_stamp(path);
        } else if (index >= 0 && index < int(assets.textures.size())) {
            key << '|' << preview_cache::file_stamp(assets.textures[index].path);
        } else {
            key << "|none";
        }
    };
    for (int index : {m.albedo_tex, m.metallic_tex, m.roughness_tex, m.normal_tex, m.alpha_tex}) {
        texture_stamp(index);
    }
    for (const auto& node : m.graph.nodes) {
        if (node.kind == graph_kind::texture_sample) {
            texture_stamp(node.texture_index, node.texture_path);
        }
    }
    return preview_cache::hash(key.str());
}

static preview_image RenderMaterialPreview(const scene& assets, const scene_material& description, int size,
                                           float yaw = 0, float pitch = 0, float zoom = 1) {
    auto runtime = build_rt_material(assets, description, LoadPreviewTexture);
    // Shade in sphere-local coordinates. Rotating the view and studio lights
    // together leaves lighting fixed while the sphere's UVs and normals turn.
    const double cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
    auto local = [=](vec3 v) {
        const vec3 y(cy*v.x()-sy*v.z(), v.y(), sy*v.x()+cy*v.z());
        return vec3(y.x(), cp*y.y()+sp*y.z(), -sp*y.y()+cp*y.z());
    };
    hittable_list empty_world;
    preview_image image;
    image.width = image.height = size;
    image.pixels.resize(size * size * 4);
    const vec3 view = local(vec3(0, 0, 1)), key = local(unit_vector(vec3(-.5, .7, 1.2))),
               fill = local(unit_vector(vec3(.8, .1, .4)));
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            double sx = (2.0 * (x + .5) / size - 1) / (.88 * zoom),
                   sy = (1 - 2.0 * (y + .5) / size) / (.88 * zoom);
            colour background(.028, .039, .052), c = background;
            double r2 = sx * sx + sy * sy;
            if (r2 < 1) {
                hit_record rec{};
                rec.p = rec.normal = local(vec3(sx, sy, std::sqrt(1 - r2)));
                rec.front_face = true;
                rec.u = (std::atan2(-rec.p.z(), rec.p.x()) + pi) / (2 * pi);
                rec.v = std::acos(-rec.p.y()) / pi;
                rec.set_tangent_frame(vec3(rec.p.z(), 0, -rec.p.x()),
                                      cross(rec.normal, vec3(rec.p.z(), 0, -rec.p.x())));
                c = runtime->albedo(rec) * .12 + runtime->emitted(rec.u, rec.v, rec.p) +
                    runtime->shade_direct(rec, view, key, colour(3.8, 3.6, 3.4), empty_world) +
                    runtime->shade_direct(rec, view, fill, colour(.8, 1, 1.3), empty_world);
                double alpha = runtime->opacity_at(rec);
                c = alpha * c + (1 - alpha) * background;
            }
            int at = (y * size + x) * 4;
            for (int channel = 0; channel < 3; ++channel) {
                double v = std::max(0.0, c[channel]);
                v /= 1 + v;
                image.pixels[at + channel] = (unsigned char)(255 * linear_to_gamma(std::clamp(v, 0.0, 1.0)));
            }
            image.pixels[at + 3] = 255;
        }
    }
    return image;
}
static preview_image CachedMaterialThumbnail(const scene& assets, const scene_material& material, int index,
                                             int size) {
    if (size != MATERIAL_THUMB_SIZE) {
        return RenderMaterialPreview(assets, material, size);
    }
    const auto identity = "material:" + std::to_string(index);
    const auto stamp = MaterialPreviewStamp(assets, material);
    auto cached = preview_cache::read(identity, stamp);
    if (!cached.image.pixels.empty()) {
        return std::move(cached.image);
    }
    auto image = RenderMaterialPreview(assets, material, size);
    preview_cache::write(identity, stamp, {image, 4});
    return image;
}
static preview_image DecodeTextureThumbnail(const std::string& path) {
    auto data = preview_cache::texture(path);
    if (data.image.pixels.empty()) {
        return {};
    }
    return preview_cache::resize(data.image.pixels.data(), data.image.width, data.image.height, 128);
}
static void UploadPreview(GLuint& texture, const preview_image& image, bool reuse_storage) {
    if (!texture) {
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    } else {
        glBindTexture(GL_TEXTURE_2D, texture);
    }
    if (reuse_storage) {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, image.width, image.height, GL_RGBA, GL_UNSIGNED_BYTE,
                        image.pixels.data());
    } else {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, image.width, image.height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                     image.pixels.data());
    }
    glBindTexture(GL_TEXTURE_2D, 0);
}
static void PumpEditorPreviews() {
    auto results = g_editor_previews.take_results();
    for (auto& result : g_texture_previews.take_results()) {
        results.push_back(std::move(result));
    }
    for (auto& result : g_sphere_previews.take_results()) {
        results.push_back(std::move(result));
    }
    for (auto& result : results) {
        if (result.key[0] == 'S') {
            auto it = g_sphere_preview_states.find(std::stoi(result.key.substr(1)));
            if (it == g_sphere_preview_states.end() || it->second.image.requested != result.revision) {
                continue;
            }
            auto& preview = it->second;
            if (!result.image.pixels.empty()) {
                UploadPreview(preview.texture, result.image,
                              preview.texture && preview.image.size == result.image.width);
                preview.image.size = result.image.width;
            }
            preview.image.displayed = result.revision;
            continue;
        }
        if (result.key[0] == 'M') {
            int id = std::stoi(result.key.substr(1));
            auto it = g_material_preview_states.find(id);
            if (it == g_material_preview_states.end() || it->second.requested != result.revision ||
                id >= (int)g_scene.materials.size()) {
                continue;
            }
            auto& state = it->second;
            if (!result.image.pixels.empty()) {
                auto& texture = g_material_thumb_cache[id];
                UploadPreview(texture, result.image, texture && state.size == result.image.width);
                state.size = result.image.width;
            }
            state.displayed = result.revision;
        } else {
            auto path = result.key.substr(1);
            auto it = g_texture_preview_requests.find(path);
            if (it == g_texture_preview_requests.end() || it->second != result.revision) {
                continue;
            }
            GLuint texture = 0;
            if (!result.image.pixels.empty()) {
                UploadPreview(texture, result.image, false);
            }
            g_texture_thumb_cache[path] =
                texture; // Also cache failures; don't retry a missing file every frame.
        }
    }
}
static GLuint GetOrCreateTextureThumbnail(const std::string& path) {
    PumpEditorPreviews();
    auto found = g_texture_thumb_cache.find(path);
    if (found != g_texture_thumb_cache.end()) {
        return found->second;
    }
    if (g_texture_preview_requests.count(path) || g_thumbs_created_this_frame >= g_thumb_budget_per_frame) {
        return 0;
    }
    uint64_t revision = ++g_preview_revision;
    if (g_texture_previews.request("T" + path, revision, [path] { return DecodeTextureThumbnail(path); })) {
        g_texture_preview_requests[path] = revision;
        ++g_thumbs_created_this_frame;
    }
    return 0;
}
static GLuint GetOrCreateMaterialThumbnail(int index) {
    PumpEditorPreviews();
    if (index < 0 || index >= (int)g_scene.materials.size()) {
        return 0;
    }
    auto& state = g_material_preview_states[index];
    auto& texture = g_material_thumb_cache[index];
    if (!texture) {
        preview_image placeholder;
        placeholder.width = placeholder.height = 1;
        placeholder.pixels = {37, 48, 61, 255};
        UploadPreview(texture, placeholder, false);
        state.size = 1;
        state.dirty = true;
    }
    bool interactive = ImGui::IsAnyItemActive();
    int size = interactive ? 64 : MATERIAL_THUMB_SIZE;
    auto now = std::chrono::steady_clock::now();
    bool refine = !interactive && state.requested_size == 64;
    bool idle = state.requested == state.displayed;
    if ((state.dirty || refine) && idle &&
        (state.requested == 0 || now - state.submitted >= std::chrono::milliseconds(80))) {
        scene assets;
        assets.textures = g_scene.textures; // Never copy geometry or touch the live scene on the worker.
        auto description = g_scene.materials[index];
        uint64_t revision = ++g_preview_revision;
        if (g_editor_previews.request(
                "M" + std::to_string(index), revision,
                [assets = std::move(assets), description = std::move(description), index, size] {
                    return CachedMaterialThumbnail(assets, description, index, size);
                },
                index == g_selected_material)) {
            state.requested = revision;
            state.requested_size = size;
            state.submitted = now;
            state.dirty = false;
        }
    }
    return texture;
}
static void InvalidateMaterialThumbnail(int index) {
    InvalidateRasterMaterial(index);
    // ImGui draw commands may already reference this GL name in this frame.
    // Retain it and the last image until the worker's replacement is ready.
    g_material_preview_states[index].dirty = true;
    g_sphere_preview_states[index].image.dirty = true;
}
static void InvalidateAllMaterialThumbnails() {
    InvalidateAllRasterMaterials();
    // Index-changing insert/erase/load operations invalidate outstanding jobs as
    // well as cached images, so an old result can't paint a different material.
    for (auto& item : g_sphere_preview_states) {
        auto& state = item.second.image;
        state.requested = state.displayed = ++g_preview_revision;
        state.dirty = true;
        state.submitted = {};
    }
    for (auto& item : g_material_preview_states) {
        item.second.requested = item.second.displayed = ++g_preview_revision;
        item.second.dirty = true;
        item.second.submitted = {};
    }
}

static GLuint GetMaterialSpherePreview(int index, bool interactive) {
    PumpEditorPreviews();
    if (index < 0 || index >= int(g_scene.materials.size())) {
        return 0;
    }
    auto& preview = g_sphere_preview_states[index];
    auto& state = preview.image;
    const int size = interactive ? 192 : 512;
    const auto now = std::chrono::steady_clock::now();
    if ((state.dirty || state.requested_size != size) && state.requested == state.displayed &&
        (state.requested == 0 || now - state.submitted >= std::chrono::milliseconds(33))) {
        scene assets;
        assets.textures = g_scene.textures;
        auto material = g_scene.materials[index];
        const uint64_t revision = ++g_preview_revision;
        float yaw = preview.yaw, pitch = preview.pitch, zoom = preview.zoom;
        if (g_sphere_previews.request(
                "S" + std::to_string(index), revision,
                [assets = std::move(assets), material = std::move(material), size, yaw, pitch, zoom] {
                    return RenderMaterialPreview(assets, material, size, yaw, pitch, zoom);
                },
                true)) {
            state.requested = revision;
            state.requested_size = size;
            state.submitted = now;
            state.dirty = false;
        }
    }
    return preview.texture;
}
static void DrawMaterialSpherePreview(int index) {
    auto& preview = g_sphere_preview_states[index];
    const float side = std::clamp(ImGui::GetContentRegionAvail().x, 32.f, 384.f);
    ImGui::PushID("SpherePreview");
    ImGui::PushID(index);
    const auto start = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("Orbit", ImVec2(side, side));
    const bool hovered = ImGui::IsItemHovered(), dragging = ImGui::IsItemActive();
    auto& io = ImGui::GetIO();
    if (hovered) ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
    if (dragging && ImGui::IsMouseDragging(0)) {
        preview.yaw = std::remainder(preview.yaw + io.MouseDelta.x * .012f, float(2 * pi));
        preview.pitch = std::clamp(preview.pitch + io.MouseDelta.y * .012f, -1.5f, 1.5f);
        preview.image.dirty = true;
    }
    if (hovered && io.MouseWheel != 0) {
        preview.zoom = std::clamp(preview.zoom + io.MouseWheel * .08f, .55f, 1.1f);
        preview.image.dirty = true;
    }
    const auto texture = GetMaterialSpherePreview(index, dragging || ImGui::IsAnyItemActive());
    if (texture) {
        ImGui::GetWindowDrawList()->AddImage((ImTextureID)(intptr_t)texture, start,
                                             ImVec2(start.x + side, start.y + side));
    } else {
        ImGui::GetWindowDrawList()->AddText(ImVec2(start.x + 12, start.y + side * .5f),
                                            ImGui::GetColorU32(ImGuiCol_TextDisabled),
                                            "Preparing material preview...");
    }
    if (hovered) {
        ImGui::SetTooltip("Drag to rotate  /  Scroll to zoom");
    }
    ImGui::TextDisabled("Drag to rotate");
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset")) {
        preview.yaw = preview.pitch = 0;
        preview.zoom = 1;
        preview.image.dirty = true;
    }
    ImGui::PopID();
    ImGui::PopID();
    ImGui::Separator();
}
static void StopEditorPreviews() {
    g_sphere_previews.stop();
    g_texture_previews.stop();
    g_editor_previews.stop();
    for (auto& item : g_sphere_preview_states) {
        if (item.second.texture) {
            glDeleteTextures(1, &item.second.texture);
        }
    }
    g_sphere_preview_states.clear();
}

static void DrawAssetCellLabel(const char* label, ImVec2 cell_min, ImVec2 cell_max, float top) {
    const auto text_size = ImGui::CalcTextSize(label);
    const float width = std::min(text_size.x, cell_max.x - cell_min.x - 12.f);
    const ImVec2 start(cell_min.x + (cell_max.x - cell_min.x - width) * .5f, top);
    ImGui::SetCursorScreenPos(start);
    ImGui::RenderTextEllipsis(ImGui::GetWindowDrawList(), start, ImVec2(start.x + width, top + text_size.y),
                              start.x + width, label, nullptr, &text_size);
    ImGui::Dummy(ImVec2(width, text_size.y));
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", label);
    }
}
