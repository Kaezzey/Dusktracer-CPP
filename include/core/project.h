#pragma once

#include "editor_camera.h"
#include "render_backend.h"
#include <filesystem>

namespace dusk_project {

// Only authored state is stored. Runtime geometry, render buffers and GPU handles
// are rebuilt by the editor after a document has loaded successfully.
struct document {
    scene world;
    camera camera_settings;
    renderer render_settings;
    editor_camera_state viewport;
    render_selection device;
    int viewport_mode = 1;
    bool match_render_resolution = true;
    float movement_speed = 4, look_sensitivity = .002f;
};

struct view {
    const scene& world;
    const camera& camera_settings;
    const renderer& render_settings;
    const editor_camera_state& viewport;
    const render_selection& device;
    int viewport_mode;
    bool match_render_resolution;
    float movement_speed, look_sensitivity;
};

view inspect(const document& doc);
document empty_document();
uint64_t fingerprint(const view& state);

// Disk paths are UTF-8, relative to the scene file. Live paths are absolute so
// switching projects never changes the process directory beneath a render job.
document load(const std::filesystem::path& file);
document save(const std::filesystem::path& file, const view& state);

enum class asset_kind { unsupported, texture, mesh };
asset_kind classify(const std::filesystem::path& file);
std::vector<std::filesystem::path> discover_assets(const std::filesystem::path& root);
std::filesystem::path import_asset(const std::filesystem::path& source, const std::filesystem::path& root);
std::vector<std::string> missing_assets(const scene& world);

} // namespace dusk_project
