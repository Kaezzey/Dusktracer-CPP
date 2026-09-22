#include "raster_shaders.h"
#include <deque>

namespace raster_preview {
constexpr int texture_slots = 12;
constexpr int sun_resolution = 2048;
constexpr int point_resolution = 512;
constexpr size_t texture_budget = 256u * 1024u * 1024u;
struct program {
    GLuint id = 0;
    std::unordered_map<std::string, GLint> uniforms;
    GLint location(const char* name) {
        auto found = uniforms.find(name);
        if (found != uniforms.end()) {
            return found->second;
        }
        return uniforms.emplace(name, glGetUniformLocation(id, name)).first->second;
    }
};
struct material {
    dusk_gpu::material_record record;
    GLuint instructions = 0;
    std::vector<std::string> paths;
    bool dirty = true;
    uint64_t opacity_key = 0;
    std::string error;
};
struct texture_asset {
    GLuint id = 0;
    int channels = 4;
    bool requested = false, loaded = false;
    size_t bytes = 0;
    uint64_t last_frame = 0;
};
struct draw {
    GLuint vao;
    unsigned first;
    GLsizei count;
    int material_index, object_index;
    float model[16], normal[9];
};
struct lighting {
    vec3 sun_direction{1, 1, .5}, sun_radiance{};
    std::vector<dusk_gpu::f4> positions, radiances;
    int shadow_source = -1;
};
static program surface_program, shadow_program;
static std::unordered_map<int, material> materials;
static std::unordered_map<std::string, texture_asset> textures;
static preview_worker texture_worker;
static std::deque<preview_worker::result> ready_textures;
static uint64_t frame = 0, revision = 1, shadow_key = 0;
static size_t texture_bytes = 0;
static unsigned queued_textures = 0;
static GLuint sun_depth = 0, point_depth = 0, shadow_fbo = 0, placeholder = 0;
static float sun_view[16], sun_projection[16], shadow_far = 100, world_texel = .01f;
static bool shadows_enabled = true;
static int debug_view = 0;
static std::string material_error;
static uint64_t shadow_updates = 0;

static void configure_texture(GLuint id, bool mipmaps) {
    glBindTexture(GL_TEXTURE_2D, id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mipmaps ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mipmaps ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (mipmaps && GLEW_EXT_texture_filter_anisotropic) {
        GLfloat maximum = 1;
        glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &maximum);
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, std::min(maximum, 8.f));
    }
}
static preview_image decode_texture(const std::string& path) {
    preview_image image;
    int info = 0;
    if (!stbi_is_hdr(path.c_str())) {
        auto cached = preview_cache::texture(path, nullptr, 2048);
        image = std::move(cached.image);
        info = cached.source_channels;
        if (image.pixels.empty()) {
            return {};
        }
        image.pixels.insert(image.pixels.begin(), sizeof(info), 0);
    } else {
        int width = 0, height = 0, channels = 0;
        std::unique_ptr<float, decltype(&stbi_image_free)> pixels(
            stbi_loadf(path.c_str(), &width, &height, &channels, 4), stbi_image_free);
        if (!pixels || width <= 0 || height <= 0) {
            return {};
        }
        double scale = std::min(1., 2048. / std::max(width, height));
        image.width = std::max(1, int(width * scale));
        image.height = std::max(1, int(height * scale));
        image.pixels.resize(sizeof(info) + size_t(image.width) * image.height * 4 * sizeof(float));
        for (int y = 0; y < image.height; ++y) {
            for (int x = 0; x < image.width; ++x) {
                int x0 = x * width / image.width, x1 = (x + 1) * width / image.width;
                int y0 = y * height / image.height, y1 = (y + 1) * height / image.height;
                float sum[4]{};
                for (int sy = y0; sy < y1; ++sy) {
                    for (int sx = x0; sx < x1; ++sx) {
                        for (int c = 0; c < 4; ++c) {
                            sum[c] += pixels.get()[(size_t(sy) * width + sx) * 4 + c];
                        }
                    }
                }
                for (float& c : sum) {
                    c /= float((x1 - x0) * (y1 - y0));
                }
                std::memcpy(image.pixels.data() + sizeof(info) + (size_t(y) * image.width + x) * sizeof(sum),
                            sum, sizeof(sum));
            }
        }
        info = channels | 256;
    }
    std::memcpy(image.pixels.data(), &info, sizeof(info));
    return image;
}
static void pump_textures() {
    for (auto& result : texture_worker.take_results()) {
        ready_textures.push_back(std::move(result));
    }
    // Keep large uploads out of graph-edit callbacks and limit them to one per frame.
    if (ready_textures.empty()) {
        return;
    }
    auto result = std::move(ready_textures.front());
    ready_textures.pop_front();
    --queued_textures;
    auto found = textures.find(result.key);
    if (found == textures.end()) {
        return;
    }
    auto& asset = found->second;
    asset.loaded = true;
    if (result.image.pixels.empty()) {
        return;
    }
    std::memcpy(&asset.channels, result.image.pixels.data(), sizeof(asset.channels));
    bool hdr = (asset.channels & 256) != 0;
    glGenTextures(1, &asset.id);
    glActiveTexture(GL_TEXTURE0);
    configure_texture(asset.id, true);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, hdr ? GL_RGBA16F : GL_RGBA8, result.image.width, result.image.height, 0,
                 GL_RGBA, hdr ? GL_FLOAT : GL_UNSIGNED_BYTE,
                 result.image.pixels.data() + sizeof(asset.channels));
    glGenerateMipmap(GL_TEXTURE_2D);
    asset.bytes = size_t(result.image.width) * result.image.height * (hdr ? 8 : 4) * 4 / 3;
    texture_bytes += asset.bytes;
    ++revision; // Alpha textures also affect cached shadows.
}
static texture_asset& get_texture(const std::string& path) {
    auto& asset = textures[path];
    asset.last_frame = frame;
    // At most two decoded uploads may be pending, even when many assets are visible.
    if (!asset.requested && !asset.loaded && queued_textures < 2) {
        asset.requested = texture_worker.request(path, 1, [path] { return decode_texture(path); });
        if (asset.requested) ++queued_textures;
    }
    return asset;
}
static uint64_t opacity_key(const material& material,
                            const std::vector<dusk_gpu::instruction>& instructions) {
    if (int(material.record.base.w) != int(scene_material_model::pbr)) {
        return 0;
    }
    uint64_t hash = 14695981039346656037ull;
    auto append = [&](const void* bytes, size_t count) {
        const auto* data = static_cast<const unsigned char*>(bytes);
        for (size_t i = 0; i < count; ++i) {
            hash ^= data[i];
            hash *= 1099511628211ull;
        }
    };
    append(&material.record.alpha.x, sizeof(float));
    auto span = material.record.programs[4];
    if (!span.y) {
        span = material.record.programs[0];
        bool samples_alpha = false;
        for (int i = 0; i < span.y; ++i) {
            samples_alpha |= instructions[span.x + i].code.x == int(graph_kind::texture_sample);
        }
        // Scalar/vector arithmetic has alpha one. Channel outputs are scalar.
        if (!samples_alpha || span.w != 0) {
            return hash;
        }
    }
    append(&span, sizeof(span));
    for (int i = 0; i < span.y; ++i) {
        const auto& op = instructions[span.x + i];
        append(&op, sizeof(op));
        if (op.code.x == int(graph_kind::texture_sample) && op.code.z >= 0) {
            const auto& path = material.paths[op.code.z];
            append(path.data(), path.size());
        }
    }
    return hash;
}
static material& get_material(int index) {
    auto& cached = materials[index];
    if (!cached.dirty) {
        return cached;
    }
    cached.dirty = false;
    cached.error.clear();
    cached.paths.clear();
    scene_material fallback;
    fallback.base_color = index == -2 ? vec3(1, 0, 1) : vec3(.5, .5, .5);
    const auto& source =
        index >= 0 && index < int(g_scene.materials.size()) ? g_scene.materials[index] : fallback;
    std::vector<dusk_gpu::instruction> instructions;
    try {
        cached.record = dusk_gpu::compile_material(
            g_scene, source,
            [&](const graph_node& node) {
                std::string path = node.texture_path;
                if (path.empty() && node.texture_index >= 0 &&
                    node.texture_index < int(g_scene.textures.size())) {
                    path = g_scene.textures[node.texture_index].path;
                }
                if (path.empty()) {
                    return -1;
                }
                auto found = std::find(cached.paths.begin(), cached.paths.end(), path);
                if (found != cached.paths.end()) {
                    return int(found - cached.paths.begin());
                }
                if (cached.paths.size() == texture_slots) {
                    throw std::runtime_error("Raster preview supports up to 12 textures per material.");
                }
                cached.paths.push_back(path);
                return int(cached.paths.size() - 1);
            },
            instructions);
    } catch (const std::exception& error) {
        cached.error = error.what();
        std::fprintf(stderr, "Raster material '%s': %s\n", source.name.c_str(), error.what());
        instructions.clear();
        cached.paths.clear();
        fallback.base_color = vec3(1, 0, 1);
        cached.record =
            dusk_gpu::compile_material(g_scene, fallback, [](const graph_node&) { return -1; }, instructions);
    }
    cached.opacity_key = opacity_key(cached, instructions);
    std::vector<float> values;
    values.reserve(std::max(size_t(1), instructions.size()) * 20);
    for (const auto& op : instructions) {
        for (const auto& lane : {op.code, op.inputs, op.channels}) {
            values.insert(values.end(), {float(lane.x), float(lane.y), float(lane.z), float(lane.w)});
        }
        for (const auto& lane : {op.value, op.defaults}) {
            values.insert(values.end(), {lane.x, lane.y, lane.z, lane.w});
        }
    }
    if (values.empty()) {
        values.resize(20, 0);
    }
    if (!cached.instructions) {
        glGenTextures(1, &cached.instructions);
    }
    glActiveTexture(GL_TEXTURE0 + 12);
    configure_texture(cached.instructions, false);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, 5, GLsizei(values.size() / 20), 0, GL_RGBA, GL_FLOAT,
                 values.data());
    glActiveTexture(GL_TEXTURE0);
    return cached;
}
static void bind_material(program& shader, int index) {
    auto& cached = get_material(index);
    const auto& m = cached.record;
    glUniform4fv(shader.location("uBase"), 1, &m.base.x);
    glUniform4fv(shader.location("uParameters"), 1, &m.parameters.x);
    glUniform4fv(shader.location("uF0"), 1, &m.f0.x);
    glUniform4fv(shader.location("uEmission"), 1, &m.emission.x);
    glUniform4fv(shader.location("uAlpha"), 1, &m.alpha.x);
    glUniform4iv(shader.location("uPrograms[0]"), 5, &m.programs[0].x);
    int channels[texture_slots]{};
    for (int i = 0; i < texture_slots; ++i) {
        GLuint id = placeholder;
        channels[i] = 4;
        if (i < int(cached.paths.size())) {
            auto& asset = get_texture(cached.paths[i]);
            if (asset.id) {
                id = asset.id;
                channels[i] = asset.channels;
            }
        }
        glActiveTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_2D, id);
    }
    glUniform1iv(shader.location("uTextureChannels[0]"), texture_slots, channels);
    glActiveTexture(GL_TEXTURE0 + 12);
    glBindTexture(GL_TEXTURE_2D, cached.instructions);
}
static void model_and_normal(const scene_object& object, float model[16], float normal[9]) {
    vec3 scale = object.scale;
    if (object.type == scene_object_type::sphere) {
        scale *= object.radius;
    }
    make_model_trs(object.translation + object.center, object.rotation_deg, scale, model);
    double linear[3][3], inverse[3][3];
    make_trs_linear(object.rotation_deg, scale, linear, inverse);
    for (int col = 0; col < 3; ++col) {
        for (int row = 0; row < 3; ++row) {
            normal[col * 3 + row] = float(inverse[col][row]);
        }
    }
}
static std::vector<draw> collect_draws() {
    std::vector<draw> draws;
    for (int i = 0; i < int(g_scene.objects.size()); ++i) {
        const auto& object = g_scene.objects[i];
        draw item{};
        item.object_index = i;
        item.material_index = object.material_index;
        if (item.material_index < 0 || item.material_index >= int(g_scene.materials.size())) {
            item.material_index = object.type == scene_object_type::mesh_instance ? -2 : -1;
        }
        model_and_normal(object, item.model, item.normal);
        if (object.type == scene_object_type::sphere) {
            item.vao = g_rasterSphereVAO;
            item.count = g_rasterSphereIndexCount;
        } else if (object.type == scene_object_type::cube) {
            item.vao = g_rasterCubeVAO;
            item.count = g_rasterCubeIndexCount;
        } else {
            if (object.mesh_index < 0 || object.mesh_index >= int(g_gpu_meshes.size())) {
                continue;
            }
            const auto& mesh = g_gpu_meshes[object.mesh_index];
            item.vao = mesh.vao;
            item.count = mesh.index_count;
            if (!mesh.ranges.empty()) {
                const int fallback = item.material_index;
                for (const auto& range : mesh.ranges) {
                    item.first = range.first;
                    item.count = range.count;
                    item.material_index = fallback;
                    if (range.material_slot >= 0 &&
                        range.material_slot < int(object.mesh_slot_materials.size())) {
                        int bound = object.mesh_slot_materials[range.material_slot];
                        if (bound >= 0 && bound < int(g_scene.materials.size())) {
                            item.material_index = bound;
                        }
                    }
                    draws.push_back(item);
                }
                continue;
            }
        }
        if (item.vao && item.count) {
            draws.push_back(item);
        }
    }
    return draws;
}
static void draw_scene(program& shader, const std::vector<draw>& draws, int skip_object = -1) {
    int last_material = INT_MIN;
    for (const auto& item : draws) {
        if (item.object_index == skip_object) {
            continue;
        }
        if (item.material_index != last_material) {
            bind_material(shader, item.material_index);
            last_material = item.material_index;
        }
        glUniformMatrix4fv(shader.location("uModel"), 1, GL_FALSE, item.model);
        glUniformMatrix3fv(shader.location("uNormalMatrix"), 1, GL_FALSE, item.normal);
        glBindVertexArray(item.vao);
        glDrawElements(GL_TRIANGLES, item.count, GL_UNSIGNED_INT,
                       reinterpret_cast<void*>(size_t(item.first) * sizeof(unsigned)));
    }
}
static lighting scene_lighting() {
    lighting light;
    bool sun = false;
    for (const auto& source : g_scene.lights) {
        if (source.radiance.length_squared() == 0) {
            continue;
        }
        if (source.type == scene_light_type::directional && !sun && !source.direction.near_zero()) {
            light.sun_direction = unit_vector(-source.direction);
            light.sun_radiance = source.radiance;
            sun = true;
        } else if (source.type == scene_light_type::point && light.positions.size() < 8) {
            light.positions.push_back({float(source.position.x()), float(source.position.y()),
                                       float(source.position.z()), float(source.range)});
            light.radiances.push_back(
                {float(source.radiance.x()), float(source.radiance.y()), float(source.radiance.z()), 0});
        }
    }
    // Emissive primitives supply a point approximation for interactive room lighting.
    // Actual area emission, reflections and indirect transport remain ray-traced.
    for (int i = 0; i < int(g_scene.objects.size()) && light.positions.size() < 8; ++i) {
        const auto& object = g_scene.objects[i];
        if (object.type == scene_object_type::mesh_instance || object.material_index < 0 ||
            object.material_index >= int(g_scene.materials.size())) {
            continue;
        }
        const auto& material = g_scene.materials[object.material_index];
        if (material.model != scene_material_model::diffuse_light) {
            continue;
        }
        vec3 emission = (material.emission.near_zero() ? material.base_color : material.emission) *
                        material.emission_intensity;
        vec3 scale = object.scale;
        double area = object.type == scene_object_type::sphere
                          ? 4 * pi * object.radius * object.radius * std::abs(scale.x() * scale.y())
                          : 2 * (std::abs(scale.x() * scale.y()) + std::abs(scale.x() * scale.z()) +
                                 std::abs(scale.y() * scale.z()));
        vec3 intensity = emission * area / 4.;
        if (intensity.near_zero()) {
            continue;
        }
        vec3 center = object.center + object.translation;
        if (light.positions.empty()) {
            light.shadow_source = i;
        }
        light.positions.push_back({float(center.x()), float(center.y()), float(center.z()), 0});
        light.radiances.push_back({float(intensity.x()), float(intensity.y()), float(intensity.z()), 0});
    }
    if (!sun && light.positions.empty()) {
        light.sun_radiance = vec3(2, 2, 2);
    }
    light.sun_direction = unit_vector(light.sun_direction);
    return light;
}
static uint64_t scene_key(const std::vector<draw>& draws, const lighting& lights) {
    uint64_t hash = 14695981039346656037ull;
    auto append = [&](const void* bytes, size_t size) {
        auto data = static_cast<const unsigned char*>(bytes);
        for (size_t i = 0; i < size; ++i) {
            hash ^= data[i];
            hash *= 1099511628211ull;
        }
    };
    append(&revision, sizeof(revision));
    append(&lights.shadow_source, sizeof(lights.shadow_source));
    for (const auto& item : draws) {
        append(item.model, sizeof(item.model));
        append(&item.vao, sizeof(item.vao));
        append(&item.first, sizeof(item.first));
        append(&item.count, sizeof(item.count));
        const uint64_t opacity = materials.at(item.material_index).opacity_key;
        append(&opacity, sizeof(opacity));
    }
    for (int c = 0; c < 3; ++c) {
        double value = lights.sun_direction[c];
        append(&value, sizeof(value));
    }
    for (const auto& p : lights.positions) {
        append(&p, sizeof(p));
    }
    return hash;
}
static void ensure_shadow_targets() {
    if (shadow_fbo) {
        return;
    }
    glGenFramebuffers(1, &shadow_fbo);
    glGenTextures(1, &sun_depth);
    glActiveTexture(GL_TEXTURE0 + 13);
    configure_texture(sun_depth, false);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, sun_resolution, sun_resolution, 0,
                 GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
    glGenTextures(1, &point_depth);
    glActiveTexture(GL_TEXTURE0 + 14);
    glBindTexture(GL_TEXTURE_CUBE_MAP, point_depth);
    for (int face = 0; face < 6; ++face) {
        glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, 0, GL_DEPTH_COMPONENT24, point_resolution,
                     point_resolution, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    }
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    for (GLenum axis : {GL_TEXTURE_WRAP_S, GL_TEXTURE_WRAP_T, GL_TEXTURE_WRAP_R}) {
        glTexParameteri(GL_TEXTURE_CUBE_MAP, axis, GL_CLAMP_TO_EDGE);
    }
}
static void fit_sun(const std::vector<draw>& draws, const lighting& light) {
    point3 minimum(infinity, infinity, infinity), maximum(-infinity, -infinity, -infinity);
    for (const auto& item : draws) {
        const auto& object = g_scene.objects[item.object_index];
        point3 local_min(-.5, -.5, -.5), local_max(.5, .5, .5);
        if (object.type == scene_object_type::sphere) {
            local_min = point3(-1, -1, -1);
            local_max = point3(1, 1, 1);
        } else if (object.type == scene_object_type::mesh_instance) {
            local_min = g_gpu_meshes[object.mesh_index].minimum;
            local_max = g_gpu_meshes[object.mesh_index].maximum;
        }
        for (int corner = 0; corner < 8; ++corner) {
            vec3 p;
            for (int c = 0; c < 3; ++c) {
                p[c] = (corner & (1 << c)) ? local_max[c] : local_min[c];
            }
            for (int row = 0; row < 3; ++row) {
                double value = item.model[12 + row];
                for (int col = 0; col < 3; ++col) {
                    value += item.model[col * 4 + row] * p[col];
                }
                minimum[row] = std::min(minimum[row], value);
                maximum[row] = std::max(maximum[row], value);
            }
        }
    }
    if (draws.empty()) {
        minimum = vec3(-1, -1, -1);
        maximum = vec3(1, 1, 1);
    }
    vec3 center = (minimum + maximum) * .5;
    float radius = float(std::max(1., (maximum - minimum).length() * .55));
    vec3 up = std::abs(light.sun_direction.y()) > .95 ? vec3(1, 0, 0) : vec3(0, 1, 0);
    make_lookat(center + light.sun_direction * radius * 2.5, center, up, sun_view);
    std::fill(std::begin(sun_projection), std::end(sun_projection), 0.f);
    sun_projection[0] = sun_projection[5] = 1 / radius;
    float near_plane = .01f, far_plane = radius * 5;
    sun_projection[10] = -2 / (far_plane - near_plane);
    sun_projection[14] = -(far_plane + near_plane) / (far_plane - near_plane);
    sun_projection[15] = 1;
    world_texel = 2 * radius / sun_resolution;
    shadow_far = radius * 4;
    if (!light.positions.empty()) {
        vec3 p(light.positions[0].x, light.positions[0].y, light.positions[0].z);
        shadow_far = float((p - center).length() + radius * 2);
    }
}
static void update_shadows(const std::vector<draw>& draws, const lighting& light) {
    uint64_t key = scene_key(draws, light);
    if (shadow_key == key || !shadows_enabled) {
        return;
    }
    ensure_shadow_targets();
    fit_sun(draws, light);
    glBindFramebuffer(GL_FRAMEBUFFER, shadow_fbo);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    glDisable(GL_CULL_FACE);
    glUseProgram(shadow_program.id);
    glUniformMatrix4fv(shadow_program.location("uView"), 1, GL_FALSE, sun_view);
    glUniformMatrix4fv(shadow_program.location("uProj"), 1, GL_FALSE, sun_projection);
    glUniform1i(shadow_program.location("uPointShadowPass"), 0);
    glViewport(0, 0, sun_resolution, sun_resolution);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, sun_depth, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        return;
    }
    glClear(GL_DEPTH_BUFFER_BIT);
    draw_scene(shadow_program, draws);
    if (!light.positions.empty()) {
        const auto& p = light.positions[0];
        vec3 position(p.x, p.y, p.z);
        const vec3 directions[] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
        const vec3 ups[] = {{0, -1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}, {0, -1, 0}, {0, -1, 0}};
        float projection[16];
        make_perspective(90, 1, .005f, shadow_far, projection);
        glUniformMatrix4fv(shadow_program.location("uProj"), 1, GL_FALSE, projection);
        glUniform1i(shadow_program.location("uPointShadowPass"), 1);
        glUniform3f(shadow_program.location("uShadowPosition"), p.x, p.y, p.z);
        glUniform1f(shadow_program.location("uShadowFar"), shadow_far);
        glViewport(0, 0, point_resolution, point_resolution);
        for (int face = 0; face < 6; ++face) {
            float view[16];
            make_lookat(position, position + directions[face], ups[face], view);
            glUniformMatrix4fv(shadow_program.location("uView"), 1, GL_FALSE, view);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_CUBE_MAP_POSITIVE_X + face,
                                   point_depth, 0);
            glClear(GL_DEPTH_BUFFER_BIT);
            draw_scene(shadow_program, draws, light.shadow_source);
        }
    }
    shadow_key = key;
    ++shadow_updates;
}
static void trim_textures() {
    while (texture_bytes > texture_budget) {
        auto victim = textures.end();
        for (auto it = textures.begin(); it != textures.end(); ++it) {
            if (it->second.id && it->second.last_frame + 1 < frame &&
                (victim == textures.end() || it->second.last_frame < victim->second.last_frame)) {
                victim = it;
            }
        }
        if (victim == textures.end()) {
            break; // Keep all textures needed by this frame resident.
        }
        texture_bytes -= victim->second.bytes;
        glDeleteTextures(1, &victim->second.id);
        textures.erase(victim);
    }
}
} // namespace raster_preview

static void InvalidateRasterMaterial(int index) {
    auto found = raster_preview::materials.find(index);
    if (found != raster_preview::materials.end()) {
        found->second.dirty = true;
    }
}
static void InvalidateAllRasterMaterials() {
    for (auto& entry : raster_preview::materials) {
        entry.second.dirty = true;
    }
}
static void BuildRasterShader() {
    using namespace raster_preview;
    if (g_rasterShader) {
        return;
    }
    BuildModelThumbnailShader();
    std::string lit = std::string(raster_shaders::material) + raster_shaders::lit;
    std::string shadow = std::string(raster_shaders::material) + raster_shaders::shadow;
    surface_program.id = g_rasterShader = CompileShader(raster_shaders::vertex, lit.c_str());
    shadow_program.id = CompileShader(raster_shaders::vertex, shadow.c_str());
    const int units[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    for (auto* shader : {&surface_program, &shadow_program}) {
        glUseProgram(shader->id);
        glUniform1iv(shader->location("uTextures[0]"), texture_slots, units);
        glUniform1i(shader->location("uInstructions"), 12);
        glUniform1i(shader->location("uSunShadow"), 13);
        glUniform1i(shader->location("uPointShadow"), 14);
    }
    glGenTextures(1, &placeholder);
    glActiveTexture(GL_TEXTURE0);
    configure_texture(placeholder, false);
    const unsigned char pixel[] = {128, 128, 255, 255};
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    glUseProgram(0);
}
static void RenderRasterScene(const float view[16], const float projection[16], const vec3& eye, int width,
                              int height) {
    using namespace raster_preview;
    ++frame;
    pump_textures();
    auto draws = collect_draws();
    material_error.clear();
    for (const auto& item : draws) {
        const auto& material = get_material(item.material_index);
        if (material_error.empty() && !material.error.empty()) {
            material_error = material.error;
        }
    }
    auto light = scene_lighting();
    update_shadows(draws, light);
    glBindFramebuffer(GL_FRAMEBUFFER, g_rasterFBO);
    glViewport(0, 0, width, height);
    glDisable(GL_CULL_FACE);
    glUseProgram(surface_program.id);
    auto& shader = surface_program;
    glUniformMatrix4fv(shader.location("uView"), 1, GL_FALSE, view);
    glUniformMatrix4fv(shader.location("uProj"), 1, GL_FALSE, projection);
    glUniformMatrix4fv(shader.location("uShadowView"), 1, GL_FALSE, sun_view);
    glUniformMatrix4fv(shader.location("uShadowProjection"), 1, GL_FALSE, sun_projection);
    glUniform3f(shader.location("uEye"), float(eye.x()), float(eye.y()), float(eye.z()));
    glUniform3f(shader.location("uSunDirection"), float(light.sun_direction.x()),
                float(light.sun_direction.y()), float(light.sun_direction.z()));
    glUniform3f(shader.location("uSunRadiance"), float(light.sun_radiance.x()), float(light.sun_radiance.y()),
                float(light.sun_radiance.z()));
    glUniform1i(shader.location("uSunShadowEnabled"), shadows_enabled && sun_depth != 0);
    glUniform1i(shader.location("uPointShadowIndex"),
                shadows_enabled && point_depth && !light.positions.empty() ? 0 : -1);
    glUniform1i(shader.location("uPointCount"), int(light.positions.size()));
    if (!light.positions.empty()) {
        glUniform4fv(shader.location("uPointPosition[0]"), GLsizei(light.positions.size()),
                     &light.positions[0].x);
        glUniform4fv(shader.location("uPointRadiance[0]"), GLsizei(light.radiances.size()),
                     &light.radiances[0].x);
    }
    glUniform1f(shader.location("uShadowFar"), shadow_far);
    glUniform1f(shader.location("uShadowWorldTexel"), world_texel);
    glUniform1i(shader.location("uDebugView"), debug_view);
    glActiveTexture(GL_TEXTURE0 + 13);
    glBindTexture(GL_TEXTURE_2D, sun_depth);
    glActiveTexture(GL_TEXTURE0 + 14);
    glBindTexture(GL_TEXTURE_CUBE_MAP, point_depth);
    draw_scene(shader, draws);
    if (g_pickShader) {
        glUseProgram(g_pickShader);
        glUniformMatrix4fv(glGetUniformLocation(g_pickShader, "uView"), 1, GL_FALSE, view);
        glUniformMatrix4fv(glGetUniformLocation(g_pickShader, "uProj"), 1, GL_FALSE, projection);
        for (const auto& light : g_scene.lights) {
            if (light.type != scene_light_type::point) {
                continue;
            }
            float model[16];
            make_model_sphere(light.position, .08, model);
            glUniformMatrix4fv(glGetUniformLocation(g_pickShader, "uModel"), 1, GL_FALSE, model);
            glUniform3f(glGetUniformLocation(g_pickShader, "uPickColor"),
                        float(std::min(1., light.radiance.x())), float(std::min(1., light.radiance.y())),
                        float(std::min(1., light.radiance.z())));
            glBindVertexArray(g_rasterSphereVAO);
            glDrawElements(GL_TRIANGLES, g_rasterSphereIndexCount, GL_UNSIGNED_INT, nullptr);
        }
    }
    glActiveTexture(GL_TEXTURE0);
    trim_textures();
}
static void StopRasterPreview() {
    using namespace raster_preview;
    texture_worker.stop();
    ready_textures.clear();
    for (auto& entry : textures) {
        if (entry.second.id) {
            glDeleteTextures(1, &entry.second.id);
        }
    }
    for (auto& entry : materials) {
        if (entry.second.instructions) {
            glDeleteTextures(1, &entry.second.instructions);
        }
    }
    textures.clear();
    materials.clear();
    glDeleteTextures(1, &placeholder);
    glDeleteTextures(1, &sun_depth);
    glDeleteTextures(1, &point_depth);
    glDeleteFramebuffers(1, &shadow_fbo);
    glDeleteProgram(shadow_program.id);
}
