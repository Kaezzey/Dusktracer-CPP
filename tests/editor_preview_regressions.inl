// Included by the production-editor integration test after its check helper.
static GLuint wait_for_sphere() {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        auto texture = GetMaterialSpherePreview(0);
        const auto& state = g_sphere_preview_states[0].image;
        bool textures_ready = true;
        if (g_sphere_preview_states[0].gpu) {
            for (const auto& path : raster_preview::get_material(0).paths) {
                textures_ready &= raster_preview::get_texture(path).loaded;
            }
        }
        if (texture && textures_ready && !state.dirty && state.requested == state.displayed &&
            state.size == MATERIAL_SPHERE_SIZE) {
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
static void check_preview_camera() {
    for (double yaw : {0., .8, 2.5, -2.5}) {
        for (double pitch : {0., .6, 1.5, -1.5}) {
            material_preview_camera camera(yaw, pitch, 1);
            check(std::abs(camera.right.y()) < 1e-12 && camera.up.y() > 0 &&
                      std::abs(dot(camera.right, camera.outward)) < 1e-12 &&
                      (cross(camera.right, camera.up) - camera.outward).length() < 1e-12,
                  "camera basis stays orthonormal and upright across yaw and pitch");
            hit_record hit{};
            vec3 view;
            check(camera.surface(0, 0, hit, view) && (hit.p - camera.outward).length() < 1e-12 &&
                      (view - camera.outward).length() < 1e-12,
                  "camera center targets the same stationary unit sphere");
            check(camera.surface(.87, 0, hit, view) && !camera.surface(.89, 0, hit, view),
                  "sphere framing is independent of camera orbit");
        }
    }
    const material_preview_camera zoomed_in(1, .5, 1.1), zoomed_out(1, .5, .55);
    check(zoomed_out.eye.length() > zoomed_in.eye.length() && zoomed_out.half_height == zoomed_in.half_height,
          "zoom dollies the camera with a fixed field of view");
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
    auto texture = wait_for_sphere();
    release.set_value();
    std::vector<unsigned char> first(MATERIAL_SPHERE_SIZE * MATERIAL_SPHERE_SIZE * 4), rotated(first.size());
    glBindTexture(GL_TEXTURE_2D, texture);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, first.data());
    check(g_sphere_preview_states[0].gpu, "interactive sphere uses the GPU material evaluator");
    const GLuint original_texture = texture;
    const auto idle_draws = material_sphere::draws;
    GetMaterialSpherePreview(0);
    check(material_sphere::draws == idle_draws, "settled sphere does not redraw");
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
        ImGui::Dummy(ImVec2(0, 600));
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
          "real ImGui drag orbits the preview camera");
    texture = wait_for_sphere();
    GLint width = 0, height = 0;
    glBindTexture(GL_TEXTURE_2D, texture);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
    check(io.MouseDown[0] && texture == original_texture && width == 1024 && height == 1024,
          "dragging reuses the same full-resolution GPU texture");
    io.AddMouseButtonEvent(0, false);
    frame();
    texture = wait_for_sphere();
    glBindTexture(GL_TEXTURE_2D, texture);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, rotated.data());
    check(first != rotated, "orbit changes the sampled material image");
    check(!g_world_dirty && graph == serialize_material_graph(g_scene.materials[0].graph),
          "preview orbit does not edit graph or invalidate renderer");
    io.AddMouseWheelEvent(0, -1);
    frame();
    frame();
    check(g_sphere_preview_states[0].zoom < 1 && panel_scroll == 0,
          "wheel zoom does not also scroll the material properties");
    wait_for_sphere();
    check(g_sphere_preview_states[0].image.size == 1024 &&
              g_sphere_preview_states[0].texture == original_texture,
          "zoom and idle preserve resolution and texture allocation");
    io.AddMousePosEvent(-100, -100);
}
static void check_gpu_sphere() {
    while (glGetError() != GL_NO_ERROR) {
    }
    auto& preview = g_sphere_preview_states[0];
    const auto shader_id = material_sphere::shader.id;
    const auto instructions = raster_preview::get_material(0).instructions;
    std::vector<double> times;
    glFinish();
    for (int i = 0; i < 30; ++i) {
        preview.yaw = .05f * i;
        preview.image.dirty = true;
        const auto start = std::chrono::steady_clock::now();
        GetMaterialSpherePreview(0);
        glFinish();
        times.push_back(
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    }
    std::sort(times.begin(), times.end());
    std::printf("SPHERE warm 1024x1024: median %.3f ms, p95 %.3f ms (30 completed GPU frames)\n", times[15],
                times[28]);
    check(shader_id == material_sphere::shader.id &&
              instructions == raster_preview::get_material(0).instructions,
          "orbit reuses shader and compiled material instructions");

    // Check the actual GPU UVs and tangent-space normal orientation against the
    // CPU camera at off-center pixels, including after a combined yaw/pitch orbit.
    scene_material probe;
    probe.model = scene_material_model::pbr;
    initialize_material_graph(probe, g_scene.textures);
    int uv = probe.graph.add(graph_kind::texcoord, 0, 0);
    probe.graph.find(uv)->value = vec3(1, 1, 1);
    const int zero = probe.graph.add(graph_kind::vector, 0, 0);
    probe.graph.find(zero)->value = vec3(0, 0, 0);
    const int colour_uv = probe.graph.add(graph_kind::add, 0, 0);
    check(probe.graph.connect({uv, 0, colour_uv, 0}) && probe.graph.connect({zero, 0, colour_uv, 1}) &&
              probe.graph.connect({colour_uv, 0, probe.graph.output_id(), 0}),
          "build typed UV comparison graph");
    int normal = probe.graph.add(graph_kind::vector, 0, 0);
    probe.graph.find(normal)->value = vec3(.7, .6, 1);
    check(probe.graph.connect({normal, 0, probe.graph.output_id(), 3}), "build normal comparison graph");
    int index = int(g_scene.materials.size());
    g_scene.materials.push_back(probe);
    auto& state = g_sphere_preview_states[index];
    state.yaw = 1.1f;
    state.pitch = .65f;
    const material_preview_camera camera(state.yaw, state.pitch, 1);
    std::vector<unsigned char> pixels(MATERIAL_SPHERE_SIZE * MATERIAL_SPHERE_SIZE * 4);
    for (int mode : {1, 2}) {
        glUseProgram(shader_id);
        glUniform1i(material_sphere::shader.location("uDebugView"), mode);
        state.image.dirty = true;
        glBindTexture(GL_TEXTURE_2D, GetMaterialSpherePreview(index));
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        for (const auto& pixel : {std::pair<int, int>{512, 512}, {350, 420}, {620, 600}}) {
            const int x = pixel.first, y = pixel.second;
            hit_record hit{};
            vec3 view;
            check(camera.surface(2.0 * (x + .5) / MATERIAL_SPHERE_SIZE - 1,
                                 1 - 2.0 * (y + .5) / MATERIAL_SPHERE_SIZE, hit, view),
                  "GPU comparison pixel intersects sphere");
            const vec3 normal = unit_vector(.4 * hit.tangent + .2 * hit.bitangent + hit.normal);
            const vec3 expected = mode == 1 ? vec3(hit.u, hit.v, 0) : .5 * normal + vec3(.5, .5, .5);
            bool matches = true;
            for (int c = 0; c < 3; ++c) {
                matches &=
                    std::abs(pixels[(y * MATERIAL_SPHERE_SIZE + x) * 4 + c] / 255. - expected[c]) < .012;
            }
            check(matches, mode == 1 ? "GPU orbit preserves sphere UV orientation"
                                     : "GPU orbit preserves normal-map tangent frame");
        }
    }
    glUseProgram(shader_id);
    glUniform1i(material_sphere::shader.location("uDebugView"), 0);
    glUseProgram(0);
    glDeleteTextures(1, &state.texture);
    g_sphere_preview_states.erase(index);
    glDeleteTextures(1, &raster_preview::materials[index].instructions);
    raster_preview::materials.erase(index);
    g_scene.materials.pop_back();

    glViewport(3, 7, 121, 89);
    glEnable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND);
    glColorMask(GL_FALSE, GL_TRUE, GL_FALSE, GL_TRUE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 8);
    glActiveTexture(GL_TEXTURE7);
    glBindTexture(GL_TEXTURE_2D, preview.texture);
    preview.yaw = preview.pitch = 0;
    preview.zoom = 1;
    preview.image.dirty = true;
    GetMaterialSpherePreview(0);
    GLint viewport[4], active = 0, texture = 0, alignment = 0;
    GLboolean mask[4];
    glGetIntegerv(GL_VIEWPORT, viewport);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
    glGetBooleanv(GL_COLOR_WRITEMASK, mask);
    check(viewport[0] == 3 && viewport[1] == 7 && viewport[2] == 121 && viewport[3] == 89 &&
              active == GL_TEXTURE7 && GLuint(texture) == preview.texture && alignment == 8 &&
              glIsEnabled(GL_SCISSOR_TEST) && glIsEnabled(GL_BLEND) && !mask[0] && mask[1] && !mask[2] &&
              mask[3],
          "sphere draw restores caller GL state");
    check(glGetError() == GL_NO_ERROR, "sphere drawing produces no GL errors");
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glActiveTexture(GL_TEXTURE0);
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
