// Exercise the full editor loop in a hidden window, with isolated fixture assets.
// Only this test translation unit substitutes these three GLFW entry points.
static bool layout_smoke = false;
static int layout_frame = 0;
static GLFWwindow* smoke_create_window(int width, int height, const char* title, GLFWmonitor* monitor,
                                       GLFWwindow* shared) {
    if (layout_smoke) {
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    }
    return glfwCreateWindow(width, height, title, monitor, shared);
}
static void smoke_poll_events() {
    glfwPollEvents();
    if (!layout_smoke) {
        return;
    }
    ++layout_frame;
    auto& io = ImGui::GetIO();
    io.ConfigFlags &= ~ImGuiConfigFlags_ViewportsEnable;
    io.IniFilename = nullptr;
    if (layout_frame == 1) {
        BuildRasterShader();
        BuildUnitSphereMesh();
        BuildUnitCubeMesh();
    }
    if (layout_frame == 3 || layout_frame == 4) {
        io.AddKeyEvent(ImGuiMod_Ctrl, layout_frame == 3);
        io.AddKeyEvent(ImGuiKey_Space, layout_frame == 3);
    }
    if (layout_frame == 5) {
        SelectSceneLight(0);
    }
    if (layout_frame == 36) {
        auto* drawer = ImGui::FindWindowByName("Content Drawer");
        check(drawer && drawer->WasActive, "Ctrl+Space opens the full Content Drawer");
        auto* tabs = GImGui->TabBars.GetByKey(drawer->GetID("AssetTypes"));
        check(tabs && tabs->Tabs.Size == 3, "drawer has three asset tabs");
        for (auto& tab : tabs->Tabs) {
            if (std::string(ImGui::TabBarGetTabName(tabs, &tab)) == "Materials") {
                tabs->NextSelectedTabId = tab.ID;
            }
        }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
}
static void smoke_swap_buffers(GLFWwindow* window) {
    if (layout_smoke && (layout_frame == 35 || layout_frame == 72)) {
        glFinish();
        int width, height;
        glfwGetFramebufferSize(window, &width, &height);
        std::vector<unsigned char> pixels(size_t(width) * height * 4);
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        stbi_flip_vertically_on_write(1);
        check(stbi_write_png(layout_frame == 35 ? "editor-textures.png" : "editor-materials.png", width,
                             height, 4, pixels.data(), width * 4) != 0,
              "full editor screenshot");
        if (layout_frame == 72) {
            check(g_material_thumb_cache.size() == 8, "material tab schedules visible material thumbnails");
            glfwSetWindowShouldClose(window, GLFW_TRUE);
        }
    }
    glfwSwapBuffers(window);
}
static int run_layout_smoke() {
    layout_smoke = true;
    g_scene_initialized = true;
    g_viewport_mode = 1;
    g_show_gizmo = false;
    g_camera.image_width = 640;
    g_camera.image_height = 360;
    g_editor_cam.set_from_lookat(point3(3, 2, 4), point3(0, 0, 0));
    std::vector<unsigned char> pixels(128 * 128 * 4, 255);
    for (int y = 0; y < 128; ++y) {
        for (int x = 0; x < 128; ++x) {
            size_t at = size_t(y * 128 + x) * 4;
            pixels[at] = x * 2;
            pixels[at + 1] = y * 2;
            pixels[at + 2] = 96;
        }
    }
    stbi_write_png("studio-texture.png", 128, 128, 4, pixels.data(), 128 * 4);
    for (int i = 0; i < 8; ++i) {
        scene_material material;
        material.name = "Surface " + std::to_string(i + 1);
        material.model = scene_material_model::pbr;
        material.metallic = i / 7.;
        material.roughness = .2 + i * .1;
        material.base_color = vec3(.55, .2 + i * .05, .09);
        g_scene.materials.push_back(material);
        g_scene.textures.push_back({"Texture " + std::to_string(i + 1), "studio-texture.png"});
    }
    scene_object object;
    object.name = "Preview sphere";
    object.material_index = 0;
    object.radius = 1;
    g_scene.objects.push_back(object);
    scene_light light;
    light.name = "Key sun";
    light.type = scene_light_type::directional;
    light.direction = unit_vector(vec3(-1, -2, -3));
    light.radiance = colour(3, 3, 3);
    g_scene.lights.push_back(light);
    const int result = dusk_editor_entry_for_test();
    std::printf("%d complete editor layout checks passed\n", checks);
    return result;
}
