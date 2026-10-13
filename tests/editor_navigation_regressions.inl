// Viewport navigation (editor_viewport_nav.inl), driven by recorded input.
// Run from the gizmo checks, which own the ImGui context.
namespace navigation_checks {
using namespace viewport_nav;
static void reset() {
    if (navigating()) end(nullptr);
    g_editor_cam = {};
    g_editor_cam.vfov = 45;
    g_editor_cam.set_from_lookat(point3(0, 0, 5), point3());
    g_camera_look_sens = .002f;
    g_camera_move_speed = 4;
    velocity = vec3(0, 0, 0);
    pivot = 5;
    framing = false;
    for (auto& down : was_down) down = false;
    f_was_down = false;
    set_area({100, 100}, {900, 600});
    g_viewport_hovered = true;
    g_viewport_focused = false;
    g_selected_object = -1;
    ImGui::GetIO().MousePos = {500, 350};
}
static input pointer(double x, double y) {
    input in;
    in.x = x;
    in.y = y;
    return in;
}
static bool mouse_hidden_from_imgui() { return (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_NoMouse) != 0; }

static void run() {
    const double frame = 1.0 / 60;
    auto& cam = g_editor_cam;

    // RMB over the viewport starts flying and hides the mouse from ImGui.
    reset();
    auto in = pointer(500, 350);
    in.down[GLFW_MOUSE_BUTTON_RIGHT] = true;
    step(nullptr, in, frame);
    check(active == mode::fly && mouse_hidden_from_imgui(), "RMB over the viewport captures the mouse for flying");
    double yaw = cam.yaw;
    in.x += 100;
    step(nullptr, in, frame);
    check(std::abs(cam.yaw - yaw - .2) < 1e-6, "mouse motion turns the camera");
    // The pointer leaving the viewport, which used to strand the drag, does nothing.
    g_viewport_hovered = false;
    ImGui::GetIO().MousePos = {1100, 50};
    yaw = cam.yaw;
    in.x += 50;
    step(nullptr, in, frame);
    check(active == mode::fly && std::abs(cam.yaw - yaw - .1) < 1e-6,
          "the drag keeps turning the camera wherever the pointer goes");
    in.down[GLFW_MOUSE_BUTTON_RIGHT] = false;
    step(nullptr, in, frame);
    check(!navigating() && !mouse_hidden_from_imgui(), "releasing RMB gives the mouse back to the UI");

    // Focus loss always releases the mouse.
    reset();
    in = pointer(500, 350);
    in.down[GLFW_MOUSE_BUTTON_RIGHT] = true;
    step(nullptr, in, frame);
    in.focused = false;
    step(nullptr, in, frame);
    check(!navigating() && !mouse_hidden_from_imgui(), "losing window focus ends a drag");

    // Presses over other panels, the toolbar, or a plain left click do not navigate.
    reset();
    g_viewport_hovered = false;
    in = pointer(500, 350);
    in.down[GLFW_MOUSE_BUTTON_RIGHT] = true;
    step(nullptr, in, frame);
    check(!navigating(), "RMB over another panel leaves the camera alone");
    reset();
    ImGui::GetIO().MousePos = {500, 60};
    step(nullptr, in, frame);
    check(!navigating(), "RMB over the viewport toolbar leaves the camera alone");
    reset();
    in = pointer(500, 350);
    in.down[GLFW_MOUSE_BUTTON_LEFT] = true;
    step(nullptr, in, frame);
    check(!navigating(), "a left click stays a selection click");

    // WASD eases in to the fly speed and glides to a stop.
    reset();
    in = pointer(500, 350);
    in.down[GLFW_MOUSE_BUTTON_RIGHT] = true;
    step(nullptr, in, frame);
    in.forward = true;
    const point3 start = cam.position;
    step(nullptr, in, frame);
    const double first = (cam.position - start).length();
    check(first > 0 && first < 4 * frame * .5, "flying eases in rather than jumping to full speed");
    for (int i = 1; i < 60; ++i) step(nullptr, in, frame);
    const double travelled = dot(cam.position - start, cam.forward);
    check(travelled > 3.4 && travelled < 4, "one second of W covers about the fly speed");
    in.forward = false;
    const point3 released = cam.position;
    for (int i = 0; i < 60; ++i) step(nullptr, in, frame);
    const double glide = (cam.position - released).length();
    check(glide > 0 && glide < .4 && velocity.near_zero(), "flying glides briefly to a stop");
    in.shift = true;
    in.forward = true;
    for (int i = 0; i < 60; ++i) step(nullptr, in, frame);
    check(velocity.length() > 11.5, "Shift flies three times faster");
    in = pointer(500, 350);
    in.down[GLFW_MOUSE_BUTTON_RIGHT] = true;
    in.wheel = 1;
    step(nullptr, in, frame);
    check(std::abs(g_camera_move_speed - 5) < 1e-5, "the wheel while flying raises the fly speed");
    in.down[GLFW_MOUSE_BUTTON_RIGHT] = false;
    in.wheel = 0;
    step(nullptr, in, frame);

    // The wheel over the viewport dollies toward the pivot.
    reset();
    in = pointer(500, 350);
    in.wheel = 1;
    step(nullptr, in, frame);
    check(std::abs(cam.position.z() - 4.25) < 1e-9 && std::abs(pivot - 4.25) < 1e-9,
          "the wheel dollies toward the point in view");

    // MMB pans so the scene follows the pointer at the pivot depth.
    reset();
    in = pointer(500, 350);
    in.down[GLFW_MOUSE_BUTTON_MIDDLE] = true;
    step(nullptr, in, frame);
    in.x += 100;
    step(nullptr, in, frame);
    const double expected = 100 * 2 * 5 * std::tan(degrees_to_radians(45) / 2) / 500;
    check(active == mode::pan && std::abs(cam.position.x() + expected) < 1e-9, "MMB pans with the pointer");
    in.down[GLFW_MOUSE_BUTTON_MIDDLE] = false;
    step(nullptr, in, frame);

    // Alt + LMB orbits around the point in view.
    reset();
    in = pointer(500, 350);
    in.alt = true;
    in.down[GLFW_MOUSE_BUTTON_LEFT] = true;
    step(nullptr, in, frame);
    const point3 target = cam.position + cam.forward * pivot;
    in.x += 300;
    in.y -= 120;
    step(nullptr, in, frame);
    check(active == mode::orbit && (cam.position + cam.forward * pivot - target).length() < 1e-9 &&
              std::abs((cam.position - target).length() - 5) < 1e-9,
          "Alt + LMB orbits the point in view at a fixed distance");
    in.down[GLFW_MOUSE_BUTTON_LEFT] = false;
    step(nullptr, in, frame);

    // F frames the selection with an eased move.
    reset();
    g_scene = {};
    scene_object box;
    box.type = scene_object_type::cube;
    box.translation = vec3(3, 1, -2);
    box.scale = vec3(2, 2, 2);
    g_scene.objects.push_back(box);
    g_selected_object = 0;
    g_viewport_focused = true;
    in = pointer(500, 350);
    in.frame_key = true;
    step(nullptr, in, frame);
    check(framing, "F starts framing the selection");
    in.frame_key = false;
    const double deadline = glfwGetTime() + 2;
    while (framing && glfwGetTime() < deadline) step(nullptr, in, frame);
    const point3 centre(3, 1, -2);
    const double radius = std::sqrt(3.0);
    check(!framing && (cam.position + cam.forward * pivot - centre).length() < 1e-6 &&
              pivot * std::sin(degrees_to_radians(45) / 2) >= radius,
          "framing centres the selection and fits it in view");
    reset();
    g_scene = {};
    std::printf("Viewport navigation checks passed\n");
}
} // namespace navigation_checks
