// Scene navigation and selection-dependent light properties.
static void SceneChanged() {
    g_world_dirty = true;
    g_cached_world.reset();
    g_camera.use_sun = false;
    for (const auto& light : g_scene.lights) {
        if (light.type == scene_light_type::directional) {
            g_camera.use_sun = true;
            g_camera.sun_dir = -light.direction;
            g_camera.sun_radiance = light.radiance;
            g_camera.sun_angular_radius = light.angular_radius_deg;
        }
    }
    if (g_scene.lights.empty()) {
        g_camera.enable_mnee = false;
    }
}
static void SelectSceneObject(int index) {
    g_focus_properties = index >= 0;
    g_selected_object = index;
    g_selected_light = -1;
    g_selected_mesh_asset = -1;
}
static void SelectSceneLight(int index) {
    g_focus_properties = index >= 0;
    g_selected_light = index;
    g_selected_object = -1;
    g_selected_mesh_asset = -1;
}
static void AddSceneObject(scene_object_type type) {
    scene_object object;
    object.name = std::string(type == scene_object_type::sphere ? "Sphere " : "Cube ") +
                  std::to_string(g_scene.objects.size() + 1);
    object.type = type;
    object.center = point3(0, 0, -1);
    object.radius = type == scene_object_type::sphere ? .5 : std::sqrt(3.) * .5;
    const int index = int(g_scene.objects.size());
    auto insert = [index, object] {
        const int at = std::min(index, int(g_scene.objects.size()));
        g_scene.objects.insert(g_scene.objects.begin() + at, object);
        SelectSceneObject(at);
        SceneChanged();
    };
    auto erase = [index] {
        if (index < int(g_scene.objects.size())) {
            g_scene.objects.erase(g_scene.objects.begin() + index);
        }
        SelectSceneObject(-1);
        SceneChanged();
    };
    insert();
    UndoManager::Instance().push(std::make_unique<LambdaAction>(erase, insert, "Add Object"));
}
static void AddSceneLight(scene_light_type type) {
    scene_light light;
    light.type = type;
    light.name = std::string(type == scene_light_type::point ? "Point " : "Sun ") +
                 std::to_string(g_scene.lights.size() + 1);
    light.radiance = type == scene_light_type::point ? vec3(3, 3, 3) : vec3(20, 20, 20);
    light.position = point3(0, 5, 0);
    light.range = 10;
    light.direction = unit_vector(vec3(-.3, -1, .2));
    light.angular_radius_deg = .266;
    const int index = int(g_scene.lights.size());
    auto insert = [index, light] {
        const int at = std::min(index, int(g_scene.lights.size()));
        g_scene.lights.insert(g_scene.lights.begin() + at, light);
        SelectSceneLight(at);
        SceneChanged();
    };
    auto erase = [index] {
        if (index < int(g_scene.lights.size())) {
            g_scene.lights.erase(g_scene.lights.begin() + index);
        }
        SelectSceneLight(-1);
        SceneChanged();
    };
    insert();
    UndoManager::Instance().push(std::make_unique<LambdaAction>(erase, insert, "Add Light"));
}
static void DeleteSceneSelection() {
    if (g_selected_light >= 0 && g_selected_light < int(g_scene.lights.size())) {
        const int index = g_selected_light;
        const auto light = g_scene.lights[index];
        auto insert = [index, light] {
            const int at = std::min(index, int(g_scene.lights.size()));
            g_scene.lights.insert(g_scene.lights.begin() + at, light);
            SelectSceneLight(at);
            SceneChanged();
        };
        auto erase = [index] {
            if (index < int(g_scene.lights.size())) {
                g_scene.lights.erase(g_scene.lights.begin() + index);
            }
            SelectSceneLight(-1);
            SceneChanged();
        };
        erase();
        UndoManager::Instance().push(std::make_unique<LambdaAction>(insert, erase, "Delete Light"));
    } else if (g_selected_object >= 0 && g_selected_object < int(g_scene.objects.size())) {
        const int index = g_selected_object;
        const auto object = g_scene.objects[index];
        auto insert = [index, object] {
            const int at = std::min(index, int(g_scene.objects.size()));
            g_scene.objects.insert(g_scene.objects.begin() + at, object);
            SelectSceneObject(at);
            SceneChanged();
        };
        auto erase = [index] {
            if (index < int(g_scene.objects.size())) {
                g_scene.objects.erase(g_scene.objects.begin() + index);
            }
            SelectSceneObject(-1);
            SceneChanged();
        };
        erase();
        UndoManager::Instance().push(std::make_unique<LambdaAction>(insert, erase, "Delete Object"));
    }
}
static void DrawSceneOutliner() {
    ImGui::Begin("Outliner");
    if (ImGui::Button("+ Add")) {
        ImGui::OpenPopup("AddSceneItem");
    }
    if (ImGui::BeginPopup("AddSceneItem")) {
        if (ImGui::MenuItem("Sphere")) {
            AddSceneObject(scene_object_type::sphere);
        }
        if (ImGui::MenuItem("Cube")) {
            AddSceneObject(scene_object_type::cube);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Point light")) {
            AddSceneLight(scene_light_type::point);
        }
        if (ImGui::MenuItem("Sun light")) {
            AddSceneLight(scene_light_type::directional);
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(g_selected_object < 0 && g_selected_light < 0);
    if (ImGui::Button("Delete")) {
        DeleteSceneSelection();
    }
    ImGui::EndDisabled();
    static ImGuiTextFilter search;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    if (ImGui::InputTextWithHint("##SceneSearch", "Search objects and lights...", search.InputBuf,
                                 IM_ARRAYSIZE(search.InputBuf))) {
        search.Build();
    }
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(6, 3));
    const float footer = ImGui::GetTextLineHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
    if (ImGui::BeginTable("SceneItems", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                          ImVec2(0, -footer))) {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 64);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        auto group = [](const char* title) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            return ImGui::TreeNodeEx(title,
                                     ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAllColumns);
        };
        if (group("Objects")) {
            for (int i = 0; i < int(g_scene.objects.size()); ++i) {
                const auto& object = g_scene.objects[i];
                if (!search.PassFilter(object.name.c_str())) {
                    continue;
                }
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushID(i);
                if (ImGui::Selectable(object.name.c_str(), g_selected_object == i,
                                      ImGuiSelectableFlags_SpanAllColumns)) {
                    SelectSceneObject(i);
                }
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", object.type == scene_object_type::sphere ? "Sphere"
                                          : object.type == scene_object_type::cube ? "Cube"
                                                                                   : "Mesh");
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
        if (group("Lights")) {
            for (int i = 0; i < int(g_scene.lights.size()); ++i) {
                const auto& light = g_scene.lights[i];
                if (!search.PassFilter(light.name.c_str())) {
                    continue;
                }
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushID(i);
                if (ImGui::Selectable(light.name.c_str(), g_selected_light == i,
                                      ImGuiSelectableFlags_SpanAllColumns)) {
                    SelectSceneLight(i);
                }
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", light.type == scene_light_type::point ? "Point" : "Sun");
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleVar();
    ImGui::TextDisabled("%d objects  /  %d lights", int(g_scene.objects.size()), int(g_scene.lights.size()));
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput &&
        ImGui::IsKeyPressed(ImGuiKey_Delete)) {
        DeleteSceneSelection();
    }
    ImGui::End();
}
static void DrawSelectedLight() {
    auto& light = g_scene.lights[g_selected_light];
    DrawPanelTitle(light.name.c_str(),
                   light.type == scene_light_type::point ? "Point light" : "Directional light");
    ImGui::PushItemWidth(std::max(100.f, ImGui::GetContentRegionAvail().x - 135.f));
    char namebuf[256] = {0};
    std::strncpy(namebuf, light.name.c_str(), sizeof(namebuf) - 1);
    if (ImGui::InputText("Name", namebuf, sizeof(namebuf))) {
        light.name = std::string(namebuf);
    }

    // Radiance (colour + intensity)
    float radR = (float)light.radiance.x();
    float radG = (float)light.radiance.y();
    float radB = (float)light.radiance.z();
    float intensity = std::max({radR, radG, radB, 0.f});
    float color[3] = {1, 1, 1};
    if (intensity > 0) {
        color[0] = radR / intensity;
        color[1] = radG / intensity;
        color[2] = radB / intensity;
    }

    bool changed = false;
    if (ImGui::ColorEdit3("Light Color", color)) {
        changed = true;
    }
    float inten_f = intensity;
    if (ImGui::DragFloat("Intensity", &inten_f, 0.1f, 0.0f, 1e6f)) {
        if (inten_f < 0.0f) {
            inten_f = 0.0f;
        }
        changed = true;
    }

    if (changed) {
        light.radiance = vec3(color[0] * inten_f, color[1] * inten_f, color[2] * inten_f);
        SceneChanged();
    }

    if (light.type == scene_light_type::directional) {

        // Convert stored light.direction (light -> scene) to a user-friendly
        // sun direction from scene -> sun for angles. We expose Azimuth and
        // Elevation (degrees) sliders which are easier to reason about.
        vec3 sun_dir = -light.direction; // scene -> sun

        // Compute azimuth (0..360) and elevation (-90..90) from sun_dir
        double toDeg = 180.0 / 3.14159265358979323846;
        double toRad = 3.14159265358979323846 / 180.0;
        double az = std::atan2((double)sun_dir.z(), (double)sun_dir.x()) * toDeg;
        if (az < 0.0) {
            az += 360.0;
        }
        double el = std::asin(std::clamp((double)sun_dir.y(), -1.0, 1.0)) * toDeg;

        float azf = (float)az;
        float elf = (float)el;
        bool ang_changed = false;
        if (ImGui::SliderFloat("Azimuth (deg)", &azf, 0.0f, 360.0f)) {
            ang_changed = true;
        }
        if (ImGui::SliderFloat("Elevation (deg)", &elf, -90.0f, 90.0f)) {
            ang_changed = true;
        }

        if (ang_changed) {
            double azr = (double)azf * toRad;
            double elr = (double)elf * toRad;
            vec3 new_sun_dir((float)(std::cos(elr) * std::cos(azr)), (float)std::sin(elr),
                             (float)(std::cos(elr) * std::sin(azr)));
            light.direction = -new_sun_dir; // store as light -> scene
            SceneChanged();

            // Mirror into camera sun parameters.
            g_camera.use_sun = true;
            g_camera.sun_dir = new_sun_dir; // scene -> sun
            g_camera.sun_radiance = colour(light.radiance.x(), light.radiance.y(), light.radiance.z());
        }

        // Angular radius control (degrees)
        double angv = light.angular_radius_deg;
        double ang_min = 0.0, ang_max = 10.0;
        if (ImGui::SliderScalar("Sun radius (deg)", ImGuiDataType_Double, &angv, &ang_min, &ang_max)) {
            light.angular_radius_deg = angv;
            g_camera.sun_angular_radius = angv;
            SceneChanged();
        }

        int sun_samples = g_camera.sun_shadow_samples;
        if (ImGui::DragInt("Shadow samples", &sun_samples, 1, 0, 64)) {
            if (sun_samples < 0) {
                sun_samples = 0;
            }
            if (sun_samples > 256) {
                sun_samples = 256;
            }
            g_camera.sun_shadow_samples = sun_samples;
            SceneChanged();
        }
    } else {
        float posf[3] = {(float)light.position.x(), (float)light.position.y(), (float)light.position.z()};
        if (ImGui::DragFloat3("Position", posf, 0.1f)) {
            light.position = point3(posf[0], posf[1], posf[2]);
            SceneChanged();
        }
        float rangef = (float)light.range;
        if (ImGui::DragFloat("Range", &rangef, 0.1f, 0.0f, 1e6f)) {
            light.range = rangef;
            SceneChanged();
        }
    }
    ImGui::PopItemWidth();
}
static void DrawCausticSettings() {
    if (ImGui::CollapsingHeader("Experimental caustics")) {
        {
            bool prev = g_camera.enable_mnee;
            ImGui::Checkbox("Approximate caustics (experimental)", &g_camera.enable_mnee);
            if (g_camera.enable_mnee && !prev) {
                UpdateMNEEFromScene();
            }
            if (!g_camera.enable_mnee) {
                g_camera.mnee_has_sphere = false;
            }
            if (g_camera.enable_mnee) {
                if (g_camera.mnee_has_sphere) {
                    ImGui::Text("MNEE Sphere: C=(%.3f, %.3f, %.3f) R=%.3f IOR=%.3f",
                                g_camera.mnee_sphere_center.x(), g_camera.mnee_sphere_center.y(),
                                g_camera.mnee_sphere_center.z(), g_camera.mnee_sphere_radius,
                                g_camera.mnee_sphere_ior);
                    ImGui::SliderInt("MNEE Budget/thread", &g_camera.mnee_per_thread_budget, 1, 256);
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Point light caustics budget. 1-16 = fast preview, 64-256 = final "
                                          "quality.\nWARNING: High values cause EXTREME slowdown!");
                    }
                    ImGui::SliderInt("MNEE Sun samples", &g_camera.mnee_sun_samples, 1, 32);
                    {
                        float gain = (float)g_camera.mnee_gain_scale;
                        if (ImGui::SliderFloat("MNEE Gain scale", &gain, 0.5f, 3.0f)) {
                            g_camera.mnee_gain_scale = gain;
                        }
                    }
                    {
                        double ang = g_camera.sun_angular_radius;
                        float angf = (float)ang;
                        if (ImGui::SliderFloat("Sun angular radius (deg)", &angf, 0.0f, 2.0f)) {
                            g_camera.sun_angular_radius = (double)angf;
                        }
                    }
                } else {
                    ImGui::TextDisabled("No dielectric sphere found in scene.");
                }
            }
        }
    }
}
