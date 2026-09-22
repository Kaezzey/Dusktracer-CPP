#include "core/affine_transform.h"
#include "core/gpu_scene.h"
#include "core/mesh_loader.h"
// src/core/editor_main.cpp

#include <cstdio>
#include <stdexcept>
#include <atomic>
#include <vector>
#include <cstdint>
#include <cmath>
#include <string>
#include <fstream>
#include <sstream>
#include <cctype>
#include <cstring>
#include <memory>   // for std::shared_ptr
#include <filesystem>
#include <thread>   // for render worker
#include <mutex>    // for result handoff
#include <chrono>   // for timing if you want
#include <unordered_map>
#include <algorithm>
#include <future>

#include "../../external/glew/include/GL/glew.h"
#include "../../include/external/GLFW/glfw3.h"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "imgui_internal.h"

// stb_image for loading PNG icon
#include "../../include/external/stb_image.h"

// Helper: set a GLFW window icon from a PNG file path (relative to working dir)
static bool SetWindowIconFromPNG(GLFWwindow* w, const char* relpath)
{
    if (!w || !relpath) return false;
    int ix = 0, iy = 0, ic = 0;
    unsigned char* pixels = stbi_load(relpath, &ix, &iy, &ic, 4);
    if (!pixels) return false;
    GLFWimage img;
    img.width = ix;
    img.height = iy;
    img.pixels = pixels;
    glfwSetWindowIcon(w, 1, &img);
    stbi_image_free(pixels);
    return true;
}

// Helper: try several likely locations for the icon filename (exe dir, resources/, parent resources/)
static void SetWindowIconAuto(GLFWwindow* w, const char* filename)
{
    if (!w || !filename) return;
    // Try exact filename first
    if (SetWindowIconFromPNG(w, filename)) return;

    // Try resources/ subfolder
    std::string res1 = std::string("resources/") + filename;
    if (SetWindowIconFromPNG(w, res1.c_str())) return;

    // Try parent resources (when running from build subfolder)
    std::string res2 = std::string("../resources/") + filename;
    SetWindowIconFromPNG(w, res2.c_str());
}

#include "../../include/core/camera.h"
#include "../../include/core/renderer.h"
#include "../../include/core/render_backend.h"
#include "../../include/core/scene.h"
#include "../../include/core/hittable_list.h"
#include "../../include/core/editor_camera.h"
#include "../../include/core/undo.h"
#include "../../include/core/image_io.h"

// Assimp for FBX/OBJ mesh import
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

// -----------------------------------------------------------------------------
// Progress state (defined in renderer.h)
// -----------------------------------------------------------------------------

static render_progress_state g_render_progress;

// -----------------------------------------------------------------------------
// Engine globals
// -----------------------------------------------------------------------------

static scene               g_scene;
static camera              g_camera;
static renderer            g_renderer;
static render_selection    g_render_selection;
static editor_camera_state g_editor_cam;
static std::atomic<bool>   g_cancel_flag{false};
static bool                g_scene_initialized = false;
static ImFont*             g_ui_font_body = nullptr;
static ImFont*             g_ui_font_heading = nullptr;

// Viewport focus/hover state
static bool g_viewport_focused = false;
static bool g_viewport_hovered = false;

// Editor camera tuning
static float g_camera_move_speed = 4.0f;    // units per second
static float g_camera_look_sens  = 0.002f;  // radians per pixel

// Currently selected object in the Scene Hierarchy (-1 = none)
static int g_selected_object = -1;
static int g_selected_light = -1;
static bool g_focus_properties = false;
static int g_selected_material = -1; // Selected material in the asset viewer for editing
static bool g_materials_dirty = false;
static std::chrono::steady_clock::time_point g_materials_last_edit;
static void MarkMaterialsDirty() {
    g_materials_dirty = true;
    g_materials_last_edit = std::chrono::steady_clock::now();
}

static int g_selected_mesh_asset = -1; // Selected mesh asset in the asset viewer for editing

// Files dropped this frame
static std::vector<std::string> g_dropped_files;

// Cached RT world + dirty flag
static std::shared_ptr<hittable_list> g_cached_world;
static bool g_world_dirty = true;

// Small GL thumbnail cache for texture assets (path -> GL texture)
static std::unordered_map<std::string, GLuint> g_texture_thumb_cache;
static int g_thumb_budget_per_frame = 4;
static int g_thumb_budget_default = 4;
static int g_thumbs_created_this_frame = 0;

static ImFont* TryLoadFont(ImGuiIO& io, const std::initializer_list<const char*>& candidates, float size, const ImFontConfig* cfg = nullptr)
{
    for (const char* candidate : candidates) {
        if (!candidate) continue;
        std::error_code ec;
        if (!std::filesystem::exists(candidate, ec)) continue;
        if (ImFont* font = io.Fonts->AddFontFromFileTTF(candidate, size, cfg)) {
            return font;
        }
    }
    return nullptr;
}

static void SetupEditorFonts(ImGuiIO& io)
{
    ImFontConfig body_cfg;
    body_cfg.OversampleH = 2;
    body_cfg.OversampleV = 2;
    body_cfg.PixelSnapH = false;
    body_cfg.RasterizerMultiply = 1.05f;
    g_ui_font_body = TryLoadFont(
        io,
        {
            "C:/Windows/Fonts/segoeui.ttf",
            "C:/Windows/Fonts/Inter-Regular.ttf",
            "C:/Windows/Fonts/arial.ttf"
        },
        17.0f,
        &body_cfg
    );

    ImFontConfig heading_cfg = body_cfg;
    heading_cfg.RasterizerMultiply = 1.10f;
    g_ui_font_heading = TryLoadFont(
        io,
        {
            "C:/Windows/Fonts/segoeuib.ttf",
            "C:/Windows/Fonts/bahnschrift.ttf",
            "C:/Windows/Fonts/arialbd.ttf"
        },
        19.0f,
        &heading_cfg
    );

    if (!g_ui_font_body) {
        g_ui_font_body = io.Fonts->AddFontDefault();
    }
    if (!g_ui_font_heading) {
        g_ui_font_heading = g_ui_font_body;
    }
    io.FontDefault = g_ui_font_body;
}

static void ApplyModernEditorTheme()
{
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* colors = style.Colors;

    style.WindowPadding = ImVec2(14.0f, 12.0f);
    style.FramePadding = ImVec2(10.0f, 7.0f);
    style.CellPadding = ImVec2(10.0f, 8.0f);
    style.ItemSpacing = ImVec2(10.0f, 8.0f);
    style.ItemInnerSpacing = ImVec2(8.0f, 6.0f);
    style.TouchExtraPadding = ImVec2(0.0f, 0.0f);
    style.IndentSpacing = 20.0f;
    style.ScrollbarSize = 14.0f;
    style.GrabMinSize = 10.0f;

    style.WindowRounding = 0.0f;
    style.ChildRounding = 0.0f;
    style.FrameRounding = 0.0f;
    style.PopupRounding = 0.0f;
    style.ScrollbarRounding = 0.0f;
    style.GrabRounding = 0.0f;
    style.TabRounding = 0.0f;

    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;
    style.TabBorderSize = 1.0f;

    colors[ImGuiCol_Text]                  = ImVec4(0.95f, 0.97f, 0.99f, 1.00f);
    colors[ImGuiCol_TextDisabled]          = ImVec4(0.56f, 0.63f, 0.70f, 1.00f);
    colors[ImGuiCol_WindowBg]              = ImVec4(0.08f, 0.10f, 0.13f, 0.98f);
    colors[ImGuiCol_ChildBg]               = ImVec4(0.10f, 0.12f, 0.16f, 0.78f);
    colors[ImGuiCol_PopupBg]               = ImVec4(0.10f, 0.12f, 0.16f, 0.98f);
    colors[ImGuiCol_Border]                = ImVec4(0.18f, 0.23f, 0.29f, 0.85f);
    colors[ImGuiCol_BorderShadow]          = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_FrameBg]               = ImVec4(0.13f, 0.16f, 0.20f, 1.00f);
    colors[ImGuiCol_FrameBgHovered]        = ImVec4(0.18f, 0.25f, 0.30f, 1.00f);
    colors[ImGuiCol_FrameBgActive]         = ImVec4(0.21f, 0.32f, 0.38f, 1.00f);
    colors[ImGuiCol_TitleBg]               = ImVec4(0.07f, 0.09f, 0.12f, 1.00f);
    colors[ImGuiCol_TitleBgActive]         = ImVec4(0.09f, 0.12f, 0.16f, 1.00f);
    colors[ImGuiCol_MenuBarBg]             = ImVec4(0.07f, 0.09f, 0.12f, 1.00f);
    colors[ImGuiCol_ScrollbarBg]           = ImVec4(0.08f, 0.10f, 0.13f, 0.80f);
    colors[ImGuiCol_ScrollbarGrab]         = ImVec4(0.24f, 0.31f, 0.38f, 0.95f);
    colors[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.31f, 0.41f, 0.49f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive]   = ImVec4(0.36f, 0.48f, 0.58f, 1.00f);
    colors[ImGuiCol_CheckMark]             = ImVec4(0.58f, 0.88f, 0.78f, 1.00f);
    colors[ImGuiCol_SliderGrab]            = ImVec4(0.42f, 0.76f, 0.67f, 0.95f);
    colors[ImGuiCol_SliderGrabActive]      = ImVec4(0.54f, 0.88f, 0.79f, 1.00f);
    colors[ImGuiCol_Button]                = ImVec4(0.17f, 0.29f, 0.33f, 1.00f);
    colors[ImGuiCol_ButtonHovered]         = ImVec4(0.24f, 0.42f, 0.47f, 1.00f);
    colors[ImGuiCol_ButtonActive]          = ImVec4(0.29f, 0.50f, 0.56f, 1.00f);
    colors[ImGuiCol_Header]                = ImVec4(0.15f, 0.22f, 0.28f, 1.00f);
    colors[ImGuiCol_HeaderHovered]         = ImVec4(0.21f, 0.31f, 0.38f, 1.00f);
    colors[ImGuiCol_HeaderActive]          = ImVec4(0.25f, 0.37f, 0.45f, 1.00f);
    colors[ImGuiCol_Separator]             = ImVec4(0.19f, 0.24f, 0.31f, 1.00f);
    colors[ImGuiCol_SeparatorHovered]      = ImVec4(0.33f, 0.54f, 0.60f, 1.00f);
    colors[ImGuiCol_SeparatorActive]       = ImVec4(0.42f, 0.68f, 0.74f, 1.00f);
    colors[ImGuiCol_ResizeGrip]            = ImVec4(0.30f, 0.46f, 0.54f, 0.30f);
    colors[ImGuiCol_ResizeGripHovered]     = ImVec4(0.42f, 0.68f, 0.74f, 0.70f);
    colors[ImGuiCol_ResizeGripActive]      = ImVec4(0.54f, 0.88f, 0.79f, 0.90f);
    colors[ImGuiCol_Tab]                   = ImVec4(0.12f, 0.16f, 0.20f, 1.00f);
    colors[ImGuiCol_TabHovered]            = ImVec4(0.20f, 0.29f, 0.35f, 1.00f);
    colors[ImGuiCol_TabActive]             = ImVec4(0.17f, 0.25f, 0.31f, 1.00f);
    colors[ImGuiCol_TabUnfocused]          = ImVec4(0.09f, 0.12f, 0.16f, 1.00f);
    colors[ImGuiCol_TabUnfocusedActive]    = ImVec4(0.13f, 0.18f, 0.22f, 1.00f);
    colors[ImGuiCol_DockingPreview]        = ImVec4(0.40f, 0.79f, 0.73f, 0.28f);
    colors[ImGuiCol_DockingEmptyBg]        = ImVec4(0.06f, 0.08f, 0.11f, 1.00f);
    colors[ImGuiCol_PlotHistogram]         = ImVec4(0.42f, 0.84f, 0.72f, 1.00f);
    colors[ImGuiCol_PlotHistogramHovered]  = ImVec4(0.56f, 0.93f, 0.81f, 1.00f);
    colors[ImGuiCol_TextSelectedBg]        = ImVec4(0.25f, 0.48f, 0.54f, 0.45f);
    colors[ImGuiCol_DragDropTarget]        = ImVec4(0.54f, 0.88f, 0.79f, 0.95f);
    colors[ImGuiCol_NavHighlight]          = ImVec4(0.54f, 0.88f, 0.79f, 1.00f);
}

static void DrawPanelTitle(const char* title, const char* subtitle = nullptr)
{
    if (g_ui_font_heading) ImGui::PushFont(g_ui_font_heading);
    ImGui::TextUnformatted(title);
    if (g_ui_font_heading) ImGui::PopFont();
    if (subtitle && subtitle[0] != '\0') {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("%s", subtitle);
        ImGui::PopStyleColor();
    }
    ImGui::Spacing();
}

static void DrawSectionLabel(const char* label)
{
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.68f, 0.91f, 0.84f, 1.0f));
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    ImGui::Separator();
}

static void DrawInfoChip(const char* label)
{
    ImVec2 pad(9.0f, 5.0f);
    ImVec2 text_size = ImGui::CalcTextSize(label);
    ImVec2 start = ImGui::GetCursorScreenPos();
    ImVec2 end(start.x + text_size.x + pad.x * 2.0f, start.y + text_size.y + pad.y * 2.0f);

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddRectFilled(start, end, IM_COL32(34, 47, 58, 235), 0.0f);
    draw_list->AddRect(start, end, IM_COL32(82, 112, 130, 180), 0.0f);
    draw_list->AddText(ImVec2(start.x + pad.x, start.y + pad.y), IM_COL32(226, 236, 242, 255), label);

    ImGui::Dummy(ImVec2(end.x - start.x, end.y - start.y));
}

static void InvalidateRasterMaterial(int index);
static void InvalidateAllRasterMaterials();
#include "editor_previews.inl"

// Persist mesh -> default material slot assignments between launches.
static const char* kMeshMatDefaultsFile = "mesh_material_defaults.txt";
static const char* kAssetsManifestFile = "assets_manifest.txt";
static void SaveAssetsManifest();
static void LoadAssetsManifest();

static void SaveMeshMaterialDefaults()
{
    std::ofstream out(kMeshMatDefaultsFile);
    if (!out) return;
    for (const auto& mesh : g_scene.meshes) {
        // Format: mesh_name|idx,idx,idx\n   (use '|' as name/data separator)
        out << mesh.name << "|";
        for (size_t i = 0; i < mesh.slot_default_materials.size(); ++i) {
            if (i) out << ',';
            out << mesh.slot_default_materials[i];
        }
        out << '\n';
    }
}

static void LoadMeshMaterialDefaults()
{
    std::ifstream in(kMeshMatDefaultsFile);
    if (!in) return;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        size_t sep = line.find('|');
        if (sep == std::string::npos) continue;
        std::string name = line.substr(0, sep);
        std::string rest = line.substr(sep + 1);
        // find matching mesh by name
        for (auto &mesh : g_scene.meshes) {
            if (mesh.name != name) continue;
            mesh.slot_default_materials.clear();
            std::stringstream ss(rest);
            std::string tok;
            while (std::getline(ss, tok, ',')) {
                try {
                    int v = std::stoi(tok);
                    mesh.slot_default_materials.push_back(v);
                } catch (...) {
                    mesh.slot_default_materials.push_back(-1);
                }
            }
            break;
        }
    }
}

// Update camera's MNEE sphere parameters by scanning the current scene for
// the first dielectric sphere. Approximates world-space center and radius.
static void UpdateMNEEFromScene()
{
    // Map material index -> is dielectric and store IOR
    std::unordered_map<int, double> dielectric_ior;
    for (int mi = 0; mi < (int)g_scene.materials.size(); ++mi) {
        const auto& m = g_scene.materials[mi];
        if (m.model == scene_material_model::dielectric) {
            dielectric_ior[mi] = m.ior;
        }
    }

    g_camera.mnee_has_sphere = false;

    for (const auto& obj : g_scene.objects) {
        if (obj.type != scene_object_type::sphere) continue;
        if (obj.material_index < 0) continue;
        auto it = dielectric_ior.find(obj.material_index);
        if (it == dielectric_ior.end()) continue;

        // World center = object translation + local center
        point3 localC = obj.center;
        vec3   worldC = obj.translation + vec3(localC.x(), localC.y(), localC.z());
        // Approximate radius with average scale
        double s = (std::abs(obj.scale.x()) + std::abs(obj.scale.y()) + std::abs(obj.scale.z())) / 3.0;
        double worldR = obj.radius * s;

        g_camera.mnee_sphere_center = point3(worldC.x(), worldC.y(), worldC.z());
        g_camera.mnee_sphere_radius = worldR;
        g_camera.mnee_sphere_ior    = it->second;
        g_camera.mnee_has_sphere    = true;
        break; // first match only
    }
}

// Async render thread + result handoff
static std::thread g_render_thread;
static std::mutex  g_render_mutex;
static bool        g_render_in_progress = false;
static std::atomic<bool> g_render_has_result{false};
static render_result g_render_result;
static std::string g_render_diagnostic;
static std::atomic<bool> g_render_final_image_ready{false};

// Progress window (separate OS window) to show progressive render
static GLFWwindow*            g_progress_window = nullptr;
static std::thread            g_progress_window_thread;
static std::atomic<bool>      g_progress_window_running{false};

// Delayed progress popup control
static int                                        g_progress_popup_delay_ms = 800; // delay before showing progress window
static bool                                       g_progress_popup_pending  = false;
static std::chrono::steady_clock::time_point      g_progress_request_time;

// -----------------------------------------------------------------------------
// Ray-traced image texture
// -----------------------------------------------------------------------------

static GLuint                    g_rtTexture  = 0;
static int                       g_rtWidth    = 0;
static int                       g_rtHeight   = 0;
static bool                      g_rtHasImage = false;
static std::vector<std::uint8_t> g_rtPixels;   // RGBA8

// -----------------------------------------------------------------------------
// Rasterised preview: sphere mesh, GPU meshes, FBO
// -----------------------------------------------------------------------------

struct raster_vertex {
    float position[3], normal[3], uv[2], tangent[3], bitangent[3];
};
struct raster_range {
    unsigned first = 0;
    GLsizei count = 0;
    int material_slot = 0;
};
struct gpu_mesh {
    GLuint vao = 0;
    GLuint vbo = 0;
    GLuint ebo = 0;
    GLsizei index_count = 0;
    std::vector<raster_range> ranges;
    point3 minimum, maximum;
};

static GLuint g_modelThumbnailShader = 0;
static GLuint g_rasterShader = 0;
static GLuint g_rasterSphereVAO = 0;
static GLuint g_rasterSphereVBO = 0;
static GLuint g_rasterSphereEBO = 0;
static GLsizei g_rasterSphereIndexCount = 0;
static GLuint g_rasterCubeVAO = 0;
static GLuint g_rasterCubeVBO = 0;
static GLuint g_rasterCubeEBO = 0;
static GLsizei g_rasterCubeIndexCount = 0;

// One gpu_mesh per scene mesh asset, same index as g_scene.meshes
static std::vector<gpu_mesh> g_gpu_meshes;

// Per-light transient UI state: yaw (degrees) and time-of-day (0..24)
// Per-light transient UI state removed (directional controls removed)

static GLuint g_rasterFBO      = 0;
static GLuint g_rasterColorTex = 0;
static GLuint g_rasterDepthRBO = 0;
static int    g_rasterWidth    = 0;
static int    g_rasterHeight   = 0;

// Picking FBO + shader
static GLuint g_pickFBO        = 0;
static GLuint g_pickColorTex   = 0;
static GLuint g_pickDepthRBO   = 0;
static GLuint g_pickShader     = 0;

// Model thumbnail cache: mesh_index -> GL texture
static std::unordered_map<int, GLuint> g_model_thumb_cache;

// Forward declarations for functions defined later in this file
static void BuildRasterShader();
static void make_lookat(const vec3& eye, const vec3& center, const vec3& up, float out[16]);
static void make_perspective(float fov_deg, float aspect, float znear, float zfar, float out[16]);

static GLuint GenerateModelThumbnail(int mesh_index)
{
    if (mesh_index < 0 || mesh_index >= (int)g_gpu_meshes.size()) return 0;
    const gpu_mesh& gm = g_gpu_meshes[mesh_index];
    if (gm.vao == 0 || gm.index_count == 0) return 0;

    const int sz = 80;

    GLint prevFBO = 0; glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFBO);
    GLint vp[4]; glGetIntegerv(GL_VIEWPORT, vp);

    // Create texture
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, sz, sz, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

    // Create FBO + depth RBO
    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);

    GLuint rbo = 0;
    glGenRenderbuffers(1, &rbo);
    glBindRenderbuffer(GL_RENDERBUFFER, rbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, sz, sz);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rbo);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (rbo) glDeleteRenderbuffers(1, &rbo);
        if (fbo) glDeleteFramebuffers(1, &fbo);
        if (tex) { glDeleteTextures(1, &tex); tex = 0; }
        return 0;
    }

    // Save previous viewport and FBO
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, sz, sz);
    glEnable(GL_DEPTH_TEST);
    glClearColor(0.12f, 0.12f, 0.12f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    if (g_modelThumbnailShader == 0) BuildRasterShader();
    glUseProgram(g_modelThumbnailShader);

    // Camera: compute distance from mesh approx radius so thumbnails are auto-framed
    float view[16]; float proj[16]; float model[16];
    for (int i = 0; i < 16; ++i) model[i] = (i % 5 == 0) ? 1.0f : 0.0f; // identity

    // Fetch approximate bounding sphere radius from the scene asset (fallback to 1.0)
    float r = 1.0f;
    if (mesh_index >= 0 && mesh_index < (int)g_scene.meshes.size()) {
        r = (float)g_scene.meshes[mesh_index].approx_radius;
        if (r <= 0.0f) r = 1.0f;
    }

    // Preview FOV and framing. Use a small margin so the mesh isn't tightly cropped.
    const float fov_deg = 45.0f;
    const float fov_rad = fov_deg * (3.14159265358979323846f / 180.0f);
    const float framing = 1.25f; // 1.0 = tight, >1 gives more padding

    // Distance so that sphere of radius r fits within the vertical FOV: d = r / tan(fov/2)
    float distance = (r / tanf(fov_rad * 0.5f)) * framing;
    if (distance < 0.5f) distance = 0.5f;

    // Place camera at a slight angle for a nicer preview (azimuth, elevation)
    const float az_deg = 30.0f;
    const float el_deg = 20.0f;
    const float az = az_deg * (3.14159265358979323846f / 180.0f);
    const float el = el_deg * (3.14159265358979323846f / 180.0f);

    float cx = distance * cosf(el) * sinf(az);
    float cy = distance * sinf(el);
    float cz = distance * cosf(el) * cosf(az);

    make_lookat(vec3(cx, cy, cz), vec3(0,0,0), vec3(0,1,0), view);
    make_perspective(fov_deg, 1.0f, 0.01f, distance * 4.0f + r, proj);

    GLint locModel    = glGetUniformLocation(g_modelThumbnailShader, "uModel");
    GLint locView     = glGetUniformLocation(g_modelThumbnailShader, "uView");
    GLint locProj     = glGetUniformLocation(g_modelThumbnailShader, "uProj");
    GLint locColor    = glGetUniformLocation(g_modelThumbnailShader, "uColor");
    GLint locLightDir = glGetUniformLocation(g_modelThumbnailShader, "uLightDir");

    glUniformMatrix4fv(locModel, 1, GL_FALSE, model);
    glUniformMatrix4fv(locView, 1, GL_FALSE, view);
    glUniformMatrix4fv(locProj, 1, GL_FALSE, proj);
    glUniform3f(locColor, 0.8f, 0.8f, 0.8f);
    glUniform3f(locLightDir, -0.5f, -1.0f, -0.3f);

    // Draw the mesh
    glBindVertexArray(gm.vao);
    glDrawElements(GL_TRIANGLES, gm.index_count, GL_UNSIGNED_INT, 0);
    glBindVertexArray(0);

    // Restore
    glBindFramebuffer(GL_FRAMEBUFFER, prevFBO);
    glViewport(vp[0], vp[1], vp[2], vp[3]);

    // Cleanup FBO/RBO but keep texture
    if (rbo) { glDeleteRenderbuffers(1, &rbo); }
    if (fbo) { glDeleteFramebuffers(1, &fbo); }

    return tex;
}

static GLuint GetOrCreateModelThumbnail(int mesh_index)
{
    auto it = g_model_thumb_cache.find(mesh_index);
    if (it != g_model_thumb_cache.end()) return it->second;
    if (g_thumbs_created_this_frame >= g_thumb_budget_per_frame) return 0;
    GLuint t = GenerateModelThumbnail(mesh_index);
    if (t) {
        g_model_thumb_cache[mesh_index] = t;
        ++g_thumbs_created_this_frame;
    }
    return t;
}

// Gizmo (lines) shader + buffers
static GLuint g_lineShader     = 0;
static GLuint g_gizmoVAO       = 0;
static GLuint g_gizmoVBO       = 0;
static GLuint g_gizmoConeVAO   = 0;
static GLuint g_gizmoConeVBO   = 0;
static int    g_gizmoConeVertexCount = 0;
// Legacy gizmo buffers (directional gizmo removed)
// CPU-side copy of cone triangle positions for precise hit-testing
static std::vector<vec3> g_gizmoConeTriangles;
static int g_gizmoConeSegments = 16;
static double g_gizmoConeLen = 0.18;
static double g_gizmoBaseRad = 0.06;

// Snapshot for object transform to support undo/redo
struct ObjSnapshot {
    point3 center;
    vec3   translation;
    vec3   rotation_deg;
    vec3   scale;
    double radius;
};

static std::unordered_map<int, ObjSnapshot> g_obj_snapshot_before;

static bool   g_show_gizmo     = false;
static int    g_active_gizmo_axis = -1; // -1 = none, 0=X,1=Y,2=Z
static vec3   g_gizmo_hit_point;       // world-space closest point on axis at mouse down
static vec3   g_gizmo_initial_obj_translation;

// 0 = Ray Traced, 1 = Rasterised
static int g_viewport_mode = 1;

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------

static void glfw_error_callback(int error, const char* description)
{
    std::fprintf(stderr, "GLFW Error %d: %s\n", error, description);
}

static inline double dot3(const vec3& a, const vec3& b)
{
    return a.x()*b.x() + a.y()*b.y() + a.z()*b.z();
}

// Ray-triangle intersection (Möller–Trumbore). Returns true and sets outT to ray parameter if hit.
static bool RayIntersectsTriangle(const ray& r, const vec3& v0, const vec3& v1, const vec3& v2, double& outT)
{
    const double EPS = 1e-8;
    vec3 edge1 = v1 - v0;
    vec3 edge2 = v2 - v0;
    vec3 h = cross(r.direction(), edge2);
    double a = dot(edge1, h);
    if (std::fabs(a) < EPS) return false; // parallel
    double f = 1.0 / a;
    vec3 s = r.origin() - v0;
    double u = f * dot(s, h);
    if (u < 0.0 || u > 1.0) return false;
    vec3 q = cross(s, edge1);
    double v = f * dot(r.direction(), q);
    if (v < 0.0 || u + v > 1.0) return false;
    double t = f * dot(edge2, q);
    if (t > EPS) {
        outT = t;
        return true;
    }
    return false;
}

// Case-insensitive extension check
static bool has_extension_ci(const std::string& path, const char* ext)
{
    size_t lenp = path.size();
    size_t lene = std::strlen(ext);
    if (lenp < lene) return false;
    size_t off = lenp - lene;
    for (size_t i = 0; i < lene; ++i) {
        char c1 = (char)std::tolower(path[off + i]);
        char c2 = (char)std::tolower(ext[i]);
        if (c1 != c2) return false;
    }
    return true;
}

static inline void zup_to_yup(float& x, float& y, float& z)
{
    float nx = x;
    float ny = z;
    float nz = -y;
    x = nx;
    y = ny;
    z = nz;
}

// Forward-declare mesh loader/result so we can optionally import a mesh
// into build_default_scene (actual implementation is below).
struct MeshLoadResult;
static MeshLoadResult load_assimp_mesh_as_gpu_mesh(
    const std::string& full_path,
    bool               z_up,
    bool               normalise_unit,
    double             user_scale);

// Lightweight wrapper usable before the loader definition: returns true on success
// and fills out a gpu_mesh, approx_radius and slot names.
static bool try_load_assimp_mesh(const std::string& full_path,
                                 gpu_mesh& out_mesh,
                                 float& out_approx_radius,
                                 std::vector<std::string>& out_slot_names);

// -----------------------------------------------------------------------------
// Default scene
// -----------------------------------------------------------------------------

// Export the current editor `scene` as a C++ snippet you can paste into
// `build_default_scene(scene& scn)`. Writes to `path` (e.g. "Renders/scene_export.cpp").
static void export_scene_as_cpp(const scene& scn, const std::string& path)
{
    namespace fs = std::filesystem;

    try {
        fs::path p(path);
        if (!p.parent_path().empty()) fs::create_directories(p.parent_path());
    } catch (...) {
        std::fprintf(stderr, "Warning: failed to create parent directory for %s\n", path.c_str());
    }

    std::ofstream out(path);
    if (!out.is_open()) {
        std::fprintf(stderr, "Failed to open %s for writing\n", path.c_str());
        return;
    }

    auto esc = [&](const std::string& s) {
        std::string r; r.reserve(s.size());
        for (char c : s) {
            if (c == '\\') r += "\\\\";
            else if (c == '"') r += "\\\"";
            else r += c;
        }
        return r;
    };

    out << "// Generated scene code - paste into build_default_scene(scene& scn)\n";
    out << "scn.textures.clear(); scn.materials.clear(); scn.objects.clear(); scn.meshes.clear(); scn.lights.clear();\n\n";

    // Textures
    for (const auto& t : scn.textures) {
        out << "scn.textures.push_back({\"" << esc(t.name) << "\", \"" << esc(t.path) << "\"});\n";
    }
    out << "\n";

    // Materials
    for (const auto& m : scn.materials) {
        out << "scn.materials.push_back({});\n";
        out << "scn.materials.back().name = \"" << esc(m.name) << "\";\n";
        // model
        const char* model_str = "scene_material_model::lambert";
        switch (m.model) {
            case scene_material_model::lambert: model_str = "scene_material_model::lambert"; break;
            case scene_material_model::metal: model_str = "scene_material_model::metal"; break;
            case scene_material_model::dielectric: model_str = "scene_material_model::dielectric"; break;
            case scene_material_model::diffuse_light: model_str = "scene_material_model::diffuse_light"; break;
            case scene_material_model::isotropic: model_str = "scene_material_model::isotropic"; break;
            case scene_material_model::pbr: model_str = "scene_material_model::pbr"; break;
        }
        out << "scn.materials.back().model = " << model_str << ";\n";
        out << "scn.materials.back().base_color = colour(" << m.base_color.x() << ", " << m.base_color.y() << ", " << m.base_color.z() << ");\n";
        out << "scn.materials.back().metallic = " << m.metallic << ";\n";
        out << "scn.materials.back().roughness = " << m.roughness << ";\n";
        out << "scn.materials.back().ior = " << m.ior << ";\n";
        if (!m.graph.nodes.empty()) out << "deserialize_material_graph(\"" << serialize_material_graph(m.graph) << "\", scn.materials.back().graph);\n";
        out << "scn.materials.back().emission = vec3(" << m.emission.x() << ", " << m.emission.y() << ", " << m.emission.z() << ");\n";
        out << "\n";
    }

    // Objects
    for (const auto& o : scn.objects) {
        out << "{\n";
        out << "    scene_object obj;\n";
        out << "    obj.name = \"" << esc(o.name) << "\";\n";
        // type
        const char* type_str = "scene_object_type::sphere";
        switch (o.type) {
            case scene_object_type::sphere: type_str = "scene_object_type::sphere"; break;
            case scene_object_type::cube: type_str = "scene_object_type::cube"; break;
            case scene_object_type::mesh_instance: type_str = "scene_object_type::mesh_instance"; break;
        }
        out << "    obj.type = " << type_str << ";\n";
        out << "    obj.material_index = " << o.material_index << ";\n";
        out << "    obj.center = point3(" << o.center.x() << ", " << o.center.y() << ", " << o.center.z() << ");\n";
        out << "    obj.radius = " << o.radius << ";\n";
        out << "    obj.translation = vec3(" << o.translation.x() << ", " << o.translation.y() << ", " << o.translation.z() << ");\n";
        out << "    obj.rotation_deg = vec3(" << o.rotation_deg.x() << ", " << o.rotation_deg.y() << ", " << o.rotation_deg.z() << ");\n";
        out << "    obj.scale = vec3(" << o.scale.x() << ", " << o.scale.y() << ", " << o.scale.z() << ");\n";
        if (!o.mesh_slot_materials.empty()) {
            out << "    obj.mesh_slot_materials = {";
            for (size_t i = 0; i < o.mesh_slot_materials.size(); ++i) {
                if (i) out << ", ";
                out << o.mesh_slot_materials[i];
            }
            out << "};\n";
        }
        out << "    scn.objects.push_back(obj);\n";
        out << "}\n";
    }

    // Lights
    for (const auto& L : scn.lights) {
        out << "scn.lights.push_back({\"" << esc(L.name) << "\", ";
        if (L.type == scene_light_type::directional) {
            out << "scene_light_type::directional";
        } else {
            out << "scene_light_type::point";
        }
        out << "});\n";
        out << "scn.lights.back().radiance = vec3(" << L.radiance.x() << ", " << L.radiance.y() << ", " << L.radiance.z() << ");\n";
        if (L.type == scene_light_type::directional) {
            out << "scn.lights.back().direction = vec3(" << L.direction.x() << ", " << L.direction.y() << ", " << L.direction.z() << ");\n";
            out << "scn.lights.back().angular_radius_deg = " << L.angular_radius_deg << ";\n";
        } else {
            out << "scn.lights.back().position = point3(" << L.position.x() << ", " << L.position.y() << ", " << L.position.z() << ");\n";
            out << "scn.lights.back().range = " << L.range << ";\n";
        }
    }

    out.close();
    std::printf("Scene exported to '%s'\n", path.c_str());
}
static void build_default_scene(scene& scn)
{
    scn.textures.clear(); scn.materials.clear(); scn.objects.clear(); scn.meshes.clear(); scn.lights.clear();

    scn.textures.push_back({"Chest_Roughness", "models\\Chest_Roughness.png"});
    scn.textures.push_back({"Helmet_Base_color", "models\\Helmet_Base_color.png"});
    scn.textures.push_back({"Helmet_Metallic", "models\\Helmet_Metallic.png"});
    scn.textures.push_back({"Helmet_Normal_OpenGL", "models\\Helmet_Normal_OpenGL.png"});
    scn.textures.push_back({"Helmet_Roughness", "models\\Helmet_Roughness.png"});
    scn.textures.push_back({"Legs_Base_color", "models\\Legs_Base_color.png"});
    scn.textures.push_back({"Legs_Metallic", "models\\Legs_Metallic.png"});
    scn.textures.push_back({"Legs_Normal_OpenGL", "models\\Legs_Normal_OpenGL.png"});
    scn.textures.push_back({"Legs_Roughness", "models\\Legs_Roughness.png"});
    scn.textures.push_back({"Arms_Base_color", "models\\Arms_Base_color.png"});
    scn.textures.push_back({"Arms_Metallic", "models\\Arms_Metallic.png"});
    scn.textures.push_back({"Arms_Normal_OpenGL", "models\\Arms_Normal_OpenGL.png"});
    scn.textures.push_back({"Arms_Roughness", "models\\Arms_Roughness.png"});
    scn.textures.push_back({"Chest_Base_color", "models\\Chest_Base_color.png"});
    scn.textures.push_back({"Chest_Metallic", "models\\Chest_Metallic.png"});
    scn.textures.push_back({"Chest_Normal_OpenGL", "models\\Chest_Normal_OpenGL.png"});
    scn.materials.push_back({});
    scn.materials.back().name = "Ground";
    scn.materials.back().model = scene_material_model::lambert;
    scn.materials.back().base_color = colour(0.8, 0.8, 0);
    scn.materials.back().metallic = 0;
    scn.materials.back().roughness = 0.5;
    scn.materials.back().ior = 1.5;
    scn.materials.back().emission = vec3(0, 0, 0);

    scn.materials.push_back({});
    scn.materials.back().name = "Center";
    scn.materials.back().model = scene_material_model::lambert;
    scn.materials.back().base_color = colour(0.1, 0.2, 0.5);
    scn.materials.back().metallic = 0;
    scn.materials.back().roughness = 0.5;
    scn.materials.back().ior = 1.5;
    scn.materials.back().emission = vec3(0, 0, 0);

    scn.materials.push_back({});
    scn.materials.back().name = "Glass";
    scn.materials.back().model = scene_material_model::dielectric;
    scn.materials.back().base_color = colour(1, 1, 1);
    scn.materials.back().metallic = 0;
    scn.materials.back().roughness = 0.5;
    scn.materials.back().ior = 1.609;
    scn.materials.back().emission = vec3(0, 0, 0);

    scn.materials.push_back({});
    scn.materials.back().name = "PBR Metal";
    scn.materials.back().model = scene_material_model::pbr;
    scn.materials.back().base_color = colour(0.8, 0.6, 0.2);
    scn.materials.back().metallic = 1;
    scn.materials.back().roughness = 0.2;
    scn.materials.back().ior = 1.5;
    scn.materials.back().emission = vec3(0, 0, 0);

    scn.materials.push_back({});
    scn.materials.back().name = "Red";
    scn.materials.back().model = scene_material_model::lambert;
    scn.materials.back().base_color = colour(0.838235, 0.0698529, 0.0698529);
    scn.materials.back().metallic = 0;
    scn.materials.back().roughness = 0.5;
    scn.materials.back().ior = 1.5;
    scn.materials.back().emission = vec3(0, 0, 0);

    scn.materials.push_back({});
    scn.materials.back().name = "Green";
    scn.materials.back().model = scene_material_model::lambert;
    scn.materials.back().base_color = colour(0.0853758, 0.452122, 0.916667);
    scn.materials.back().metallic = 0;
    scn.materials.back().roughness = 0.5;
    scn.materials.back().ior = 1.5;
    scn.materials.back().emission = vec3(0, 0, 0);

    scn.materials.push_back({});
    scn.materials.back().name = "Material 6";
    scn.materials.back().model = scene_material_model::diffuse_light;
    scn.materials.back().base_color = colour(1, 1, 1);
    scn.materials.back().metallic = 0;
    scn.materials.back().roughness = 0.5;
    scn.materials.back().ior = 1.5;
    // Emission color and strength for Cube 3 (Material 6)
    scn.materials.back().emission = vec3(1, 1, 1);
    scn.materials.back().emission_intensity = 45.0;

    scn.materials.push_back({});
    scn.materials.back().name = "Material 7";
    scn.materials.back().model = scene_material_model::pbr;
    scn.materials.back().base_color = colour(0.8, 0.8, 0.8);
    scn.materials.back().metallic = 0;
    scn.materials.back().roughness = 0.5;
    scn.materials.back().ior = 1.5;
    scn.materials.back().emission = vec3(0, 0, 0);

    scn.materials.push_back({});
    scn.materials.back().name = "Material 8";
    scn.materials.back().model = scene_material_model::pbr;
    scn.materials.back().base_color = colour(0.8, 0.8, 0.8);
    scn.materials.back().metallic = 0;
    scn.materials.back().roughness = 0.5;
    scn.materials.back().ior = 1.5;
    scn.materials.back().emission = vec3(0, 0, 0);

    scn.materials.push_back({});
    scn.materials.back().name = "Material 9";
    scn.materials.back().model = scene_material_model::pbr;
    scn.materials.back().base_color = colour(0.8, 0.8, 0.8);
    scn.materials.back().metallic = 0;
    scn.materials.back().roughness = 0.5;
    scn.materials.back().ior = 1.5;
    scn.materials.back().emission = vec3(0, 0, 0);

    scn.materials.push_back({});
    scn.materials.back().name = "Material 10";
    scn.materials.back().model = scene_material_model::pbr;
    scn.materials.back().base_color = colour(0.8, 0.8, 0.8);
    scn.materials.back().metallic = 0;
    scn.materials.back().roughness = 0.5;
    scn.materials.back().ior = 1.5;
    scn.materials.back().emission = vec3(0, 0, 0);

    scn.materials.push_back({});
    scn.materials.back().name = "Material 11";
    scn.materials.back().model = scene_material_model::diffuse_light;
    scn.materials.back().base_color = colour(0.8, 0.8, 0.8);
    scn.materials.back().metallic = 0;
    scn.materials.back().roughness = 0.5;
    scn.materials.back().ior = 1.5;
    scn.materials.back().emission = vec3(4.70588, 4.70588, 4.70588);

    // --- Apply model textures to PBR materials (materials 7..10) ---
    auto find_texture_index = [&](const std::string& texname) -> int {
        for (size_t i = 0; i < scn.textures.size(); ++i) {
            if (scn.textures[i].name == texname) return (int)i;
        }
        return -1;
    };

    // Material 7 -> Legs
    if (scn.materials.size() > 7) {
        int a = find_texture_index("Legs_Base_color");
        int m = find_texture_index("Legs_Metallic");
        int r = find_texture_index("Legs_Roughness");
        int n = find_texture_index("Legs_Normal_OpenGL");
        if (a >= 0) scn.materials[7].albedo_tex = a;
        if (m >= 0) scn.materials[7].metallic_tex = m;
        if (r >= 0) scn.materials[7].roughness_tex = r;
        if (n >= 0) scn.materials[7].normal_tex = n;
    }

    // Material 8 -> Chest
    if (scn.materials.size() > 8) {
        int a = find_texture_index("Chest_Base_color");
        int m = find_texture_index("Chest_Metallic");
        int r = find_texture_index("Chest_Roughness");
        int n = find_texture_index("Chest_Normal_OpenGL");
        if (a >= 0) scn.materials[8].albedo_tex = a;
        if (m >= 0) scn.materials[8].metallic_tex = m;
        if (r >= 0) scn.materials[8].roughness_tex = r;
        if (n >= 0) scn.materials[8].normal_tex = n;
    }

    // Material 9 -> Arms
    if (scn.materials.size() > 9) {
        int a = find_texture_index("Arms_Base_color");
        int m = find_texture_index("Arms_Metallic");
        int r = find_texture_index("Arms_Roughness");
        int n = find_texture_index("Arms_Normal_OpenGL");
        if (a >= 0) scn.materials[9].albedo_tex = a;
        if (m >= 0) scn.materials[9].metallic_tex = m;
        if (r >= 0) scn.materials[9].roughness_tex = r;
        if (n >= 0) scn.materials[9].normal_tex = n;
    }

    // Material 10 -> Helmet
    if (scn.materials.size() > 10) {
        int a = find_texture_index("Helmet_Base_color");
        int m = find_texture_index("Helmet_Metallic");
        int r = find_texture_index("Helmet_Roughness");
        int n = find_texture_index("Helmet_Normal_OpenGL");
        if (a >= 0) scn.materials[10].albedo_tex = a;
        if (m >= 0) scn.materials[10].metallic_tex = m;
        if (r >= 0) scn.materials[10].roughness_tex = r;
        if (n >= 0) scn.materials[10].normal_tex = n;
    }

    {
        scene_object obj;
        obj.name = "Cube 0";
        obj.type = scene_object_type::cube;
        obj.material_index = 5;
        obj.center = point3(0, 0, -1);
        obj.radius = 0.866025;
        obj.translation = vec3(1.59406, -0.0392758, 0);
        obj.rotation_deg = vec3(0, 0, 0);
        obj.scale = vec3(0.1, 2.2, 4);
        scn.objects.push_back(obj);
    }
    {
        scene_object obj;
        obj.name = "Cube 1";
        obj.type = scene_object_type::cube;
        obj.material_index = -1;
        obj.center = point3(0, 0, -1);
        obj.radius = 0.866025;
        obj.translation = vec3(0.0535498, -0.0249851, -1.92386);
        obj.rotation_deg = vec3(0, 0, 0);
        obj.scale = vec3(3, 2.2, 0.1);
        scn.objects.push_back(obj);
    }
    {
        scene_object obj;
        obj.name = "Cube 2";
        obj.type = scene_object_type::cube;
        obj.material_index = -1;
        obj.center = point3(0, 0, -1);
        obj.radius = 0.866025;
        obj.translation = vec3(0.14756, 1.11287, -0.00132418);
        obj.rotation_deg = vec3(0, 0, 0);
        obj.scale = vec3(3, 0.1, 4);
        scn.objects.push_back(obj);
    }
    {
        scene_object obj;
        obj.name = "Cube 3";
        obj.type = scene_object_type::cube;
        obj.material_index = 6;
        obj.center = point3(0, 0, -1);
        obj.radius = 0.866025;
        obj.translation = vec3(0.148591, 1.01711, 0.457007);
        obj.rotation_deg = vec3(0, 0, 0);
        obj.scale = vec3(0.5, 0.01, 0.5);
        scn.objects.push_back(obj);
    }
    {
        scene_object obj;
        obj.name = "Cube 4";
        obj.type = scene_object_type::cube;
        obj.material_index = -1;
        obj.center = point3(0, 0, -1);
        obj.radius = 0.866025;
        obj.translation = vec3(0.145035, -1.17922, 0);
        obj.rotation_deg = vec3(0, 0, 0);
        obj.scale = vec3(3, 0.1, 4);
        scn.objects.push_back(obj);
    }
    {
        scene_object obj;
        obj.name = "Cube 5";
        obj.type = scene_object_type::cube;
        obj.material_index = 4;
        obj.center = point3(0, 0, -1);
        obj.radius = 0.866025;
        obj.translation = vec3(-1.30493, -0.033585, 0);
        obj.rotation_deg = vec3(0, 0, 0);
        obj.scale = vec3(0.1, 2.2, 4);
        scn.objects.push_back(obj);
    }
    {
        scene_object obj;
        obj.name = "Cube 6";
        obj.type = scene_object_type::cube;
        obj.material_index = -1;
        obj.center = point3(0, 0, -1);
        obj.radius = 0.866025;
        obj.translation = vec3(-0.514723, -0.674796, -0.958614);
        obj.rotation_deg = vec3(0, -24, 0);
        obj.scale = vec3(0.69, 2.25, 0.75);
        scn.objects.push_back(obj);
    }
    {
        scene_object obj;
        obj.name = "Cube 7";
        obj.type = scene_object_type::cube;
        obj.material_index = -1;
        obj.center = point3(0, 0, -1);
        obj.radius = 0.866025;
        obj.translation = vec3(0.829535, -0.875483, 0.431332);
        obj.rotation_deg = vec3(0, 36, 0);
        obj.scale = vec3(0.5, 0.5, 0.5);
        scn.objects.push_back(obj);
    }
    {
        scene_object obj;
        obj.name = "Sphere 8";
        obj.type = scene_object_type::sphere;
        obj.material_index = 2;
        obj.center = point3(0, 0, -1);
        obj.radius = 0.5;
        obj.translation = vec3(-0.492861, -0.28823, 0.51203);
        obj.rotation_deg = vec3(0, 0, 0);
        obj.scale = vec3(0.6, 0.6, 0.6);
        scn.objects.push_back(obj);
    }
    {
        scene_object obj;
        obj.name = "Sphere 9";
        obj.type = scene_object_type::sphere;
        obj.material_index = 2;
        obj.center = point3(0, 0, -1);
        obj.radius = 0.5;
        obj.translation = vec3(0.839512, -0.428069, 0.403038);
        obj.rotation_deg = vec3(0, 0, 0);
        obj.scale = vec3(0.4, 0.4, 0.4);
        scn.objects.push_back(obj);
    }
    {
        scene_object obj;
        obj.name = "Atlasted MK IV";
        obj.type = scene_object_type::mesh_instance;
        obj.material_index = -1;
        obj.center = point3(0, 0, 0);
        obj.radius = 2.00049;
        obj.translation = vec3(0.0773224, -1.13078, -1.13846);
        obj.rotation_deg = vec3(0, -103, 0);
        obj.scale = vec3(0.7, 0.7, 0.7);
        obj.mesh_slot_materials = {7, 8, 9, 10};
        scn.objects.push_back(obj);
    }


    // If AtlasedMKIV model exists, import as a mesh asset and attach to the
    // pre-created mesh_instance object named "Atlasted MK IV" (if present).
    try {
        const std::string model_rel = "models/AtlastedMKIV.fbx";
        std::string full_path = model_rel;
        if (!std::filesystem::exists(full_path)) {
            std::string alt = std::string("../") + model_rel;
            if (std::filesystem::exists(alt)) full_path = alt;
        }

        if (std::filesystem::exists(full_path)) {
            std::printf("Attempting to load AtlasedMKIV from %s\n", full_path.c_str());
            gpu_mesh loaded_gm{};
            float approx_r = 1.0f;
            std::vector<std::string> slot_names;
            if (try_load_assimp_mesh(full_path, loaded_gm, approx_r, slot_names)) {
                scene_mesh_asset asset;
                asset.name = "AtlasedMKIV";
                asset.file_path = full_path;
                asset.mesh_bvh = nullptr;
                asset.slot_names = slot_names;
                asset.slot_default_materials.assign(asset.slot_names.size(), -1);

                int mesh_index = (int)scn.meshes.size();
                scn.meshes.push_back(std::move(asset));

                if ((int)g_gpu_meshes.size() < mesh_index + 1) g_gpu_meshes.resize(mesh_index + 1);
                g_gpu_meshes[mesh_index] = loaded_gm;

                // Find the existing object by name and attach the mesh_index
                for (auto &o : scn.objects) {
                    if (o.name == "Atlasted MK IV") {
                        o.mesh_index = mesh_index;
                        // Preserve any provided per-slot bindings but ensure correct size
                        if ((int)o.mesh_slot_materials.size() != (int)scn.meshes[mesh_index].slot_names.size()) {
                            o.mesh_slot_materials.resize(scn.meshes[mesh_index].slot_names.size(), -1);
                        }
                        break;
                    }
                }

                std::printf("AtlasedMKIV loaded as mesh_index=%d (slots=%zu)\n", mesh_index, scn.meshes[mesh_index].slot_names.size());
            } else {
                std::fprintf(stderr, "AtlasedMKIV found but failed to load via Assimp: %s\n", full_path.c_str());
            }
        } else {
            std::printf("AtlasedMKIV not found at %s (skipping)\n", model_rel.c_str());
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Exception while trying to attach AtlasedMKIV: %s\n", e.what());
    } catch (...) {
        std::fprintf(stderr, "Unknown exception while trying to attach AtlasedMKIV\n");
    }


}

// -----------------------------------------------------------------------------
// Raster shader + math
// -----------------------------------------------------------------------------

static GLuint CompileShader(const char* vs, const char* fs)
{
    GLint status;
    char  log[1024];

    GLuint v = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(v, 1, &vs, nullptr);
    glCompileShader(v);
    glGetShaderiv(v, GL_COMPILE_STATUS, &status);
    if (!status) {
        glGetShaderInfoLog(v, sizeof(log), nullptr, log);
        std::fprintf(stderr, "Vertex shader error: %s\n", log);
    }

    GLuint f = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(f, 1, &fs, nullptr);
    glCompileShader(f);
    glGetShaderiv(f, GL_COMPILE_STATUS, &status);
    if (!status) {
        glGetShaderInfoLog(f, sizeof(log), nullptr, log);
        std::fprintf(stderr, "Fragment shader error: %s\n", log);
    }

    GLuint prog = glCreateProgram();
    glAttachShader(prog, v);
    glAttachShader(prog, f);
    glBindAttribLocation(prog, 0, "aPos");
    glBindAttribLocation(prog, 1, "aNormal");
    glBindAttribLocation(prog, 2, "aUV");
    glBindAttribLocation(prog, 3, "aTangent");
    glBindAttribLocation(prog, 4, "aBitangent");
    glLinkProgram(prog);
    glGetProgramiv(prog, GL_LINK_STATUS, &status);
    if (!status) {
        glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
        std::fprintf(stderr, "Program link error: %s\n", log);
    }

    glDeleteShader(v);
    glDeleteShader(f);
    return prog;
}

static void BuildModelThumbnailShader()
{
    const char* vs = R"(#version 130
        in vec3 aPos;
        in vec3 aNormal;

        uniform mat4 uModel;
        uniform mat4 uView;
        uniform mat4 uProj;

        out vec3 vNormal;

        void main() {
            mat3 normalMat = mat3(uModel);
            vNormal = normalize(normalMat * aNormal);
            gl_Position = uProj * uView * uModel * vec4(aPos, 1.0);
        }
    )";

    const char* fs = R"(#version 130
        in vec3 vNormal;

        uniform vec3 uColor;
        uniform vec3 uLightDir; // direction from light toward scene

        out vec4 FragColor;

        void main() {
            vec3 n = normalize(vNormal);
            vec3 l = normalize(-uLightDir); // light→scene

            float NdotL   = max(dot(n, l), 0.0);
            float ambient = 0.2;
            float diffuse = NdotL;

            vec3 shaded = uColor * (ambient + diffuse);
            FragColor   = vec4(shaded, 1.0);
        }
    )";

    g_modelThumbnailShader = CompileShader(vs, fs);
}

// Simple flat shader used for picking (outputs a uniform color)
static void BuildPickShader()
{
    const char* vs = R"(#version 130
        in vec3 aPos;
        uniform mat4 uModel;
        uniform mat4 uView;
        uniform mat4 uProj;
        void main() {
            gl_Position = uProj * uView * uModel * vec4(aPos, 1.0);
        }
    )";

    const char* fs = R"(#version 130
        uniform vec3 uPickColor;
        out vec4 FragColor;
        void main() {
            FragColor = vec4(uPickColor, 1.0);
        }
    )";

    g_pickShader = CompileShader(vs, fs);
}

// Line shader (vertex color) for gizmo axes
static void BuildLineShader()
{
    const char* vs = R"(#version 130
        in vec3 aPos;
        in vec3 aColor;
        out vec3 vColor;
        uniform mat4 uModel;
        uniform mat4 uView;
        uniform mat4 uProj;
        void main() {
            vColor = aColor;
            gl_Position = uProj * uView * uModel * vec4(aPos, 1.0);
        }
    )";

    const char* fs = R"(#version 130
        in vec3 vColor;
        out vec4 FragColor;
        void main() { FragColor = vec4(vColor, 1.0); }
    )";

    g_lineShader = CompileShader(vs, fs);
}

static void make_lookat(const vec3& eye, const vec3& center, const vec3& up, float out[16])
{
    vec3 f = unit_vector(center - eye);
    vec3 s = unit_vector(cross(f, up));
    vec3 u = cross(s, f);

    out[0]  = (float)s.x();  out[1]  = (float)u.x();  out[2]  = (float)-f.x(); out[3]  = 0.0f;
    out[4]  = (float)s.y();  out[5]  = (float)u.y();  out[6]  = (float)-f.y(); out[7]  = 0.0f;
    out[8]  = (float)s.z();  out[9]  = (float)u.z();  out[10] = (float)-f.z(); out[11] = 0.0f;

    out[12] = (float)-dot3(s, eye);
    out[13] = (float)-dot3(u, eye);
    out[14] = (float) dot3(f, eye);
    out[15] = 1.0f;
}

static void make_perspective(float fov_deg, float aspect, float znear, float zfar, float out[16])
{
    const float PI  = 3.14159265359f;
    float fov_rad   = fov_deg * (PI / 180.0f);
    float f         = 1.0f / std::tan(fov_rad * 0.5f);

    out[0]  = f / aspect; out[1]  = 0.0f; out[2]  = 0.0f;                                out[3]  = 0.0f;
    out[4]  = 0.0f;       out[5]  = f;    out[6]  = 0.0f;                                out[7]  = 0.0f;
    out[8]  = 0.0f;       out[9]  = 0.0f; out[10] = (zfar + znear) / (znear - zfar);     out[11] = -1.0f;
    out[12] = 0.0f;       out[13] = 0.0f; out[14] = (2.0f * zfar * znear) / (znear - zfar); out[15] = 0.0f;
}

// translate + uniform scale sphere
static void make_model_sphere(const point3& c, double radius, float out[16])
{
    float s = (float)radius;

    out[0]  = s;    out[1]  = 0.0f; out[2]  = 0.0f; out[3]  = 0.0f;
    out[4]  = 0.0f; out[5]  = s;    out[6]  = 0.0f; out[7]  = 0.0f;
    out[8]  = 0.0f; out[9]  = 0.0f; out[10] = s;    out[11] = 0.0f;
    out[12] = (float)c.x(); out[13] = (float)c.y(); out[14] = (float)c.z(); out[15] = 1.0f;
}

static void make_model_translate_only(const vec3& t, float out[16])
{
    out[0]  = 1.0f; out[1]  = 0.0f; out[2]  = 0.0f; out[3]  = 0.0f;
    out[4]  = 0.0f; out[5]  = 1.0f; out[6]  = 0.0f; out[7]  = 0.0f;
    out[8]  = 0.0f; out[9]  = 0.0f; out[10] = 1.0f; out[11] = 0.0f;
    out[12] = (float)t.x(); out[13] = (float)t.y(); out[14] = (float)t.z(); out[15] = 1.0f;
}

// make model matrix from Translation, Rotation (degrees XYZ), and non-uniform Scale
static void make_model_trs(const vec3& translate, const vec3& rotation_deg, const vec3& scale,
                           float out[16]) {
    make_trs_column_major(translate, rotation_deg, scale, out);
}

// All raster geometry retains the same UVs and tangent frames as ray tracing.
static void BindRasterVertexLayout() {
    const int counts[] = {3, 3, 2, 3, 3};
    const size_t offsets[] = {0, 3, 6, 8, 11};
    for (int i = 0; i < 5; ++i) {
        glEnableVertexAttribArray(i);
        glVertexAttribPointer(i, counts[i], GL_FLOAT, GL_FALSE, sizeof(raster_vertex),
                              reinterpret_cast<void*>(offsets[i] * sizeof(float)));
    }
}
static gpu_mesh UploadRasterMesh(const std::vector<raster_vertex>& vertices,
                                 const std::vector<unsigned>& indices) {
    gpu_mesh mesh;
    if (vertices.empty() || indices.empty()) {
        return mesh;
    }
    mesh.minimum = mesh.maximum =
        point3(vertices[0].position[0], vertices[0].position[1], vertices[0].position[2]);
    for (const auto& vertex : vertices) {
        for (int c = 0; c < 3; ++c) {
            mesh.minimum[c] = std::min(mesh.minimum[c], double(vertex.position[c]));
            mesh.maximum[c] = std::max(mesh.maximum[c], double(vertex.position[c]));
        }
    }
    glGenVertexArrays(1, &mesh.vao);
    glBindVertexArray(mesh.vao);
    glGenBuffers(1, &mesh.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo);
    glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(raster_vertex), vertices.data(), GL_STATIC_DRAW);
    glGenBuffers(1, &mesh.ebo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(unsigned), indices.data(), GL_STATIC_DRAW);
    BindRasterVertexLayout();
    glBindVertexArray(0);
    mesh.index_count = GLsizei(indices.size());
    return mesh;
}
static raster_vertex MakeRasterVertex(const vec3& p, const vec3& n, double u, double v, const vec3& tangent,
                                      const vec3& bitangent) {
    raster_vertex result{};
    for (int c = 0; c < 3; ++c) {
        result.position[c] = float(p[c]);
        result.normal[c] = float(n[c]);
        result.tangent[c] = float(tangent[c]);
        result.bitangent[c] = float(bitangent[c]);
    }
    result.uv[0] = float(u);
    result.uv[1] = float(v);
    return result;
}
static void BuildUnitSphereMesh(int segments = 64, int rings = 32) {
    std::vector<raster_vertex> vertices;
    std::vector<unsigned> indices;
    for (int y = 0; y <= rings; ++y) {
        const double v = double(y) / rings, phi = v * pi;
        for (int x = 0; x <= segments; ++x) {
            const double u = double(x) / segments, theta = u * 2 * pi;
            vec3 p(-std::cos(theta) * std::sin(phi), -std::cos(phi), std::sin(theta) * std::sin(phi));
            vec3 t(std::sin(theta), 0, std::cos(theta));
            vec3 b(-std::cos(theta) * std::cos(phi), std::sin(phi), std::sin(theta) * std::cos(phi));
            vertices.push_back(MakeRasterVertex(p, p, u, v, t, b));
        }
    }
    for (int y = 0; y < rings; ++y) {
        for (int x = 0; x < segments; ++x) {
            unsigned a = y * (segments + 1) + x, b = a + 1, c = a + segments + 1, d = c + 1;
            indices.insert(indices.end(), {a, b, c, b, d, c});
        }
    }
    auto mesh = UploadRasterMesh(vertices, indices);
    g_rasterSphereVAO = mesh.vao;
    g_rasterSphereVBO = mesh.vbo;
    g_rasterSphereEBO = mesh.ebo;
    g_rasterSphereIndexCount = mesh.index_count;
}
static void BuildUnitCubeMesh() {
    const point3 origins[] = {{-.5, -.5, .5},  {.5, -.5, .5}, {.5, -.5, -.5},
                              {-.5, -.5, -.5}, {-.5, .5, .5}, {-.5, -.5, -.5}};
    const vec3 tangents[] = {{1, 0, 0}, {0, 0, -1}, {-1, 0, 0}, {0, 0, 1}, {1, 0, 0}, {1, 0, 0}};
    const vec3 bitangents[] = {{0, 1, 0}, {0, 1, 0}, {0, 1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}};
    const double uv[][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    std::vector<raster_vertex> vertices;
    std::vector<unsigned> indices;
    for (int face = 0; face < 6; ++face) {
        const auto& t = tangents[face];
        const auto& b = bitangents[face];
        for (const auto& coord : uv) {
            vertices.push_back(MakeRasterVertex(origins[face] + coord[0] * t + coord[1] * b, cross(t, b),
                                                coord[0], coord[1], t, b));
        }
        unsigned a = face * 4;
        indices.insert(indices.end(), {a, a + 1, a + 2, a, a + 2, a + 3});
    }
    auto mesh = UploadRasterMesh(vertices, indices);
    g_rasterCubeVAO = mesh.vao;
    g_rasterCubeVBO = mesh.vbo;
    g_rasterCubeEBO = mesh.ebo;
    g_rasterCubeIndexCount = mesh.index_count;
}
struct MeshLoadResult {
    gpu_mesh mesh;
    float approx_radius = 1;
    std::vector<std::string> material_slot_names;
};
static MeshLoadResult load_assimp_mesh_as_gpu_mesh(const std::string& full_path, bool z_up,
                                                   bool normalise_unit, double user_scale) {
    MeshLoadResult result;
    try {
        auto data = build_cached_mesh_data(full_path, z_up, normalise_unit, user_scale);
        std::vector<raster_vertex> vertices;
        std::vector<unsigned> indices;
        std::vector<raster_range> ranges;
        vertices.reserve(data->triangles.size() * 3);
        indices.reserve(data->triangles.size() * 3);
        double radius_squared = 0;
        for (const auto& t : data->triangles) {
            if (ranges.empty() || ranges.back().material_slot != t.material_index) {
                ranges.push_back({unsigned(indices.size()), 0, t.material_index});
            }
            const vec3 positions[] = {t.p0, t.p1, t.p2}, normals[] = {t.n0, t.n1, t.n2};
            const vec3 tangents[] = {t.t0, t.t1, t.t2}, bitangents[] = {t.b0, t.b1, t.b2};
            const double us[] = {t.u0, t.u1, t.u2}, vs[] = {t.v0, t.v1, t.v2};
            for (int i = 0; i < 3; ++i) {
                indices.push_back(unsigned(vertices.size()));
                vertices.push_back(
                    MakeRasterVertex(positions[i], normals[i], us[i], vs[i], tangents[i], bitangents[i]));
                radius_squared = std::max(radius_squared, positions[i].length_squared());
            }
            ranges.back().count += 3;
        }
        result.mesh = UploadRasterMesh(vertices, indices);
        result.mesh.ranges = std::move(ranges);
        result.approx_radius = float(std::sqrt(radius_squared));
        result.material_slot_names = std::move(data->material_names);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Raster mesh: %s\n", error.what());
    }
    return result;
}

// try_load_assimp_mesh implementation - uses the loader defined above
static bool try_load_assimp_mesh(const std::string& full_path,
                                 gpu_mesh& out_mesh,
                                 float& out_approx_radius,
                                 std::vector<std::string>& out_slot_names)
{
    MeshLoadResult mlr = load_assimp_mesh_as_gpu_mesh(full_path,
                                                     /*z_up=*/std::filesystem::path(full_path).extension() == ".fbx" ||
                                                              std::filesystem::path(full_path).extension() == ".FBX",
                                                     /*normalise_unit=*/true,
                                                     /*user_scale=*/2.0);
    if (mlr.mesh.vao == 0 || mlr.mesh.index_count == 0)
        return false;

    out_mesh = mlr.mesh;
    out_approx_radius = mlr.approx_radius;
    out_slot_names = std::move(mlr.material_slot_names);
    return true;
}

// Persist imported textures and meshes between launches.
static void SaveAssetsManifest()
{
    std::ofstream out(kAssetsManifestFile);
    if (!out) return;
    // Textures
    for (const auto &tex : g_scene.textures) {
        out << "T|" << tex.name << "|" << tex.path << "\n";
    }
    // Meshes
    for (const auto &mesh : g_scene.meshes) {
        out << "M|" << mesh.name << "|" << mesh.file_path << "|" << mesh.approx_radius << "\n";
    }
}

static void LoadAssetsManifest()
{
    std::ifstream in(kAssetsManifestFile);
    if (!in) return;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        if (line.size() < 2 || line[1] != '|') continue;
        char type = line[0];
        std::string rest = line.substr(2);
        if (type == 'T') {
            size_t sep = rest.find('|');
            if (sep == std::string::npos) continue;
            std::string name = rest.substr(0, sep);
            std::string path = rest.substr(sep + 1);
            // Avoid duplicates
            bool found = false;
            for (const auto &t : g_scene.textures) if (t.path == path) { found = true; break; }
            if (found) continue;
            if (!std::filesystem::exists(path)) continue;
            scene_texture tex; tex.name = name; tex.path = path;
            g_scene.textures.push_back(std::move(tex));
        } else if (type == 'M') {
            // M|name|path|approx_radius
            std::stringstream ss(rest);
            std::string name, path, approx_s;
            if (!std::getline(ss, name, '|')) continue;
            if (!std::getline(ss, path, '|')) continue;
            if (!std::getline(ss, approx_s, '|')) approx_s = "1.0";
            // Avoid duplicates
            bool found = false;
            for (const auto &m : g_scene.meshes) if (m.file_path == path) { found = true; break; }
            if (found) continue;
            if (!std::filesystem::exists(path)) continue;
            // Attempt to load mesh via Assimp loader
            gpu_mesh gm{}; float approx_r = 1.0f; std::vector<std::string> slot_names;
            if (try_load_assimp_mesh(path, gm, approx_r, slot_names)) {
                scene_mesh_asset asset;
                asset.name = name;
                asset.file_path = path;
                asset.approx_radius = approx_r;
                asset.mesh_bvh = nullptr;
                asset.slot_names = slot_names;
                asset.slot_default_materials.assign(asset.slot_names.size(), -1);

                int mesh_index = (int)g_scene.meshes.size();
                g_scene.meshes.push_back(std::move(asset));
                if ((int)g_gpu_meshes.size() < mesh_index + 1) g_gpu_meshes.resize(mesh_index + 1);
                g_gpu_meshes[mesh_index] = gm;
            }
        }
    }
}

// Persist editor materials between launches.
static void SaveMaterialsManifest()
{
    const char* fname = "materials.txt";
    std::ofstream out(fname);
    if (!out) return;
    for (const auto &m : g_scene.materials) {
        // Serialize as: name|model|base_r,base_g,base_b|metallic|roughness|fuzz|ior|em_r,em_g,em_b|use_sss|sss_strength|sss_scale|sss_model|sss_samples|sss_radius|sss_eta|sss_color_override_enabled|sss_cr,sss_cg,sss_cb|emission_intensity|dielectricF0_r,dielectricF0_g,dielectricF0_b|normal_strength|albedo_tex|metallic_tex|roughness_tex|normal_tex|alpha_tex|alpha_double_sided|alpha_cutoff
        out << m.name << "|" << (int)m.model << "|";
        out << m.base_color.x() << "," << m.base_color.y() << "," << m.base_color.z() << "|";
        out << m.metallic << "|" << m.roughness << "|" << m.fuzz << "|" << m.ior << "|";
        out << m.emission.x() << "," << m.emission.y() << "," << m.emission.z() << "|";
        out << (m.use_sss ? 1 : 0) << "|";
        out << m.sss_strength << "|" << m.sss_scale << "|" << m.sss_model << "|" << m.sss_samples << "|" << m.sss_radius << "|" << m.sss_eta << "|";
        out << (m.sss_color_override_enabled ? 1 : 0) << "|";
        out << m.sss_color_override_color.x() << "," << m.sss_color_override_color.y() << "," << m.sss_color_override_color.z() << "|";
        out << m.emission_intensity << "|";
        out << m.dielectric_F0.x() << "," << m.dielectric_F0.y() << "," << m.dielectric_F0.z() << "|";
        out << m.normal_strength << "|";
        out << m.albedo_tex << "|" << m.metallic_tex << "|" << m.roughness_tex << "|" << m.normal_tex << "|" << m.alpha_tex << "|" << (m.alpha_double_sided?1:0) << "|" << m.alpha_cutoff << "|" << (m.unreal_pbr?1:0) << "|" << serialize_material_graph(m.graph) << "\n";
    }
}

// Batch persistence after editing settles; shutdown still flushes pending edits.
static void TickMaterialPersistence() {
    if (g_materials_dirty && !ImGui::IsAnyItemActive()
        && std::chrono::steady_clock::now()-g_materials_last_edit >= std::chrono::milliseconds(350)) {
        SaveMaterialsManifest(); g_materials_dirty = false;
    }
}

static void LoadMaterialsManifest()
{
    const char* fname = "materials.txt";
    std::ifstream in(fname);
    if (!in) return;
    std::vector<scene_material> loaded;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        std::vector<std::string> toks;
        std::stringstream ss(line);
        std::string tok;
        while (std::getline(ss, tok, '|')) toks.push_back(tok);
        if (toks.size() < 3) continue;
        try {
        scene_material m;
        m.name = toks[0];
        try {
            int model_i = std::stoi(toks[1]);
            m.model = (scene_material_model)model_i;
        } catch(...) { m.model = scene_material_model::pbr; }
        // base color
        try {
            std::stringstream sb(toks[2]); double r = 0.8, g = 0.8, b = 0.8; char c;
            sb >> r >> c >> g >> c >> b; m.base_color = vec3(r,g,b);
        } catch(...) {}
        size_t idx = 3;
        if (idx < toks.size()) m.metallic = std::stod(toks[idx++]);
        if (idx < toks.size()) m.roughness = std::stod(toks[idx++]);
        if (idx < toks.size()) m.fuzz = std::stod(toks[idx++]);
        if (idx < toks.size()) m.ior = std::stod(toks[idx++]);
        if (idx < toks.size()) {
            try { std::stringstream se(toks[idx++]); double er = 0, eg = 0, eb = 0; char c; se >> er >> c >> eg >> c >> eb; m.emission = vec3(er,eg,eb); } catch(...) {}
        }
        if (idx < toks.size()) m.use_sss = (std::stoi(toks[idx++]) != 0);
        if (idx < toks.size()) m.sss_strength = std::stod(toks[idx++]);
        if (idx < toks.size()) m.sss_scale = std::stod(toks[idx++]);
        if (idx < toks.size()) m.sss_model = std::stoi(toks[idx++]);
        if (idx < toks.size()) m.sss_samples = std::stoi(toks[idx++]);
        if (idx < toks.size()) m.sss_radius = std::stod(toks[idx++]);
        if (idx < toks.size()) m.sss_eta = std::stod(toks[idx++]);
        if (idx < toks.size()) m.sss_color_override_enabled = (std::stoi(toks[idx++]) != 0);
        if (idx < toks.size()) {
            try { std::stringstream sc(toks[idx++]); double cr = 1, cg = 1, cb = 1; char c; sc >> cr >> c >> cg >> c >> cb; m.sss_color_override_color = vec3(cr,cg,cb); } catch(...) {}
        }
        if (idx < toks.size()) m.emission_intensity = std::stod(toks[idx++]);
        if (idx < toks.size()) {
            try { std::stringstream sf(toks[idx++]); double fr = 0.04, fg = 0.04, fb = 0.04; char c; sf >> fr >> c >> fg >> c >> fb; m.dielectric_F0 = vec3(fr,fg,fb); } catch(...) {}
        }
        if (idx < toks.size()) m.normal_strength = std::stod(toks[idx++]);
        if (idx < toks.size()) m.albedo_tex = std::stoi(toks[idx++]);
        if (idx < toks.size()) m.metallic_tex = std::stoi(toks[idx++]);
        if (idx < toks.size()) m.roughness_tex = std::stoi(toks[idx++]);
        if (idx < toks.size()) m.normal_tex = std::stoi(toks[idx++]);
        if (idx < toks.size()) m.alpha_tex = std::stoi(toks[idx++]);
        if (idx < toks.size()) m.alpha_double_sided = (std::stoi(toks[idx++]) != 0);
        if (idx < toks.size()) m.alpha_cutoff = std::stod(toks[idx++]);
        if (idx < toks.size()) m.unreal_pbr = (std::stoi(toks[idx++]) != 0);

        if (idx < toks.size()) deserialize_material_graph(toks[idx], m.graph);
        loaded.push_back(std::move(m));
        } catch (const std::exception&) {
            std::fprintf(stderr, "Invalid material manifest; keeping the current scene materials.\n");
            return;
        }
    }
    if (!loaded.empty()) { g_scene.materials = std::move(loaded); InvalidateAllMaterialThumbnails(); }
}

// -----------------------------------------------------------------------------
// Raster FBO
// -----------------------------------------------------------------------------

static void EnsureRasterFBO(int width, int height)
{
    if (width <= 0 || height <= 0) return;

    if (g_rasterFBO == 0) {
        glGenFramebuffers(1, &g_rasterFBO);
        glGenTextures(1, &g_rasterColorTex);
        glGenRenderbuffers(1, &g_rasterDepthRBO);
    }

    if (width != g_rasterWidth || height != g_rasterHeight) {
        g_rasterWidth  = width;
        g_rasterHeight = height;

        glBindTexture(GL_TEXTURE_2D, g_rasterColorTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
                     width, height, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

        glBindRenderbuffer(GL_RENDERBUFFER, g_rasterDepthRBO);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8,
                              width, height);

        glBindFramebuffer(GL_FRAMEBUFFER, g_rasterFBO);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, g_rasterColorTex, 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                                  GL_RENDERBUFFER, g_rasterDepthRBO);

        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            std::fprintf(stderr, "Raster FBO incomplete: 0x%X\n", status);
        }

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
}

#include "editor_raster.inl"

// Render rasterised scene into g_rasterColorTex
static void RenderRasterToTexture(int width, int height) {
    if (g_rasterShader == 0) {
        return;
    }

    EnsureRasterFBO(width, height);
    if (g_rasterFBO == 0) {
        return;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, g_rasterFBO);
    glViewport(0, 0, width, height);
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_LESS);
    glClearColor(0.05f, 0.05f, 0.06f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    float view[16];
    float proj[16];

    const vec3& pos = g_editor_cam.position;
    const vec3& f = g_editor_cam.forward;
    const vec3& u = g_editor_cam.up;

    make_lookat(pos, pos + f, u, view);
    make_perspective(g_editor_cam.vfov, (float)width / (float)height, 0.01f, 500.0f, proj);

    RenderRasterScene(view, proj, pos, width, height);

    // Draw gizmo (render into raster FBO so it appears in the preview)
    if (g_show_gizmo && g_selected_object >= 0 && g_selected_object < (int)g_scene.objects.size() &&
        g_lineShader != 0) {
        const auto& obj = g_scene.objects[g_selected_object];
        float model[16];
        vec3 trans = obj.translation + vec3(obj.center.x(), obj.center.y(), obj.center.z());
        // Build gizmo model without object rotation so axes remain world-aligned
        vec3 rot = vec3(0, 0, 0);
        // gizmo scale based on object's radius (mesh/cube) for reasonable size
        float gizmo_scale = 0.5f * (float)std::max(0.5, obj.radius);
        vec3 scl = vec3(gizmo_scale, gizmo_scale, gizmo_scale);
        make_model_trs(trans, rot, scl, model);

        // Save depth state
        GLboolean wasDepthTest = glIsEnabled(GL_DEPTH_TEST);
        GLint prevDepthFunc = 0;
        glGetIntegerv(GL_DEPTH_FUNC, &prevDepthFunc);
        // Draw on top of scene
        glDisable(GL_DEPTH_TEST);

        glUseProgram(g_lineShader);
        GLint locModelL = glGetUniformLocation(g_lineShader, "uModel");
        GLint locViewL = glGetUniformLocation(g_lineShader, "uView");
        GLint locProjL = glGetUniformLocation(g_lineShader, "uProj");
        glUniformMatrix4fv(locViewL, 1, GL_FALSE, view);
        glUniformMatrix4fv(locProjL, 1, GL_FALSE, proj);
        glUniformMatrix4fv(locModelL, 1, GL_FALSE, model);

        // draw axis lines (slightly thicker)
        glLineWidth(2.0f);
        glBindVertexArray(g_gizmoVAO);
        glDrawArrays(GL_LINES, 0, 6);
        glBindVertexArray(0);
        glLineWidth(1.0f);

        // draw cone arrowheads (triangles) using same shader (vertex color baked)
        glBindVertexArray(g_gizmoConeVAO);
        if (g_gizmoConeVertexCount > 0) {
            glDrawArrays(GL_TRIANGLES, 0, g_gizmoConeVertexCount);
        }
        glBindVertexArray(0);

        glUseProgram(0);

        // Restore depth state
        if (wasDepthTest) {
            glEnable(GL_DEPTH_TEST);
        } else {
            glDisable(GL_DEPTH_TEST);
        }
        glDepthFunc(prevDepthFunc);
    }

    glBindVertexArray(0);
    glUseProgram(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

// Ensure pick FBO (RGB8 id buffer + depth)
static void EnsurePickFBO(int width, int height)
{
    if (width <= 0 || height <= 0) return;

    if (g_pickFBO == 0) {
        glGenFramebuffers(1, &g_pickFBO);
        glGenTextures(1, &g_pickColorTex);
        glGenRenderbuffers(1, &g_pickDepthRBO);
    }

    // resize
    glBindTexture(GL_TEXTURE_2D, g_pickColorTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8,
                 width, height, 0,
                 GL_RGB, GL_UNSIGNED_BYTE, nullptr);

    glBindRenderbuffer(GL_RENDERBUFFER, g_pickDepthRBO);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);

    glBindFramebuffer(GL_FRAMEBUFFER, g_pickFBO);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, g_pickColorTex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                              GL_RENDERBUFFER, g_pickDepthRBO);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        std::fprintf(stderr, "Pick FBO incomplete: 0x%X\n", status);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

// Render pick-pass and read clicked pixel; returns object index or -1
static int PerformPick(int width, int height, int click_x, int click_y)
{
    if (g_pickShader == 0) return -1;

    EnsurePickFBO(width, height);

    glBindFramebuffer(GL_FRAMEBUFFER, g_pickFBO);
    glViewport(0, 0, width, height);
    glEnable(GL_DEPTH_TEST);
    glClearColor(0,0,0,0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glUseProgram(g_pickShader);
    GLint locModel    = glGetUniformLocation(g_pickShader, "uModel");
    GLint locView     = glGetUniformLocation(g_pickShader, "uView");
    GLint locProj     = glGetUniformLocation(g_pickShader, "uProj");
    GLint locColor    = glGetUniformLocation(g_pickShader, "uPickColor");

    float view[16];
    float proj[16];
    const vec3& pos = g_editor_cam.position;
    const vec3& f   = g_editor_cam.forward;
    const vec3& u   = g_editor_cam.up;
    make_lookat(pos, pos + f, u, view);
    make_perspective(g_editor_cam.vfov, (float)width/(float)height, 0.01f, 500.0f, proj);

    glUniformMatrix4fv(locView, 1, GL_FALSE, view);
    glUniformMatrix4fv(locProj, 1, GL_FALSE, proj);

    int idx = 0;
    for (const auto& obj : g_scene.objects) {
        int id = idx + 1; // reserve 0 = no object
        unsigned char r = (id >> 16) & 0xFF;
        unsigned char g = (id >> 8) & 0xFF;
        unsigned char b = id & 0xFF;
        float fr = r / 255.0f, fg = g / 255.0f, fb = b / 255.0f;

        float model[16];
        if (obj.type == scene_object_type::sphere) {
            vec3 trans = obj.translation + vec3(obj.center.x(), obj.center.y(), obj.center.z());
            vec3 rot   = obj.rotation_deg;
            vec3 scl   = obj.scale * vec3(obj.radius, obj.radius, obj.radius);
            make_model_trs(trans, rot, scl, model);

            glUniformMatrix4fv(locModel, 1, GL_FALSE, model);
            glUniform3f(locColor, fr, fg, fb);

            glBindVertexArray(g_rasterSphereVAO);
            glDrawElements(GL_TRIANGLES, g_rasterSphereIndexCount, GL_UNSIGNED_INT, (void*)0);
        }
        else if (obj.type == scene_object_type::cube) {
            vec3 trans = obj.translation + vec3(obj.center.x(), obj.center.y(), obj.center.z());
            vec3 rot   = obj.rotation_deg;
            vec3 scl   = obj.scale;
            make_model_trs(trans, rot, scl, model);

            glUniformMatrix4fv(locModel, 1, GL_FALSE, model);
            glUniform3f(locColor, fr, fg, fb);

            glBindVertexArray(g_rasterCubeVAO);
            glDrawElements(GL_TRIANGLES, g_rasterCubeIndexCount, GL_UNSIGNED_INT, (void*)0);
        }
        else if (obj.type == scene_object_type::mesh_instance) {
            if (obj.mesh_index >= 0 && obj.mesh_index < (int)g_gpu_meshes.size()) {
                const gpu_mesh& gm = g_gpu_meshes[obj.mesh_index];
                if (gm.vao && gm.index_count) {
                    vec3 trans = obj.translation + vec3(obj.center.x(), obj.center.y(), obj.center.z());
                    vec3 rot   = obj.rotation_deg;
                    vec3 scl   = obj.scale;
                    make_model_trs(trans, rot, scl, model);

                    glUniformMatrix4fv(locModel, 1, GL_FALSE, model);
                    glUniform3f(locColor, fr, fg, fb);

                    glBindVertexArray(gm.vao);
                    glDrawElements(GL_TRIANGLES, gm.index_count, GL_UNSIGNED_INT, (void*)0);
                }
            }
        }

        ++idx;
    }

    // Read pixel (OpenGL origin is bottom-left)
    unsigned char px[3] = {0,0,0};
    int read_x = click_x;
    int read_y = height - 1 - click_y;
    glFlush(); glFinish();
    glReadPixels(read_x, read_y, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, px);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    int picked_id = (px[0] << 16) | (px[1] << 8) | (px[2]);
    if (picked_id == 0) return -1;
    return picked_id - 1;
}

// Convert screen (texture-local) coords to a world-space ray using editor camera
static ray ScreenPointToRay(const editor_camera_state& cam, int tex_w, int tex_h, float sx, float sy)
{
    // sx,sy are in texture-local pixels (origin top-left)
    const double PI = 3.14159265358979323846;
    double u = (sx + 0.5) / (double)tex_w; // [0,1]
    double v = (sy + 0.5) / (double)tex_h; // [0,1]

    // NDC-like coords with y flipped so +y is up
    double ndc_x = (u - 0.5) * 2.0; // -1..1
    double ndc_y = (0.5 - v) * 2.0; // -1..1 (flip)

    double fov_rad = cam.vfov * (PI / 180.0);
    double tan_fov = std::tan(fov_rad * 0.5);
    double aspect = (double)tex_w / (double)tex_h;

    double px = ndc_x * aspect * tan_fov;
    double py = ndc_y * tan_fov;

    vec3 dir = unit_vector(cam.forward + cam.right * (float)px + cam.up * (float)py);
    return ray(cam.position, dir, 0.0);
}

// Closest points between two infinite lines (p1 + s*d1, p2 + t*d2)
// Returns true if solved; out_s and out_t are parameters along the lines.
static bool ClosestPointsBetweenLines(const vec3& p1, const vec3& d1, const vec3& p2, const vec3& d2,
                                      double& out_s, double& out_t)
{
    const double EPS = 1e-9;
    double a = dot(d1, d1);
    double b = dot(d1, d2);
    double c = dot(d2, d2);
    vec3 r = p1 - p2;
    double d = dot(d1, r);
    double e = dot(d2, r);

    double denom = a * c - b * b;
    if (std::fabs(denom) < EPS) return false; // parallel or nearly

    out_s = (b * e - c * d) / denom;
    out_t = (a * e - b * d) / denom;
    return true;
}

// -----------------------------------------------------------------------------
// Engine init
// -----------------------------------------------------------------------------

static void init_engine_once()
{
    if (g_scene_initialized)
        return;

    build_default_scene(g_scene);
    LoadMaterialsManifest();
    // Load persisted mesh default material assignments (if present)
    LoadMeshMaterialDefaults();
    // Load persisted imported assets (textures / meshes)
    LoadAssetsManifest();

    // Editor camera pose
    g_editor_cam.vfov = 40.0f;
    g_editor_cam.set_from_lookat(point3(0.246, 0.560, 3.08),
                                 point3(0, 0, -1));

    // RT camera
    g_camera.aspect_ratio      = 16.0 / 9.0;
    g_camera.image_width       = 1920;
    g_camera.image_height      = 1080;
    g_camera.samples_per_pixel = 4;  // Low default for interactive preview
    g_camera.max_depth         = 8;   // Reduced from 20 for faster renders
    g_camera.background        = colour(0.0, 0.0, 0.0);
    to_shirley_camera(g_editor_cam, g_camera);

    // Keep approximate caustics opt-in; the default path uses BSDF/NEE transport.
    g_camera.enable_mnee = false;
    g_camera.enable_mis = true;
    g_camera.direct_light_samples = 1;
    UpdateMNEEFromScene();

    // Add a default sun to the scene and mirror into the preview camera (nice default lighting)
    {
        scene_light sun;
        sun.name = "Sun";
        sun.type = scene_light_type::directional;
        sun.radiance = vec3(6.0, 6.0, 6.0);
        sun.direction = unit_vector(vec3(-0.3f, -1.0f, 0.2f));
        sun.angular_radius_deg = 0.53; // approximate sun
        g_scene.lights.push_back(sun);

        // Mirror into camera preview defaults.
        g_camera.use_sun = true;
        g_camera.sun_dir = -sun.direction; // camera expects scene->sun
        g_camera.sun_radiance = colour(sun.radiance.x(), sun.radiance.y(), sun.radiance.z());
        g_camera.sun_angular_radius = sun.angular_radius_deg;
        g_camera.sun_shadow_samples = 16;
    }

    BuildUnitSphereMesh();
    BuildUnitCubeMesh();
    BuildRasterShader();
    BuildPickShader();
    BuildLineShader();

    // Create gizmo VAO/VBO (6 verts: 3 axes, each a line of 2 verts)
    // Vertex format: pos.xyz, color.xyz
    float gizmo_verts[] = {
        // X axis (red)
         0.0f, 0.0f, 0.0f,   1.0f, 0.0f, 0.0f,
         1.0f, 0.0f, 0.0f,   1.0f, 0.0f, 0.0f,
        // Y axis (green)
         0.0f, 0.0f, 0.0f,   0.0f, 1.0f, 0.0f,
         0.0f, 1.0f, 0.0f,   0.0f, 1.0f, 0.0f,
        // Z axis (blue)
         0.0f, 0.0f, 0.0f,   0.0f, 0.0f, 1.0f,
         0.0f, 0.0f, 1.0f,   0.0f, 0.0f, 1.0f
    };

    // Build smoother cone arrowheads (procedural) for each axis
    std::vector<float> gizmo_cones;
    // use global constants so hit-test can reuse identical geometry
    const int cone_segments = g_gizmoConeSegments;
    const float cone_len    = (float)g_gizmoConeLen; // along axis from base to apex
    const float base_rad    = (float)g_gizmoBaseRad; // radius of cone base

    // Also store triangle positions (model-space) into g_gizmoConeTriangles
    g_gizmoConeTriangles.clear();
    auto push_vertex = [&](float px, float py, float pz, float r, float g, float b) {
        gizmo_cones.push_back(px); gizmo_cones.push_back(py); gizmo_cones.push_back(pz);
        gizmo_cones.push_back(r);  gizmo_cones.push_back(g);  gizmo_cones.push_back(b);
        g_gizmoConeTriangles.emplace_back((double)px, (double)py, (double)pz);
    };

    // X axis (red) - base circle centered at x=1.0, apex at x=1.0 + cone_len
    for (int s = 0; s < cone_segments; ++s) {
        float t0 = (float)s / (float)cone_segments;
        float t1 = (float)(s+1) / (float)cone_segments;
        float a0 = t0 * 2.0f * 3.14159265f;
        float a1 = t1 * 2.0f * 3.14159265f;
        // apex
        float ax = 1.0f + cone_len; float ay = 0.0f; float az = 0.0f;
        // base points in YZ plane
        float b0x = 1.0f, b0y = std::cos(a0) * base_rad, b0z = std::sin(a0) * base_rad;
        float b1x = 1.0f, b1y = std::cos(a1) * base_rad, b1z = std::sin(a1) * base_rad;
        push_vertex(ax, ay, az, 1.0f, 0.0f, 0.0f);
        push_vertex(b0x, b0y, b0z, 1.0f, 0.0f, 0.0f);
        push_vertex(b1x, b1y, b1z, 1.0f, 0.0f, 0.0f);
    }

    // Y axis (green) - base circle centered at y=1.0, apex at y=1.0 + cone_len
    for (int s = 0; s < cone_segments; ++s) {
        float t0 = (float)s / (float)cone_segments;
        float t1 = (float)(s+1) / (float)cone_segments;
        float a0 = t0 * 2.0f * 3.14159265f;
        float a1 = t1 * 2.0f * 3.14159265f;
        float ax = 0.0f, ay = 1.0f + cone_len, az = 0.0f;
        // base points in XZ plane
        float b0x = std::cos(a0) * base_rad, b0y = 1.0f, b0z = std::sin(a0) * base_rad;
        float b1x = std::cos(a1) * base_rad, b1y = 1.0f, b1z = std::sin(a1) * base_rad;
        push_vertex(ax, ay, az, 0.0f, 1.0f, 0.0f);
        push_vertex(b0x, b0y, b0z, 0.0f, 1.0f, 0.0f);
        push_vertex(b1x, b1y, b1z, 0.0f, 1.0f, 0.0f);
    }

    // Z axis (blue) - base circle centered at z=1.0, apex at z=1.0 + cone_len
    for (int s = 0; s < cone_segments; ++s) {
        float t0 = (float)s / (float)cone_segments;
        float t1 = (float)(s+1) / (float)cone_segments;
        float a0 = t0 * 2.0f * 3.14159265f;
        float a1 = t1 * 2.0f * 3.14159265f;
        float ax = 0.0f, ay = 0.0f, az = 1.0f + cone_len;
        // base points in XY plane
        float b0x = std::cos(a0) * base_rad, b0y = std::sin(a0) * base_rad, b0z = 1.0f;
        float b1x = std::cos(a1) * base_rad, b1y = std::sin(a1) * base_rad, b1z = 1.0f;
        push_vertex(ax, ay, az, 0.0f, 0.0f, 1.0f);
        push_vertex(b0x, b0y, b0z, 0.0f, 0.0f, 1.0f);
        push_vertex(b1x, b1y, b1z, 0.0f, 0.0f, 1.0f);
    }

    glGenVertexArrays(1, &g_gizmoVAO);
    glGenBuffers(1, &g_gizmoVBO);
    glBindVertexArray(g_gizmoVAO);
    glBindBuffer(GL_ARRAY_BUFFER, g_gizmoVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(gizmo_verts), gizmo_verts, GL_STATIC_DRAW);
    // pos
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
    // color
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
    glBindVertexArray(0);

    // Cone geometry
    glGenVertexArrays(1, &g_gizmoConeVAO);
    glGenBuffers(1, &g_gizmoConeVBO);
    glBindVertexArray(g_gizmoConeVAO);
    glBindBuffer(GL_ARRAY_BUFFER, g_gizmoConeVBO);
    g_gizmoConeVertexCount = 0;
    if (!gizmo_cones.empty()) {
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(gizmo_cones.size() * sizeof(float)), gizmo_cones.data(), GL_STATIC_DRAW);
        g_gizmoConeVertexCount = (int)gizmo_cones.size() / 6; // each vertex = pos(3) + color(3)
    } else {
        glBufferData(GL_ARRAY_BUFFER, 0, nullptr, GL_STATIC_DRAW);
    }
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
    glBindVertexArray(0);

    g_scene_initialized = true;
    g_world_dirty       = true;
    g_cached_world.reset();
}

// Sync editor camera into RT camera
static void sync_camera_from_editor(float viewport_width, float viewport_height)
{
    if (viewport_width > 0.0f && viewport_height > 0.0f) {
        g_camera.aspect_ratio = viewport_width / viewport_height;
    }
    to_shirley_camera(g_editor_cam, g_camera);

    // Mirror scene lights into the RT camera for preview rendering.
    g_camera.point_lights.clear();
    g_camera.use_sun = false;
    for (const auto& L : g_scene.lights) {
        if (L.type == scene_light_type::directional) {
            g_camera.use_sun = true;
            // In UI 'Add Sun' we set sun_dir = -sl.direction, so mirror that here.
            g_camera.sun_dir = -L.direction;
            g_camera.sun_radiance = colour((float)L.radiance.x(), (float)L.radiance.y(), (float)L.radiance.z());
            g_camera.sun_angular_radius = L.angular_radius_deg;
        } else {
            camera::point_light pl;
            pl.position = L.position;
            pl.radiance = colour((float)L.radiance.x(), (float)L.radiance.y(), (float)L.radiance.z());
            pl.range = L.range;
            g_camera.point_lights.push_back(pl);
            }
    }
}

struct viewport_image_layout {
    ImVec2 offset = ImVec2(0.0f, 0.0f);
    ImVec2 size   = ImVec2(0.0f, 0.0f);
};

static viewport_image_layout ComputeViewportImageLayout(
    const ImVec2& available_region,
    int           image_width,
    int           image_height)
{
    viewport_image_layout layout;
    if (available_region.x <= 0.0f || available_region.y <= 0.0f ||
        image_width <= 0 || image_height <= 0) {
        return layout;
    }

    layout.size = available_region;

    const float image_aspect = (float)image_width / (float)image_height;
    const float region_aspect = available_region.x / available_region.y;

    if (region_aspect > image_aspect) {
        layout.size.x = available_region.y * image_aspect;
    } else {
        layout.size.y = available_region.x / image_aspect;
    }

    layout.offset.x = 0.5f * (available_region.x - layout.size.x);
    layout.offset.y = 0.5f * (available_region.y - layout.size.y);
    return layout;
}

// Convert render_result to RGBA8 texture
static void UploadRenderToTexture(const render_result& img)
{
    const int width  = img.width;
    const int height = img.height;

    const std::vector<std::uint8_t>& src = img.pixels;

    if (width <= 0 || height <= 0 || src.empty())
        return;

    g_rtWidth  = width;
    g_rtHeight = height;
    g_rtPixels.resize(width * height * 4);

    for (int i = 0; i < width * height; ++i) {
        int src_idx = 3 * i;
        int dst_idx = 4 * i;

        g_rtPixels[dst_idx + 0] = src[src_idx + 0];
        g_rtPixels[dst_idx + 1] = src[src_idx + 1];
        g_rtPixels[dst_idx + 2] = src[src_idx + 2];
        g_rtPixels[dst_idx + 3] = 255;
    }

    if (g_rtTexture == 0) {
        glGenTextures(1, &g_rtTexture);
        glBindTexture(GL_TEXTURE_2D, g_rtTexture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, g_rtPixels.data());
    } else {
        glBindTexture(GL_TEXTURE_2D, g_rtTexture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, g_rtPixels.data());
    }

    g_rtHasImage = true;
}

// -----------------------------------------------------------------------------
// Input
// -----------------------------------------------------------------------------

static void update_editor_camera_from_input(GLFWwindow* window, double dt)
{
    if (!g_viewport_focused && !g_viewport_hovered)
        return;

    float move_forward = 0.0f;
    float move_right   = 0.0f;
    float move_up      = 0.0f;

    if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) move_forward += 1.0f;
    if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) move_forward -= 1.0f;
    if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) move_right   += 1.0f;
    if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) move_right   -= 1.0f;
    if (glfwGetKey(window, GLFW_KEY_E) == GLFW_PRESS) move_up      += 1.0f;
    if (glfwGetKey(window, GLFW_KEY_Q) == GLFW_PRESS) move_up      -= 1.0f;

    if (move_forward != 0.0f || move_right != 0.0f || move_up != 0.0f) {
        g_editor_cam.move_speed = (double)g_camera_move_speed;
        g_editor_cam.move_from_input(
            move_forward, move_right, move_up, dt
        );
        // camera movement does NOT dirty the world
    }

    static bool   rotating = false;
    static double last_x   = 0.0;
    static double last_y   = 0.0;

    if (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS)
    {
        double x, y;
        glfwGetCursorPos(window, &x, &y);

        if (!rotating) {
            rotating = true;
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
            last_x = x;
            last_y = y;
            return;
        }

        double dx = x - last_x;
        double dy = y - last_y;
        last_x = x;
        last_y = y;

        double yaw_delta   =  dx * g_camera_look_sens;
        double pitch_delta = -dy * g_camera_look_sens;

        g_editor_cam.look(yaw_delta, pitch_delta);
    }
    else
    {
        if (rotating) {
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        }
        rotating = false;
    }
}

// Start a separate GLFW window thread that displays progressive render updates.
static void start_progress_window_thread()
{
    if (g_progress_window_running.load()) return;
    g_progress_window_running.store(true);
    g_progress_window_thread = std::thread([]() {
        // Create a simple GLFW window for progress display
        int win_w = std::max(640, g_rtWidth);
        int win_h = std::max(480, g_rtHeight);
        GLFWwindow* pw = glfwCreateWindow(win_w, win_h, "Render Progress", NULL, NULL);
        if (!pw) {
            g_progress_window_running.store(false);
            return;
        }

        // Try to set the same app icon for the progress window
        SetWindowIconAuto(pw, "dusktracer.png");

        // Make its context current on this thread
        glfwMakeContextCurrent(pw);
        // Initialize GLEW for this context
        glewExperimental = GL_TRUE;
        if (glewInit() != GLEW_OK) {
            glfwDestroyWindow(pw);
            g_progress_window_running.store(false);
            return;
        }

        // Create GL objects: texture + quad
        GLuint tex = 0;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        // Simple textured quad shader (GLSL 130)
        const char* vs_src = R"GLSL(#version 130
        in vec2 aPos;
        in vec2 aUV;
        out vec2 vUV;
        void main() {
            vUV = aUV;
            gl_Position = vec4(aPos, 0.0, 1.0);
        }
        )GLSL";

        const char* fs_src = R"GLSL(#version 130
        uniform sampler2D uTex;
        in vec2 vUV;
        out vec4 FragColor;
        void main() {
            FragColor = texture(uTex, vUV);
        }
        )GLSL";

        auto compile_shader = [](GLenum type, const char* src) -> GLuint {
            GLuint s = glCreateShader(type);
            glShaderSource(s, 1, &src, nullptr);
            glCompileShader(s);
            GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
            if (!ok) {
                char buf[1024]; glGetShaderInfoLog(s, 1024, nullptr, buf);
                fprintf(stderr, "Shader compile error: %s\n", buf);
                glDeleteShader(s);
                return 0;
            }
            return s;
        };

        GLuint vs = compile_shader(GL_VERTEX_SHADER, vs_src);
        GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fs_src);
        GLuint prog = 0;
        if (vs && fs) {
            prog = glCreateProgram();
            glAttachShader(prog, vs);
            glAttachShader(prog, fs);
            glBindAttribLocation(prog, 0, "aPos");
            glBindAttribLocation(prog, 1, "aUV");
            glLinkProgram(prog);
            GLint ok = 0; glGetProgramiv(prog, GL_LINK_STATUS, &ok);
            if (!ok) { char buf[1024]; glGetProgramInfoLog(prog, 1024, nullptr, buf); fprintf(stderr, "Prog link err: %s\n", buf); glDeleteProgram(prog); prog = 0; }
        }
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);

        float quad_verts[] = {
            // pos.xy   uv.xy (flipped V so texture appears upright)
            -1.0f, -1.0f, 0.0f, 1.0f,
             1.0f, -1.0f, 1.0f, 1.0f,
             1.0f,  1.0f, 1.0f, 0.0f,
            -1.0f,  1.0f, 0.0f, 0.0f
        };
        unsigned int quad_idx[] = {0,1,2, 0,2,3};
        GLuint quadVBO=0, quadVAO=0, quadEBO=0;
        glGenVertexArrays(1, &quadVAO);
        glGenBuffers(1, &quadVBO);
        glGenBuffers(1, &quadEBO);
        glBindVertexArray(quadVAO);
        glBindBuffer(GL_ARRAY_BUFFER, quadVBO);
        glBufferData(GL_ARRAY_BUFFER, sizeof(quad_verts), quad_verts, GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, quadEBO);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(quad_idx), quad_idx, GL_STATIC_DRAW);
        glEnableVertexAttribArray(0); glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(1); glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
        glBindVertexArray(0);

        // Main loop for progress window
        while (g_progress_window_running.load() && !glfwWindowShouldClose(pw)) {
            glfwPollEvents();

            // copy latest render pixels under mutex
            int copy_w = 0, copy_h = 0;
            std::vector<std::uint8_t> copy_pixels;
            {
                std::lock_guard<std::mutex> lock(g_render_mutex);
                if (!g_render_result.pixels.empty()) {
                    copy_w = g_render_result.width;
                    copy_h = g_render_result.height;
                    // convert rgb->rgba for upload
                    copy_pixels.resize(copy_w * copy_h * 4);
                    for (int i = 0; i < copy_w * copy_h; ++i) {
                        int si = 3*i;
                        int di = 4*i;
                        copy_pixels[di+0] = g_render_result.pixels[si+0];
                        copy_pixels[di+1] = g_render_result.pixels[si+1];
                        copy_pixels[di+2] = g_render_result.pixels[si+2];
                        copy_pixels[di+3] = 255;
                    }
                }
            }

            if (!copy_pixels.empty()) {
                int fb_w, fb_h; glfwGetFramebufferSize(pw, &fb_w, &fb_h);
                glViewport(0,0,fb_w,fb_h);
                glClearColor(0.05f,0.05f,0.06f,1.0f);
                glClear(GL_COLOR_BUFFER_BIT);

                glBindTexture(GL_TEXTURE_2D, tex);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, copy_w, copy_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, copy_pixels.data());

                if (prog) glUseProgram(prog);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, tex);
                if (prog) glUniform1i(glGetUniformLocation(prog, "uTex"), 0);
                glBindVertexArray(quadVAO);
                glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
                glBindVertexArray(0);

                glfwSwapBuffers(pw);
            } else {
                // no image yet – still poll and sleep a bit
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }

        // cleanup
        glDeleteTextures(1, &tex);
        if (prog) glDeleteProgram(prog);
        if (quadVBO) glDeleteBuffers(1, &quadVBO);
        if (quadEBO) glDeleteBuffers(1, &quadEBO);
        if (quadVAO) glDeleteVertexArrays(1, &quadVAO);
        glfwDestroyWindow(pw);
        g_progress_window_running.store(false);
    });
}

static void stop_progress_window_thread()
{
    if (!g_progress_window_running.load()) return;
    g_progress_window_running.store(false);
    if (g_progress_window_thread.joinable()) {
        g_progress_window_thread.join();
    }
}

// -----------------------------------------------------------------------------
// Material inspector helper
// -----------------------------------------------------------------------------

static void DrawMaterialInspector(scene_material& mat, scene& scn, int mat_index, bool include_texture_bindings = true)
{
    // Snapshot original material so we can detect changes and invalidate thumbnails
    scene_material_parameters orig = mat;
    // editable material name buffer (persist across frames keyed by material index)
    static std::unordered_map<int, std::string> s_mat_name_bufs;
    auto& name_buf = s_mat_name_bufs[mat_index];
    if (name_buf.empty()) {
        name_buf = mat.name;
        name_buf.resize(256, '\0');
    } else {
        std::string current_name = name_buf.c_str();
        if (current_name != mat.name && !ImGui::IsAnyItemActive()) {
            name_buf = mat.name;
            name_buf.resize(256, '\0');
        } else if (name_buf.size() < 256) {
            name_buf.resize(256, '\0');
        }
    }

    // Name input: commit on Enter
    // use unique ImGui ID suffix so multiple "Name" widgets don't conflict
    std::string mat_label = std::string("Name##mat_") + std::to_string(mat_index);
    if (ImGui::InputText(mat_label.c_str(), &name_buf[0], name_buf.size(), ImGuiInputTextFlags_EnterReturnsTrue)) {
        std::string entered_name = name_buf.c_str();
        if (mat.name != entered_name) {
            std::string before = mat.name;
            std::string after  = entered_name;
            UndoManager::Instance().push(std::make_unique<LambdaAction>(
                [mat_index, before]() {
                    if (mat_index >= 0 && mat_index < (int)g_scene.materials.size())
                        g_scene.materials[mat_index].name = before;
                },
                [mat_index, after]() {
                    if (mat_index >= 0 && mat_index < (int)g_scene.materials.size())
                        g_scene.materials[mat_index].name = after;
                },
                "Rename Material"
            ));
            mat.name = entered_name;
        }
        name_buf = mat.name;
        name_buf.resize(256, '\0');
    }

    DrawSectionLabel("Material Class");

    const char* model_label = "Unknown";
    switch (mat.model) {
        case scene_material_model::lambert:       model_label = "Lambert";       break;
        case scene_material_model::metal:         model_label = "Metal";         break;
        case scene_material_model::dielectric:    model_label = "Dielectric";    break;
        case scene_material_model::diffuse_light: model_label = "Diffuse Light"; break;
        case scene_material_model::isotropic:     model_label = "Isotropic";     break;
        case scene_material_model::pbr:           model_label = "PBR";           break;
        default:                                  model_label = "Unknown";       break;
    }

    if (ImGui::BeginCombo("Class", model_label)) {
        struct Option { const char* label; scene_material_model model; };
        Option opts[] = {
            { "Lambert",        scene_material_model::lambert },
            { "Metal",          scene_material_model::metal },
            { "Dielectric",     scene_material_model::dielectric },
            { "Diffuse Light",  scene_material_model::diffuse_light },
            { "Isotropic",      scene_material_model::isotropic },
            { "PBR GGX",        scene_material_model::pbr },
        };

        for (const auto& opt : opts) {
            bool selected = (mat.model == opt.model);
            if (ImGui::Selectable(opt.label, selected)) {
                mat.model = opt.model;
                g_world_dirty = true;
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }

    DrawSectionLabel("Material Parameters");
    if (!mat.graph.nodes.empty()) ImGui::TextWrapped("Connected graph inputs override the values below. Unconnected inputs use these defaults.");

    {
        float base[3] = {
            (float)mat.base_color.x(),
            (float)mat.base_color.y(),
            (float)mat.base_color.z()
        };
        // Use HDR and Float flags for linear color space (matches renderer's expectations)
        if (ImGui::ColorEdit3("Base Color", base, ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_Float)) {
            mat.base_color = colour(base[0], base[1], base[2]);
            g_world_dirty  = true;
        }
    }

    switch (mat.model) {
    case scene_material_model::lambert:
        ImGui::TextDisabled("Lambert: pure diffuse.");
        break;

    case scene_material_model::metal:
        ImGui::TextDisabled("Metal: Shirley metal (fuzz not exposed yet).");
        break;

    case scene_material_model::dielectric:
    {
        ImGui::TextDisabled("Dielectric: glass-like.");
        float ior_f = (float)mat.ior;
        if (ImGui::SliderFloat("IOR", &ior_f, 1.0f, 2.5f)) {
            mat.ior = ior_f;
            g_world_dirty = true;
        }
    } break;

    case scene_material_model::diffuse_light:
    {
        // Separate colour and intensity: colour selects hue, slider controls scalar intensity
        float ecol[3] = {
            (float)mat.emission.x(),
            (float)mat.emission.y(),
            (float)mat.emission.z()
        };
        float eint = (float)mat.emission_intensity;

        if (ImGui::ColorEdit3("Emission Color", ecol)) {
            mat.emission = vec3(ecol[0], ecol[1], ecol[2]);
            g_world_dirty = true;
        }

        // Allow typing the intensity value directly
        if (ImGui::InputFloat("Emission Intensity", &eint, 0.1f, 1.0f, "%.3f")) {
            if (eint < 0.0f) eint = 0.0f;
            mat.emission_intensity = (double)eint;
            g_world_dirty = true;
        }

        ImGui::TextDisabled("Colour selects hue; intensity scales brightness.");
    } break;

    case scene_material_model::isotropic:
        ImGui::TextDisabled("Isotropic (volume).");
        break;

    case scene_material_model::pbr:
    {
        ImGui::TextDisabled("PBR / GGX surface");

        float metallic_f  = (float)mat.metallic;
        float roughness_f = (float)mat.roughness;
        float norm_str_f  = (float)mat.normal_strength;

        if (ImGui::SliderFloat("Metallic", &metallic_f, 0.0f, 1.0f)) {
            mat.metallic = metallic_f;
            g_world_dirty = true;
        }
        if (ImGui::SliderFloat("Roughness", &roughness_f, 0.02f, 1.0f)) {
            mat.roughness = roughness_f;
            g_world_dirty = true;
        }
        if (ImGui::SliderFloat("Normal Strength", &norm_str_f, 0.0f, 4.0f)) {
            mat.normal_strength = norm_str_f;
            g_world_dirty = true;
        }

        float f0[3] = {
            (float)mat.dielectric_F0.x(),
            (float)mat.dielectric_F0.y(),
            (float)mat.dielectric_F0.z()
        };
        if (ImGui::ColorEdit3("Dielectric F0", f0)) {
            mat.dielectric_F0 = vec3(f0[0], f0[1], f0[2]);
            g_world_dirty     = true;
        }

        // Subsurface scattering (simplified controls)
        if (ImGui::Checkbox("Use Subsurface Scattering (SSS)", &mat.use_sss)) {
            g_world_dirty = true;
        }
        
        if (mat.use_sss) {
            ImGui::Indent();
            
            // Editor exposes SSS strength in [0..1] for convenience, but the
            // renderer expects a smaller physical scale. Map UI [0..1] -> internal [0..0.1].
            float sss_ui = (float)std::clamp(mat.sss_strength / 0.1, 0.0, 1.0);
            if (ImGui::SliderFloat("SSS Strength", &sss_ui, 0.0f, 1.0f)) {
                mat.sss_strength = (double)(sss_ui * 0.1f);
                g_world_dirty = true;
            }

            float sss_radius_f = (float)mat.sss_radius;
            if (ImGui::SliderFloat("SSS Radius", &sss_radius_f, 0.01f, 10.0f)) {
                mat.sss_radius = (double)sss_radius_f;
                // Keep legacy sss_scale in sync for the single-scatter fallback
                mat.sss_scale = mat.sss_radius;
                g_world_dirty = true;
            }

            // Compact model selector: None / Single / Multi-Single / Dipole / Skin / Foliage
            const char* sss_items_simple[] = { "None", "Single", "Multi-Single", "Dipole (Burley)", "Skin", "Foliage" };
            int sss_model_idx = 0;
            // Map runtime enum values to combo index
            if (mat.sss_model == SSS_NONE) sss_model_idx = 0;
            else if (mat.sss_model == SSS_SINGLE_SCATTER) sss_model_idx = 1;
            else if (mat.sss_model == SSS_MULTI_SINGLE_SCATTER) sss_model_idx = 2;
            else if (mat.sss_model == SSS_DIPOLE_BURLEY) sss_model_idx = 3;
            else if (mat.sss_model == SSS_SKIN) sss_model_idx = 4;
            else if (mat.sss_model == SSS_FOLIAGE) sss_model_idx = 5;

            if (ImGui::Combo("SSS Model", &sss_model_idx, sss_items_simple, IM_ARRAYSIZE(sss_items_simple))) {
                if (sss_model_idx == 0) mat.sss_model = SSS_NONE;
                else if (sss_model_idx == 1) mat.sss_model = SSS_SINGLE_SCATTER;
                else if (sss_model_idx == 2) mat.sss_model = SSS_MULTI_SINGLE_SCATTER;
                else if (sss_model_idx == 3) mat.sss_model = SSS_DIPOLE_BURLEY;
                else if (sss_model_idx == 4) mat.sss_model = SSS_SKIN;
                else mat.sss_model = SSS_FOLIAGE;
                g_world_dirty = true;
            }

            // (Unreal PBR checkbox moved to the Textures section)

            // Only expose sample count when Dipole is selected (advanced)
            if (mat.sss_model == SSS_DIPOLE_BURLEY) {
                int sss_samples_i = mat.sss_samples;
                if (ImGui::SliderInt("SSS Samples (advanced)", &sss_samples_i, 1, 32)) {
                    mat.sss_samples = sss_samples_i;
                    g_world_dirty = true;
                }
            }
            
            // SSS Color Override (only when SSS is enabled)
            if (ImGui::Checkbox("Override SSS Color", &mat.sss_color_override_enabled)) {
                g_world_dirty = true;
            }
            if (mat.sss_color_override_enabled) {
                float sss_col[3] = {
                    (float)mat.sss_color_override_color.x(),
                    (float)mat.sss_color_override_color.y(),
                    (float)mat.sss_color_override_color.z()
                };
                if (ImGui::ColorEdit3("SSS Scatter Color", sss_col)) {
                    mat.sss_color_override_color = vec3(sss_col[0], sss_col[1], sss_col[2]);
                    g_world_dirty = true;
                }
            }
            
            ImGui::Unindent();
        }

        ImGui::Separator();
        if (include_texture_bindings && mat.graph.nodes.empty()) {
            ImGui::Text("PBR Texture Maps (drag from Textures window)");

            auto draw_tex_slot = [&](const char* label, int& tex_index)
            {
                ImGui::Text("%s", label);
                ImGui::SameLine();
                const char* btn_label = "<none>";
                if (tex_index >= 0 &&
                    tex_index < (int)scn.textures.size()) {
                    btn_label = scn.textures[tex_index].name.c_str();
                }
                ImGui::Button(btn_label, ImVec2(140.0f, 0.0f));

                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* payload =
                            ImGui::AcceptDragDropPayload("TEXTURE_ASSET_ID"))
                    {
                        int asset_index = *(const int*)payload->Data;
                        if (asset_index >= 0 &&
                            asset_index < (int)scn.textures.size()) {
                            tex_index = asset_index;
                            g_world_dirty = true;
                        }
                    }
                    ImGui::EndDragDropTarget();
                }

                if (tex_index >= 0) {
                    ImGui::SameLine();
                    std::string clear_id = std::string("X##clear_") + label;
                    if (ImGui::SmallButton(clear_id.c_str())) {
                        tex_index = -1;
                        g_world_dirty = true;
                    }
                }
            };

            draw_tex_slot("Albedo",    mat.albedo_tex);
            draw_tex_slot("Metallic",  mat.metallic_tex);
            draw_tex_slot("Roughness", mat.roughness_tex);
            draw_tex_slot("Normal",    mat.normal_tex);
            draw_tex_slot("Opacity Mask (optional)", mat.alpha_tex);
            ImGui::TextDisabled("If Opacity Mask is empty, the albedo's alpha channel will be used.");

        } else {
            ImGui::TextDisabled("Texture inputs are edited in the Material Graph.");
        }

        // Graphs express packed maps explicitly through their channel outputs.
        if (mat.graph.nodes.empty()) {
            if (ImGui::Checkbox("Unreal PBR (G=roughness, B=metallic)", &mat.unreal_pbr)) g_world_dirty = true;
        } else {
            ImGui::TextDisabled("Packed maps: connect G to Roughness, B to Metallic.");
        }
    } break;

    default:
        ImGui::TextDisabled("Unknown material model.");
        break;
    }

    // Detect material edits by comparing important fields to the snapshot
    bool material_changed = false;
    if (orig.name != mat.name) material_changed = true;
    if (orig.model != mat.model) material_changed = true;
    if (fabs(orig.base_color.x() - mat.base_color.x()) > 1e-6) material_changed = true;
    if (fabs(orig.base_color.y() - mat.base_color.y()) > 1e-6) material_changed = true;
    if (fabs(orig.base_color.z() - mat.base_color.z()) > 1e-6) material_changed = true;
    if (fabs(orig.emission.x() - mat.emission.x()) > 1e-6) material_changed = true;
    if (fabs(orig.emission.y() - mat.emission.y()) > 1e-6) material_changed = true;
    if (fabs(orig.emission.z() - mat.emission.z()) > 1e-6) material_changed = true;
    if (fabs(orig.emission_intensity - mat.emission_intensity) > 1e-6) material_changed = true;
    if (fabs(orig.metallic - mat.metallic) > 1e-6) material_changed = true;
    if (fabs(orig.roughness - mat.roughness) > 1e-6) material_changed = true;
    if (fabs(orig.normal_strength - mat.normal_strength) > 1e-6) material_changed = true;
    if (fabs(orig.ior - mat.ior) > 1e-6) material_changed = true;
    if (fabs(orig.dielectric_F0.x() - mat.dielectric_F0.x()) > 1e-6) material_changed = true;
    if (fabs(orig.dielectric_F0.y() - mat.dielectric_F0.y()) > 1e-6) material_changed = true;
    if (fabs(orig.dielectric_F0.z() - mat.dielectric_F0.z()) > 1e-6) material_changed = true;
    if (orig.sss_model != mat.sss_model) material_changed = true;
    if (orig.sss_samples != mat.sss_samples) material_changed = true;
    if (fabs(orig.sss_strength - mat.sss_strength) > 1e-6) material_changed = true;
    if (fabs(orig.sss_radius - mat.sss_radius) > 1e-6) material_changed = true;
    if (orig.albedo_tex != mat.albedo_tex) material_changed = true;
    if (orig.metallic_tex != mat.metallic_tex) material_changed = true;
    if (orig.roughness_tex != mat.roughness_tex) material_changed = true;
    if (orig.normal_tex != mat.normal_tex) material_changed = true;
    if (orig.alpha_tex != mat.alpha_tex) material_changed = true;
    if (orig.unreal_pbr != mat.unreal_pbr) material_changed = true;

    if (material_changed) {
        InvalidateMaterialThumbnail(mat_index);
        MarkMaterialsDirty();
        g_world_dirty = true;
    }

    ImGui::Separator();
    ImGui::TextDisabled("Edit transform/material/textures, then re-render.");
}

#include "material_graph_editor.inl"

// -----------------------------------------------------------------------------
// File-drop
// -----------------------------------------------------------------------------

static void glfw_drop_callback(GLFWwindow* window, int count, const char** paths)
{
    CaptureMaterialGraphFileDrop(window);
    for (int i = 0; i < count; ++i) {
        if (paths[i]) {
            g_dropped_files.emplace_back(paths[i]);
        }
    }
}

static void process_dropped_files()
{
    if (g_dropped_files.empty())
        return;

    bool imported_any = false;

    for (const std::string& full_path : g_dropped_files) {
        // Copy dropped files into project Assets folders (textures/ or models/)
        auto ensure_dir = [](const std::string& dir){
            try { std::filesystem::create_directories(dir); } catch(...) {}
        };
        auto base_name = [](const std::string& p)->std::pair<std::string,std::string>{
            size_t slash = p.find_last_of("/\\");
            std::string name = (slash==std::string::npos)? p : p.substr(slash+1);
            size_t dot = name.find_last_of('.');
            if (dot==std::string::npos) return {name, std::string{}};
            return { name.substr(0,dot), name.substr(dot) };
        };
        auto unique_dest = [&](const std::string& dir, const std::string& stem, const std::string& ext){
            std::string candidate = dir + "/" + stem + ext;
            int idx = 1;
            while (std::filesystem::exists(candidate)) {
                candidate = dir + "/" + stem + "_" + std::to_string(idx++) + ext;
            }
            return candidate;
        };

        // Textures
        if (has_extension_ci(full_path, ".png")  ||
            has_extension_ci(full_path, ".jpg")  ||
            has_extension_ci(full_path, ".jpeg") ||
            has_extension_ci(full_path, ".tga")  ||
            has_extension_ci(full_path, ".bmp")  ||
            has_extension_ci(full_path, ".hdr")  ||
            has_extension_ci(full_path, ".ppm"))
        {
            // Copy into textures/
            ensure_dir("textures");
            auto [stem, ext] = base_name(full_path);
            std::string dest = unique_dest("textures", stem, ext.empty()? std::string{}: ext);
            try { std::filesystem::copy_file(full_path, dest, std::filesystem::copy_options::overwrite_existing); }
            catch(...) { /* ignore copy failure; fallback to original path */ dest = full_path; }

            scene_texture tex;
            tex.name = stem;
            tex.path = dest;

            g_scene.textures.push_back(std::move(tex));
            MaterialGraphImportedTexture((int)g_scene.textures.size() - 1);

            std::printf("Imported texture: %s (%s)\n",
                        g_scene.textures.back().name.c_str(),
                        g_scene.textures.back().path.c_str());

            imported_any = true;
        }
        // FBX / OBJ meshes → mesh_instance
        else if (has_extension_ci(full_path, ".fbx") ||
                 has_extension_ci(full_path, ".obj"))
        {
            // Copy into models/
            ensure_dir("models");
            auto [stem, ext] = base_name(full_path);
            std::string dest = unique_dest("models", stem, ext.empty()? std::string{}: ext);
            try { std::filesystem::copy_file(full_path, dest, std::filesystem::copy_options::overwrite_existing); }
            catch(...) { dest = full_path; }

            std::printf("Importing mesh (Assimp): %s\n", dest.c_str());

            // Match RT loader: FBX is Z-up, OBJ usually Y-up.
            bool z_up = has_extension_ci(dest, ".fbx");
            MeshLoadResult mlr = load_assimp_mesh_as_gpu_mesh(
                dest,
                z_up,
                /*normalise_unit=*/true,
                /*user_scale=*/2.0
            );

            if (mlr.mesh.vao == 0 || mlr.mesh.index_count == 0) {
                std::fprintf(stderr, "Mesh import failed or empty: %s\n",
                             full_path.c_str());
                continue;
            }

            std::string name = stem;

            // Prepare mesh asset (with material slots)
            scene_mesh_asset asset;
            asset.name      = name;
            asset.file_path = dest;
            asset.approx_radius = mlr.approx_radius;
            asset.mesh_bvh  = nullptr; // RT support via mesh_loader.h now built in scene.cpp
            asset.slot_names = mlr.material_slot_names;
            asset.slot_default_materials.assign(asset.slot_names.size(), -1);

            int mesh_index = (int)g_scene.meshes.size();

            // Create mesh_instance object
            scene_object obj;
            obj.name           = name;
            obj.type           = scene_object_type::mesh_instance;
            obj.material_index = -1; // per-slot binding will be used instead

            obj.center       = point3(0,0,0);
            obj.radius       = mlr.approx_radius;
            obj.mesh_index   = mesh_index;
            obj.translation  = vec3(0, 0, -1);
            obj.rotation_deg = vec3(0, 0, 0);
            obj.scale        = vec3(1, 1, 1);
            obj.mesh_slot_materials = asset.slot_default_materials;

            // Push asset + GPU mesh
            g_scene.meshes.push_back(std::move(asset));

            if ((int)g_gpu_meshes.size() < mesh_index + 1) {
                g_gpu_meshes.resize(mesh_index + 1);
            }
            g_gpu_meshes[mesh_index] = mlr.mesh;

            // Invalidate any existing generated thumbnail for this mesh
            auto it = g_model_thumb_cache.find(mesh_index);
            if (it != g_model_thumb_cache.end()) {
                if (it->second) glDeleteTextures(1, &it->second);
                g_model_thumb_cache.erase(it);
            }

            g_scene.objects.push_back(std::move(obj));

            std::printf("Mesh imported as mesh_instance '%s' (mesh_index=%d, slots=%zu)\n",
                        name.c_str(), mesh_index,
                        g_scene.meshes[mesh_index].slot_names.size());

            imported_any = true;
        }
        else {
            std::printf("Dropped file not recognised as texture or supported mesh: %s\n",
                        full_path.c_str());
        }
    }

    g_dropped_files.clear();
    g_graph_file_drop_material = -1;

    if (imported_any) {
        g_world_dirty = true;
        g_cached_world.reset();
        // Persist imported assets for next launch
        SaveAssetsManifest();
    }
}

// -----------------------------------------------------------------------------
// main
// -----------------------------------------------------------------------------

#include "editor_scene_panels.inl"

int main()
{
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit()) {
        std::fprintf(stderr, "Failed to init GLFW\n");
        return 1;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_ANY_PROFILE);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

    GLFWwindow* window = glfwCreateWindow(1600, 900, "Dusktracer Editor", nullptr, nullptr);
    if (!window) {
        std::fprintf(stderr, "Failed to create GLFW window\n");
        glfwTerminate();
        return 1;
    }

    // Try to set application icon from resources/dusktracer.png
    SetWindowIconAuto(window, "dusktracer.png");

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
    glfwSetDropCallback(window, glfw_drop_callback);

    glewExperimental = GL_TRUE;
    if (glewInit() != GLEW_OK) {
        std::fprintf(stderr, "Failed to init GLEW\n");
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    (void)io;

    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;

    SetupEditorFonts(io);
    ImGui::StyleColorsDark();
    ApplyModernEditorTheme();

    ImGuiStyle& style = ImGui::GetStyle();
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
    {
        style.WindowRounding = 0.0f;
        style.Colors[ImGuiCol_WindowBg].w = 1.0f;
    }

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 130");

    init_engine_once();

    bool   show_demo_window = false;
    double last_time        = glfwGetTime();
    static bool s_viewport_match_render = true;
    static bool s_content_drawer_open = false;
    static bool s_focus_content_drawer = false;
    static float s_content_drawer_anim = 0.0f;

    while (!glfwWindowShouldClose(window))
    {
        glfwPollEvents();
        process_dropped_files();

        double current_time = glfwGetTime();
        double dt = current_time - last_time;
        last_time = current_time;

        update_editor_camera_from_input(window, dt);

        // If render thread produced a partial or final result, upload texture.
        if (g_render_has_result) {
            {
                std::lock_guard<std::mutex> lock(g_render_mutex);
                UploadRenderToTexture(g_render_result);
                g_render_has_result = false;
            }
            // Only join/cleanup when final image is ready
            if (g_render_final_image_ready.load()) {
                if (g_render_thread.joinable()) {
                    g_render_thread.join();
                }
                
                // CRITICAL: Explicitly release the cached world after render completes
                // to ensure Embree/BVH resources are freed before the next render starts.
                // This prevents crashes when rebuilding the world with modified scenes.
                g_cached_world.reset();
                g_world_dirty = true;
                
                // Stop the separate progress window now that render finished
                stop_progress_window_thread();
                // clear any pending popup
                g_progress_popup_pending = false;
                // Save final image to disk using existing helper
                if (!g_render_result.pixels.empty()) {
                    const std::string out_path = "Renders/LastRender.ppm";
                    bool saved = write_ppm(out_path, g_render_result);
                    if (saved) {
                        std::printf("Saved render to '%s'\n", out_path.c_str());
                    } else {
                        std::fprintf(stderr, "Failed to save render to '%s'\n", out_path.c_str());
                    }
                }

                g_render_in_progress = false;
                g_cancel_flag.store(false);
                g_render_final_image_ready.store(false);
            }
        }

        // Start progress window after configured delay
        if (g_render_in_progress && g_progress_popup_pending && !g_progress_window_running.load()) {
            auto now = std::chrono::steady_clock::now();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_progress_request_time).count();
            if (ms >= g_progress_popup_delay_ms) {
                start_progress_window_thread();
                g_progress_popup_pending = false;
            }
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // Ctrl+Z / Ctrl+Y handling (undo/redo) using GLFW key state; debounce on key transition
        static bool s_prev_ctrlz = false;
        static bool s_prev_ctrly = false;
        static bool s_prev_ctrlspace = false;
        bool ctrl_down = io.KeyCtrl;
        bool z_down = ImGui::IsKeyDown(ImGuiKey_Z);
        bool y_down = ImGui::IsKeyDown(ImGuiKey_Y);
        bool space_down = ImGui::IsKeyDown(ImGuiKey_Space);
        bool cur_ctrlz = ctrl_down && z_down && !io.KeyShift;
        bool cur_ctrly = ctrl_down && (y_down || (io.KeyShift && z_down));
        bool cur_ctrlspace = ctrl_down && space_down;
        if (cur_ctrlz && !s_prev_ctrlz && !io.WantTextInput) {
            UndoManager::Instance().undo();
        }
        if (cur_ctrly && !s_prev_ctrly && !io.WantTextInput) {
            UndoManager::Instance().redo();
        }
        if (cur_ctrlspace && !s_prev_ctrlspace) {
            s_content_drawer_open = !s_content_drawer_open;
            s_focus_content_drawer = s_content_drawer_open;
        }
        s_prev_ctrlz = cur_ctrlz;
        s_prev_ctrly = cur_ctrly;
        s_prev_ctrlspace = cur_ctrlspace;

        // Global Delete key: remove selected object when Delete pressed.
        // Allow Delete when viewport is focused even if ImGui requests keyboard capture
        ImGuiIO& io = ImGui::GetIO();
        if (ImGui::IsKeyPressed(ImGuiKey_Delete) && (g_viewport_focused || !io.WantCaptureKeyboard)) {
            if (g_selected_object >= 0 && g_selected_object < (int)g_scene.objects.size()) {
                int del_idx = g_selected_object;
                scene_object removed = g_scene.objects[del_idx];

                // perform erase
                g_scene.objects.erase(g_scene.objects.begin() + del_idx);

                // Fix selection index
                if (g_scene.objects.empty()) {
                    g_selected_object = -1;
                } else if (g_selected_object >= (int)g_scene.objects.size()) {
                    g_selected_object = (int)g_scene.objects.size() - 1;
                }

                // Mark RT world dirty
                g_world_dirty = true;
                g_cached_world.reset();

                // push undo action: undo = re-insert, redo = delete again
                UndoManager::Instance().push(std::make_unique<LambdaAction>(
                    [del_idx, removed]() {
                        // undo = re-insert
                        int insert_at = del_idx;
                        if (insert_at < 0) insert_at = 0;
                        if (insert_at > (int)g_scene.objects.size()) insert_at = (int)g_scene.objects.size();
                        g_scene.objects.insert(g_scene.objects.begin() + insert_at, removed);
                        g_selected_object = insert_at;
                        g_world_dirty = true;
                        g_cached_world.reset();
                    },
                    [del_idx]() {
                        // redo = perform delete again (after undo)
                        if (del_idx >= 0 && del_idx < (int)g_scene.objects.size()) {
                            g_scene.objects.erase(g_scene.objects.begin() + del_idx);
                            if (g_scene.objects.empty()) g_selected_object = -1;
                            else if (g_selected_object >= (int)g_scene.objects.size()) g_selected_object = (int)g_scene.objects.size() - 1;
                            g_world_dirty = true;
                            g_cached_world.reset();
                        }
                    },
                    "Delete Object"
                ));
            }
        }

        // Dockspace
        {
            ImGuiWindowFlags window_flags =
                ImGuiWindowFlags_MenuBar |
                ImGuiWindowFlags_NoDocking |
                ImGuiWindowFlags_NoTitleBar |
                ImGuiWindowFlags_NoCollapse |
                ImGuiWindowFlags_NoResize |
                ImGuiWindowFlags_NoMove |
                ImGuiWindowFlags_NoBringToFrontOnFocus |
                ImGuiWindowFlags_NoNavFocus;

            const ImGuiViewport* viewport = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(viewport->WorkPos);
            ImGui::SetNextWindowSize(viewport->WorkSize);
            ImGui::SetNextWindowViewport(viewport->ID);

            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

            ImGui::Begin("DockSpaceRoot", nullptr, window_flags);
            ImGui::PopStyleVar(2);

            ImGuiID dockspace_id = ImGui::GetID("DusktracerDockSpace");
            ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f));

                // --- One-time dock layout setup ---
            static bool s_dockspace_built = false;
            if (!s_dockspace_built)
            {
                s_dockspace_built = true;

                // Clear any previous layout for this dockspace ID
                ImGui::DockBuilderRemoveNode(dockspace_id);
                ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
                ImGui::DockBuilderSetNodeSize(dockspace_id, viewport->WorkSize);

                ImGuiID dock_main_id = dockspace_id;
                ImGuiID dock_right_id = ImGui::DockBuilderSplitNode(dock_main_id, ImGuiDir_Right, .28f, nullptr, &dock_main_id);
                ImGuiID dock_tree_id = ImGui::DockBuilderSplitNode(dock_right_id, ImGuiDir_Up, .34f, nullptr, &dock_right_id);
                ImGui::DockBuilderDockWindow("Viewport", dock_main_id);
                ImGui::DockBuilderDockWindow("Outliner", dock_tree_id);
                ImGui::DockBuilderDockWindow("Properties", dock_right_id);
                ImGui::DockBuilderDockWindow("Render Settings", dock_right_id);
                ImGui::DockBuilderDockWindow("Debug Camera", dock_right_id);

                ImGui::DockBuilderFinish(dockspace_id);
            }


            if (ImGui::BeginMenuBar())
            {
                if (g_ui_font_heading) ImGui::PushFont(g_ui_font_heading);
                ImGui::TextUnformatted("Dusktracer");
                if (g_ui_font_heading) ImGui::PopFont();
                ImGui::SameLine(0.0f, 18.0f);

                if (ImGui::BeginMenu("File"))
                {
                    bool canExport = !g_render_in_progress;
                    if (ImGui::MenuItem("Export Scene as C++...", nullptr, false, canExport)) {
                        const std::string out = "Renders/scene_export.cpp";
                        export_scene_as_cpp(g_scene, out);
                    }

                    if (ImGui::MenuItem("Exit")) {
                        glfwSetWindowShouldClose(window, GLFW_TRUE);
                    }
                    ImGui::EndMenu();
                }
                if (ImGui::BeginMenu("View"))
                {
                    ImGui::MenuItem("ImGui Demo", nullptr, &show_demo_window);
                    if (ImGui::MenuItem("Content Drawer", "Ctrl+Space", &s_content_drawer_open)) {
                        s_focus_content_drawer = s_content_drawer_open;
                    }
                    ImGui::EndMenu();
                }
                if (ImGui::BeginMenu("Edit"))
                {
                    bool canU = UndoManager::Instance().can_undo();
                    bool canR = UndoManager::Instance().can_redo();
                    if (ImGui::MenuItem("Undo", "Ctrl+Z", false, canU)) {
                        UndoManager::Instance().undo();
                    }
                    if (ImGui::MenuItem("Redo", "Ctrl+Y", false, canR)) {
                        UndoManager::Instance().redo();
                    }
                    ImGui::EndMenu();
                }
                float right_anchor = ImGui::GetWindowContentRegionMax().x - 220.0f;
                if (ImGui::GetCursorPosX() < right_anchor) {
                    ImGui::SetCursorPosX(right_anchor);
                }
                DrawInfoChip(g_render_in_progress ? "Rendering" : "Realtime Editor");
                ImGui::SameLine();
                DrawInfoChip(g_world_dirty ? "Scene Dirty" : "Scene Synced");
                ImGui::EndMenuBar();
            }

            ImGui::End();
        }

        // ---------------------------------------------------------------------
        // Scene Hierarchy
        // ---------------------------------------------------------------------
        DrawSceneOutliner();

        // ---------------------------------------------------------------------
        // Content drawer (textures + models), hidden by default and toggled
        // with Ctrl+Space so the viewport keeps its full height when closed.
        // ---------------------------------------------------------------------
        g_thumbs_created_this_frame = 0; // reset per-frame budget before rendering assets
        const float drawer_anim_duration = 0.18f;
        const float drawer_anim_step = (drawer_anim_duration > 0.0f)
            ? std::clamp((float)io.DeltaTime / drawer_anim_duration, 0.0f, 1.0f)
            : 1.0f;
        if (s_content_drawer_open) {
            s_content_drawer_anim = std::min(1.0f, s_content_drawer_anim + drawer_anim_step);
        } else {
            s_content_drawer_anim = std::max(0.0f, s_content_drawer_anim - drawer_anim_step);
        }
        const float drawer_eased = s_content_drawer_anim * s_content_drawer_anim * (3.0f - 2.0f * s_content_drawer_anim);

        if (drawer_eased > 0.001f) {
            const ImGuiViewport* drawer_viewport = ImGui::GetMainViewport();
            const float drawer_margin = 12.0f;
            const float drawer_height = std::clamp(drawer_viewport->WorkSize.y * 0.34f, 260.0f, 430.0f);
            const float hidden_y = drawer_viewport->WorkPos.y + drawer_viewport->WorkSize.y + drawer_margin;
            const float shown_y = drawer_viewport->WorkPos.y + drawer_viewport->WorkSize.y - drawer_height - drawer_margin;
            const float drawer_y = hidden_y + (shown_y - hidden_y) * drawer_eased;
            const bool drawer_interactive = s_content_drawer_open && drawer_eased >= 0.98f;
            ImGui::SetNextWindowViewport(drawer_viewport->ID);
            ImGui::SetNextWindowPos(
                ImVec2(drawer_viewport->WorkPos.x + drawer_margin,
                       drawer_y),
                ImGuiCond_Always
            );
            ImGui::SetNextWindowSize(
                ImVec2(drawer_viewport->WorkSize.x - drawer_margin * 2.0f, drawer_height),
                ImGuiCond_Always
            );
            ImGui::SetNextWindowBgAlpha(0.72f + 0.24f * drawer_eased);
            if (s_focus_content_drawer) {
                ImGui::SetNextWindowFocus();
                s_focus_content_drawer = false;
            }

            ImGuiWindowFlags content_drawer_flags =
                ImGuiWindowFlags_NoDocking |
                ImGuiWindowFlags_NoCollapse |
                ImGuiWindowFlags_NoSavedSettings;
            if (!drawer_interactive) {
                content_drawer_flags |= ImGuiWindowFlags_NoInputs;
            }
            ImGui::Begin("Content Drawer", nullptr, content_drawer_flags);
            ImGui::TextDisabled("Drop files to import. Drag assets into the scene or graph. Ctrl+Space to close.");
            g_thumb_budget_per_frame = g_thumb_budget_default;
            ImGui::Separator();

        // Assets search
        static char s_assets_search[256] = {0};
        ImGui::SetNextItemWidth(300.0f);
        ImGui::InputTextWithHint("##asset_search", "Search assets...", s_assets_search, sizeof(s_assets_search));
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear")) { s_assets_search[0] = '\0'; }
        std::string assets_query = s_assets_search;
        std::transform(assets_query.begin(), assets_query.end(), assets_query.begin(), [](unsigned char c){ return (char)std::tolower(c); });
        auto matches_query = [&](const std::string& name){
            if (assets_query.empty()) return true;
            std::string n = name;
            std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c){ return (char)std::tolower(c); });
            return n.find(assets_query) != std::string::npos;
        };

        if (ImGui::BeginTabBar("AssetTypes")) {
            if (ImGui::BeginTabItem("Textures")) {
            ImGui::BeginChild("TexturesGrid", ImVec2(0,0));
            const float cellPad = 12.0f;
            const ImVec2 thumbSize(80, 80);
            const float labelH = ImGui::GetTextLineHeightWithSpacing();
            const float cellW = thumbSize.x + cellPad * 2;
            const float cellH = thumbSize.y + labelH + cellPad * 2 + 4.0f;
            float avail = ImGui::GetContentRegionAvail().x;
            int cols = (int)std::max(1.0f, std::floor((avail + ImGui::GetStyle().ItemSpacing.x) / (cellW + ImGui::GetStyle().ItemSpacing.x)));
            int col = 0;

            int matched = 0;
            for (int i = 0; i < (int)g_scene.textures.size(); ++i) {
                auto& tex = g_scene.textures[i];
                if (!matches_query(tex.name)) continue;
                GLuint thumb = ImGui::IsRectVisible(ImVec2(cellW,cellH)) ? GetOrCreateTextureThumbnail(tex.path) : 0;

                ImGui::BeginGroup();
                std::string cell_id = std::string("tex_cell_") + std::to_string(i);
                ImGui::InvisibleButton(cell_id.c_str(), ImVec2(cellW, cellH));
                ImVec2 p0 = ImGui::GetItemRectMin();
                ImVec2 p1 = ImGui::GetItemRectMax();

                bool cell_hovered = ImGui::IsItemHovered();
                if (cell_hovered) {
                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    ImU32 col = ImGui::GetColorU32(ImGuiCol_HeaderHovered);
                    dl->AddRect(p0, p1, col, 4.0f);
                }
                if (ImGui::BeginDragDropSource()) {
                    ImGui::SetDragDropPayload("TEXTURE_ASSET_ID", &i, sizeof(int));
                    ImGui::Text("Texture: %s", tex.name.c_str());
                    ImGui::EndDragDropSource();
                }

                // Center image horizontally in the cell
                float imgX = p0.x + (cellW - thumbSize.x) * 0.5f;
                ImGui::SetCursorScreenPos(ImVec2(imgX, p0.y + cellPad));
                if (thumb) {
                    ImGui::Image((ImTextureID)(intptr_t)thumb, thumbSize, ImVec2(0,0), ImVec2(1,1));
                } else {
                    ImGui::Dummy(thumbSize);
                }
                // Center label beneath the image
                const char* label = tex.name.c_str();
                DrawAssetCellLabel(label,p0,p1,p0.y+cellPad+thumbSize.y+4);

                ImGui::EndGroup();

                // Next cell placement
                col++;
                if (col < cols) {
                    ImGui::SameLine();
                } else {
                    col = 0;
                }
                matched++;
            }
            if (g_scene.textures.empty()) {
                ImGui::TextDisabled("No textures imported yet.");
            } else if (matched == 0 && !assets_query.empty()) {
                ImGui::TextDisabled("No matching textures.");
            }
            if (col != 0) ImGui::NewLine();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Meshes")) {
            ImGui::BeginChild("MeshesGrid", ImVec2(0,0));
            const float cellPad = 12.0f;
            const ImVec2 iconSize(80, 80); // placeholder size or thumbnail
            const float labelH = ImGui::GetTextLineHeightWithSpacing();
            const float cellW = iconSize.x + cellPad * 2;
            const float cellH = iconSize.y + labelH + cellPad * 2 + 4.0f;
            float avail = ImGui::GetContentRegionAvail().x;
            int cols = (int)std::max(1.0f, std::floor((avail + ImGui::GetStyle().ItemSpacing.x) / (cellW + ImGui::GetStyle().ItemSpacing.x)));
            int col = 0;

            int matched = 0;
            for (int m = 0; m < (int)g_scene.meshes.size(); ++m) {
                auto& mesh = g_scene.meshes[m];
                if (!matches_query(mesh.name)) continue;
                ImGui::BeginGroup();
                std::string cell_id = std::string("mesh_cell_") + std::to_string(m);
                ImGui::InvisibleButton(cell_id.c_str(), ImVec2(cellW, cellH));
                ImVec2 p0 = ImGui::GetItemRectMin();
                ImVec2 p1 = ImGui::GetItemRectMax();

                bool cell_hovered = ImGui::IsItemHovered();
                bool cell_clicked = cell_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
                bool cell_double_clicked = cell_hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
                
                // Highlight if selected
                if (g_selected_mesh_asset == m) {
                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    ImU32 col = ImGui::GetColorU32(ImGuiCol_Header);
                    dl->AddRectFilled(p0, p1, col, 4.0f);
                }
                
                if (cell_hovered) {
                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    ImU32 col = ImGui::GetColorU32(ImGuiCol_HeaderHovered);
                    dl->AddRect(p0, p1, col, 4.0f, 0, 2.0f);
                }
                
                // Single-click to select for Inspector
                if (cell_clicked && !cell_double_clicked) {
                    SelectSceneObject(-1);
                    g_selected_mesh_asset = m;
                }
                // Drag from whole cell
                if (ImGui::BeginDragDropSource()) {
                    ImGui::SetDragDropPayload("MESH_ASSET_ID", &m, sizeof(int));
                    ImGui::Text("Model: %s", mesh.name.c_str());
                    ImGui::EndDragDropSource();
                }

                // Double-click to create a mesh_instance in the scene
                if (cell_double_clicked) {
                    scene_object obj;
                    obj.name = mesh.name + " Instance";
                    obj.type = scene_object_type::mesh_instance;
                    obj.material_index = -1; // use per-slot materials
                    obj.center = point3(0,0,0);
                    double radius = 1.0;
                    for (const auto& o : g_scene.objects) {
                        if (o.type == scene_object_type::mesh_instance && o.mesh_index == m) { radius = o.radius; break; }
                    }
                    obj.radius = radius;
                    obj.mesh_index = m;
                    obj.translation = vec3(0, 0, -1);
                    obj.rotation_deg = vec3(0, 0, 0);
                    obj.scale = vec3(1, 1, 1);
                    obj.mesh_slot_materials = g_scene.meshes[m].slot_default_materials;

                    g_scene.objects.push_back(obj);
                    int new_idx = (int)g_scene.objects.size() - 1;
                    scene_object snapshot = g_scene.objects[new_idx];
                    UndoManager::Instance().push(std::make_unique<LambdaAction>(
                        [new_idx]() {
                            if (new_idx >= 0 && new_idx < (int)g_scene.objects.size()) {
                                g_scene.objects.erase(g_scene.objects.begin() + new_idx);
                                if (g_scene.objects.empty()) g_selected_object = -1;
                                else if (g_selected_object >= (int)g_scene.objects.size()) g_selected_object = (int)g_scene.objects.size() - 1;
                                g_world_dirty = true; g_cached_world.reset();
                            }
                        },
                        [new_idx, snapshot]() {
                            if (new_idx < 0) return;
                            int insert_at = new_idx;
                            if (insert_at > (int)g_scene.objects.size()) insert_at = (int)g_scene.objects.size();
                            g_scene.objects.insert(g_scene.objects.begin() + insert_at, snapshot);
                            g_selected_object = insert_at;
                            g_world_dirty = true; g_cached_world.reset();
                        },
                        "Add Mesh Instance"
                    ));

                    g_selected_object = new_idx;
                    g_world_dirty = true;
                    g_cached_world.reset();
                    std::printf("Created mesh_instance from asset '%s' (mesh_index=%d)\n", mesh.name.c_str(), m);
                }

                // Center icon or model thumbnail image
                float iconX = p0.x + (cellW - iconSize.x) * 0.5f;
                ImGui::SetCursorScreenPos(ImVec2(iconX, p0.y + cellPad));
                GLuint meshThumb = 0;
                if (!mesh.thumbnail_path.empty()) {
                    meshThumb = GetOrCreateTextureThumbnail(mesh.thumbnail_path);
                } else {
                    meshThumb = ImGui::IsRectVisible(iconSize) ? GetOrCreateModelThumbnail(m) : 0;
                }
                if (meshThumb) {
                    ImGui::Image((ImTextureID)(intptr_t)meshThumb, iconSize, ImVec2(0,1), ImVec2(1,0));
                } else {
                    ImGui::Dummy(iconSize);
                }
                // Center label
                const char* label = mesh.name.c_str();
                DrawAssetCellLabel(label,p0,p1,p0.y+cellPad+iconSize.y+4);
                ImGui::EndGroup();

                // Allow dropping a texture onto the model tile to set thumbnail
                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("TEXTURE_ASSET_ID")) {
                        int texIndex = *(const int*)payload->Data;
                        if (texIndex >= 0 && texIndex < (int)g_scene.textures.size()) {
                            g_scene.meshes[m].thumbnail_path = g_scene.textures[texIndex].path;
                        }
                    }
                    ImGui::EndDragDropTarget();
                }

                col++;
                if (col < cols) {
                    ImGui::SameLine();
                } else {
                    col = 0;
                }
                matched++;
            }
            if (g_scene.meshes.empty()) {
                ImGui::TextDisabled("No models imported yet.");
            } else if (matched == 0 && !assets_query.empty()) {
                ImGui::TextDisabled("No matching models.");
            }
            if (col != 0) ImGui::NewLine();
            
            // Click in empty space to deselect
            if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsAnyItemHovered()) {
                g_selected_mesh_asset = -1;
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }

        // Materials tab
        if (ImGui::BeginTabItem("Materials")) {
            ImGui::BeginChild("MaterialsGrid", ImVec2(0,0));
            const float cellPad = 12.0f;
            const ImVec2 thumbSize(80, 80);
            const float labelH = ImGui::GetTextLineHeightWithSpacing();
            const float cellW = thumbSize.x + cellPad * 2;
            const float cellH = thumbSize.y + labelH + cellPad * 2 + 4.0f;
            float avail = ImGui::GetContentRegionAvail().x;
            int cols = (int)std::max(1.0f, std::floor((avail + ImGui::GetStyle().ItemSpacing.x) / (cellW + ImGui::GetStyle().ItemSpacing.x)));
            int col = 0;

            // Right-click context menu for creating new materials
            if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && !ImGui::IsAnyItemHovered()) {
                ImGui::OpenPopup("MaterialsContextMenu");
            }
            if (ImGui::BeginPopup("MaterialsContextMenu")) {
                if (ImGui::MenuItem("Create Material")) {
                    scene_material new_mat;
                    new_mat.name = "New Material";
                    new_mat.model = scene_material_model::pbr;
                    new_mat.base_color = colour(0.8, 0.8, 0.8);
                    new_mat.metallic = 0.0;
                    new_mat.roughness = 0.5;
                    new_mat.ior = 1.5;
                    new_mat.emission = colour(0, 0, 0);
                    new_mat.albedo_tex = -1;
                    new_mat.metallic_tex = -1;
                    new_mat.roughness_tex = -1;
                    new_mat.normal_tex = -1;
                    new_mat.alpha_tex = -1;
                    
                    g_scene.materials.push_back(new_mat);
                    int new_idx = (int)g_scene.materials.size() - 1;
                    g_selected_material = new_idx;
                    g_world_dirty = true;
                    g_cached_world.reset();
                    MarkMaterialsDirty();
                    
                    // Undo support
                    UndoManager::Instance().push(std::make_unique<LambdaAction>(
                        [new_idx]() {
                            if (new_idx >= 0 && new_idx < (int)g_scene.materials.size()) {
                                g_scene.materials.erase(g_scene.materials.begin() + new_idx); InvalidateAllMaterialThumbnails();
                                if (g_selected_material == new_idx) g_selected_material = -1;
                                g_world_dirty = true; g_cached_world.reset();
                            }
                        },
                        [new_idx, new_mat]() {
                            if (new_idx < 0) return;
                            int insert_at = new_idx;
                            if (insert_at > (int)g_scene.materials.size()) insert_at = (int)g_scene.materials.size();
                            g_scene.materials.insert(g_scene.materials.begin() + insert_at, new_mat); InvalidateAllMaterialThumbnails();
                            g_selected_material = insert_at;
                            g_world_dirty = true; g_cached_world.reset();
                        },
                        "Create Material"
                    ));
                    
                    std::printf("Created new material '%s'\n", new_mat.name.c_str());
                }
                ImGui::EndPopup();
            }

            int matched = 0;
            for (int mat_idx = 0; mat_idx < (int)g_scene.materials.size(); ++mat_idx) {
                auto& mat = g_scene.materials[mat_idx];
                if (!matches_query(mat.name)) continue;

                GLuint thumb = ImGui::IsRectVisible(ImVec2(cellW,cellH)) ? GetOrCreateMaterialThumbnail(mat_idx) : 0;

                ImGui::BeginGroup();
                std::string cell_id = std::string("mat_cell_") + std::to_string(mat_idx);
                ImGui::InvisibleButton(cell_id.c_str(), ImVec2(cellW, cellH));
                ImVec2 p0 = ImGui::GetItemRectMin();
                ImVec2 p1 = ImGui::GetItemRectMax();

                bool cell_hovered = ImGui::IsItemHovered();
                bool cell_clicked = cell_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
                
                // Highlight if selected
                if (g_selected_material == mat_idx) {
                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    ImU32 col = ImGui::GetColorU32(ImGuiCol_Header);
                    dl->AddRectFilled(p0, p1, col, 4.0f);
                }
                
                if (cell_hovered) {
                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    ImU32 col = ImGui::GetColorU32(ImGuiCol_HeaderHovered);
                    dl->AddRect(p0, p1, col, 4.0f, 0, 2.0f);
                }
                
                // Click to select for editing
                if (cell_clicked) {
                    g_selected_material = mat_idx;
                }
                
                // Drag-and-drop source
                if (ImGui::BeginDragDropSource()) {
                    ImGui::SetDragDropPayload("MATERIAL_ASSET_ID", &mat_idx, sizeof(int));
                    ImGui::Text("Material: %s", mat.name.c_str());
                    ImGui::EndDragDropSource();
                }

                // Center image horizontally in the cell
                float imgX = p0.x + (cellW - thumbSize.x) * 0.5f;
                ImGui::SetCursorScreenPos(ImVec2(imgX, p0.y + cellPad));
                if (thumb) {
                    ImGui::Image((ImTextureID)(intptr_t)thumb, thumbSize, ImVec2(0,0), ImVec2(1,1));
                } else {
                    ImGui::Dummy(thumbSize);
                }
                
                // Center label beneath the image
                const char* label = mat.name.c_str();
                DrawAssetCellLabel(label,p0,p1,p0.y+cellPad+thumbSize.y+4);

                ImGui::EndGroup();

                // Next cell placement
                col++;
                if (col < cols) {
                    ImGui::SameLine();
                } else {
                    col = 0;
                }
                matched++;
            }
            
            if (g_scene.materials.empty()) {
                ImGui::TextDisabled("No materials created yet. Right-click to create one.");
            } else if (matched == 0 && !assets_query.empty()) {
                ImGui::TextDisabled("No matching materials.");
            }
            if (col != 0) ImGui::NewLine();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }

            ImGui::EndTabBar();
        }

            ImGui::End();
        } else {
            g_thumb_budget_per_frame = 0;
            s_focus_content_drawer = false;
        }

        // ---------------------------------------------------------------------
        // Render settings
        // ---------------------------------------------------------------------
        ImGui::Begin("Render Settings");
        DrawPanelTitle("Render Settings");
        DrawSectionLabel("Camera");
        ImGui::Text("Position: (%.2f, %.2f, %.2f)",
            g_editor_cam.position.x(),
            g_editor_cam.position.y(),
            g_editor_cam.position.z());
        ImGui::Text("Yaw: %.2f, Pitch: %.2f", g_editor_cam.yaw, g_editor_cam.pitch);

        if (ImGui::CollapsingHeader("Editor navigation")) {
        ImGui::Text("Controls:");
        ImGui::Text("  WASD = move, Q/E = down/up");
        ImGui::Text("  RMB drag in Viewport = look");

        DrawSectionLabel("Editor Camera");
        ImGui::SliderFloat("Move speed",  &g_camera_move_speed, 0.1f, 20.0f);
        ImGui::SliderFloat("Look sens",   &g_camera_look_sens,  0.0005f, 0.02f);
        ImGui::SliderFloat("FOV",         &g_editor_cam.vfov,   20.0f, 90.0f);

        if (ImGui::Button("Reset Camera")) {
            g_editor_cam.vfov = 40.0f;
            g_editor_cam.set_from_lookat(point3(3, 3, 2),
                                         point3(0, 0, -1));
            // camera reset doesn't dirty world
        }

        }

        DrawSectionLabel("Render Quality");

        int spp = g_camera.samples_per_pixel;
        if (ImGui::DragInt("Samples per pixel", &spp, 1, 1, 4096)) {
            if (spp < 1)  spp = 1;
            g_camera.samples_per_pixel = spp;
        }

        int max_depth = g_camera.max_depth;
        if (ImGui::DragInt("Max bounce depth", &max_depth, 1, 1, 128)) {
            if (max_depth < 1) max_depth = 1;
            g_camera.max_depth = max_depth;
        }

        const char* presets[] = {"Custom", "Preview", "Medium", "Final"};
        int preset = 0;
        if (ImGui::Combo("Quality preset", &preset, presets, IM_ARRAYSIZE(presets))) {
            const int samples[] = {0,256,512,2048}, depths[] = {0,20,35,50}, lights[] = {0,2,4,6};
            if (preset > 0) {
                g_camera.samples_per_pixel = samples[preset]; g_camera.max_depth = depths[preset];
                g_camera.direct_light_samples = lights[preset];
            }
        }
        DrawSectionLabel("Post");
        // Exposure control (linear multiplier applied in renderer)
        {
            float exp_f = (float)g_renderer.exposure;
            if (ImGui::DragFloat("Exposure", &exp_f, 0.01f, 0.0f, 100.0f, "%.3f")) {
                if (exp_f < 0.0f) exp_f = 0.0f;
                g_renderer.exposure = exp_f;
            }
            ImGui::TextDisabled("Linear exposure multiplier applied before tone mapping.");
        }

        // Denoiser (OpenImageDenoise) controls
        {
            bool use_dn = g_renderer.use_denoiser;
            if (ImGui::Checkbox("Use Denoiser (OIDN)", &use_dn)) {
                g_renderer.use_denoiser = use_dn;
            }
#ifdef HAVE_OIDN
            float ds = (float)g_renderer.denoiser_strength;
            if (ImGui::DragFloat("Denoiser Strength", &ds, 0.01f, 0.0f, 1.0f, "%.3f")) {
                if (ds < 0.0f) ds = 0.0f;
                if (ds > 1.0f) ds = 1.0f;
                g_renderer.denoiser_strength = ds;
            }
            ImGui::TextDisabled("Final image blend: 0 = raw, 1 = fully denoised.");
#else
            ImGui::TextDisabled("OpenImageDenoise not available in this build.");
#endif
            // Adaptive sampling controls
            DrawSectionLabel("Adaptive Sampling");
            bool adapt = g_renderer.adaptive_sampling;
            if (ImGui::Checkbox("Adaptive Sampling (per-pixel)", &adapt)) {
                g_renderer.adaptive_sampling = adapt;
            }
            if (g_renderer.adaptive_sampling) {
                int amin = g_renderer.adaptive_min_samples;
                if (ImGui::InputInt("Min samples before check", &amin)) {
                    if (amin < 1) amin = 1;
                    g_renderer.adaptive_min_samples = amin;
                }
                int aint = g_renderer.adaptive_check_interval;
                if (ImGui::InputInt("Check interval (samples)", &aint)) {
                    if (aint < 1) aint = 1;
                    g_renderer.adaptive_check_interval = aint;
                }
                float arel = (float)g_renderer.adaptive_rel_threshold;
                if (ImGui::DragFloat("Relative std-error threshold", &arel, 0.001f, 0.0001f, 0.5f, "%.4f")) {
                    if (arel < 0.0f) arel = 0.0f;
                    g_renderer.adaptive_rel_threshold = arel;
                }
                float aabs = (float)g_renderer.adaptive_abs_threshold;
                if (ImGui::InputFloat("Absolute std-error threshold", &aabs, 0.0f, 0.0f, "%.6f")) {
                    if (aabs < 0.0f) aabs = 0.0f;
                    g_renderer.adaptive_abs_threshold = aabs;
                }
                ImGui::TextDisabled("Adaptive stops sampling a pixel when estimated std-error is small.");
            }
        }

        ImGui::TextDisabled("Higher = cleaner but slower.");

        DrawSectionLabel("Resolution");

        static int res_w = g_camera.image_width;
        static int res_h = g_camera.image_height;

        // Common 16:9 presets + custom
        static const char* res_names[] = {
            "640 x 360",
            "854 x 480",
            "1280 x 720",
            "1600 x 900",
            "1920 x 1080",
            "2560 x 1440",
            "3840 x 2160",
            "Custom"
        };
        static const int res_vals[][2] = {
            {640,360}, {854,480}, {1280,720}, {1600,900}, {1920,1080}, {2560,1440}, {3840,2160}, {0,0}
        };
        // Default to 1920 x 1080 preset (index 4)
        static int res_preset = 4;

        if (ImGui::Combo("Resolution Preset", &res_preset, res_names, IM_ARRAYSIZE(res_names))) {
            if (res_preset >= 0 && res_preset < (int)(IM_ARRAYSIZE(res_names) - 1)) {
                res_w = res_vals[res_preset][0];
                res_h = res_vals[res_preset][1];
                // Apply immediately when user selects a preset
                g_camera.image_width  = res_w;
                g_camera.image_height = res_h;
            }
        }

        if (ImGui::InputInt("Width", &res_w)) {
            if (res_w < 16) res_w = 16;
            res_preset = -1;
            g_camera.image_width = res_w; // apply immediately for custom edits
        }
        if (ImGui::InputInt("Height", &res_h)) {
            if (res_h < 16) res_h = 16;
            res_preset = -1;
            g_camera.image_height = res_h; // apply immediately for custom edits
        }
        if (ImGui::Checkbox("Viewport matches Render Resolution", &s_viewport_match_render)) {
            // no immediate action; viewport will pick this up next frame
        }
        ImGui::TextDisabled("Changes take effect next render.");

        ImGui::Separator();
        ImGui::Text("Background Color");

        float bg[3] = {
            (float)g_camera.background.x(),
            (float)g_camera.background.y(),
            (float)g_camera.background.z()
        };

        if (ImGui::ColorEdit3("Sky / Background", bg)) {
            g_camera.background = colour(bg[0], bg[1], bg[2]);
            // background doesn't require world rebuild
        }

        DrawCausticSettings();
        ImGui::End();
        if (g_focus_properties) { ImGui::SetNextWindowFocus(); g_focus_properties = false; }
        ImGui::Begin("Properties");
        if (g_selected_light >= 0 && g_selected_light < int(g_scene.lights.size())) {
            DrawSelectedLight();
        } else {
        // Show mesh asset inspector if a mesh asset is selected
        if (g_selected_mesh_asset >= 0 && g_selected_mesh_asset < (int)g_scene.meshes.size()) {
            auto& mesh = g_scene.meshes[g_selected_mesh_asset];
            ImGui::Text("Mesh Asset: %s", mesh.name.c_str());
            ImGui::Separator();
            
            ImGui::TextWrapped("Configure default materials for this mesh. New instances will spawn with these materials.");
            ImGui::Spacing();
            
            ImGui::Text("Default Material Slots:");
            ImGui::Spacing();
            
            for (int slot = 0; slot < (int)mesh.slot_default_materials.size(); ++slot) {
                ImGui::PushID(slot);
                ImGui::Text("Slot %d:", slot);
                ImGui::SameLine();
                
                int& mat_idx = mesh.slot_default_materials[slot];
                std::string preview = (mat_idx >= 0 && mat_idx < (int)g_scene.materials.size()) 
                    ? g_scene.materials[mat_idx].name 
                    : "(none)";
                
                ImGui::SetNextItemWidth(200);
                if (ImGui::BeginCombo("##slot_mat", preview.c_str())) {
                        if (ImGui::Selectable("(none)", mat_idx == -1)) {
                            int old_idx = mat_idx;
                            mat_idx = -1;
                            g_world_dirty = true;
                            g_cached_world.reset();
                            g_material_thumb_cache.erase(old_idx);
                            SaveMeshMaterialDefaults();
                        }
                    for (int i = 0; i < (int)g_scene.materials.size(); ++i) {
                        bool is_selected = (i == mat_idx);
                        if (ImGui::Selectable(g_scene.materials[i].name.c_str(), is_selected)) {
                            int old_idx = mat_idx;
                            mat_idx = i;
                            g_world_dirty = true;
                            g_cached_world.reset();
                            g_material_thumb_cache.erase(old_idx);
                            g_material_thumb_cache.erase(i);
                            SaveMeshMaterialDefaults();
                        }
                        if (is_selected) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                
                // Drag-drop target for materials
                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("MATERIAL_ASSET_ID")) {
                        int dropped_mat = *(const int*)payload->Data;
                        if (dropped_mat >= 0 && dropped_mat < (int)g_scene.materials.size()) {
                            int old_idx = mat_idx;
                            int mesh_idx = g_selected_mesh_asset;
                            
                            // Undo support
                            UndoManager::Instance().push(std::make_unique<LambdaAction>(
                                [mesh_idx, slot, old_idx]() {
                                    if (mesh_idx >= 0 && mesh_idx < (int)g_scene.meshes.size()) {
                                        auto& m = g_scene.meshes[mesh_idx];
                                        if (slot < (int)m.slot_default_materials.size()) {
                                            m.slot_default_materials[slot] = old_idx;
                                            g_world_dirty = true;
                                            g_cached_world.reset();
                                            g_material_thumb_cache.erase(old_idx);
                                            SaveMeshMaterialDefaults();
                                        }
                                    }
                                },
                                [mesh_idx, slot, dropped_mat]() {
                                    if (mesh_idx >= 0 && mesh_idx < (int)g_scene.meshes.size()) {
                                        auto& m = g_scene.meshes[mesh_idx];
                                        if (slot < (int)m.slot_default_materials.size()) {
                                            m.slot_default_materials[slot] = dropped_mat;
                                            g_world_dirty = true;
                                            g_cached_world.reset();
                                            g_material_thumb_cache.erase(dropped_mat);
                                            SaveMeshMaterialDefaults();
                                        }
                                    }
                                },
                                "Assign Mesh Default Material"
                            ));
                            
                            mat_idx = dropped_mat;
                            g_world_dirty = true;
                            g_cached_world.reset();
                            g_material_thumb_cache.erase(old_idx);
                            g_material_thumb_cache.erase(dropped_mat);
                            SaveMeshMaterialDefaults();
                        }
                    }
                    ImGui::EndDragDropTarget();
                }
                
                ImGui::PopID();
            }
            
            if (ImGui::Button("Deselect Mesh")) {
                g_selected_mesh_asset = -1;
            }
        }
        else if (g_selected_object >= 0 &&
            g_selected_object < (int)g_scene.objects.size())
        {
            auto& obj = g_scene.objects[g_selected_object];
            ImGui::Text("Selected object:");
            // editable object name (persist across frames)
            static std::unordered_map<int, std::string> s_obj_name_bufs;
            int sel_idx = g_selected_object;
            auto& obj_name_buf = s_obj_name_bufs[sel_idx];
            if (obj_name_buf.empty()) {
                obj_name_buf = obj.name;
                obj_name_buf.resize(256, '\0');
            }
            std::string obj_label = std::string("Name##obj_") + std::to_string(sel_idx);
            if (ImGui::InputText(obj_label.c_str(), &obj_name_buf[0], obj_name_buf.size(), ImGuiInputTextFlags_EnterReturnsTrue)) {
                size_t len = std::strlen(obj_name_buf.c_str());
                obj_name_buf.resize(len);
                if (obj.name != obj_name_buf) {
                    std::string before = obj.name;
                    std::string after  = obj_name_buf;
                    int idx = sel_idx;
                    UndoManager::Instance().push(std::make_unique<LambdaAction>(
                        [idx, before]() {
                            if (idx >= 0 && idx < (int)g_scene.objects.size())
                                g_scene.objects[idx].name = before;
                        },
                        [idx, after]() {
                            if (idx >= 0 && idx < (int)g_scene.objects.size())
                                g_scene.objects[idx].name = after;
                        },
                        "Rename Object"
                    ));
                    obj.name = obj_name_buf;
                }
            }

            // Basic transform
            if (obj.type == scene_object_type::sphere) {
                point3 pos = obj.center;
                float pos_f[3] = { (float)pos.x(), (float)pos.y(), (float)pos.z() };
                if (ImGui::DragFloat3("Center", pos_f, 0.05f)) {
                    obj.center = point3(pos_f[0], pos_f[1], pos_f[2]);
                    g_world_dirty = true;
                    g_cached_world.reset();
                }
                if (ImGui::IsItemActivated()) {
                    int idx = g_selected_object;
                    if (idx >= 0) g_obj_snapshot_before[idx] = ObjSnapshot{obj.center, obj.translation, obj.rotation_deg, obj.scale, obj.radius};
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    int idx = g_selected_object;
                    if (idx >= 0) {
                        ObjSnapshot after{obj.center, obj.translation, obj.rotation_deg, obj.scale, obj.radius};
                        ObjSnapshot before = g_obj_snapshot_before[idx];
                        UndoManager::Instance().push(std::make_unique<LambdaAction>(
                            [idx, before]() {
                                if (idx >= 0 && idx < (int)g_scene.objects.size()) {
                                    auto& o = g_scene.objects[idx];
                                    o.center = before.center;
                                    o.translation = before.translation;
                                    o.rotation_deg = before.rotation_deg;
                                    o.scale = before.scale;
                                    o.radius = before.radius;
                                    g_world_dirty = true;
                                    g_cached_world.reset();
                                }
                            },
                            [idx, after]() {
                                if (idx >= 0 && idx < (int)g_scene.objects.size()) {
                                    auto& o = g_scene.objects[idx];
                                    o.center = after.center;
                                    o.translation = after.translation;
                                    o.rotation_deg = after.rotation_deg;
                                    o.scale = after.scale;
                                    o.radius = after.radius;
                                    g_world_dirty = true;
                                    g_cached_world.reset();
                                }
                            },
                            "Transform Object"
                        ));
                    }
                }
                if (ImGui::IsItemActivated()) {
                    int idx = g_selected_object;
                    if (idx >= 0) {
                        g_obj_snapshot_before[idx] = ObjSnapshot{obj.center, obj.translation, obj.rotation_deg, obj.scale, obj.radius};
                    }
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    int idx = g_selected_object;
                    if (idx >= 0) {
                        ObjSnapshot after{obj.center, obj.translation, obj.rotation_deg, obj.scale, obj.radius};
                        ObjSnapshot before = g_obj_snapshot_before[idx];
                        UndoManager::Instance().push(std::make_unique<LambdaAction>(
                            [idx, before]() {
                                if (idx >= 0 && idx < (int)g_scene.objects.size()) {
                                    auto& o = g_scene.objects[idx];
                                    o.center = before.center;
                                    o.translation = before.translation;
                                    o.rotation_deg = before.rotation_deg;
                                    o.scale = before.scale;
                                    o.radius = before.radius;
                                    g_world_dirty = true;
                                    g_cached_world.reset();
                                }
                            },
                            [idx, after]() {
                                if (idx >= 0 && idx < (int)g_scene.objects.size()) {
                                    auto& o = g_scene.objects[idx];
                                    o.center = after.center;
                                    o.translation = after.translation;
                                    o.rotation_deg = after.rotation_deg;
                                    o.scale = after.scale;
                                    o.radius = after.radius;
                                    g_world_dirty = true;
                                    g_cached_world.reset();
                                }
                            },
                            "Transform Object"
                        ));
                    }
                }

                float radius_f = (float)obj.radius;
                if (ImGui::DragFloat("Radius", &radius_f, 0.01f, 0.01f, 1000.0f)) {
                    obj.radius = radius_f;
                    g_world_dirty = true;
                    g_cached_world.reset();
                }
                
                // Rotation (degrees) for editor/raster preview (no effect on RT sphere geometry beyond transform)
                float rot_f[3] = { (float)obj.rotation_deg.x(), (float)obj.rotation_deg.y(), (float)obj.rotation_deg.z() };
                if (ImGui::DragFloat3("Rotation (deg)", rot_f, 1.0f)) {
                    obj.rotation_deg = vec3(rot_f[0], rot_f[1], rot_f[2]);
                    g_world_dirty = true;
                    g_cached_world.reset();
                }
                if (ImGui::IsItemActivated()) {
                    int idx = g_selected_object;
                    if (idx >= 0) g_obj_snapshot_before[idx] = ObjSnapshot{obj.center, obj.translation, obj.rotation_deg, obj.scale, obj.radius};
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    int idx = g_selected_object;
                    if (idx >= 0) {
                        ObjSnapshot after{obj.center, obj.translation, obj.rotation_deg, obj.scale, obj.radius};
                        ObjSnapshot before = g_obj_snapshot_before[idx];
                        UndoManager::Instance().push(std::make_unique<LambdaAction>(
                            [idx, before]() {
                                if (idx >= 0 && idx < (int)g_scene.objects.size()) {
                                    auto& o = g_scene.objects[idx];
                                    o.center = before.center;
                                    o.translation = before.translation;
                                    o.rotation_deg = before.rotation_deg;
                                    o.scale = before.scale;
                                    o.radius = before.radius;
                                    g_world_dirty = true;
                                    g_cached_world.reset();
                                }
                            },
                            [idx, after]() {
                                if (idx >= 0 && idx < (int)g_scene.objects.size()) {
                                    auto& o = g_scene.objects[idx];
                                    o.center = after.center;
                                    o.translation = after.translation;
                                    o.rotation_deg = after.rotation_deg;
                                    o.scale = after.scale;
                                    o.radius = after.radius;
                                    g_world_dirty = true;
                                    g_cached_world.reset();
                                }
                            },
                            "Transform Object"
                        ));
                    }
                }

                // Per-axis scale for preview
                float scale_fv[3] = { (float)obj.scale.x(), (float)obj.scale.y(), (float)obj.scale.z() };
                if (ImGui::DragFloat3("Scale", scale_fv, 0.01f)) {
                    obj.scale = vec3(scale_fv[0], scale_fv[1], scale_fv[2]);
                    g_world_dirty = true;
                    g_cached_world.reset();
                }
                if (ImGui::IsItemActivated()) {
                    int idx = g_selected_object;
                    if (idx >= 0) g_obj_snapshot_before[idx] = ObjSnapshot{obj.center, obj.translation, obj.rotation_deg, obj.scale, obj.radius};
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    int idx = g_selected_object;
                    if (idx >= 0) {
                        ObjSnapshot after{obj.center, obj.translation, obj.rotation_deg, obj.scale, obj.radius};
                        ObjSnapshot before = g_obj_snapshot_before[idx];
                        UndoManager::Instance().push(std::make_unique<LambdaAction>(
                            [idx, before]() {
                                if (idx >= 0 && idx < (int)g_scene.objects.size()) {
                                    auto& o = g_scene.objects[idx];
                                    o.center = before.center;
                                    o.translation = before.translation;
                                    o.rotation_deg = before.rotation_deg;
                                    o.scale = before.scale;
                                    o.radius = before.radius;
                                    g_world_dirty = true;
                                    g_cached_world.reset();
                                }
                            },
                            [idx, after]() {
                                if (idx >= 0 && idx < (int)g_scene.objects.size()) {
                                    auto& o = g_scene.objects[idx];
                                    o.center = after.center;
                                    o.translation = after.translation;
                                    o.rotation_deg = after.rotation_deg;
                                    o.scale = after.scale;
                                    o.radius = after.radius;
                                    g_world_dirty = true;
                                    g_cached_world.reset();
                                }
                            },
                            "Transform Object"
                        ));
                    }
                }
            }
            else if (obj.type == scene_object_type::cube) {
                // Cube inspector: centre + optional translation, rotation, scale
                point3 pos = obj.center;
                float pos_f[3] = { (float)pos.x(), (float)pos.y(), (float)pos.z() };
                if (ImGui::DragFloat3("Center", pos_f, 0.05f)) {
                    obj.center = point3(pos_f[0], pos_f[1], pos_f[2]);
                    g_world_dirty = true;
                    g_cached_world.reset();
                }

                float t[3] = {
                    (float)obj.translation.x(),
                    (float)obj.translation.y(),
                    (float)obj.translation.z()
                };
                if (ImGui::DragFloat3("Translation", t, 0.05f)) {
                    obj.translation = vec3(t[0], t[1], t[2]);
                    g_world_dirty = true;
                    g_cached_world.reset();
                }
                if (ImGui::IsItemActivated()) {
                    int idx = g_selected_object;
                    if (idx >= 0) g_obj_snapshot_before[idx] = ObjSnapshot{obj.center, obj.translation, obj.rotation_deg, obj.scale, obj.radius};
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    int idx = g_selected_object;
                    if (idx >= 0) {
                        ObjSnapshot after{obj.center, obj.translation, obj.rotation_deg, obj.scale, obj.radius};
                        ObjSnapshot before = g_obj_snapshot_before[idx];
                        UndoManager::Instance().push(std::make_unique<LambdaAction>(
                            [idx, before]() {
                                if (idx >= 0 && idx < (int)g_scene.objects.size()) {
                                    auto& o = g_scene.objects[idx];
                                    o.center = before.center;
                                    o.translation = before.translation;
                                    o.rotation_deg = before.rotation_deg;
                                    o.scale = before.scale;
                                    o.radius = before.radius;
                                    g_world_dirty = true;
                                    g_cached_world.reset();
                                }
                            },
                            [idx, after]() {
                                if (idx >= 0 && idx < (int)g_scene.objects.size()) {
                                    auto& o = g_scene.objects[idx];
                                    o.center = after.center;
                                    o.translation = after.translation;
                                    o.rotation_deg = after.rotation_deg;
                                    o.scale = after.scale;
                                    o.radius = after.radius;
                                    g_world_dirty = true;
                                    g_cached_world.reset();
                                }
                            },
                            "Transform Object"
                        ));
                    }
                }
                if (ImGui::IsItemActivated()) {
                    int idx = g_selected_object;
                    if (idx >= 0) g_obj_snapshot_before[idx] = ObjSnapshot{obj.center, obj.translation, obj.rotation_deg, obj.scale, obj.radius};
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    int idx = g_selected_object;
                    if (idx >= 0) {
                        ObjSnapshot after{obj.center, obj.translation, obj.rotation_deg, obj.scale, obj.radius};
                        ObjSnapshot before = g_obj_snapshot_before[idx];
                        UndoManager::Instance().push(std::make_unique<LambdaAction>(
                            [idx, before]() {
                                if (idx >= 0 && idx < (int)g_scene.objects.size()) {
                                    auto& o = g_scene.objects[idx];
                                    o.center = before.center;
                                    o.translation = before.translation;
                                    o.rotation_deg = before.rotation_deg;
                                    o.scale = before.scale;
                                    o.radius = before.radius;
                                    g_world_dirty = true;
                                    g_cached_world.reset();
                                }
                            },
                            [idx, after]() {
                                if (idx >= 0 && idx < (int)g_scene.objects.size()) {
                                    auto& o = g_scene.objects[idx];
                                    o.center = after.center;
                                    o.translation = after.translation;
                                    o.rotation_deg = after.rotation_deg;
                                    o.scale = after.scale;
                                    o.radius = after.radius;
                                    g_world_dirty = true;
                                    g_cached_world.reset();
                                }
                            },
                            "Transform Object"
                        ));
                    }
                }

                // Rotation
                float rot_c[3] = { (float)obj.rotation_deg.x(), (float)obj.rotation_deg.y(), (float)obj.rotation_deg.z() };
                if (ImGui::DragFloat3("Rotation (deg)", rot_c, 1.0f)) {
                    obj.rotation_deg = vec3(rot_c[0], rot_c[1], rot_c[2]);
                    g_world_dirty = true;
                    g_cached_world.reset();
                }
                if (ImGui::IsItemActivated()) {
                    int idx = g_selected_object;
                    if (idx >= 0) g_obj_snapshot_before[idx] = ObjSnapshot{obj.center, obj.translation, obj.rotation_deg, obj.scale, obj.radius};
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    int idx = g_selected_object;
                    if (idx >= 0) {
                        ObjSnapshot after{obj.center, obj.translation, obj.rotation_deg, obj.scale, obj.radius};
                        ObjSnapshot before = g_obj_snapshot_before[idx];
                        UndoManager::Instance().push(std::make_unique<LambdaAction>(
                            [idx, before]() {
                                if (idx >= 0 && idx < (int)g_scene.objects.size()) {
                                    auto& o = g_scene.objects[idx];
                                    o.center = before.center;
                                    o.translation = before.translation;
                                    o.rotation_deg = before.rotation_deg;
                                    o.scale = before.scale;
                                    o.radius = before.radius;
                                    g_world_dirty = true;
                                    g_cached_world.reset();
                                }
                            },
                            [idx, after]() {
                                if (idx >= 0 && idx < (int)g_scene.objects.size()) {
                                    auto& o = g_scene.objects[idx];
                                    o.center = after.center;
                                    o.translation = after.translation;
                                    o.rotation_deg = after.rotation_deg;
                                    o.scale = after.scale;
                                    o.radius = after.radius;
                                    g_world_dirty = true;
                                    g_cached_world.reset();
                                }
                            },
                            "Transform Object"
                        ));
                    }
                }

                // Scale
                float scl_c[3] = { (float)obj.scale.x(), (float)obj.scale.y(), (float)obj.scale.z() };
                if (ImGui::DragFloat3("Scale", scl_c, 0.01f)) {
                    obj.scale = vec3(scl_c[0], scl_c[1], scl_c[2]);
                    g_world_dirty = true;
                    g_cached_world.reset();
                }
                if (ImGui::IsItemActivated()) {
                    int idx = g_selected_object;
                    if (idx >= 0) g_obj_snapshot_before[idx] = ObjSnapshot{obj.center, obj.translation, obj.rotation_deg, obj.scale, obj.radius};
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    int idx = g_selected_object;
                    if (idx >= 0) {
                        ObjSnapshot after{obj.center, obj.translation, obj.rotation_deg, obj.scale, obj.radius};
                        ObjSnapshot before = g_obj_snapshot_before[idx];
                        UndoManager::Instance().push(std::make_unique<LambdaAction>(
                            [idx, before]() {
                                if (idx >= 0 && idx < (int)g_scene.objects.size()) {
                                    auto& o = g_scene.objects[idx];
                                    o.center = before.center;
                                    o.translation = before.translation;
                                    o.rotation_deg = before.rotation_deg;
                                    o.scale = before.scale;
                                    o.radius = before.radius;
                                    g_world_dirty = true;
                                    g_cached_world.reset();
                                }
                            },
                            [idx, after]() {
                                if (idx >= 0 && idx < (int)g_scene.objects.size()) {
                                    auto& o = g_scene.objects[idx];
                                    o.center = after.center;
                                    o.translation = after.translation;
                                    o.rotation_deg = after.rotation_deg;
                                    o.scale = after.scale;
                                    o.radius = after.radius;
                                    g_world_dirty = true;
                                    g_cached_world.reset();
                                }
                            },
                            "Transform Object"
                        ));
                    }
                }
            }
            else if (obj.type == scene_object_type::mesh_instance) {
                float t[3] = {
                    (float)obj.translation.x(),
                    (float)obj.translation.y(),
                    (float)obj.translation.z()
                };
                if (ImGui::DragFloat3("Translation", t, 0.05f)) {
                    obj.translation = vec3(t[0], t[1], t[2]);
                    g_world_dirty = true;
                    g_cached_world.reset();
                }

                // Rotation
                float rot_m[3] = { (float)obj.rotation_deg.x(), (float)obj.rotation_deg.y(), (float)obj.rotation_deg.z() };
                if (ImGui::DragFloat3("Rotation (deg)", rot_m, 1.0f)) {
                    obj.rotation_deg = vec3(rot_m[0], rot_m[1], rot_m[2]);
                    g_world_dirty = true;
                    g_cached_world.reset();
                }
                if (ImGui::IsItemActivated()) {
                    int idx = g_selected_object;
                    if (idx >= 0) g_obj_snapshot_before[idx] = ObjSnapshot{obj.center, obj.translation, obj.rotation_deg, obj.scale, obj.radius};
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    int idx = g_selected_object;
                    if (idx >= 0) {
                        ObjSnapshot after{obj.center, obj.translation, obj.rotation_deg, obj.scale, obj.radius};
                        ObjSnapshot before = g_obj_snapshot_before[idx];
                        UndoManager::Instance().push(std::make_unique<LambdaAction>(
                            [idx, before]() {
                                if (idx >= 0 && idx < (int)g_scene.objects.size()) {
                                    auto& o = g_scene.objects[idx];
                                    o.center = before.center;
                                    o.translation = before.translation;
                                    o.rotation_deg = before.rotation_deg;
                                    o.scale = before.scale;
                                    o.radius = before.radius;
                                    g_world_dirty = true;
                                    g_cached_world.reset();
                                }
                            },
                            [idx, after]() {
                                if (idx >= 0 && idx < (int)g_scene.objects.size()) {
                                    auto& o = g_scene.objects[idx];
                                    o.center = after.center;
                                    o.translation = after.translation;
                                    o.rotation_deg = after.rotation_deg;
                                    o.scale = after.scale;
                                    o.radius = after.radius;
                                    g_world_dirty = true;
                                    g_cached_world.reset();
                                }
                            },
                            "Transform Object"
                        ));
                    }
                }

                // Scale (non-uniform)
                float scl_m[3] = { (float)obj.scale.x(), (float)obj.scale.y(), (float)obj.scale.z() };
                if (ImGui::DragFloat3("Scale", scl_m, 0.01f)) {
                    obj.scale = vec3(scl_m[0], scl_m[1], scl_m[2]);
                    g_world_dirty = true;
                    g_cached_world.reset();
                }
                if (ImGui::IsItemActivated()) {
                    int idx = g_selected_object;
                    if (idx >= 0) g_obj_snapshot_before[idx] = ObjSnapshot{obj.center, obj.translation, obj.rotation_deg, obj.scale, obj.radius};
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    int idx = g_selected_object;
                    if (idx >= 0) {
                        ObjSnapshot after{obj.center, obj.translation, obj.rotation_deg, obj.scale, obj.radius};
                        ObjSnapshot before = g_obj_snapshot_before[idx];
                        UndoManager::Instance().push(std::make_unique<LambdaAction>(
                            [idx, before]() {
                                if (idx >= 0 && idx < (int)g_scene.objects.size()) {
                                    auto& o = g_scene.objects[idx];
                                    o.center = before.center;
                                    o.translation = before.translation;
                                    o.rotation_deg = before.rotation_deg;
                                    o.scale = before.scale;
                                    o.radius = before.radius;
                                    g_world_dirty = true;
                                    g_cached_world.reset();
                                }
                            },
                            [idx, after]() {
                                if (idx >= 0 && idx < (int)g_scene.objects.size()) {
                                    auto& o = g_scene.objects[idx];
                                    o.center = after.center;
                                    o.translation = after.translation;
                                    o.rotation_deg = after.rotation_deg;
                                    o.scale = after.scale;
                                    o.radius = after.radius;
                                    g_world_dirty = true;
                                    g_cached_world.reset();
                                }
                            },
                            "Transform Object"
                        ));
                    }
                }
            }

            // Mesh asset handle (for slots)
            bool is_mesh_instance = (obj.type == scene_object_type::mesh_instance);
            scene_mesh_asset* mesh_asset = nullptr;
            if (is_mesh_instance &&
                obj.mesh_index >= 0 &&
                obj.mesh_index < (int)g_scene.meshes.size())
            {
                mesh_asset = &g_scene.meshes[obj.mesh_index];

                // keep overrides in sync with slot count
                if (obj.mesh_slot_materials.size() != mesh_asset->slot_names.size()) {
                    obj.mesh_slot_materials.assign(mesh_asset->slot_names.size(), -1);
                    g_world_dirty = true;
                    g_cached_world.reset();
                }
            }

            ImGui::Separator();
            ImGui::Text("Material Binding");

            // Button to create new materials
            if (ImGui::Button("Create New Material")) {
                scene_material m;
                m.name = "Material " + std::to_string(g_scene.materials.size());
                g_scene.materials.push_back(m);
                int new_mat_idx = (int)g_scene.materials.size() - 1;
                scene_material snapshot = g_scene.materials[new_mat_idx];
                // push undo: remove on undo, re-insert on redo
                UndoManager::Instance().push(std::make_unique<LambdaAction>(
                    [new_mat_idx]() {
                        if (new_mat_idx >= 0 && new_mat_idx < (int)g_scene.materials.size()) {
                            g_scene.materials.erase(g_scene.materials.begin() + new_mat_idx); InvalidateAllMaterialThumbnails();
                            g_world_dirty = true;
                            g_cached_world.reset();
                        }
                    },
                    [new_mat_idx, snapshot]() {
                        int insert_at = new_mat_idx;
                        if (insert_at < 0) insert_at = 0;
                        if (insert_at > (int)g_scene.materials.size()) insert_at = (int)g_scene.materials.size();
                        g_scene.materials.insert(g_scene.materials.begin() + insert_at, snapshot); InvalidateAllMaterialThumbnails();
                        g_world_dirty = true;
                        g_cached_world.reset();
                    },
                    "Create Material"
                ));

                g_world_dirty = true;
                g_cached_world.reset();
            }

            // --- Mesh instance: per-slot binding ---
            if (is_mesh_instance && mesh_asset && !mesh_asset->slot_names.empty())
            {
                ImGui::Text("Mesh material slots:");

                for (size_t s = 0; s < mesh_asset->slot_names.size(); ++s) {
                    std::string slot_label = "Slot " + std::to_string(s) +
                                             " (" + mesh_asset->slot_names[s] + ")";
                    int mat_idx = (int)((s < obj.mesh_slot_materials.size()) ?
                                        obj.mesh_slot_materials[s] : -1);

                    const char* current_label = "<none>";
                    if (mat_idx >= 0 && mat_idx < (int)g_scene.materials.size()) {
                        current_label = g_scene.materials[mat_idx].name.c_str();
                    }

                    ImGui::TextUnformatted(slot_label.c_str());
                    ImGui::SameLine();

                    std::string combo_id = "##slot_mat_" + std::to_string(s);
                    if (ImGui::BeginCombo(combo_id.c_str(), current_label)) {
                        // None
                        bool sel_none = (mat_idx == -1);
                        if (ImGui::Selectable("<none>", sel_none)) {
                            mat_idx = -1;
                        }
                        if (sel_none) ImGui::SetItemDefaultFocus();

                        // existing materials
                        for (int i = 0; i < (int)g_scene.materials.size(); ++i) {
                            bool sel = (i == mat_idx);
                            std::string item = g_scene.materials[i].name +
                                               "##slotitem_" +
                                               std::to_string(s) + "_" +
                                               std::to_string(i);
                            if (ImGui::Selectable(item.c_str(), sel)) {
                                mat_idx = i;
                            }
                            if (sel) ImGui::SetItemDefaultFocus();
                        }

                        ImGui::EndCombo();
                    }
                    
                    // Drag-and-drop target for materials from asset viewer
                    if (ImGui::BeginDragDropTarget()) {
                        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("MATERIAL_ASSET_ID")) {
                            int dropped_mat_idx = *(const int*)payload->Data;
                            if (dropped_mat_idx >= 0 && dropped_mat_idx < (int)g_scene.materials.size()) {
                                int old_mat_idx = mat_idx;
                                mat_idx = dropped_mat_idx;
                                
                                // Undo support
                                size_t slot_copy = s;
                                int obj_idx = g_selected_object;
                                UndoManager::Instance().push(std::make_unique<LambdaAction>(
                                    [obj_idx, slot_copy, old_mat_idx]() {
                                        if (obj_idx >= 0 && obj_idx < (int)g_scene.objects.size()) {
                                            auto& o = g_scene.objects[obj_idx];
                                            if (slot_copy < o.mesh_slot_materials.size()) {
                                                o.mesh_slot_materials[slot_copy] = old_mat_idx;
                                                g_world_dirty = true; g_cached_world.reset();
                                            }
                                        }
                                    },
                                    [obj_idx, slot_copy, dropped_mat_idx]() {
                                        if (obj_idx >= 0 && obj_idx < (int)g_scene.objects.size()) {
                                            auto& o = g_scene.objects[obj_idx];
                                            if (slot_copy < o.mesh_slot_materials.size()) {
                                                o.mesh_slot_materials[slot_copy] = dropped_mat_idx;
                                                g_world_dirty = true; g_cached_world.reset();
                                            }
                                        }
                                    },
                                    "Assign Material to Slot"
                                ));
                            }
                        }
                        ImGui::EndDragDropTarget();
                    }

                    if (s < obj.mesh_slot_materials.size()) {
                        if (obj.mesh_slot_materials[s] != mat_idx) {
                            obj.mesh_slot_materials[s] = mat_idx;
                            g_world_dirty = true;
                            g_cached_world.reset();
                        }
                    }
                }

                if (!mesh_asset->slot_names.empty()) {
                    ImGui::Separator();
                    ImGui::TextDisabled("Material parameters are edited from the Content Drawer.");
                    ImGui::TextDisabled("Select the material asset there to change its shader settings.");
                }
            }
            // --- Spheres / non-mesh: single material index as before ---
            else
            {
                int current_mat = obj.material_index;
                if (current_mat < 0 || current_mat >= (int)g_scene.materials.size()) {
                    current_mat = -1;
                }

                const char* current_label = "<none>";
                if (current_mat >= 0) {
                    current_label = g_scene.materials[current_mat].name.c_str();
                }

                if (ImGui::BeginCombo("Material", current_label)) {
                    for (int i = 0; i < (int)g_scene.materials.size(); ++i) {
                        bool is_sel = (i == current_mat);
                        std::string label = g_scene.materials[i].name +
                                            "##mat_" + std::to_string(i);
                        if (ImGui::Selectable(label.c_str(), is_sel)) {
                            obj.material_index = i;
                            current_mat        = i;
                            g_world_dirty      = true;
                            g_cached_world.reset();
                        }
                        if (is_sel) {
                            ImGui::SetItemDefaultFocus();
                        }
                    }
                    ImGui::EndCombo();
                }

                if (obj.material_index >= 0 &&
                    obj.material_index < (int)g_scene.materials.size())
                {
                    ImGui::TextDisabled("Material parameters are edited from the Content Drawer.");
                    ImGui::TextDisabled("Select '%s' there to edit it.", g_scene.materials[obj.material_index].name.c_str());
                }
                else {
                    ImGui::TextDisabled("Object has no valid material bound.");
                }
            }
        }
        else {
            ImGui::TextDisabled("No object selected.");
        }

        }
        ImGui::End();

        // ---------------------------------------------------------------------
        // Material Editor (for materials selected in Assets window)
        // ---------------------------------------------------------------------
        if (g_selected_material >= 0 && g_selected_material < (int)g_scene.materials.size()) {
            bool material_editor_open = true;
            ImGui::SetNextWindowSize(ImVec2(1340.0f, 820.0f), ImGuiCond_FirstUseEver);
            ImGui::Begin("Material Editor", &material_editor_open);
            if (!material_editor_open) {
                g_selected_material = -1;
            } else {
                DrawPanelTitle("Material Editor", "Node-based editing for the selected asset material.");
                ImGui::Text("Editing Material: %s", g_scene.materials[g_selected_material].name.c_str());
                ImGui::Separator();

                bool material_deleted = false;
                if (ImGui::BeginTable("MaterialEditorLayout", 2,
                                      ImGuiTableFlags_Resizable |
                                      ImGuiTableFlags_BordersInnerV |
                                      ImGuiTableFlags_SizingStretchProp))
                {
                    ImGui::TableSetupColumn("Details", ImGuiTableColumnFlags_WidthFixed, 360.0f);
                    ImGui::TableSetupColumn("Graph", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableNextRow();

                    ImGui::TableSetColumnIndex(0);
                    ImGui::BeginChild("MaterialEditorDetails", ImVec2(0.0f, 0.0f), false);
                    if (g_selected_material >= 0 && g_selected_material < (int)g_scene.materials.size()) {
                        auto& mat = g_scene.materials[g_selected_material];
                        DrawMaterialGraphDetails(mat, g_scene, g_selected_material);
                        DrawMaterialInspector(mat, g_scene, g_selected_material, false);
                        ImGui::Separator();
                        if (ImGui::Button("Delete Material")) {
                            int del_idx = g_selected_material;
                            scene_material snapshot = mat;

                            UndoManager::Instance().push(std::make_unique<LambdaAction>(
                                [del_idx]() {
                                    if (del_idx >= 0 && del_idx < (int)g_scene.materials.size()) {
                                        g_scene.materials.erase(g_scene.materials.begin() + del_idx); InvalidateAllMaterialThumbnails();
                                        if (g_selected_material == del_idx) g_selected_material = -1;
                                        g_world_dirty = true; g_cached_world.reset();
                                    }
                                },
                                [del_idx, snapshot]() {
                                    if (del_idx < 0) return;
                                    int insert_at = del_idx;
                                    if (insert_at > (int)g_scene.materials.size()) insert_at = (int)g_scene.materials.size();
                                    g_scene.materials.insert(g_scene.materials.begin() + insert_at, snapshot); InvalidateAllMaterialThumbnails();
                                    g_selected_material = insert_at;
                                    g_world_dirty = true; g_cached_world.reset();
                                },
                                "Delete Material"
                            ));

                            g_scene.materials.erase(g_scene.materials.begin() + del_idx); InvalidateAllMaterialThumbnails();
                            g_selected_material = -1;
                            g_world_dirty = true;
                            g_cached_world.reset();
                            MarkMaterialsDirty();
                            material_deleted = true;

                            // Invalidate thumbnail cache for this material
                            if (g_material_thumb_cache.find(del_idx) != g_material_thumb_cache.end()) {
                                GLuint tex = g_material_thumb_cache[del_idx];
                                if (tex) glDeleteTextures(1, &tex);
                                g_material_thumb_cache.erase(del_idx);
                            }
                        }
                    }
                    ImGui::EndChild();

                    ImGui::TableSetColumnIndex(1);
                    ImGui::BeginChild("MaterialEditorGraph", ImVec2(0.0f, 0.0f), false);
                    if (!material_deleted &&
                        g_selected_material >= 0 &&
                        g_selected_material < (int)g_scene.materials.size())
                    {
                        DrawMaterialGraphEditor(g_scene.materials[g_selected_material], g_scene, g_selected_material);
                    } else {
                        ImGui::TextDisabled("No material selected.");
                    }
                    ImGui::EndChild();

                    ImGui::EndTable();
                }
            }
            ImGui::End();
        }

        // ---------------------------------------------------------------------
        // Debug Camera
        // ---------------------------------------------------------------------
        ImGui::Begin("Debug Camera");
        DrawPanelTitle("Diagnostics", "Live editor camera and renderer state.");
        ImGui::Text("Editor cam position:");
        ImGui::Text("  x = %.3f", g_editor_cam.position.x());
        ImGui::Text("  y = %.3f", g_editor_cam.position.y());
        ImGui::Text("  z = %.3f", g_editor_cam.position.z());
        ImGui::Separator();
        ImGui::Text("Yaw   = %.3f", g_editor_cam.yaw);
        ImGui::Text("Pitch = %.3f", g_editor_cam.pitch);
        ImGui::Separator();
        ImGui::Text("Viewport focused: %s", g_viewport_focused ? "true" : "false");
        ImGui::Text("Viewport hovered: %s", g_viewport_hovered ? "true" : "false");
        ImGui::Separator();
        ImGui::Text("Samples per pixel: %d", g_camera.samples_per_pixel);
        ImGui::Text("Max depth:         %d", g_camera.max_depth);
        ImGui::Separator();
        ImGui::Text("World dirty: %s", g_world_dirty ? "true" : "false");
        ImGui::Separator();
        ImGui::Text("Render in progress: %s", g_render_in_progress ? "true" : "false");
        ImGui::End();

        // ---------------------------------------------------------------------
        // Viewport
        // ---------------------------------------------------------------------
        ImGui::Begin("Viewport");

        g_viewport_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        g_viewport_hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows);

        char viewport_res_chip[64];
        char viewport_quality_chip[64];
        std::snprintf(viewport_res_chip, sizeof(viewport_res_chip), "%dx%d output", g_camera.image_width, g_camera.image_height);
        std::snprintf(viewport_quality_chip, sizeof(viewport_quality_chip), "%d spp | depth %d", g_camera.samples_per_pixel, g_camera.max_depth);
        auto start_viewport_render = [&](const ImVec2& render_area) {
                // Sync RT camera from editor view. If the user requested the
                // viewport to match the configured render resolution, sync
                // using the camera's image size so the ray tracer uses that
                // resolution instead of the current ImGui viewport size.
                if (s_viewport_match_render) {
                    sync_camera_from_editor((float)g_camera.image_width, (float)g_camera.image_height);
                } else {
                    sync_camera_from_editor(render_area.x, render_area.y);
                }

                // Build world only if needed
                const bool needs_cpu_world = g_render_selection.backend == render_backend::cpu || g_camera.enable_mnee;
                if (needs_cpu_world && (!g_cached_world || g_world_dirty)) {
                    auto new_world = std::make_shared<hittable_list>(
                        build_world_from_scene(g_scene)
                    );
                    g_cached_world = new_world;
                    g_world_dirty  = false;
                    
                    // Populate emissive surfaces for area-light sampling
                    build_emissive_surfaces(g_scene, g_camera);
                }

                // Reset progress struct (renderer will set tile totals if used)
                g_render_progress.total_scanlines = g_camera.image_height;
                g_render_progress.completed_scanlines.store(0);
                g_render_progress.total_tiles = 0;
                g_render_progress.completed_tiles.store(0);
                g_render_progress.eta_seconds.store(0.0);
                g_render_progress.elapsed_seconds.store(0.0);

                g_cancel_flag.store(false);
                g_render_in_progress = true;

                g_progress_popup_pending = false;

                // Kick off worker thread
                auto world_copy = needs_cpu_world ? g_cached_world : nullptr;
                camera cam_copy = g_camera;
                renderer settings_copy = g_renderer;
                render_selection selection_copy = g_render_selection;
                auto scene_copy = std::make_shared<const scene>(g_scene);
                {
                    std::lock_guard<std::mutex> lock(g_render_mutex);
                    g_render_result = {};
                    g_render_has_result.store(false);
                    g_render_diagnostic = selection_copy.backend == render_backend::cpu
                        ? "Rendering with CPU." : "Preparing GPU ray tracing...";
                }
                g_render_final_image_ready.store(false);

                // Safety check: ensure world is valid before starting render
                if (needs_cpu_world && !world_copy) {
                    std::fprintf(stderr, "ERROR: Cannot start render - world is null!\n");
                    g_render_in_progress = false;
                    g_cancel_flag.store(false);
                    g_progress_popup_pending = false;
                } else {
                    // Defensive: join any previous thread before starting new one
                    if (g_render_thread.joinable()) {
                        std::fprintf(stderr, "Warning: joining previous render thread before starting new one\n");
                        try {
                            g_render_thread.join();
                        } catch (...) {
                            std::fprintf(stderr, "Exception while joining previous render thread\n");
                        }
                    }

                    g_render_thread = std::thread([world_copy, cam_copy, settings_copy, selection_copy, scene_copy]() mutable {
                        try {
                            size_t pixel_count = (size_t)cam_copy.image_width * (size_t)cam_copy.image_height;
                            
                            // Build caustics photon map once before render. Build when
                            // we have either a sun or point lights so point-light
                            // caustics are captured even if the sun is disabled.
                            if (cam_copy.enable_mnee && (cam_copy.use_sun || !cam_copy.point_lights.empty())) {
                                caustics_config cfg;
                                cfg.photon_count   = (int)std::clamp<size_t>(pixel_count / 3, 150000, 750000);
                                cfg.max_bounces    = std::max(2, std::min(8, cam_copy.max_depth));
                                cfg.deposit_radius = 0.18;
                                cfg.intensity_scale= 100.0;
                                cfg.knn_k          = 0;

                                int caustic_emitters = (cam_copy.use_sun ? 1 : 0) + (!cam_copy.point_lights.empty() ? 1 : 0);
                                int sun_budget = cam_copy.use_sun ? std::max(1, cfg.photon_count / std::max(1, caustic_emitters)) : 0;
                                int point_budget = !cam_copy.point_lights.empty() ? std::max(1, cfg.photon_count - sun_budget) : 0;

                                photon_map pm;

                                // Build sun caustics if enabled
                                if (cam_copy.use_sun) {
                                    caustics_config sun_cfg = cfg;
                                    sun_cfg.photon_count = sun_budget;
                                    build_sun_caustics(cam_copy.sun_dir, cam_copy.sun_radiance,
                                                       *world_copy, sun_cfg, pm);
                                }

                                // Also emit photons from point lights so they contribute
                                // to caustics (MNEE/photon capture). Share the budget
                                // equally across point lights to keep it simple.
                                if (!cam_copy.point_lights.empty()) {
                                    int npl = (int)cam_copy.point_lights.size();
                                    // Cap per-light photons so point-light caustics don't dominate render startup.
                                    int photons_per_pl = std::min(75000, std::max(1, point_budget / npl));

                                    for (const auto& pl : cam_copy.point_lights) {
                                        for (int pi = 0; pi < photons_per_pl; ++pi) {
                                            // Sample a random direction (uniform on sphere)
                                            double z = 1.0 - 2.0 * random_double();
                                            double r = std::sqrt(std::max(0.0, 1.0 - z*z));
                                            double phi = 2.0 * M_PI * random_double();
                                            double x = r * std::cos(phi);
                                            double y = r * std::sin(phi);
                                            vec3 dir = unit_vector(vec3((float)x, (float)y, (float)z));

                                            ray r0(pl.position, dir, 0.0);
                                            colour throughput = pl.radiance * (float)(1.0 / std::max(1, photons_per_pl));

                                            bool saw_dielectric = false;
                                            int spec_events = 0;
                                            for (int b = 0; b < cfg.max_bounces; ++b) {
                                                hit_record rec;
                                                if (!world_copy->hit(r0, interval(0.001, infinity), rec)) break;

                                                if (!rec.mat || (!rec.mat->is_specular())) {
                                                    if (saw_dielectric && spec_events >= cfg.min_specular_events) {
                                                        photon ph;
                                                        ph.pos = rec.p;
                                                        ph.dir = unit_vector(r0.direction());
                                                        ph.power = throughput;
                                                        pm.insert(ph);
                                                    }
                                                    break; // terminate on diffuse
                                                }

                                                // specular event
                                                ray scattered;
                                                colour atten;
                                                if (!rec.mat->scatter(r0, rec, atten, scattered)) break;
                                                throughput = throughput * atten;
                                                if (rec.mat && rec.mat->is_dielectric()) saw_dielectric = true;
                                                ++spec_events;
                                                r0 = scattered;
                                            }
                                        }
                                    }
                                }

                                cam_copy.set_caustics(pm, cfg.deposit_radius);
                            }
                            
                            // Sanity check render dimensions to avoid massive allocations
                            if (cam_copy.image_width <= 0 || cam_copy.image_height <= 0) {
                                std::fprintf(stderr, "[ERROR] Invalid image dimensions: %dx%d\n", 
                                    cam_copy.image_width, cam_copy.image_height);
                                throw std::runtime_error("Invalid render dimensions");
                            }
                            if (cam_copy.image_width > 8192 || cam_copy.image_height > 8192) {
                                std::fprintf(stderr, "[ERROR] Image dimensions too large: %dx%d (max 8192x8192)\n",
                                    cam_copy.image_width, cam_copy.image_height);
                                throw std::runtime_error("Render dimensions exceed maximum");
                            }
                            
                            std::string diagnostic;
                            render_result img =
                                render_selected(selection_copy, *scene_copy, world_copy.get(), cam_copy, settings_copy,
                                                  &g_cancel_flag, &g_render_progress,
                                                  // progress callback: write partial image into shared result
                                                  [](const render_result& partial) {
                                                      try {
                                                          std::lock_guard<std::mutex> lock(g_render_mutex);
                                                          g_render_result = partial;
                                                          g_render_has_result = true;
                                                      } catch (const std::exception& e) {
                                                          std::fprintf(stderr, "[ERROR] Progress callback exception: %s\n", e.what());
                                                      } catch (...) {
                                                          std::fprintf(stderr, "[ERROR] Progress callback unknown exception\n");
                                                      }
                                                  }, diagnostic, [](const std::string& status) {
                                                      std::lock_guard<std::mutex> lock(g_render_mutex);
                                                      g_render_diagnostic = status;
                                                  });

                            // final result: store and mark final-ready
                            {
                                std::lock_guard<std::mutex> lock(g_render_mutex);
                                g_render_result = std::move(img);
                                g_render_diagnostic = g_cancel_flag.load() ? "Render cancelled." : diagnostic;
                                g_render_has_result = true;
                                g_render_final_image_ready.store(true);
                            }
                        } catch (const std::bad_alloc& e) {
                            std::lock_guard<std::mutex> lock(g_render_mutex);
                            g_render_diagnostic = "Render failed: insufficient memory.";
                            std::fprintf(stderr, "\n!!! FATAL: Memory allocation failed in render thread !!!\n");
                            std::fprintf(stderr, "[EXCEPTION] bad_alloc: %s\n", e.what());
                            std::fprintf(stderr, "[CRASH] Likely ran out of memory during photon map or render buffer allocation\n");
                        } catch (const std::exception& e) {
                            std::lock_guard<std::mutex> lock(g_render_mutex);
                            g_render_diagnostic = std::string("Render failed: ") + e.what();
                            std::fprintf(stderr, "\n!!! FATAL: Exception in render thread !!!\n");
                            std::fprintf(stderr, "[EXCEPTION] Type: std::exception\n");
                            std::fprintf(stderr, "[EXCEPTION] what(): %s\n", e.what());
                        } catch (...) {
                            std::lock_guard<std::mutex> lock(g_render_mutex);
                            g_render_diagnostic = "Render failed with an unknown error.";
                            std::fprintf(stderr, "\n!!! FATAL: Unknown exception in render thread !!!\n");
                            std::fprintf(stderr, "[CRASH] Caught non-standard exception (possible access violation or segfault)\n");
                        }
                        // Completion must reach the UI on errors and cancellation too.
                        g_render_final_image_ready.store(true);
                        g_render_has_result.store(true);
                    });
                }
        };

        bool request_render = false;
        // Device discovery does not stall a frame or require a Vulkan driver.
        static auto discovery = std::async(std::launch::async, enumerate_render_devices);
        static render_device_list devices;
        static bool devices_ready = false;
        if (!devices_ready && discovery.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            devices = discovery.get();
            devices_ready = true;
        }
        std::string device_label = "CPU";
        for (const auto& device : devices.devices)
            if (g_render_selection.backend == render_backend::vulkan && device.id == g_render_selection.device_id)
                device_label = device.name;
        ImGui::BeginDisabled(g_render_in_progress);
        ImGui::SetNextItemWidth(250);
        if (ImGui::BeginCombo("##RenderDevice", device_label.c_str())) {
            if (ImGui::Selectable("CPU", g_render_selection.backend == render_backend::cpu))
                g_render_selection = {};
            for (const auto& device : devices.devices) {
                ImGui::BeginDisabled(!device.compatible);
                if (ImGui::Selectable(device.name.c_str(), g_render_selection.device_id == device.id))
                    g_render_selection = {render_backend::vulkan, device.id};
                ImGui::EndDisabled();
                if (!device.compatible && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("%s", device.reason.c_str());
            }
            if (!devices_ready) ImGui::TextDisabled("Detecting GPUs...");
            else if (!devices.diagnostic.empty()) ImGui::TextWrapped("%s", devices.diagnostic.c_str());
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f, 4.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 6.0f));

        if (!g_render_in_progress) {
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.22f, 0.55f, 0.57f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.29f, 0.68f, 0.70f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.18f, 0.45f, 0.47f, 1.00f));
            request_render = ImGui::Button("Render");
            ImGui::PopStyleColor(3);
        } else {
            ImGui::BeginDisabled();
            ImGui::Button("Rendering");
            ImGui::EndDisabled();
        }

        ImGui::SameLine();
        if (g_render_in_progress) {
            if (ImGui::Button("Cancel")) {
                g_cancel_flag.store(true);
                // close the progress window immediately on cancel
                stop_progress_window_thread();
                // cancel any pending popup
                g_progress_popup_pending = false;
            }
        } else {
            ImGui::TextDisabled("RMB look | WASD move");
        }

        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Mode");
        ImGui::SameLine();
        ImGui::RadioButton("RT##ViewportMode", &g_viewport_mode, 0);
        ImGui::SameLine();
        ImGui::RadioButton("Raster##ViewportMode", &g_viewport_mode, 1);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Live materials and shadows. Use Render for full reflections, glass and indirect lighting.");
        if (g_viewport_mode == 1 && !raster_preview::material_error.empty())
            ImGui::TextColored(ImVec4(1,.6f,.3f,1),"%s",raster_preview::material_error.c_str());
        ImGui::PopStyleVar(2);

        DrawInfoChip(viewport_res_chip);
        ImGui::SameLine();
        DrawInfoChip(viewport_quality_chip);
        ImGui::SameLine();
        DrawInfoChip(g_render_in_progress ? "Render Active" : "Ready");
        {
            std::lock_guard<std::mutex> lock(g_render_mutex);
            if (!g_render_diagnostic.empty()) ImGui::TextWrapped("%s", g_render_diagnostic.c_str());
        }

        if (g_render_in_progress) {
            ImGui::Spacing();
            int   done;
            int   total;
            bool  using_tiles = (g_render_progress.total_tiles > 0);
            if (using_tiles) {
                done  = g_render_progress.completed_tiles.load();
                total = g_render_progress.total_tiles;
            } else {
                done  = g_render_progress.completed_scanlines.load();
                total = g_render_progress.total_scanlines;
            }

            float pct    = (total > 0) ? (float)done / (float)total : 0.0f;
            double eta   = g_render_progress.eta_seconds.load();
            double el    = g_render_progress.elapsed_seconds.load();

            if (using_tiles) {
                ImGui::Text("Rendering: %d / %d tiles", done, total);
            } else {
                ImGui::Text("Rendering: %d / %d scanlines", done, total);
            }
            ImGui::ProgressBar(pct, ImVec2(-FLT_MIN, 0.0f));

            int rem = (int)eta;
            int rem_min = rem / 60;
            int rem_sec = rem % 60;

            ImGui::Text("Elapsed: %.0fs | Remaining: %d:%02d", el, rem_min, rem_sec);
        }

        ImGui::Separator();
        ImVec2 vp_size = ImGui::GetContentRegionAvail();
        if (request_render) {
            start_viewport_render(vp_size);
        }

        if (g_viewport_mode == 0) {
            if (g_rtHasImage && g_rtTexture != 0) {
                const viewport_image_layout layout =
                    ComputeViewportImageLayout(vp_size, g_rtWidth, g_rtHeight);
                ImVec2 cursor = ImGui::GetCursorPos();
                ImGui::SetCursorPos(ImVec2(cursor.x + layout.offset.x, cursor.y + layout.offset.y));

                ImGui::Image(
                    (ImTextureID)(intptr_t)g_rtTexture,
                    layout.size,
                    ImVec2(0, 0),
                    ImVec2(1, 1)
                );
            } else {
                ImGui::Text("No render yet. Click 'Render Current View'.");
            }
        } else {
            int tex_w = s_viewport_match_render ? g_camera.image_width : (int)vp_size.x;
            int tex_h = s_viewport_match_render ? g_camera.image_height : (int)vp_size.y;
            if (vp_size.x > 0.0f && vp_size.y > 0.0f && tex_w > 0 && tex_h > 0) {
                const viewport_image_layout layout =
                    ComputeViewportImageLayout(vp_size, tex_w, tex_h);
                const ImVec2 panel_cursor = ImGui::GetCursorPos();
                const ImVec2 draw_cursor(
                    panel_cursor.x + layout.offset.x,
                    panel_cursor.y + layout.offset.y
                );
                const ImVec2 panel_screen = ImGui::GetCursorScreenPos();
                const ImVec2 image_screen(
                    panel_screen.x + layout.offset.x,
                    panel_screen.y + layout.offset.y
                );

                RenderRasterToTexture(tex_w, tex_h);

                // Handle mouse click / drag -> gizmo interaction or pick
                ImGuiIO& io = ImGui::GetIO();
                if (g_viewport_hovered && layout.size.x > 0.0f && layout.size.y > 0.0f) {
                    ImVec2 m = io.MousePos;
                    float local_x = m.x - image_screen.x;
                    float local_y = m.y - image_screen.y;

                    // Map the mouse position in fitted viewport pixels to texture-local coords.
                    double scale_x = (double)tex_w / (double)layout.size.x;
                    double scale_y = (double)tex_h / (double)layout.size.y;
                    double sx = (double)local_x * scale_x;
                    double sy = (double)local_y * scale_y;

                    if (sx >= 0 && sx < tex_w && sy >= 0 && sy < tex_h) {
                        // Mouse down: attempt gizmo axis hit first, otherwise pick scene
                        if (ImGui::IsMouseClicked(0)) {
                            bool did_hit_gizmo = false;
                            if (g_show_gizmo && g_selected_object >= 0 && g_selected_object < (int)g_scene.objects.size()) {
                                // build ray (map to texture coords sx,sy)
                                ray r = ScreenPointToRay(g_editor_cam, tex_w, tex_h, (float)sx, (float)sy);
                                // gizmo origin and axes in world space
                                const auto& obj = g_scene.objects[g_selected_object];
                                vec3 origin = obj.translation + vec3(obj.center.x(), obj.center.y(), obj.center.z());
                                // Use world-aligned axes (gizmo should follow world X/Y/Z)
                                vec3 axis_world[3] = { vec3(1,0,0), vec3(0,1,0), vec3(0,0,1) };

                                // test closest distance to each axis and to arrow tip (cone)
                                double best_dist = 1e9; int best_axis = -1; vec3 best_cp1, best_cp2;
                                for (int a=0;a<3;++a) {
                                    double s,t;
                                    if (!ClosestPointsBetweenLines(origin, axis_world[a], r.origin(), r.direction(), s, t)) continue;
                                    vec3 cp_axis = origin + axis_world[a] * (float)s;
                                    vec3 cp_ray  = r.origin() + r.direction() * (float)t;
                                    double dist = (cp_axis - cp_ray).length();

                                    // threshold based on object size for axis-line hit
                                    double gizmo_scale = 0.5 * std::max(0.5, obj.radius);
                                    double axis_thresh = gizmo_scale * 0.12; // heuristic

                                    bool axis_hit = (dist < axis_thresh);

                                    // Precise cone-triangle intersection for arrowhead: transform cone triangles for this axis
                                    // Cone triangles are stored in g_gizmoConeTriangles in model-space, grouped per-axis.
                                    // Build full model matrix used when rendering the gizmo so we transform triangles the same way.

                                    // Build cone triangle model without object rotation so arrowheads point along world axes
                                    vec3 trans = obj.translation + vec3(obj.center.x(), obj.center.y(), obj.center.z());
                                    float modelFull[16];
                                    vec3 scl = vec3((float)gizmo_scale, (float)gizmo_scale, (float)gizmo_scale);
                                    make_model_trs(trans, vec3(0,0,0), scl, modelFull);

                                    // helper to transform a model-space point (includes translation)
                                    auto transform_point_by_model = [&](const float M[16], const vec3& v) {
                                        return vec3(
                                            M[0]*v.x() + M[1]*v.y() + M[2]*v.z() + M[12],
                                            M[4]*v.x() + M[5]*v.y() + M[6]*v.z() + M[13],
                                            M[8]*v.x() + M[9]*v.y() + M[10]*v.z() + M[14]
                                        );
                                    };

                                    // cone triangles per axis: cone_segments triangles, each triangle = 3 consecutive vec3 in g_gizmoConeTriangles
                                    int segs = g_gizmoConeSegments;
                                    int verts_per_axis = segs * 3; // number of vec3 entries per axis
                                    int axis_offset = a * verts_per_axis;

                                    double best_t_tri = 1e18;
                                    bool tri_hit = false;
                                    vec3 tri_hit_point;

                                    for (int sidx = 0; sidx < segs; ++sidx) {
                                        int base = axis_offset + sidx * 3;
                                        if (base + 2 >= (int)g_gizmoConeTriangles.size()) break;
                                        vec3 v0 = transform_point_by_model(modelFull, g_gizmoConeTriangles[base + 0]);
                                        vec3 v1 = transform_point_by_model(modelFull, g_gizmoConeTriangles[base + 1]);
                                        vec3 v2 = transform_point_by_model(modelFull, g_gizmoConeTriangles[base + 2]);
                                        double ttri;
                                        if (RayIntersectsTriangle(r, v0, v1, v2, ttri)) {
                                            if (ttri > 0.0 && ttri < best_t_tri) {
                                                best_t_tri = ttri;
                                                tri_hit = true;
                                                tri_hit_point = r.origin() + r.direction() * (float)ttri;
                                            }
                                        }
                                    }

                                    if (tri_hit && best_t_tri < best_dist) {
                                        best_dist = best_t_tri;
                                        best_axis = a;
                                        best_cp1 = tri_hit_point;
                                        best_cp2 = tri_hit_point; // both points approximate
                                    } else if (axis_hit && dist < best_dist) {
                                        best_dist = dist;
                                        best_axis = a;
                                        best_cp1 = cp_axis;
                                        best_cp2 = cp_ray;
                                    }
                                }

                                if (best_axis >= 0) {
                                    // begin gizmo drag
                                    g_active_gizmo_axis = best_axis;
                                    g_gizmo_hit_point = best_cp1;
                                    g_gizmo_initial_obj_translation = g_scene.objects[g_selected_object].translation;
                                    // capture snapshot for undo at drag start
                                    int sidx = g_selected_object;
                                    if (sidx >= 0) {
                                        auto& o = g_scene.objects[sidx];
                                        g_obj_snapshot_before[sidx] = ObjSnapshot{ o.center, o.translation, o.rotation_deg, o.scale, o.radius };
                                    }
                                    did_hit_gizmo = true;
                                }
                            }

                            if (!did_hit_gizmo) {
                                int picked = PerformPick(tex_w, tex_h, (int)sx, (int)sy);
                                if (picked >= 0 && picked < (int)g_scene.objects.size()) {
                                    SelectSceneObject(picked);
                                    g_show_gizmo = true;
                                } else {
                                    g_selected_object = -1;
                                    g_show_gizmo = false;
                                }
                                g_active_gizmo_axis = -1;
                            }
                        }

                        // Mouse dragging: if active axis, compute new closest point and move object
                        if (g_active_gizmo_axis >= 0 && ImGui::IsMouseDown(0) && g_selected_object >= 0) {
                            ray rnow = ScreenPointToRay(g_editor_cam, tex_w, tex_h, (float)sx, (float)sy);
                            const auto& obj = g_scene.objects[g_selected_object];
                            vec3 origin = obj.translation + vec3(obj.center.x(), obj.center.y(), obj.center.z());
                            // World-aligned axis for dragging
                            vec3 axis_world = vec3(0,0,0);
                            if (g_active_gizmo_axis == 0) axis_world = vec3(1,0,0);
                            else if (g_active_gizmo_axis == 1) axis_world = vec3(0,1,0);
                            else if (g_active_gizmo_axis == 2) axis_world = vec3(0,0,1);
                            axis_world = unit_vector(axis_world);

                            double s_now, t_now;
                            if (ClosestPointsBetweenLines(origin, axis_world, rnow.origin(), rnow.direction(), s_now, t_now)) {
                                vec3 cp_axis_now = origin + axis_world * (float)s_now;
                                float move_along = (float)dot(cp_axis_now - g_gizmo_hit_point, axis_world);
                                vec3 new_trans = g_gizmo_initial_obj_translation + axis_world * move_along;
                                g_scene.objects[g_selected_object].translation = new_trans;
                                g_world_dirty = true;
                                g_cached_world.reset();
                            }
                        }

                        // Mouse release: if we were dragging a gizmo axis, push an undo action
                        if (!ImGui::IsMouseDown(0)) {
                            if (g_active_gizmo_axis >= 0 && g_selected_object >= 0) {
                                int idx = g_selected_object;
                                if (idx >= 0 && idx < (int)g_scene.objects.size()) {
                                    auto& obj = g_scene.objects[idx];
                                    ObjSnapshot after{ obj.center, obj.translation, obj.rotation_deg, obj.scale, obj.radius };
                                    ObjSnapshot before = g_obj_snapshot_before[idx];
                                    // only push if something changed
                                    bool changed = (
                                        std::abs(after.translation.x() - before.translation.x()) > 1e-6 ||
                                        std::abs(after.translation.y() - before.translation.y()) > 1e-6 ||
                                        std::abs(after.translation.z() - before.translation.z()) > 1e-6 ||
                                        std::abs(after.center.x() - before.center.x()) > 1e-6 ||
                                        std::abs(after.center.y() - before.center.y()) > 1e-6 ||
                                        std::abs(after.center.z() - before.center.z()) > 1e-6 ||
                                        std::abs(after.rotation_deg.x() - before.rotation_deg.x()) > 1e-6 ||
                                        std::abs(after.rotation_deg.y() - before.rotation_deg.y()) > 1e-6 ||
                                        std::abs(after.rotation_deg.z() - before.rotation_deg.z()) > 1e-6 ||
                                        std::abs(after.scale.x() - before.scale.x()) > 1e-6 ||
                                        std::abs(after.scale.y() - before.scale.y()) > 1e-6 ||
                                        std::abs(after.scale.z() - before.scale.z()) > 1e-6 ||
                                        std::abs(after.radius - before.radius) > 1e-9
                                    );
                                    if (changed) {
                                        UndoManager::Instance().push(std::make_unique<LambdaAction>(
                                            [idx, before]() {
                                                if (idx >= 0 && idx < (int)g_scene.objects.size()) {
                                                    auto& o = g_scene.objects[idx];
                                                    o.center = before.center;
                                                    o.translation = before.translation;
                                                    o.rotation_deg = before.rotation_deg;
                                                    o.scale = before.scale;
                                                    o.radius = before.radius;
                                                    g_world_dirty = true;
                                                    g_cached_world.reset();
                                                }
                                            },
                                            [idx, after]() {
                                                if (idx >= 0 && idx < (int)g_scene.objects.size()) {
                                                    auto& o = g_scene.objects[idx];
                                                    o.center = after.center;
                                                    o.translation = after.translation;
                                                    o.rotation_deg = after.rotation_deg;
                                                    o.scale = after.scale;
                                                    o.radius = after.radius;
                                                    g_world_dirty = true;
                                                    g_cached_world.reset();
                                                }
                                            },
                                            "Transform Object"
                                        ));
                                    }
                                }
                            }
                            g_active_gizmo_axis = -1;
                        }
                    }
                }

                if (g_rasterColorTex != 0) {
                    ImGui::SetCursorPos(draw_cursor);
                    ImGui::Image(
                        (ImTextureID)(intptr_t)g_rasterColorTex,
                        layout.size,
                        ImVec2(0, 1),
                        ImVec2(1, 0)  // flip Y
                    );
                } else {
                    ImGui::Text("Raster texture not ready.");
                }
            } else {
                ImGui::Text("Viewport too small.");
            }
        }

        ImGui::End();

        if (show_demo_window)
            ImGui::ShowDemoWindow(&show_demo_window);

        ImGui::Render();

        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glDisable(GL_DEPTH_TEST);
        glClearColor(0.10f, 0.10f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
        {
            GLFWwindow* backup_current_context = glfwGetCurrentContext();
            ImGui::UpdatePlatformWindows();
            for (auto* viewport : ImGui::GetPlatformIO().Viewports)
                if (viewport->PlatformHandle) glfwSetDropCallback(static_cast<GLFWwindow*>(viewport->PlatformHandle), glfw_drop_callback);
            ImGui::RenderPlatformWindowsDefault();
            glfwMakeContextCurrent(backup_current_context);
        }

        // Auto-save materials if modified
        TickMaterialPersistence();

        glfwSwapBuffers(window);
    }

    StopEditorPreviews();
    StopRasterPreview();
    if (g_materials_dirty) SaveMaterialsManifest();

    // Cleanup
    if (g_render_in_progress && g_render_thread.joinable()) {
        g_cancel_flag.store(true);
        g_render_thread.join();
    }

    // Ensure progress window is stopped on shutdown
    stop_progress_window_thread();

    if (g_rtTexture != 0) {
        glDeleteTextures(1, &g_rtTexture);
    }

    if (g_rasterSphereVAO) glDeleteVertexArrays(1, &g_rasterSphereVAO);
    if (g_rasterSphereVBO) glDeleteBuffers(1, &g_rasterSphereVBO);
    if (g_rasterSphereEBO) glDeleteBuffers(1, &g_rasterSphereEBO);

    if (g_rasterCubeVAO) glDeleteVertexArrays(1, &g_rasterCubeVAO);
    if (g_rasterCubeVBO) glDeleteBuffers(1, &g_rasterCubeVBO);
    if (g_rasterCubeEBO) glDeleteBuffers(1, &g_rasterCubeEBO);

    for (auto& gm : g_gpu_meshes) {
        if (gm.vao) glDeleteVertexArrays(1, &gm.vao);
        if (gm.vbo) glDeleteBuffers(1, &gm.vbo);
        if (gm.ebo) glDeleteBuffers(1, &gm.ebo);
    }

    if (g_rasterShader)   glDeleteProgram(g_rasterShader);
    if (g_modelThumbnailShader) glDeleteProgram(g_modelThumbnailShader);
    if (g_rasterColorTex) glDeleteTextures(1, &g_rasterColorTex);
    if (g_rasterDepthRBO) glDeleteRenderbuffers(1, &g_rasterDepthRBO);
    if (g_rasterFBO)      glDeleteFramebuffers(1, &g_rasterFBO);

    if (g_pickShader)     glDeleteProgram(g_pickShader);
    if (g_pickColorTex)   glDeleteTextures(1, &g_pickColorTex);
    if (g_pickDepthRBO)   glDeleteRenderbuffers(1, &g_pickDepthRBO);
    if (g_pickFBO)        glDeleteFramebuffers(1, &g_pickFBO);

    if (g_lineShader)     glDeleteProgram(g_lineShader);
    if (g_gizmoVBO)       glDeleteBuffers(1, &g_gizmoVBO);
    if (g_gizmoVAO)       glDeleteVertexArrays(1, &g_gizmoVAO);

    for (auto& item : g_material_thumb_cache) if (item.second) glDeleteTextures(1,&item.second);
    g_material_thumb_cache.clear();

    // Cleanup texture thumbnail cache
    for (auto& kv : g_texture_thumb_cache) {
        if (kv.second) glDeleteTextures(1, &kv.second);
    }
    g_texture_thumb_cache.clear();

    // Cleanup model thumbnail cache
    for (auto& kv : g_model_thumb_cache) {
        if (kv.second) glDeleteTextures(1, &kv.second);
    }
    g_model_thumb_cache.clear();

    g_cached_world.reset();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
