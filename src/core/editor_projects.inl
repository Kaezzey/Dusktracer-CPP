#include "core/project.h"
#include "core/project_dialog.h"
#include <unordered_set>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define GLFW_EXPOSE_NATIVE_WIN32
#include "GLFW/glfw3native.h"
#endif

namespace project_ui {
namespace fs = std::filesystem;
enum class action { none, new_scene, open_scene, open_folder, save, save_as, refresh, close };
static fs::path file, pending_path;
static uint64_t saved_fingerprint = 0;
static bool dirty = false, confirm_changes = false, discard_confirmed = false, exit_requested = false;
static action pending = action::none;
static std::string notice;
static std::chrono::steady_clock::time_point last_check;
} // namespace project_ui

static bool g_viewport_match_render = true;

static dusk_project::view CurrentProjectView() {
    return {g_scene,
            g_camera,
            g_renderer,
            g_editor_cam,
            g_render_selection,
            g_viewport_mode,
            g_viewport_match_render,
            g_camera_move_speed,
            g_camera_look_sens};
}

static dusk_project::document CurrentProjectDocument() {
    auto v = CurrentProjectView();
    return {v.world,         v.camera_settings,         v.render_settings, v.viewport,        v.device,
            v.viewport_mode, v.match_render_resolution, v.movement_speed,  v.look_sensitivity};
}

static std::filesystem::path ProjectRoot() {
    return project_ui::file.empty() ? std::filesystem::current_path() : project_ui::file.parent_path();
}

static std::string ProjectOutputPath(const char* name) {
    const auto directory = ProjectRoot() / "Renders";
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    return (directory / name).u8string();
}

static bool ProjectHasChanges() {
    project_ui::dirty = dusk_project::fingerprint(CurrentProjectView()) != project_ui::saved_fingerprint;
    return project_ui::dirty;
}

static void ReleaseProjectMeshes(std::vector<gpu_mesh>& meshes) {
    for (auto& mesh : meshes) {
        glDeleteVertexArrays(1, &mesh.vao);
        glDeleteBuffers(1, &mesh.vbo);
        glDeleteBuffers(1, &mesh.ebo);
    }
    meshes.clear();
}

struct prepared_project {
    dusk_project::document state;
    std::vector<gpu_mesh> meshes;
    std::string warnings;
    ~prepared_project() {
        ReleaseProjectMeshes(meshes);
    }
};

static void DiscoverProjectAssets(scene& world, const std::filesystem::path& root) {
    std::unordered_set<std::string> known;
    for (const auto& t : world.textures) {
        known.insert(t.path);
    }
    for (const auto& m : world.meshes) {
        known.insert(m.file_path);
    }
    for (const auto& path : dusk_project::discover_assets(root)) {
        if (!known.insert(path.u8string()).second) {
            continue;
        }
        if (dusk_project::classify(path) == dusk_project::asset_kind::texture) {
            world.textures.push_back({path.stem().u8string(), path.u8string()});
        } else {
            scene_mesh_asset asset;
            asset.name = path.stem().u8string();
            asset.file_path = path.u8string();
            world.meshes.push_back(std::move(asset));
        }
    }
}

static void PrepareProjectMeshes(prepared_project& project) {
    auto& world = project.state.world;
    project.meshes.resize(world.meshes.size());
    for (size_t i = 0; i < world.meshes.size(); ++i) {
        auto& asset = world.meshes[i];
        float radius = 1;
        std::vector<std::string> names;
        if (!try_load_assimp_mesh(asset.file_path, project.meshes[i], radius, names)) {
            project.warnings += "Could not load mesh: " + asset.file_path + "\n";
            continue;
        }
        // Reimported geometry may reorder its slots. Preserve bindings by name
        // for both shared defaults and instance overrides, in CPU and raster.
        auto remap = [&](const std::vector<int>& old) {
            std::vector<int> result(names.size(), -1);
            for (size_t slot = 0; slot < names.size(); ++slot) {
                auto found = std::find(asset.slot_names.begin(), asset.slot_names.end(), names[slot]);
                size_t index = size_t(found - asset.slot_names.begin());
                if (found != asset.slot_names.end() && index < old.size()) {
                    result[slot] = old[index];
                }
            }
            return result;
        };
        if (names != asset.slot_names) {
            asset.slot_default_materials = remap(asset.slot_default_materials);
            for (auto& object : world.objects) {
                if (object.mesh_index == int(i)) {
                    object.mesh_slot_materials = remap(object.mesh_slot_materials);
                }
            }
            asset.slot_names = std::move(names);
        }
        asset.approx_radius = radius;
    }
    auto missing = dusk_project::missing_assets(world);
    for (const auto& path : missing) {
        project.warnings += "Missing asset: " + path + "\n";
    }
}

static void ClearProjectPreviews() {
    // Workers keep immutable snapshots. Clearing their lookup tables rejects old
    // results; the global revision counter is never reused between documents.
    g_editor_previews.discard_pending();
    g_texture_previews.discard_pending();
    g_sphere_previews.discard_pending();
    for (auto& item : g_material_thumb_cache) {
        glDeleteTextures(1, &item.second);
    }
    for (auto& item : g_texture_thumb_cache) {
        glDeleteTextures(1, &item.second);
    }
    for (auto& item : g_model_thumb_cache) {
        glDeleteTextures(1, &item.second);
    }
    for (auto& item : g_sphere_preview_states) {
        glDeleteTextures(1, &item.second.texture);
    }
    g_material_thumb_cache.clear();
    g_texture_thumb_cache.clear();
    g_model_thumb_cache.clear();
    g_material_preview_states.clear();
    g_texture_preview_requests.clear();
    g_sphere_preview_states.clear();
    for (auto& item : raster_preview::materials) {
        glDeleteTextures(1, &item.second.instructions);
    }
    raster_preview::materials.clear();
    raster_preview::shadow_key = 0;
    raster_preview::material_error.clear();
    // Refresh also reloads files edited outside the application. A generation
    // distinguishes old decodes even if the new project uses the same path.
    ++raster_preview::texture_generation;
    raster_preview::texture_worker.discard_pending();
    raster_preview::ready_textures.clear();
    for (auto& item : raster_preview::textures) {
        glDeleteTextures(1, &item.second.id);
    }
    raster_preview::textures.clear();
    raster_preview::texture_bytes = 0;
    raster_preview::queued_textures = 0;
}

static void InstallProject(prepared_project& project, const std::filesystem::path& file) {
    transform_gizmo::finish(true);
    ReleaseProjectMeshes(g_gpu_meshes);
    ClearProjectPreviews();
    g_gpu_meshes = std::move(project.meshes);
    auto& d = project.state;
    g_scene = std::move(d.world);
    g_camera = std::move(d.camera_settings);
    g_renderer = d.render_settings;
    g_editor_cam = d.viewport;
    g_render_selection = std::move(d.device);
    g_viewport_mode = d.viewport_mode;
    g_viewport_match_render = d.match_render_resolution;
    g_camera_move_speed = d.movement_speed;
    g_camera_look_sens = d.look_sensitivity;
    g_editor_cam.update_basis();
    g_selected_object = g_selected_light = g_selected_material = g_selected_mesh_asset = -1;
    g_focus_properties = false;
    g_show_gizmo = false;
    g_graph_states.clear();
    g_graph_clipboard = {};
    g_graph_canvas_material = g_graph_file_drop_material = -1;
    g_obj_snapshot_before.clear();
    g_dropped_files.clear();
    UndoManager::Instance().clear();
    g_cached_world.reset();
    g_world_dirty = true;
    g_materials_dirty = false;
    g_rtHasImage = false;
    glDeleteTextures(1, &g_rtTexture);
    g_rtTexture = 0;
    g_rtWidth = g_rtHeight = 0;
    {
        std::lock_guard<std::mutex> lock(g_render_mutex);
        g_render_result = {};
        g_render_diagnostic.clear();
    }
    g_render_has_result = false;
    g_render_final_image_ready = false;
    UpdateMNEEFromScene();
    sync_camera_from_editor(float(g_camera.image_width), float(g_camera.image_height));
    project_ui::file = file;
    project_ui::notice = project.warnings;
}

static bool OpenProjectFile(const std::filesystem::path& path) {
    if (g_render_in_progress) {
        return false;
    }
    try {
        prepared_project project;
        const auto file = std::filesystem::weakly_canonical(std::filesystem::absolute(path));
        project.state = dusk_project::load(file);
        const auto saved = dusk_project::fingerprint(dusk_project::inspect(project.state));
        DiscoverProjectAssets(project.state.world, file.parent_path());
        PrepareProjectMeshes(project);
        InstallProject(project, file);
        project_ui::saved_fingerprint = saved;
        ProjectHasChanges();
        return true;
    } catch (const std::exception& e) {
        project_ui::notice = e.what();
        return false;
    }
}

static bool CreateProjectFile(const std::filesystem::path& path) {
    if (g_render_in_progress) {
        return false;
    }
    try {
        prepared_project project;
        const auto file = std::filesystem::weakly_canonical(std::filesystem::absolute(path));
        std::filesystem::create_directories(file.parent_path());
        project.state = dusk_project::empty_document();
        DiscoverProjectAssets(project.state.world, file.parent_path());
        PrepareProjectMeshes(project);
        project.state = dusk_project::save(file, dusk_project::inspect(project.state));
        InstallProject(project, file);
        project_ui::saved_fingerprint = dusk_project::fingerprint(CurrentProjectView());
        project_ui::dirty = false;
        return true;
    } catch (const std::exception& e) {
        project_ui::notice = e.what();
        return false;
    }
}

static bool OpenProjectLocation(const std::filesystem::path& path) {
    try {
        if (!std::filesystem::is_directory(path)) {
            return OpenProjectFile(path);
        }
        std::vector<std::filesystem::path> scenes;
        for (const auto& entry : std::filesystem::directory_iterator(path)) {
            if (entry.is_regular_file() && has_extension_ci(entry.path().u8string(), ".dusk")) {
                scenes.push_back(entry.path());
            }
        }
        if (scenes.empty()) {
            return CreateProjectFile(path / "Scene.dusk");
        }
        if (scenes.size() == 1) {
            return OpenProjectFile(scenes.front());
        }
        project_ui::notice =
            "This folder contains several scenes. Use File > Open Scene to choose a .dusk file.";
    } catch (const std::exception& e) {
        project_ui::notice = e.what();
    }
    return false;
}

static bool SaveProjectFile(const std::filesystem::path& path) {
    try {
        auto saved = dusk_project::save(path, CurrentProjectView());
        // Save As can relocate paths. Keep scene/mesh indices stable and retain
        // the current GL geometry, selection, camera, and undo history.
        g_scene = std::move(saved.world);
        InvalidateAllMaterialThumbnails();
        g_world_dirty = true;
        project_ui::file = std::filesystem::weakly_canonical(std::filesystem::absolute(path));
        project_ui::saved_fingerprint = dusk_project::fingerprint(CurrentProjectView());
        project_ui::dirty = false;
        g_materials_dirty = false;
        return true;
    } catch (const std::exception& e) {
        project_ui::notice = e.what();
        return false;
    }
}

static std::optional<std::filesystem::path> ChooseProjectPath(GLFWwindow* window,
                                                              dusk_project::dialog_kind kind) {
#ifdef _WIN32
    void* owner = glfwGetWin32Window(window);
#else
    void* owner = nullptr;
#endif
    return dusk_project::choose_path(owner, kind, project_ui::file);
}

static bool SaveCurrentProject(GLFWwindow* window, bool save_as = false) {
    try {
        auto path = project_ui::file;
        if (path.empty() || save_as) {
            auto selected = ChooseProjectPath(window, dusk_project::dialog_kind::save_scene);
            if (!selected) {
                return false;
            }
            path = *selected;
            if (!has_extension_ci(path.u8string(), ".dusk")) {
                throw std::runtime_error("Choose a .dusk scene filename.");
            }
        }
        return SaveProjectFile(path);
    } catch (const std::exception& e) {
        project_ui::notice = e.what();
        return false;
    }
}

static void RequestProjectAction(project_ui::action action, const std::filesystem::path& path = {}) {
    if (project_ui::pending == project_ui::action::none) {
        project_ui::pending = action;
        project_ui::pending_path = path;
    }
}

// Called before constructing ImGui draw commands: switching projects may delete
// GL thumbnails that the preceding frame referenced.
static void ProcessProjectActions(GLFWwindow* window) {
    using namespace project_ui;
    const auto requested = pending;
    if (requested == action::none || confirm_changes) {
        return;
    }
    const bool switches =
        requested == action::new_scene || requested == action::open_scene || requested == action::open_folder;
    if (g_render_in_progress && (switches || requested == action::refresh)) {
        pending = action::none;
        return;
    }
    if ((switches || requested == action::close) && !discard_confirmed && ProjectHasChanges()) {
        confirm_changes = true;
        return;
    }
    pending = action::none;
    discard_confirmed = false;
    auto selected_path = std::move(pending_path);
    pending_path.clear();
    try {
        if (requested == action::save || requested == action::save_as) {
            SaveCurrentProject(window, requested == action::save_as);
        } else if (requested == action::close) {
            exit_requested = true;
        } else if (requested == action::refresh) {
            prepared_project project;
            project.state = CurrentProjectDocument();
            DiscoverProjectAssets(project.state.world, ProjectRoot());
            PrepareProjectMeshes(project);
            InstallProject(project, file);
            ProjectHasChanges();
        } else if (requested == action::open_folder) {
            auto folder = selected_path.empty()
                              ? ChooseProjectPath(window, dusk_project::dialog_kind::open_folder)
                              : std::optional<fs::path>(selected_path);
            if (folder) {
                OpenProjectLocation(*folder);
            }
        } else {
            auto path = selected_path.empty()
                            ? ChooseProjectPath(window, requested == action::new_scene
                                                            ? dusk_project::dialog_kind::save_scene
                                                            : dusk_project::dialog_kind::open_scene)
                            : std::optional<fs::path>(selected_path);
            if (!path) {
                return;
            }
            if (requested == action::new_scene) {
                CreateProjectFile(*path);
            } else {
                OpenProjectFile(*path);
            }
        }
    } catch (const std::exception& e) {
        notice = e.what();
    }
}

static void DrawProjectDialogs(GLFWwindow* window) {
    using namespace project_ui;
    if (confirm_changes) {
        ImGui::OpenPopup("Unsaved changes");
    }
    if (ImGui::BeginPopupModal("Unsaved changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Save your changes before continuing?");
        ImGui::TextDisabled("%s", file.empty() ? "Untitled scene" : file.filename().u8string().c_str());
        if (ImGui::Button("Save", ImVec2(100, 0)) && SaveCurrentProject(window)) {
            confirm_changes = false;
            discard_confirmed = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Discard", ImVec2(100, 0))) {
            confirm_changes = false;
            discard_confirmed = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100, 0))) {
            confirm_changes = false;
            pending = action::none;
            ImGui::CloseCurrentPopup();
        }
        if (!notice.empty()) {
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 460);
            ImGui::TextUnformatted(notice.c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::EndPopup();
    }
    if (!notice.empty() && !confirm_changes) {
        ImGui::OpenPopup("Project message");
    }
    if (ImGui::BeginPopupModal("Project message", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::BeginChild("Details", ImVec2(600, 180));
        ImGui::TextWrapped("%s", notice.c_str());
        ImGui::EndChild();
        if (ImGui::Button("OK", ImVec2(100, 0))) {
            notice.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    const auto now = std::chrono::steady_clock::now();
    if (!ImGui::IsAnyItemActive() && now - last_check > std::chrono::milliseconds(250)) {
        ProjectHasChanges();
        last_check = now;
        const auto title = (file.empty() ? std::string("Untitled") : file.filename().u8string()) +
                           (dirty ? " *" : "") + " - Dusktracer";
        static std::string shown_title;
        if (title != shown_title) {
            glfwSetWindowTitle(window, title.c_str());
            shown_title = title;
        }
    }
}

static void DrawProjectMenu() {
    using namespace project_ui;
    const bool available = !g_render_in_progress;
    if (ImGui::MenuItem("New Project...", "Ctrl+N", false, available)) {
        RequestProjectAction(action::new_scene);
    }
    if (ImGui::MenuItem("Open Scene...", "Ctrl+O", false, available)) {
        RequestProjectAction(action::open_scene);
    }
    if (ImGui::MenuItem("Open Project Folder...", nullptr, false, available)) {
        RequestProjectAction(action::open_folder);
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Save", "Ctrl+S")) {
        RequestProjectAction(action::save);
    }
    if (ImGui::MenuItem("Save As...", "Ctrl+Shift+S")) {
        RequestProjectAction(action::save_as);
    }
    if (ImGui::MenuItem("Refresh Project Assets", nullptr, false, available && !file.empty())) {
        RequestProjectAction(action::refresh);
    }
    ImGui::Separator();
}

static void ProjectShortcuts() {
    const auto& io = ImGui::GetIO();
    if (!io.KeyCtrl || io.WantTextInput || ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) {
        return;
    }
    using project_ui::action;
    if (ImGui::IsKeyPressed(ImGuiKey_S, false)) {
        RequestProjectAction(io.KeyShift ? action::save_as : action::save);
    }
    if (!g_render_in_progress) {
        if (ImGui::IsKeyPressed(ImGuiKey_N, false)) {
            RequestProjectAction(action::new_scene);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_O, false)) {
            RequestProjectAction(action::open_scene);
        }
    }
}
