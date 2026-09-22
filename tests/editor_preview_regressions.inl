// Included by the production-editor integration test after its check helper.
static GLuint wait_for_sphere(bool interactive) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        auto texture = GetMaterialSpherePreview(0, interactive);
        const auto& state = g_sphere_preview_states[0].image;
        if (texture && !state.dirty && state.requested == state.displayed &&
            state.size == (interactive ? 192 : 512)) {
            return texture;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    check(false, "sphere preview completes");
    return 0;
}
static void check_preview_cache(const char* profile_path) {
    std::vector<unsigned char> pixels(2048 * 1024 * 4);
    for (size_t p = 0; p < pixels.size(); ++p) {
        pixels[p] = static_cast<unsigned char>((p * 73 + p / 117) % 256);
    }
    check(stbi_write_png("cache-fixture.png", 2048, 1024, 4, pixels.data(), 2048 * 4) != 0,
          "write cache fixture");
    const std::string path = profile_path ? profile_path : "cache-fixture.png";
    const auto identity = "texture1024:" + std::filesystem::absolute(path).lexically_normal().string();
    std::filesystem::remove(preview_cache::cache_path(identity));
    bool hit = false;
    auto start = std::chrono::steady_clock::now();
    auto cold = preview_cache::texture(path, &hit);
    const double cold_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    check(!hit && !cold.image.pixels.empty(), "cold cache decodes texture");
    start = std::chrono::steady_clock::now();
    auto warm = preview_cache::texture(path, &hit);
    const double warm_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    check(hit && warm.image.pixels == cold.image.pixels,
          "warm cache reproduces decoded preview without source decode");
    check(std::max(warm.image.width, warm.image.height) == 1024, "preview source is bounded to 1024 pixels");
    std::printf("Preview texture cache: cold %.2f ms, warm %.2f ms\n", cold_ms, warm_ms);
    auto fixture = preview_cache::texture("cache-fixture.png");
    pixels.assign(pixels.size(), 17);
    check(stbi_write_png("cache-fixture.png", 2048, 1024, 4, pixels.data(), 2048 * 4) != 0,
          "replace texture fixture");
    auto edited = preview_cache::texture("cache-fixture.png", &hit);
    check(!hit && edited.image.pixels != fixture.image.pixels, "texture file edits invalidate disk cache");
    const auto key =
        "texture1024:" + std::filesystem::absolute("cache-fixture.png").lexically_normal().string();
    {
        std::ofstream truncated(preview_cache::cache_path(key), std::ios::binary | std::ios::trunc);
        truncated << "bad";
    }
    auto recovered = preview_cache::texture("cache-fixture.png", &hit);
    check(!hit && recovered.image.pixels == edited.image.pixels, "corrupt cache regenerates from source");
    scene_material material = g_scene.materials[0];
    auto before = MaterialPreviewStamp(g_scene, material);
    material.normal_strength += .1;
    check(before != MaterialPreviewStamp(g_scene, material),
          "material parameters invalidate cached sphere thumbnails");
    rtw_image rgb(1, 1, 3, {80, 120, 160, 255}), rgba(1, 1, 4, {80, 120, 160, 42});
    check(!rgb.has_alpha() && rgba.has_alpha() && rgba.pixel_alpha_byte(0, 0) == 42,
          "preview source preserves original alpha semantics");
}
static void check_sphere_interaction() {
    // A blocked texture task must not hold the interactive sphere queue hostage.
    std::promise<void> release, entered;
    auto released = release.get_future().share();
    auto started = entered.get_future();
    g_texture_previews.request("Tblocked-fixture", 1, [&] {
        entered.set_value();
        released.wait();
        return preview_image{};
    });
    check(started.wait_for(std::chrono::seconds(2)) == std::future_status::ready,
          "asset worker blocker starts");
    auto texture = wait_for_sphere(false);
    release.set_value();
    std::vector<unsigned char> first(512 * 512 * 4), rotated(first.size());
    glBindTexture(GL_TEXTURE_2D, texture);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, first.data());
    const auto graph = serialize_material_graph(g_scene.materials[0].graph);
    g_world_dirty = false;
    auto& io = ImGui::GetIO();
    ImVec2 center;
    float panel_scroll = 0;
    auto frame = [&] {
        io.DisplaySize = ImVec2(1440, 960);
        io.DeltaTime = 1.f / 60;
        ImGui_ImplOpenGL3_NewFrame();
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(400, 500));
        ImGui::Begin("Sphere test", nullptr, ImGuiWindowFlags_NoSavedSettings);
        center = ImGui::GetCursorScreenPos();
        center.x += 100;
        center.y += 100;
        DrawMaterialSpherePreview(0);
        ImGui::Dummy(ImVec2(0,600));
        panel_scroll = ImGui::GetScrollY();
        ImGui::End();
        ImGui::Render();
    };
    frame();
    frame();
    io.AddMousePosEvent(center.x, center.y);
    frame();
    io.AddMouseButtonEvent(0, true);
    frame();
    io.AddMousePosEvent(center.x + 70, center.y + 30);
    frame();
    frame();
    check(g_sphere_preview_states[0].yaw != 0 && g_sphere_preview_states[0].pitch != 0,
          "real ImGui drag rotates the preview sphere");
    io.AddMouseButtonEvent(0, false);
    frame();
    texture = wait_for_sphere(false);
    glBindTexture(GL_TEXTURE_2D, texture);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, rotated.data());
    check(first != rotated, "orbit changes the sampled material image");
    check(!g_world_dirty && graph == serialize_material_graph(g_scene.materials[0].graph),
          "preview orbit does not edit graph or invalidate renderer");
    io.AddMouseWheelEvent(0,-1); frame(); frame();
    check(g_sphere_preview_states[0].zoom < 1 && panel_scroll == 0,
          "wheel zoom does not also scroll the material properties");
    wait_for_sphere(true);
    check(g_sphere_preview_states[0].image.size == 192, "interactive preview uses bounded fast resolution");
    wait_for_sphere(false);
    check(g_sphere_preview_states[0].image.size == 512, "idle preview refines to 512 pixels");
    io.AddMousePosEvent(-100, -100);
}
static void check_outliner_selection() {
    UndoManager::Instance().clear();
    AddSceneObject(scene_object_type::sphere);
    AddSceneLight(scene_light_type::point);
    check(g_selected_object == -1 && g_selected_light == 0, "adding a light selects its properties");
    AddSceneLight(scene_light_type::directional);
    check(g_camera.use_sun && g_camera.sun_angular_radius == .266,
          "adding a sun immediately updates the raster lighting");
    SelectSceneLight(0);
    DeleteSceneSelection();
    check(g_scene.lights.size() == 1 && g_scene.lights[0].type == scene_light_type::directional,
          "delete removes the selected light rather than last light");
    UndoManager::Instance().undo();
    check(g_scene.lights.size() == 2 && g_selected_light == 0,
          "light deletion undo restores selection and order");
    UndoManager::Instance().redo();
    check(g_scene.lights.size() == 1, "light deletion redo");
    SelectSceneObject(0);
    check(g_selected_light == -1, "object selection clears light selection");
    auto& io = ImGui::GetIO();
    for (int i = 0; i < 3; ++i) {
        io.DisplaySize = ImVec2(1440, 960);
        io.DeltaTime = 1.f / 60;
        ImGui_ImplOpenGL3_NewFrame();
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(400, 360));
        DrawSceneOutliner();
        ImGui::SetNextWindowPos(ImVec2(0, 360));
        ImGui::SetNextWindowSize(ImVec2(400, 600));
        ImGui::Begin("Properties");
        SelectSceneLight(0);
        DrawSelectedLight();
        ImGui::End();
        ImGui::Render();
        glViewport(0, 0, 1440, 960);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glFinish();
    }
    std::vector<unsigned char> pixels(1440 * 960 * 4);
    glReadPixels(0, 0, 1440, 960, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    stbi_flip_vertically_on_write(1);
    check(stbi_write_png("outliner-properties.png", 1440, 960, 4, pixels.data(), 1440 * 4) != 0,
          "outliner and selected light screenshot");
    stbi_flip_vertically_on_write(0);
    g_scene.objects.clear();
    g_scene.lights.clear();
    SelectSceneObject(-1);
    UndoManager::Instance().clear();
}
