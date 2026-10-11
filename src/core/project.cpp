#include "core/project.h"
#include <cctype>
#include <fstream>
#include <iomanip>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace dusk_project {
namespace {
namespace fs = std::filesystem;
constexpr size_t max_entries = 100000;
constexpr uintmax_t max_document_bytes = 64 * 1024 * 1024;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

struct writer {
    std::ostream& out;
    void value(const std::string& v) {
        out << std::quoted(v) << ' ';
    }
    void value(const vec3& v) {
        for (double n : v.e) {
            value(n);
        }
    }
    template <class T> void value(const T& v) {
        if constexpr (std::is_enum_v<T>) {
            out << int(v) << ' ';
        } else {
            out << v << ' ';
        }
    }
    template <class T> void value(const std::vector<T>& v) {
        value(v.size());
        for (const auto& item : v) {
            value(item);
        }
    }
    template <class... T> void record(const char* name, const T&... v) {
        out << name << ' ';
        (value(v), ...);
        out << '\n';
    }
    template <class T, class F> void list(const char* name, const std::vector<T>& v, F fields) {
        record(name, v.size());
        for (const auto& item : v) {
            fields(item);
        }
    }
};

struct reader {
    std::istream& in;
    void value(std::string& v) {
        require(bool(in >> std::quoted(v)), "Invalid text in scene file.");
    }
    void value(vec3& v) {
        for (double& n : v.e) {
            value(n);
        }
    }
    template <class T> void value(T& v) {
        if constexpr (std::is_enum_v<T>) {
            int n;
            value(n);
            v = T(n);
        } else {
            require(bool(in >> v), "Scene file is incomplete or contains an invalid value.");
            if constexpr (std::is_floating_point_v<T>) {
                require(std::isfinite(v), "Non-finite scene value.");
            }
        }
    }
    size_t count() {
        size_t n = 0;
        value(n);
        require(n <= max_entries, "Too many entries in scene file.");
        return n;
    }
    template <class T> void value(std::vector<T>& v) {
        v.resize(count());
        for (auto& item : v) {
            value(item);
        }
    }
    template <class... T> void record(const char* name, T&... v) {
        std::string tag;
        in >> tag;
        require(tag == name, std::string("Expected '") + name + "' in scene file.");
        (value(v), ...);
    }
    template <class T, class F> void list(const char* name, std::vector<T>& v, F fields) {
        record(name);
        v.resize(count());
        for (auto& item : v) {
            fields(item);
        }
    }
};

// The same field list drives disk I/O and dirty tracking. Hashing visits live
// values without copying scenes, formatting text, or touching the filesystem.
struct hasher {
    uint64_t hash = 14695981039346656037ull;
    void bytes(const void* data, size_t size) {
        const auto* p = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < size; ++i) {
            hash ^= p[i];
            hash *= 1099511628211ull;
        }
    }
    template <class T> void value(const T& v) {
        bytes(&v, sizeof(v));
    }
    void value(const std::string& v) {
        value(v.size());
        bytes(v.data(), v.size());
    }
    void value(const vec3& v) {
        for (double n : v.e) {
            value(n);
        }
    }
    template <class T> void value(const std::vector<T>& v) {
        value(v.size());
        for (const auto& item : v) {
            value(item);
        }
    }
    template <class... T> void record(const char*, const T&... v) {
        (value(v), ...);
    }
    template <class T, class F> void list(const char*, const std::vector<T>& v, F fields) {
        value(v.size());
        for (const auto& item : v) {
            fields(item);
        }
    }
};

template <class A, class D> void archive(A& a, D& d, int version = 5) {
    auto& c = d.camera_settings;
    // Camera pose and lens belong to the viewport; directional/point lights
    // belong to the scene. Derived camera copies must not mark a project dirty.
    a.record("camera", c.image_width, c.image_height, c.samples_per_pixel, c.max_depth, c.background,
             c.defocus_angle, c.focus_dist, c.sampling_method, c.sun_shadow_samples, c.enable_mnee,
             c.mnee_per_thread_budget, c.mnee_newton_max_iters, c.mnee_newton_tol, c.mnee_step_eps,
             c.mnee_sun_samples, c.mnee_gain_scale, c.direct_light_samples, c.enable_mis);
    auto& r = d.render_settings;
    a.record("render", r.exposure, r.use_denoiser, r.denoiser_strength, r.progressive_denoise,
             r.adaptive_sampling, r.adaptive_min_samples, r.adaptive_check_interval, r.adaptive_rel_threshold,
             r.adaptive_abs_threshold, d.device.backend, d.device.device_id);
    if(version>=2) a.record("gpu_filter",r.gpu_firefly_filter);
    auto& v = d.viewport;
    a.record("viewport", v.position, v.yaw, v.pitch, v.vfov, v.world_up, v.min_pitch, v.max_pitch,
             d.viewport_mode, d.match_render_resolution, d.movement_speed, d.look_sensitivity);
    a.list("textures", d.world.textures, [&](auto& t) { a.record("texture", t.name, t.path); });
    a.list("materials", d.world.materials, [&](auto& m) {
        a.record("material", m.name, m.model, m.base_color, m.metallic, m.roughness, m.fuzz, m.ior,
                 m.emission, m.use_sss, m.sss_strength, m.sss_scale, m.sss_model, m.sss_samples, m.sss_radius,
                 m.sss_eta, m.sss_color_override_enabled, m.sss_color_override_color, m.unreal_pbr,
                 m.emission_intensity, m.dielectric_F0, m.normal_strength, m.albedo_tex, m.metallic_tex,
                 m.roughness_tex, m.normal_tex, m.alpha_tex, m.alpha_double_sided, m.alpha_cutoff);
        if(version>=3) a.record("sss_profile",m.sss_profile,m.sss_radius_rgb);
        if(version>=4) a.record("sss_unreal",m.sss_shading,m.sss_subsurface_color,m.sss_opacity,
                                m.sss_transmission_distance,m.sss_surface_albedo,m.sss_mfp_color,m.sss_mfp_mm,
                                m.sss_units_per_metre,m.sss_profile_tint,m.sss_lobe_roughness0,
                                m.sss_lobe_roughness1,m.sss_lobe_mix);
        if(version>=5) a.record("sss_base_color",m.sss_color_from_base);
        auto& g = m.graph;
        a.record("graph", g.next_id, g.pan_x, g.pan_y, g.zoom);
        a.list("nodes", g.nodes, [&](auto& n) {
            a.record("node", n.id, n.kind, n.x, n.y, n.value, n.defaults[0], n.defaults[1], n.defaults[2],
                     n.texture_index, n.texture_path, n.space);
        });
        a.list("links", g.links, [&](auto& l) { a.record("link", l.from, l.output, l.to, l.input); });
    });
    a.list("meshes", d.world.meshes, [&](auto& m) {
        a.record("mesh", m.name, m.file_path, m.thumbnail_path, m.approx_radius, m.slot_names,
                 m.slot_default_materials);
    });
    a.list("objects", d.world.objects, [&](auto& o) {
        a.record("object", o.name, o.type, o.material_index, o.center, o.radius, o.mesh_index, o.translation,
                 o.rotation_deg, o.scale, o.mesh_slot_materials);
    });
    a.list("lights", d.world.lights, [&](auto& l) {
        a.record("light", l.name, l.type, l.radiance, l.direction, l.angular_radius_deg, l.position, l.range);
    });
    a.record("end");
}

void validate(const document& d) {
    const auto& s = d.world;
    auto index = [](int n, size_t count) {
        require(n >= -1 && (n == -1 || size_t(n) < count), "Invalid asset reference in scene file.");
    };
    auto slots = [&](const std::vector<int>& v) {
        for (int i : v) {
            index(i, s.materials.size());
        }
    };
    for (const auto& m : s.materials) {
        require(m.sss_profile>=0 && m.sss_profile<=2 && m.sss_radius_rgb.x()>0 &&
                m.sss_radius_rgb.y()>0 && m.sss_radius_rgb.z()>0,"Invalid SSS distance profile.");
        require(m.sss_shading>=sss_shading_burley && m.sss_shading<=sss_shading_unreal_profile &&
                m.sss_transmission_distance>0 && m.sss_mfp_mm>=0 && m.sss_units_per_metre>0 &&
                m.sss_opacity>=0 && m.sss_opacity<=1 && m.sss_lobe_mix>=0 && m.sss_lobe_mix<=1 &&
                m.sss_lobe_roughness0>=0 && m.sss_lobe_roughness1>=0,"Invalid Unreal subsurface settings.");
        require(int(m.model) >= 0 && int(m.model) <= int(scene_material_model::pbr),
                "Unknown material model.");
        for (int i : {m.albedo_tex, m.metallic_tex, m.roughness_tex, m.normal_tex, m.alpha_tex}) {
            index(i, s.textures.size());
        }
        require(m.graph.nodes.size() <= material_graph::max_nodes, "Material graph is too large.");
        require(m.graph.zoom >= .35f && m.graph.zoom <= 1.65f, "Invalid material graph zoom.");
        material_graph checked;
        require(deserialize_material_graph(serialize_material_graph(m.graph), checked),
                "Invalid material graph.");
        require(m.graph.next_id >= checked.next_id && m.graph.next_id <= 1000000001,
                "Invalid graph node counter.");
        for (const auto& n : m.graph.nodes) {
            index(n.texture_index, s.textures.size());
        }
    }
    for (const auto& m : s.meshes) {
        slots(m.slot_default_materials);
        require(m.slot_names.size() == m.slot_default_materials.size(), "Mesh material slots do not match.");
    }
    for (const auto& o : s.objects) {
        require(int(o.type) >= 0 && int(o.type) <= 2, "Unknown object type.");
        index(o.material_index, s.materials.size());
        index(o.mesh_index, s.meshes.size());
        slots(o.mesh_slot_materials);
        require(o.type != scene_object_type::mesh_instance || o.mesh_index >= 0,
                "Mesh instance has no mesh.");
    }
    for (const auto& l : s.lights) {
        require(int(l.type) >= 0 && int(l.type) <= 1, "Unknown light type.");
    }
    const auto& c = d.camera_settings;
    require(c.image_width >= 16 && c.image_width <= 32768 && c.image_height >= 16 && c.image_height <= 32768,
            "Render dimensions must be between 16 and 32768 pixels.");
    require(c.samples_per_pixel > 0 && c.max_depth > 0 && int(c.sampling_method) >= 0 &&
                int(c.sampling_method) <= 1,
            "Invalid sampling settings.");
    require(d.viewport.vfov > 0 && d.viewport.vfov < 180 && d.viewport.world_up.length_squared() > 1e-12,
            "Invalid viewport camera.");
    const auto& r = d.render_settings;
    require(r.exposure >= 0 && r.denoiser_strength >= 0 && r.denoiser_strength <= 1 &&
                r.adaptive_min_samples > 0 && r.adaptive_check_interval > 0 &&
                r.adaptive_rel_threshold >= 0 && r.adaptive_abs_threshold >= 0,
            "Invalid renderer settings.");
    require(c.focus_dist > 0 && c.defocus_angle >= 0 && c.defocus_angle < 180 &&
                c.direct_light_samples >= 0 && c.sun_shadow_samples >= 0 && c.mnee_newton_max_iters > 0 &&
                c.mnee_newton_tol > 0 && c.mnee_step_eps > 0 && c.mnee_per_thread_budget >= 0 &&
                c.mnee_sun_samples >= 0,
            "Invalid camera or light sampling settings.");
    require(d.movement_speed > 0 && d.look_sensitivity > 0 && d.viewport.min_pitch < d.viewport.max_pitch,
            "Invalid camera navigation settings.");
    require(d.viewport_mode >= 0 && d.viewport_mode <= 1 && int(d.device.backend) >= 0 &&
                int(d.device.backend) <= 1,
            "Unknown renderer or viewport mode.");
}

template <class F> void asset_paths(scene& s, F visit) {
    for (auto& t : s.textures) {
        visit(t.path);
    }
    for (auto& m : s.meshes) {
        visit(m.file_path);
        visit(m.thumbnail_path);
    }
    for (auto& m : s.materials) {
        for (auto& n : m.graph.nodes) {
            visit(n.texture_path);
        }
    }
}

fs::path resolved_path(const fs::path& path) {
    return fs::weakly_canonical(fs::absolute(path));
}
bool contained(const fs::path& root, const fs::path& path) {
    const auto relative = path.lexically_relative(root);
    return !relative.empty() && !relative.is_absolute() && *relative.begin() != "..";
}

std::string read_text(const fs::path& file) {
    require(fs::file_size(file) <= max_document_bytes, "Scene or material sidecar exceeds 64 MiB.");
    std::ifstream in(file, std::ios::binary);
    require(bool(in), "Cannot read " + file.u8string());
    std::ostringstream out;
    out << in.rdbuf();
    require(!in.bad(), "Failed reading " + file.u8string());
    return out.str();
}

fs::path temporary_path(const fs::path& directory, const std::string& prefix) {
    static std::atomic<uint64_t> serial{0};
#ifdef _WIN32
    const auto process = GetCurrentProcessId();
#else
    static const auto process = std::chrono::steady_clock::now().time_since_epoch().count();
#endif
    for (;;) {
        auto path = directory / fs::u8path(prefix + std::to_string(process) + "-" + std::to_string(++serial));
        if (!fs::exists(path)) {
            return path;
        }
    }
}

void atomic_write(const fs::path& file, const std::string& contents) {
    // A sibling temporary keeps replacement on the same volume. The previous
    // document stays intact if a write, flush, or replacement fails.
    const auto temporary = temporary_path(file.parent_path(), file.filename().u8string() + ".saving-");
    try {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        require(bool(out), "Cannot create " + temporary.u8string());
        out.write(contents.data(), std::streamsize(contents.size()));
        out.flush();
        require(bool(out), "Unable to finish writing scene file.");
        out.close();
        require(bool(out), "Unable to close scene file.");
#ifdef _WIN32
        require(MoveFileExW(temporary.c_str(), file.c_str(),
                            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0,
                "Unable to replace scene file. Check its permissions or whether another application has it "
                "open.");
#else
        fs::rename(temporary, file);
#endif
    } catch (...) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        throw;
    }
}

std::string digest(std::istream& in) {
    hasher h;
    char buffer[65536];
    while (in) {
        in.read(buffer, sizeof(buffer));
        h.bytes(buffer, size_t(in.gcount()));
    }
    require(!in.bad(), "Failed reading asset contents.");
    std::ostringstream out;
    out << std::hex << h.hash;
    return out.str();
}

// Content-addressed filenames reuse identical assets and never overwrite a
// different file with the same display name. Source files are never modified.
fs::path pack_file(const fs::path& source, const fs::path& directory, const std::string* rewritten = nullptr,
                   const fs::path* rewritten_file = nullptr) {
    fs::create_directories(directory);
    std::ifstream in(source, std::ios::binary);
    require(bool(in), "Missing or unreadable asset: " + source.u8string());
    std::istringstream text(rewritten ? *rewritten : "");
    const auto original_hash = digest(in);
    std::ifstream file_contents;
    if (rewritten_file) {
        file_contents.open(*rewritten_file, std::ios::binary);
        require(bool(file_contents), "Cannot read temporary imported model.");
    }
    const auto hash = rewritten ? digest(text) : rewritten_file ? digest(file_contents) : original_hash;
    auto stem = source.stem().u8string();
    const auto previous_suffix = "-" + original_hash;
    if (stem.size() > previous_suffix.size() &&
        stem.compare(stem.size() - previous_suffix.size(), previous_suffix.size(), previous_suffix) == 0) {
        stem.resize(stem.size() - previous_suffix.size());
    }
    const auto ext = source.extension().u8string();
    auto destination = directory / fs::u8path(stem + "-" + hash + ext);
    for (unsigned suffix = 1; fs::exists(destination); ++suffix) {
        std::ifstream existing(destination, std::ios::binary);
        if (existing && digest(existing) == hash) {
            return destination;
        }
        destination = directory / fs::u8path(stem + "-" + hash + "-" + std::to_string(suffix) + ext);
    }
    if (rewritten) {
        atomic_write(destination, *rewritten);
    } else {
        fs::copy_file(rewritten_file ? *rewritten_file : source, destination);
    }
    return destination;
}

fs::path copy_mtl(const fs::path& source, const fs::path& root) {
    std::istringstream input(read_text(source));
    std::ostringstream output;
    std::string line;
    while (std::getline(input, line)) {
        std::istringstream fields(line);
        std::string command;
        fields >> command;
        if (command.rfind("map_", 0) == 0 || command == "bump" || command == "disp" || command == "decal" ||
            command == "norm") {
            // Preserve map options, then resolve the longest existing filename
            // suffix. This also handles common unquoted filenames with spaces.
            size_t start = line.find(command) + command.size();
            bool found = false;
            while ((start = line.find_first_not_of(" \t\r", start)) != std::string::npos) {
                std::string name = line.substr(start);
                if (!name.empty() && name.back() == '\r') {
                    name.pop_back();
                }
                if (name.size() > 1 && name.front() == '"' && name.back() == '"') {
                    name = name.substr(1, name.size() - 2);
                }
                auto texture = source.parent_path() / fs::u8path(name);
                if (fs::is_regular_file(texture)) {
                    auto copied = pack_file(texture, root / "textures");
                    line =
                        line.substr(0, start) + copied.lexically_relative(root / "models").generic_u8string();
                    found = true;
                    break;
                }
                start = line.find_first_of(" \t", start);
                if (start == std::string::npos) {
                    break;
                }
            }
            require(found, "Missing texture referenced by " + source.u8string() + ": " + line);
        }
        output << line << '\n';
    }
    const auto contents = output.str();
    return pack_file(source, root / "models", &contents);
}

fs::path copy_obj(const fs::path& source, const fs::path& root) {
    // OBJ geometry can be much larger than the scene manifest. Rewrite only its
    // sidecar references while streaming the vertex/face data with bounded memory.
    fs::create_directories(root / "models");
    struct temporary_model {
        fs::path path;
        ~temporary_model() {
            std::error_code ignored;
            fs::remove(path, ignored);
        }
    } temporary;
    temporary.path = temporary_path(root / "models", ".dusk-import-");
    std::ifstream input(source, std::ios::binary);
    std::ofstream output(temporary.path, std::ios::binary);
    require(bool(input) && bool(output), "Cannot read or copy model: " + source.u8string());
    std::string line;
    while (std::getline(input, line)) {
        std::istringstream fields(line);
        std::string command;
        fields >> command;
        if (command == "mtllib") {
            std::string rest;
            std::getline(fields >> std::ws, rest);
            if (!rest.empty() && rest.back() == '\r') {
                rest.pop_back();
            }
            std::vector<std::string> names;
            if (fs::is_regular_file(source.parent_path() / fs::u8path(rest))) {
                names.push_back(rest);
            } else {
                std::istringstream tokens(rest);
                std::string name;
                while (tokens >> std::quoted(name)) {
                    names.push_back(name);
                }
            }
            require(!names.empty(), "Empty mtllib in " + source.u8string());
            for (const auto& name : names) {
                auto mtl = copy_mtl(source.parent_path() / fs::u8path(name), root);
                output << "mtllib " << mtl.filename().u8string() << '\n';
            }
        } else {
            output << line << '\n';
        }
    }
    require(!input.bad(), "Failed reading model: " + source.u8string());
    output.flush();
    require(bool(output), "Failed writing imported model.");
    output.close();
    require(bool(output), "Failed closing imported model.");
    return pack_file(source, root / "models", nullptr, &temporary.path);
}

std::string extension(const fs::path& file) {
    std::string ext = file.extension().u8string();
    for (char& c : ext) {
        c = char(std::tolower(static_cast<unsigned char>(c)));
    }
    return ext;
}
} // namespace

view inspect(const document& d) {
    return {d.world,         d.camera_settings,         d.render_settings, d.viewport,        d.device,
            d.viewport_mode, d.match_render_resolution, d.movement_speed,  d.look_sensitivity};
}

document empty_document() {
    document d;
    d.camera_settings.image_width = 1920;
    d.camera_settings.image_height = 1080;
    d.camera_settings.samples_per_pixel = 64;
    d.camera_settings.max_depth = 8;
    d.camera_settings.aspect_ratio = 16. / 9.;
    d.viewport.update_basis();
    to_shirley_camera(d.viewport, d.camera_settings);
    return d;
}

uint64_t fingerprint(const view& state) {
    hasher h;
    archive(h, state);
    return h.hash;
}

document load(const fs::path& file) {
    const auto root = resolved_path(file).parent_path();
    std::istringstream in(read_text(file));
    in.imbue(std::locale::classic());
    reader a{in};
    int version = 0;
    a.record("DUSK_SCENE", version);
    require(version >= 1 && version <= 5, "Unsupported Dusk scene version.");
    document d;
    archive(a, d, version);
    if (version < 5) {
        // Earlier Unreal Subsurface colours were not multiplied by base colour.
        for (auto& m : d.world.materials) m.sss_color_from_base = false;
    }
    in >> std::ws;
    require(in.eof(), "Unexpected data after the scene document.");
    validate(d);
    asset_paths(d.world, [&](std::string& path) {
        if (path.empty()) {
            return;
        }
        auto relative = fs::u8path(path);
        require(!relative.is_absolute() && !relative.has_root_name(),
                "Scene assets must use project-relative paths.");
        auto resolved = resolved_path(root / relative);
        require(contained(root, resolved), "Scene asset escapes the project folder.");
        path = resolved.u8string();
    });
    d.viewport.update_basis();
    d.viewport.move_speed = d.movement_speed;
    to_shirley_camera(d.viewport, d.camera_settings);
    d.camera_settings.aspect_ratio = double(d.camera_settings.image_width) / d.camera_settings.image_height;
    return d;
}

document save(const fs::path& file, const view& state) {
    const auto destination = resolved_path(file), root = destination.parent_path();
    document d{
        state.world,           state.camera_settings, state.render_settings,         state.viewport,
        state.device,          state.viewport_mode,   state.match_render_resolution, state.movement_speed,
        state.look_sensitivity};
    validate(d);
    fs::create_directories(root);
    std::unordered_map<std::string, std::string> packed_paths;
    asset_paths(d.world, [&](std::string& path) {
        if (path.empty()) {
            return;
        }
        const auto source = resolved_path(fs::u8path(path));
        const auto key = source.u8string();
        auto existing = packed_paths.find(key);
        if (existing != packed_paths.end()) {
            path = existing->second;
            return;
        }
        // Missing local assets keep their identity, allowing a scene to be saved
        // and repaired later. A missing external asset cannot be made portable.
        const auto local = contained(root, source) ? source : import_asset(source, root);
        require(contained(root, resolved_path(local)), "Asset destination escapes the project folder.");
        path = local.lexically_relative(root).generic_u8string();
        packed_paths.emplace(key, path);
    });
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(17);
    writer a{out};
    a.record("DUSK_SCENE", 5);
    archive(a, d);
    const auto contents = out.str();
    require(contents.size() <= max_document_bytes, "Scene document exceeds 64 MiB.");
    // Re-read our output before replacing the previous save, including finite
    // values. This also protects against invalid state introduced by editor code.
    std::istringstream check(contents);
    check.imbue(std::locale::classic());
    reader check_reader{check};
    int version;
    check_reader.record("DUSK_SCENE", version);
    document checked;
    archive(check_reader, checked, version);
    validate(checked);
    atomic_write(destination, contents);
    asset_paths(d.world, [&](std::string& path) {
        if (!path.empty()) {
            path = (root / fs::u8path(path)).make_preferred().u8string();
        }
    });
    return d;
}

asset_kind classify(const fs::path& file) {
    const auto ext = extension(file);
    if (ext == ".fbx" || ext == ".obj") {
        return asset_kind::mesh;
    }
    for (const auto* suffix : {".png", ".jpg", ".jpeg", ".tga", ".bmp", ".hdr", ".ppm"}) {
        if (ext == suffix) {
            return asset_kind::texture;
        }
    }
    return asset_kind::unsupported;
}

fs::path import_asset(const fs::path& source_path, const fs::path& root_path) {
    const auto source = resolved_path(source_path), root = resolved_path(root_path);
    for (const auto* folder : {"models", "textures"}) {
        require(contained(root, resolved_path(root / folder)),
                "Project asset folder points outside the project.");
    }
    require(fs::is_regular_file(source), "Missing asset: " + source.u8string());
    if (contained(root, source)) {
        return source;
    }
    if (extension(source) == ".obj") {
        return copy_obj(source, root);
    }
    return pack_file(source, root / (classify(source) == asset_kind::mesh ? "models" : "textures"));
}

std::vector<fs::path> discover_assets(const fs::path& root_path) {
    const auto root = resolved_path(root_path);
    std::vector<fs::path> result;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied), end;
         it != end; ++it) {
        const auto name = it->path().filename().u8string();
        if (it->is_symlink() || !contained(root, resolved_path(it->path())) ||
            (!name.empty() && name[0] == '.') || name == "Renders") {
            if (it->is_directory()) {
                it.disable_recursion_pending();
            }
            continue;
        }
        if (it->is_regular_file() && classify(it->path()) != asset_kind::unsupported) {
            result.push_back(it->path());
            require(result.size() <= max_entries, "Too many project assets.");
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

std::vector<std::string> missing_assets(const scene& world) {
    std::vector<std::string> result;
    auto check = [&](const std::string& path) {
        if (!path.empty() && !fs::is_regular_file(fs::u8path(path))) {
            result.push_back(path);
        }
    };
    for (const auto& t : world.textures) {
        check(t.path);
    }
    for (const auto& m : world.meshes) {
        check(m.file_path);
        check(m.thumbnail_path);
    }
    for (const auto& m : world.materials) {
        for (const auto& n : m.graph.nodes) {
            check(n.texture_path);
        }
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}
} // namespace dusk_project
