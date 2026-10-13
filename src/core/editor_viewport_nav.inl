// Viewport navigation, in the manner of Unreal's editor viewport:
//   RMB drag             look; W/A/S/D move, Q/E down/up, Shift 3x faster,
//                        mouse wheel changes the fly speed
//   MMB drag             pan
//   Alt + LMB drag       orbit around the point the view looks at
//   Alt + RMB drag       dolly toward that point
//   Mouse wheel          dolly toward that point
//   F                    frame the selection, or the whole scene
// A drag starts only on a press over the viewport and then owns the mouse
// until its button is released, wherever the pointer goes: the cursor is
// hidden and locked, raw (unaccelerated) motion drives the camera, and ImGui
// ignores the mouse so no other panel hovers or clicks. Losing window focus
// ends the drag, so the cursor can never stay captured.
// Included by editor_main.cpp.
namespace viewport_nav {
enum class mode { none, fly, pan, orbit, dolly };
static mode active = mode::none;
static int button = -1;
static double last_x = 0, last_y = 0;
static bool settle = false;          // drop the first delta after capturing the cursor
static vec3 velocity(0, 0, 0);       // smoothed fly velocity
static double wheel = 0;             // main-window wheel notches since the last update
static double pivot = 5;             // distance to the point that orbit and dolly use
static ImVec2 area_min, area_max;    // viewport content, screen space, from the last frame
static bool area_valid = false;
static double speed_changed = -10;   // glfwGetTime() when the fly speed last changed
static bool focus_viewport = false;
static bool was_down[3] = {};
static bool f_was_down = false;
static bool framing = false;         // F: an eased move to the framed view
static point3 frame_from, frame_to;
static double frame_start = 0;
constexpr double frame_seconds = .22;
constexpr double fly_smoothing = .07; // seconds for velocity to reach 63% of its target

static void scroll_callback(GLFWwindow*, double, double y) { wheel += y; }

// One frame of the main window's input, read from GLFW (or made by tests).
struct input {
    bool focused = true;
    bool down[3] = {};           // left, right, middle
    double x = 0, y = 0;         // cursor; unbounded while captured
    double wheel = 0;            // notches
    bool forward = false, back = false, left = false, right = false, up = false, down_key = false;
    bool shift = false, alt = false, frame_key = false;
};
static input read_input(GLFWwindow* window) {
    auto key = [&](int k) { return glfwGetKey(window, k) == GLFW_PRESS; };
    input in;
    in.focused = glfwGetWindowAttrib(window, GLFW_FOCUSED) != 0;
    for (int b = 0; b < 3; ++b) in.down[b] = glfwGetMouseButton(window, b) == GLFW_PRESS;
    glfwGetCursorPos(window, &in.x, &in.y);
    in.wheel = wheel;
    wheel = 0;
    in.forward = key(GLFW_KEY_W); in.back = key(GLFW_KEY_S);
    in.left = key(GLFW_KEY_A); in.right = key(GLFW_KEY_D);
    in.up = key(GLFW_KEY_E); in.down_key = key(GLFW_KEY_Q);
    in.shift = key(GLFW_KEY_LEFT_SHIFT) || key(GLFW_KEY_RIGHT_SHIFT);
    in.alt = key(GLFW_KEY_LEFT_ALT) || key(GLFW_KEY_RIGHT_ALT);
    in.frame_key = key(GLFW_KEY_F);
    return in;
}
static bool navigating() { return active != mode::none; }

static void set_area(ImVec2 min, ImVec2 max) {
    area_min = min;
    area_max = max;
    area_valid = max.x > min.x && max.y > min.y;
}
static bool over_area() {
    const ImVec2 p = ImGui::GetIO().MousePos;
    return area_valid && p.x >= area_min.x && p.y >= area_min.y && p.x < area_max.x && p.y < area_max.y;
}
// Last frame's hover is current enough: it already accounts for windows,
// popups and menus drawn over the viewport.
static bool pointer_on_viewport() {
    return g_viewport_hovered && over_area() &&
           !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
}

static void begin(GLFWwindow* window, mode m, int pressed_button) {
    active = m;
    button = pressed_button;
    settle = true;
    framing = false;
    if (window) {
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        if (glfwRawMouseMotionSupported()) glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
    }
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouse;
    focus_viewport = true;
}
static void end(GLFWwindow* window) {
    if (window) {
        if (glfwRawMouseMotionSupported()) glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_FALSE);
        // GLFW puts the pointer back where the drag began.
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
    }
    ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
    active = mode::none;
    button = -1;
}

// World-space bounds of an object, from the same transforms the viewport draws.
static bool object_bounds(const scene_object& object, point3& lo, point3& hi) {
    point3 a(-.5, -.5, -.5), b(.5, .5, .5);
    vec3 scale = object.scale;
    if (object.type == scene_object_type::sphere) {
        a = point3(-1, -1, -1);
        b = point3(1, 1, 1);
        scale *= object.radius;
    } else if (object.type == scene_object_type::mesh_instance) {
        if (object.mesh_index < 0 || object.mesh_index >= int(g_gpu_meshes.size())) return false;
        a = g_gpu_meshes[object.mesh_index].minimum;
        b = g_gpu_meshes[object.mesh_index].maximum;
        if (!(b.x() >= a.x())) return false;
    }
    float m[16];
    make_model_trs(object.translation + object.center, object.rotation_deg, scale, m);
    for (int corner = 0; corner < 8; ++corner) {
        const point3 c((corner & 1) ? b.x() : a.x(), (corner & 2) ? b.y() : a.y(), (corner & 4) ? b.z() : a.z());
        const point3 w(m[0] * c.x() + m[4] * c.y() + m[8] * c.z() + m[12],
                       m[1] * c.x() + m[5] * c.y() + m[9] * c.z() + m[13],
                       m[2] * c.x() + m[6] * c.y() + m[10] * c.z() + m[14]);
        for (int k = 0; k < 3; ++k) {
            lo[k] = std::min(lo[k], w[k]);
            hi[k] = std::max(hi[k], w[k]);
        }
    }
    return true;
}
// Moves the camera, keeping its direction, until the selection (or the
// whole scene) fills the view.
static void frame_selection() {
    point3 lo(1e30, 1e30, 1e30), hi(-1e30, -1e30, -1e30);
    bool found = false;
    if (g_selected_object >= 0 && g_selected_object < int(g_scene.objects.size())) {
        found = object_bounds(g_scene.objects[g_selected_object], lo, hi);
    } else {
        for (const auto& object : g_scene.objects) found |= object_bounds(object, lo, hi);
    }
    if (!found) return;
    auto& cam = g_editor_cam;
    const point3 centre = (lo + hi) * .5;
    const double radius = std::max((hi - lo).length() * .5, 1e-3);
    const double aspect = area_valid ? (area_max.x - area_min.x) / std::max(1.0f, area_max.y - area_min.y) : 16.0 / 9;
    const double half = degrees_to_radians(cam.vfov) * .5;
    const double narrowest = std::min(half, std::atan(std::tan(half) * aspect));
    const double distance = radius / std::sin(narrowest) * 1.08;
    frame_from = cam.position;
    frame_to = centre - cam.forward * distance;
    frame_start = glfwGetTime();
    framing = true;
    pivot = distance;
}
// Moves toward (factor < 1) or away from the pivot point.
static void dolly(double factor) {
    auto& cam = g_editor_cam;
    const point3 target = cam.position + cam.forward * pivot;
    const double next = pivot * factor;
    if (next < .05) {
        // At the pivot: keep moving through it at the same pace.
        cam.position += cam.forward * (pivot * (1 - factor));
        return;
    }
    pivot = std::min(next, 1e5);
    cam.position = target - cam.forward * pivot;
}

// Runs before ImGui::NewFrame, so a drag hides the mouse from ImGui on the
// very frame it starts. window may be null (tests): the cursor is left alone.
static void step(GLFWwindow* window, const input& in, double dt) {
    auto& io = ImGui::GetIO();
    auto& cam = g_editor_cam;
    dt = std::min(dt, 1.0 / 20); // a hitch must not fling the camera
    bool pressed[3];
    for (int b = 0; b < 3; ++b) {
        pressed[b] = in.down[b] && !was_down[b];
        was_down[b] = in.down[b];
    }
    const double notches = in.wheel;

    if (navigating() && (!in.focused || !in.down[button])) end(window);
    if (!navigating() && in.focused && !transform_gizmo::drag.active && pointer_on_viewport()) {
        if (pressed[GLFW_MOUSE_BUTTON_RIGHT]) begin(window, in.alt ? mode::dolly : mode::fly, GLFW_MOUSE_BUTTON_RIGHT);
        else if (pressed[GLFW_MOUSE_BUTTON_MIDDLE]) begin(window, mode::pan, GLFW_MOUSE_BUTTON_MIDDLE);
        else if (pressed[GLFW_MOUSE_BUTTON_LEFT] && in.alt) begin(window, mode::orbit, GLFW_MOUSE_BUTTON_LEFT);
    }

    double dx = 0, dy = 0;
    if (navigating()) {
        // Capturing the cursor can move it: the first frame only records it.
        if (!settle) {
            dx = in.x - last_x;
            dy = in.y - last_y;
        }
        settle = false;
        last_x = in.x;
        last_y = in.y;
    }
    const double look = g_camera_look_sens;
    switch (active) {
    case mode::fly:
        cam.look(dx * look, -dy * look);
        break;
    case mode::orbit: {
        const point3 target = cam.position + cam.forward * pivot;
        cam.look(dx * look, -dy * look);
        cam.position = target - cam.forward * pivot;
    } break;
    case mode::pan: {
        // The point under the cursor at the pivot depth follows the cursor.
        const double height = area_valid ? std::max(1.0f, area_max.y - area_min.y) : 600.0;
        const double scale = 2 * pivot * std::tan(degrees_to_radians(cam.vfov) * .5) / height;
        cam.position += (cam.up * dy - cam.right * dx) * scale;
    } break;
    case mode::dolly:
        dolly(std::exp(dy * .006));
        break;
    case mode::none:
        break;
    }

    if (notches != 0) {
        if (active == mode::fly) {
            g_camera_move_speed = float(std::clamp(g_camera_move_speed * std::pow(1.25, notches), .01, 500.0));
            speed_changed = glfwGetTime();
        } else if (!navigating() && pointer_on_viewport()) {
            dolly(std::pow(.85, notches));
            framing = false;
        }
    }

    // Fly movement eases in and out instead of jumping to full speed.
    vec3 target(0, 0, 0);
    if (active == mode::fly) {
        const double forward = double(in.forward) - double(in.back);
        const double right = double(in.right) - double(in.left);
        const double up = double(in.up) - double(in.down_key);
        const vec3 direction = cam.forward * forward + cam.right * right + cam.world_up * up;
        const double boost = in.shift ? 3 : 1;
        if (direction.length_squared() > 0) target = unit_vector(direction) * (g_camera_move_speed * boost);
    }
    velocity += (target - velocity) * (1 - std::exp(-dt / fly_smoothing));
    if (target.near_zero() && velocity.length_squared() < 1e-8) velocity = vec3(0, 0, 0);
    if (!velocity.near_zero()) {
        cam.position += velocity * dt;
        framing = false;
    }
    cam.move_speed = g_camera_move_speed;

    if (in.frame_key && !f_was_down && !navigating() && !io.WantTextInput && !io.KeyCtrl &&
        (g_viewport_hovered || g_viewport_focused))
        frame_selection();
    f_was_down = in.frame_key;
    if (framing) {
        const double t = std::clamp((glfwGetTime() - frame_start) / frame_seconds, 0.0, 1.0);
        cam.position = frame_from + (frame_to - frame_from) * (t * t * (3 - 2 * t));
        framing = t < 1 && !navigating();
    }
}

static void update(GLFWwindow* window, double dt) { step(window, read_input(window), dt); }

static void after_new_frame() {
    if (focus_viewport) {
        ImGui::SetWindowFocus("Viewport");
        focus_viewport = false;
    }
}

// The fly speed after a wheel change, and a key hint while flying.
static void draw_overlay(ImDrawList* draw) {
    if (!area_valid) return;
    const double since = glfwGetTime() - speed_changed;
    auto pill = [&](ImVec2 at, const char* text, float alpha) {
        const ImVec2 size = ImGui::CalcTextSize(text);
        draw->AddRectFilled(at, ImVec2(at.x + size.x + 20, at.y + size.y + 10), IM_COL32(12, 16, 22, int(200 * alpha)), 6);
        draw->AddText(ImVec2(at.x + 10, at.y + 5), IM_COL32(230, 236, 242, int(255 * alpha)), text);
    };
    if (since < 1.4) {
        char text[64];
        std::snprintf(text, sizeof(text), "Fly speed  %.2f", g_camera_move_speed);
        const float alpha = float(std::clamp((1.4 - since) / .3, 0.0, 1.0));
        const ImVec2 size = ImGui::CalcTextSize(text);
        pill(ImVec2((area_min.x + area_max.x - size.x - 20) * .5f, area_min.y + 14), text, alpha);
    }
    const char* hint = active == mode::fly ? "WASD move   Q/E down/up   Shift faster   Wheel speed"
                     : active == mode::orbit ? "Orbit" : active == mode::pan ? "Pan" : active == mode::dolly ? "Dolly" : nullptr;
    if (hint) pill(ImVec2(area_min.x + 12, area_max.y - ImGui::GetTextLineHeight() - 22), hint, 1);
}
} // namespace viewport_nav
