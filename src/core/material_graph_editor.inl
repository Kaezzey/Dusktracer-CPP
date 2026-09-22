// Included by editor_main.cpp: canvas interaction stays separate from graph data
// and renderer evaluation. All hit testing uses the same pan/zoom transform.
struct graph_pin {
    int node = 0, slot = 0;
    bool input = false;
    explicit operator bool() const { return node != 0; }
};
struct material_graph_editor_state {
    std::vector<int> selected;
    graph_pin dragging, popup_pin, rewire_pin, popup_rewire;
    int selected_wire = -1;
    bool moving = false, marquee = false, popup_focus = false, fit = false;
    ImVec2 marquee_start, popup_local;
    material_graph before_drag, before_edit;
    bool editing = false;
    uint64_t surface_key = 0, topology_key = 0;
    bool surface_known = false;
    std::unique_ptr<graph_analysis> analysis;
    graph_pin compatibility_anchor;
    std::unordered_map<uint64_t,bool> compatible_pins;
    std::array<int,(int)graph_kind::count> compatible_kinds{};
    char search[160] = {};
    int search_row = 0;
    std::string message;
    double message_until = 0;
};
static std::unordered_map<int, material_graph_editor_state> g_graph_states;
static material_graph g_graph_clipboard;
static ImVec2 g_graph_canvas_min, g_graph_canvas_max;
static int g_graph_canvas_material = -1, g_graph_file_drop_material = -1;
static ImVec2 g_graph_file_drop_local;

static ImVec2 GraphAdd(ImVec2 a, ImVec2 b) { return ImVec2(a.x + b.x, a.y + b.y); }
static ImVec2 GraphSub(ImVec2 a, ImVec2 b) { return ImVec2(a.x - b.x, a.y - b.y); }
static ImVec2 GraphMul(ImVec2 a, float v) { return ImVec2(a.x * v, a.y * v); }
static float GraphDistance(ImVec2 a, ImVec2 b) { ImVec2 d = GraphSub(a,b); return d.x*d.x + d.y*d.y; }
static bool GraphContains(ImVec2 p, ImVec2 a, ImVec2 b) { return p.x >= a.x && p.y >= a.y && p.x <= b.x && p.y <= b.y; }
static bool GraphSelected(const material_graph_editor_state& s, int id) { return std::find(s.selected.begin(), s.selected.end(), id) != s.selected.end(); }
static int GraphActiveSlots(int index) { return g_scene.materials[index].model == scene_material_model::pbr ? 5 : 1; }
static void GraphDirty(int index, bool shading = true) {
    MarkMaterialsDirty();
    if (!shading) return;
    auto& state = g_graph_states[index];
    auto key = graph_surface_key(g_scene.materials[index].graph,GraphActiveSlots(index));
    if (!state.surface_known || state.surface_key != key) {
        // Retain the runtime world until Render replaces it. Deleting a large
        // BVH/Embree scene during a color drag can stall the main thread.
        g_world_dirty = true; InvalidateMaterialThumbnail(index);
    }
    state.surface_key = key; state.surface_known = true;
}
static void RestoreGraph(int index, const material_graph& graph) {
    if (index < 0 || index >= (int)g_scene.materials.size()) return;
    auto& state = g_graph_states[index];
    state.surface_key = graph_surface_key(g_scene.materials[index].graph,GraphActiveSlots(index)); state.surface_known = true;
    g_scene.materials[index].graph = graph;
    state.dragging = {}; state.moving = false; GraphDirty(index);
}
static void GraphCommit(int index, const material_graph& before, const char* label, bool shading = true) {
    auto& graph = g_scene.materials[index].graph;
    if (graph_equal(before,graph)) return;
    auto& state = g_graph_states[index];
    // A disconnected node, layout edit, or inactive output cannot alter shading.
    if (!state.surface_known) {
        state.surface_key = graph_surface_key(before,GraphActiveSlots(index)); state.surface_known = true;
    }
    UndoManager::Instance().push(std::make_unique<LambdaAction>(
        [index, before]() { RestoreGraph(index,before); },
        [index, after = graph]() { RestoreGraph(index,after); },label));
    state.selected_wire = -1;
    GraphDirty(index,shading);
}
static const graph_analysis& GraphAnalysis(material_graph_editor_state& state, const material_graph& graph) {
    auto key = graph_topology_key(graph);
    if (!state.analysis || state.topology_key != key) {
        state.analysis = std::make_unique<graph_analysis>(graph); state.topology_key = key;
        state.compatible_pins.clear(); state.compatible_kinds.fill(0);
    }
    return *state.analysis;
}
static bool GraphCanConnect(material_graph_editor_state& state, const material_graph& graph, graph_pin anchor, graph_pin other) {
    GraphAnalysis(state,graph);
    if (state.compatibility_anchor.node != anchor.node || state.compatibility_anchor.slot != anchor.slot || state.compatibility_anchor.input != anchor.input) {
        state.compatibility_anchor = anchor; state.compatible_pins.clear(); state.compatible_kinds.fill(0);
    }
    uint64_t key = ((uint64_t)other.node << 4) | (other.slot << 1) | (other.input ? 1u : 0u);
    auto found = state.compatible_pins.find(key);
    if (found != state.compatible_pins.end()) return found->second;
    auto a = anchor, b = other; if (a.input) std::swap(a,b);
    return state.compatible_pins[key] = graph.can_connect({a.node,a.slot,b.node,b.slot});
}
static void GraphMessage(material_graph_editor_state& s, const std::string& message) { s.message = message; s.message_until = ImGui::GetTime() + 4; }
static ImU32 GraphColor(int width) {
    return width == 3 ? IM_COL32(111, 191, 228, 255) : width == 2 ? IM_COL32(190, 156, 225, 255) : IM_COL32(144, 213, 179, 255);
}
static ImU32 GraphNodeColor(graph_kind k) {
    if (k == graph_kind::output) return IM_COL32(82, 184, 164, 255);
    if (k == graph_kind::texture_sample) return IM_COL32(93, 157, 204, 255);
    if (k == graph_kind::texcoord) return GraphColor(2);
    if (k == graph_kind::scalar || k == graph_kind::vector) return IM_COL32(193, 169, 111, 255);
    return IM_COL32(106, 156, 153, 255);
}
static int GraphInputCount(const graph_node& n, const scene_material& mat) {
    return n.kind == graph_kind::output && mat.model != scene_material_model::pbr ? 1 : graph_inputs(n.kind);
}
static ImVec2 GraphNodeSize(const graph_node& n, const scene_material& mat) {
    if (n.kind == graph_kind::reroute) return ImVec2(72, 40);
    int rows = std::max(GraphInputCount(n, mat), graph_outputs(n.kind));
    return ImVec2(n.kind == graph_kind::output ? 238.0f : 218.0f,
        88.0f + rows * 27.0f + (n.kind == graph_kind::texture_sample ? 44.0f : 0.0f));
}
static ImVec2 GraphPinLocal(const graph_node& n, graph_pin p, const scene_material& m) {
    ImVec2 size = GraphNodeSize(n, m);
    return ImVec2(n.x + (p.input ? 0.0f : size.x), n.y + (n.kind == graph_kind::reroute ? 20.0f : 62.0f + p.slot * 27.0f));
}
static graph_link GraphLink(graph_pin a, graph_pin b) {
    if (a.input) std::swap(a, b);
    return {a.node, a.slot, b.node, b.slot};
}
static int GraphTextureIndex(const graph_node& n, const scene& scn) {
    if (!n.texture_path.empty()) {
        for (int i = 0; i < (int)scn.textures.size(); ++i) if (scn.textures[i].path == n.texture_path) return i;
        return -1;
    }
    return n.texture_index >= 0 && n.texture_index < (int)scn.textures.size() ? n.texture_index : -1;
}
static int GraphAddTexture(material_graph& g, const scene& scn, int index, ImVec2 local) {
    int id = g.add(graph_kind::texture_sample, local.x, local.y);
    if (auto n = g.find(id)) { n->texture_index = index; n->texture_path = scn.textures[index].path; }
    return id;
}
static bool GraphAutoConnect(material_graph& g, graph_pin anchor, int id) {
    const auto* node = g.find(id);
    if (!node || !anchor) return false;
    int count = anchor.input ? graph_outputs(node->kind) : graph_inputs(node->kind);
    for (int p = 0; p < count; ++p) {
        graph_pin other{id, p, !anchor.input};
        if (g.connect(GraphLink(anchor, other))) return true;
    }
    return false;
}
static void GraphCopy(const material_graph& g, const material_graph_editor_state& s) {
    g_graph_clipboard = {};
    for (const auto& n : g.nodes) if (GraphSelected(s, n.id) && n.kind != graph_kind::output) g_graph_clipboard.nodes.push_back(n);
    for (const auto& l : g.links) if (g_graph_clipboard.find(l.from) && g_graph_clipboard.find(l.to)) g_graph_clipboard.links.push_back(l);
}
static void GraphPaste(material_graph& g, material_graph_editor_state& s, ImVec2 at) {
    if (g_graph_clipboard.nodes.empty()) return;
    float x = g_graph_clipboard.nodes.front().x, y = g_graph_clipboard.nodes.front().y;
    for (const auto& n : g_graph_clipboard.nodes) { x = std::min(x,n.x); y = std::min(y,n.y); }
    std::unordered_map<int,int> remap; s.selected.clear();
    for (auto n : g_graph_clipboard.nodes) {
        int id = g.add(n.kind, 0, 0); if (!id) break;
        remap[n.id] = id; n.id = id; n.x += at.x - x; n.y += at.y - y;
        *g.find(id) = n; s.selected.push_back(id);
    }
    for (auto l : g_graph_clipboard.links) if (remap.count(l.from) && remap.count(l.to)) {
        l.from = remap[l.from]; l.to = remap[l.to]; g.connect(l);
    }
}
static void CaptureMaterialGraphFileDrop(GLFWwindow* window) {
    g_graph_file_drop_material = -1;
    if (g_selected_material < 0 || g_graph_canvas_material != g_selected_material) return;
    double x, y; int wx, wy; glfwGetCursorPos(window, &x, &y); glfwGetWindowPos(window, &wx, &wy);
    ImVec2 mouse((float)x, (float)y);
    if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) mouse = GraphAdd(mouse, ImVec2((float)wx,(float)wy));
    if (!GraphContains(mouse, g_graph_canvas_min, g_graph_canvas_max)) return;
    g_graph_file_drop_material = g_selected_material;
    auto& g = g_scene.materials[g_selected_material].graph;
    g_graph_file_drop_local = GraphMul(GraphSub(GraphSub(mouse, g_graph_canvas_min), ImVec2(g.pan_x,g.pan_y)), 1/g.zoom);
}
static void MaterialGraphImportedTexture(int index) {
    if (g_graph_file_drop_material < 0 || g_graph_file_drop_material >= (int)g_scene.materials.size()) return;
    int material = g_graph_file_drop_material;
    auto& g = g_scene.materials[material].graph; auto before = g;
    int id = GraphAddTexture(g, g_scene, index, g_graph_file_drop_local);
    g_graph_states[material].selected = {id};
    g_graph_file_drop_local = GraphAdd(g_graph_file_drop_local, ImVec2(32,32));
    GraphCommit(material, before, "Drop texture into graph");
}

// Capture history only on the first change, before applying the widget value.
static void GraphBeginEdit(material_graph& g, material_graph_editor_state& state, bool changed) {
    if (!state.editing && changed) { state.before_edit = g; state.editing = true; }
}
static void GraphEndEdit(material_graph_editor_state& state, int index, bool changed) {
    if (changed) GraphDirty(index);
    if (ImGui::IsItemDeactivatedAfterEdit() && state.editing) {
        GraphCommit(index,state.before_edit,"Edit node value"); state.editing = false;
    }
}
static void GraphNodeProperties(scene_material& mat, scene& scn, int index, graph_node& node) {
    auto& g = mat.graph; auto& state = g_graph_states[index];
    ImGui::PushID(node.id);
    if (node.kind == graph_kind::texture_sample) {
        int ti = GraphTextureIndex(node, scn);
        if (ImGui::BeginCombo("Texture", ti >= 0 ? scn.textures[ti].name.c_str() : "Choose texture...")) {
            for (int i = 0; i < (int)scn.textures.size(); ++i) if (ImGui::Selectable(scn.textures[i].name.c_str(), i == ti)) {
                auto before = g; node.texture_index = i; node.texture_path = scn.textures[i].path;
                GraphCommit(index, before, "Assign texture sample");
            }
            ImGui::EndCombo();
        }
        int mode = static_cast<int>(node.space);
        if (ImGui::Combo("Sample space", &mode, "Automatic\0sRGB color\0Linear data\0")) {
            auto before = g; node.space = static_cast<graph_space>(mode); GraphCommit(index, before, "Set texture sample space");
        }
        ImGui::TextDisabled("Auto: color for Base Color; linear for data.");
    } else if (node.kind == graph_kind::scalar || node.kind == graph_kind::vector || node.kind == graph_kind::texcoord) {
        float v[] = {(float)node.value.x(), (float)node.value.y(), (float)node.value.z()};
        bool changed = node.kind == graph_kind::scalar ? ImGui::DragFloat("Value", v, 0.01f) :
            node.kind == graph_kind::vector ? ImGui::ColorEdit3("Color", v, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR) :
            ImGui::DragFloat2("UV tiling", v, 0.05f);
        GraphBeginEdit(g,state,changed);
        if (changed) node.value = vec3(v[0],v[1],v[2]);
        GraphEndEdit(state,index,changed);
    }
    if (node.kind != graph_kind::output && node.kind != graph_kind::texture_sample) {
        for (int p = 0; p < graph_inputs(node.kind); ++p) if (!g.incoming(node.id,p)) {
            float v = (float)node.defaults[p];
            bool changed = ImGui::DragFloat(graph_input_name(node.kind,p), &v, 0.01f);
            GraphBeginEdit(g,state,changed);
            if (changed) node.defaults[p] = v;
            GraphEndEdit(state,index,changed);
        }
    }
    ImGui::PopID();
}
static void DrawMaterialGraphDetails(scene_material& mat, scene& scn, int index) {
    if (mat.graph.nodes.empty()) {
        initialize_material_graph(mat, scn.textures); InvalidateMaterialThumbnail(index);
        g_graph_states[index].fit = true; MarkMaterialsDirty();
    }
    DrawMaterialSpherePreview(index);
    auto& state = g_graph_states[index];
    if (state.selected.size() == 1) if (auto node = mat.graph.find(state.selected.front())) {
        if (node->kind != graph_kind::output) {
            DrawPanelTitle(graph_name(node->kind), "Selected node");
            GraphNodeProperties(mat,scn,index,*node);
            ImGui::Separator();
        }
    }
}

static void DrawMaterialGraphEditor(scene_material& mat, scene& scn, int material_index) {
    auto& g = mat.graph; auto& state = g_graph_states[material_index];
    if (g.nodes.empty()) { initialize_material_graph(mat,scn.textures); state.fit = true; MarkMaterialsDirty(); }
    state.selected.erase(std::remove_if(state.selected.begin(),state.selected.end(),[&](int id) { return !g.find(id); }),state.selected.end());
    if (state.dragging && !g.find(state.dragging.node)) state.dragging = {};
    if (!state.surface_known) {
        state.surface_key = graph_surface_key(g,GraphActiveSlots(material_index)); state.surface_known = true;
    }
    ImGui::PushID(material_index);
    bool open_search = ImGui::Button("+ Add node");
    if (open_search) { state.popup_local = ImVec2(80,80); state.popup_pin = {}; }
    ImGui::SameLine();
    if (ImGui::Button("Frame  F")) state.fit = true;
    ImGui::SameLine();
    if (ImGui::Button("1:1")) { g.zoom = 1; MarkMaterialsDirty(); }
    ImGui::SameLine(); ImGui::TextDisabled("%d%%", (int)(g.zoom * 100));
    ImGui::SameLine();
    ImGui::TextDisabled("  %d nodes  /  %d links", (int)g.nodes.size(),(int)g.links.size());
    ImGui::SameLine();
    if (ImGui::SmallButton("?")) ImGui::OpenPopup("GraphHelp");
    if (ImGui::BeginPopup("GraphHelp")) {
        ImGui::TextUnformatted("MATERIAL GRAPH"); ImGui::Separator();
        ImGui::TextUnformatted("Right-click: search nodes and textures\nDrag a pin: connect in either direction\nCtrl-drag connected input: move its wire\nRelease a wire on the canvas: add connected node\nAlt-click pin / wire: break connection\nDouble-click wire: insert reroute\nDouble-click node: edit values\nRight / middle drag: pan   |   Wheel: zoom\nLeft drag: box select   |   Ctrl-click: multi-select\nF: frame selection / all   |   Delete: remove\nCtrl+C / V / D: copy / paste / duplicate\nCtrl+Z / Y: undo / redo\nHold 1 / 3 / T / M / A / L + click: quick add\nCtrl+Space: open the Content Drawer");
        ImGui::EndPopup();
    }
    ImGui::TextDisabled("Drag textures here  /  Right-click to add  /  Drag pins to connect");
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0,0));
    ImGui::BeginChild("GraphCanvas", ImVec2(0,0), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    ImVec2 origin = ImGui::GetCursorScreenPos(), size = ImGui::GetContentRegionAvail();
    size.x = std::max(80.0f,size.x); size.y = std::max(80.0f,size.y);
    ImVec2 end = GraphAdd(origin,size);
    g_graph_canvas_min = origin; g_graph_canvas_max = end; g_graph_canvas_material = material_index;
    ImGui::InvisibleButton("CanvasHit",size,ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    bool hovered = ImGui::IsItemHovered();
    bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);
    auto& io = ImGui::GetIO(); ImVec2 mouse = io.MousePos;
    auto local = [&](ImVec2 p) { return GraphMul(GraphSub(GraphSub(p,origin),ImVec2(g.pan_x,g.pan_y)),1/g.zoom); };
    auto screen = [&](ImVec2 p) { return GraphAdd(origin,GraphAdd(ImVec2(g.pan_x,g.pan_y),GraphMul(p,g.zoom))); };

    if (state.fit || (hovered && !io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_F))) {
        ImVec2 lo(1e10f,1e10f), hi(-1e10f,-1e10f);
        for (const auto& n : g.nodes) {
            if (!state.selected.empty() && !GraphSelected(state,n.id)) continue;
            ImVec2 ns = GraphNodeSize(n,mat);
            lo.x = std::min(lo.x,n.x); lo.y = std::min(lo.y,n.y);
            hi.x = std::max(hi.x,n.x+ns.x); hi.y = std::max(hi.y,n.y+ns.y);
        }
        if (lo.x <= hi.x) {
            g.zoom = std::clamp(std::min((size.x-100)/(hi.x-lo.x),(size.y-100)/(hi.y-lo.y)),0.35f,1.1f);
            g.pan_x = size.x/2 - (lo.x+hi.x)*g.zoom/2; g.pan_y = size.y/2 - (lo.y+hi.y)*g.zoom/2;
        }
        state.fit = false; MarkMaterialsDirty();
    }
    if (hovered && io.MouseWheel && !state.dragging) {
        ImVec2 anchor = local(mouse);
        g.zoom = std::clamp(g.zoom * std::pow(1.12f,io.MouseWheel),0.35f,1.65f);
        g.pan_x = mouse.x-origin.x-anchor.x*g.zoom; g.pan_y = mouse.y-origin.y-anchor.y*g.zoom;
        MarkMaterialsDirty();
    }
    if (ImGui::IsItemActive() && (ImGui::IsMouseDragging(ImGuiMouseButton_Middle) || ImGui::IsMouseDragging(ImGuiMouseButton_Right))) {
        g.pan_x += io.MouseDelta.x; g.pan_y += io.MouseDelta.y; MarkMaterialsDirty();
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    }
    auto draw = ImGui::GetWindowDrawList();
    draw->PushClipRect(origin,end,true);
    draw->AddRectFilled(origin,end,IM_COL32(17,23,30,255));
    float step = 32*g.zoom;
    for (float x = std::fmod(g.pan_x,step); x < size.x; x += step)
        for (float y = std::fmod(g.pan_y,step); y < size.y; y += step)
            draw->AddCircleFilled(GraphAdd(origin,ImVec2(x,y)),0.85f,IM_COL32(43,54,64,255),4);

    graph_pin hot_pin; int hot_node = 0;
    for (auto it = g.nodes.rbegin(); it != g.nodes.rend(); ++it) {
        const auto& n = *it; ImVec2 ns = GraphNodeSize(n,mat);
        ImVec2 a = screen(ImVec2(n.x,n.y)), b = GraphAdd(a,GraphMul(ns,g.zoom));
        for (int side = 0; side < 2 && !hot_pin; ++side) {
            int count = side ? graph_outputs(n.kind) : GraphInputCount(n,mat);
            for (int p = 0; p < count; ++p) {
                graph_pin pin{n.id,p,side == 0};
                if (GraphDistance(mouse,screen(GraphPinLocal(n,pin,mat))) <= 121) { hot_pin = pin; break; }
            }
        }
        if (hot_pin || GraphContains(mouse,a,b)) { hot_node = n.id; break; }
    }
    if (!hovered) { hot_pin = {}; hot_node = 0; }

    // ImGui's drag target is the entire canvas; dropping onto a sample replaces
    // its asset, dropping onto a pin creates and wires a new sample.
    if (ImGui::BeginDragDropTarget()) {
        if (const auto* payload = ImGui::AcceptDragDropPayload("TEXTURE_ASSET_ID")) {
            if (payload->DataSize == sizeof(int)) {
                int ti = *static_cast<const int*>(payload->Data);
                if (ti >= 0 && ti < (int)scn.textures.size()) {
                    auto before = g; int id = hot_node; auto n = g.find(id);
                    if (n && n->kind == graph_kind::texture_sample && !hot_pin) {
                        n->texture_index = ti; n->texture_path = scn.textures[ti].path;
                    } else {
                        ImVec2 at = local(mouse);
                        if (hot_pin.input) at = GraphAdd(at,ImVec2(-290,-60));
                        id = GraphAddTexture(g,scn,ti,at);
                        if (hot_pin && hot_pin.input) GraphAutoConnect(g,hot_pin,id);
                    }
                    if (id) state.selected = {id};
                    GraphCommit(material_index,before,"Drop texture into graph");
                }
            }
        }
        ImGui::EndDragDropTarget();
    }
    const auto& analysis = GraphAnalysis(state,g);
    auto pin_screen = [&](graph_pin p) { auto n = g.find(p.node); return n ? screen(GraphPinLocal(*n,p,mat)) : mouse; };
    auto bezier = [&](ImVec2 a, ImVec2 b, ImU32 color, float thickness, bool hit_test) {
        float bend = std::max(48*g.zoom,std::abs(b.x-a.x)*0.48f);
        ImVec2 c1(a.x+bend,a.y), c2(b.x-bend,b.y);
        draw->AddBezierCubic(a,c1,c2,b,color,thickness);
        bool hit = false; ImVec2 prev = a;
        if (hit_test) for (int i = 1; i <= 40; ++i) {
            float t = i/40.0f, q = 1-t;
            ImVec2 p = GraphAdd(GraphAdd(GraphMul(a,q*q*q),GraphMul(c1,3*q*q*t)),GraphAdd(GraphMul(c2,3*q*t*t),GraphMul(b,t*t*t)));
            ImVec2 v = GraphSub(p,prev), w = GraphSub(mouse,prev);
            float denom = v.x*v.x+v.y*v.y;
            float d = denom > 0 ? std::clamp((w.x*v.x+w.y*v.y)/denom,0.0f,1.0f) : 0;
            if (GraphDistance(mouse,GraphAdd(prev,GraphMul(v,d))) < 49) hit = true;
            prev = p;
        }
        return hit;
    };
    int hot_wire = -1;
    for (int i = 0; i < (int)g.links.size(); ++i) {
        auto l = g.links[i];
        auto target = g.find(l.to);
        if (!target || l.input >= GraphInputCount(*target,mat)) continue;
        ImVec2 a = pin_screen({l.from,l.output,false}), b = pin_screen({l.to,l.input,true});
        ImU32 color = i == state.selected_wire ? IM_COL32(234,211,155,255) : GraphColor(analysis.width(l.from,l.output));
        bezier(a,b,IM_COL32(0,0,0,90),5.5f,false);
        if (bezier(a,b,color,2.2f,hovered && !hot_node)) hot_wire = i;
    }
    auto text = [&](ImVec2 at, ImU32 color, const char* value) {
        draw->AddText(ImGui::GetFont(),ImGui::GetFontSize()*g.zoom,screen(at),color,value);
    };
    const ImU32 muted = IM_COL32(139,158,173,255), bright = IM_COL32(228,236,241,255);
    for (const auto& n : g.nodes) {
        ImVec2 ns = GraphNodeSize(n,mat), a = screen(ImVec2(n.x,n.y)), b = GraphAdd(a,GraphMul(ns,g.zoom));
        if (b.x < origin.x-12 || a.x > end.x+12 || b.y < origin.y || a.y > end.y) continue;
        draw->AddRectFilled(GraphAdd(a,ImVec2(4,5)),GraphAdd(b,ImVec2(4,5)),IM_COL32(0,0,0,75),4);
        draw->AddRectFilled(a,b,IM_COL32(29,38,48,255),3);
        draw->AddRect(a,b,GraphSelected(state,n.id) ? IM_COL32(132,223,195,255) : IM_COL32(61,78,91,255),3,0,GraphSelected(state,n.id) ? 2.0f : 1.0f);
        if (n.kind != graph_kind::reroute) {
            draw->AddRectFilled(a,ImVec2(b.x,a.y+35*g.zoom),IM_COL32(37,50,61,255),3);
            draw->AddRectFilled(a,ImVec2(a.x+3*g.zoom,a.y+35*g.zoom),GraphNodeColor(n.kind));
            text(ImVec2(n.x+14,n.y+8),bright,graph_name(n.kind));
        } else text(ImVec2(n.x+16,n.y+10),muted,"Pass");
        for (int side = 0; side < 2; ++side) {
            int count = side ? graph_outputs(n.kind) : GraphInputCount(n,mat);
            for (int p = 0; p < count; ++p) {
                graph_pin pin{n.id,p,side == 0}; ImVec2 ps = screen(GraphPinLocal(n,pin,mat));
                int node_index = analysis.indices.at(n.id);
                bool connected = side ? (analysis.output_mask[node_index] & (1 << p)) != 0 : analysis.sources[node_index][p] >= 0;
                int width = side ? analysis.width(n.id,p) : n.kind == graph_kind::texture_sample ? 2 :
                    n.kind == graph_kind::output ? (p == 0 || p == 3 ? 3 : 1) : analysis.width(n.id);
                ImU32 color = GraphColor(width);
                bool is_hot = hot_pin.node == n.id && hot_pin.slot == p && hot_pin.input == pin.input;
                if (state.dragging && state.dragging.input != pin.input && state.dragging.node != n.id) {
                    if (GraphCanConnect(state,g,state.dragging,pin)) draw->AddCircle(ps,9*g.zoom,color,16,1.5f);
                    else color = IM_COL32(73,83,95,255);
                }
                draw->AddCircleFilled(ps,6*g.zoom,connected ? color : IM_COL32(19,27,34,255),16);
                draw->AddCircle(ps,6*g.zoom,color,16,1.5f);
                if (is_hot) draw->AddCircle(ps,10*g.zoom,bright,20,1.5f);
                if (n.kind == graph_kind::reroute) continue;
                std::string label = side ? graph_output_name(n.kind,p) : graph_input_name(n.kind,p);
                if (!side && n.kind == graph_kind::output && p == 0 && mat.model == scene_material_model::diffuse_light) label = "Emission";
                if (!side && !connected && n.kind != graph_kind::texture_sample && n.kind != graph_kind::output) {
                    char value[32]; std::snprintf(value,sizeof(value),"  %.3g",n.defaults[p]); label += value;
                }
                if (!side && !connected && n.kind == graph_kind::output && (p == 1 || p == 2)) {
                    char value[32]; std::snprintf(value,sizeof(value),"  %.3g",p == 1 ? mat.metallic : mat.roughness); label += value;
                }
                float tx = side ? n.x+ns.x-14-ImGui::CalcTextSize(label.c_str()).x : n.x+14;
                text(ImVec2(tx,n.y+53+p*27.0f),connected ? bright : muted,label.c_str());
            }
        }
        if (n.kind == graph_kind::texture_sample) {
            int ti = GraphTextureIndex(n,scn);
            GLuint thumb = ti >= 0 ? GetOrCreateTextureThumbnail(scn.textures[ti].path) : 0;
            ImVec2 ta = screen(ImVec2(n.x+14,n.y+86)), tb = screen(ImVec2(n.x+115,n.y+187));
            if (thumb) draw->AddImage((ImTextureID)(intptr_t)thumb,ta,tb);
            else { draw->AddRectFilled(ta,tb,IM_COL32(42,54,65,255),2); text(ImVec2(n.x+24,n.y+127),muted,"No texture"); }
            std::string name = ti >= 0 ? scn.textures[ti].name : "Drop a texture here";
            while (name.size() > 3 && ImGui::CalcTextSize(name.c_str()).x > ns.x-30) name.resize(name.size()-1);
            text(ImVec2(n.x+14,n.y+ns.y-47),bright,name.c_str());
            text(ImVec2(n.x+14,n.y+ns.y-25),muted,n.space == graph_space::automatic ? "AUTO  /  color + data" : n.space == graph_space::srgb ? "sRGB  /  color" : "LINEAR  /  data");
        } else if (n.kind == graph_kind::scalar || n.kind == graph_kind::texcoord) {
            char value[64]; std::snprintf(value,sizeof(value),n.kind == graph_kind::scalar ? "%.4g" : "U %.3g   V %.3g",n.value.x(),n.value.y());
            text(ImVec2(n.x+14,n.y+ns.y-25),bright,value);
        } else if (n.kind == graph_kind::vector) {
            ImVec4 color((float)std::clamp(n.value.x(),0.0,1.0),(float)std::clamp(n.value.y(),0.0,1.0),(float)std::clamp(n.value.z(),0.0,1.0),1);
            draw->AddRectFilled(screen(ImVec2(n.x+14,n.y+60)),screen(ImVec2(n.x+110,n.y+ns.y-17)),ImGui::ColorConvertFloat4ToU32(color),2);
        } else if (n.kind == graph_kind::output) {
            text(ImVec2(n.x+14,n.y+ns.y-25),muted,mat.model == scene_material_model::pbr ? "SURFACE  /  PBR" : "SURFACE");
        }
    }

    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        ImGui::SetWindowFocus();
        state.selected_wire = -1;
        if (hot_pin) {
            if (io.KeyAlt) { auto before = g; g.disconnect(hot_pin.node,hot_pin.slot,hot_pin.input); GraphCommit(material_index,before,"Break pin connections"); }
            else {
                state.dragging = hot_pin; state.rewire_pin = {};
                if (io.KeyCtrl && hot_pin.input) if (auto link = g.incoming(hot_pin.node,hot_pin.slot)) {
                    state.dragging = {link->from,link->output,false}; state.rewire_pin = hot_pin;
                }
            }
        } else if (hot_node) {
            if (io.KeyCtrl) {
                if (GraphSelected(state,hot_node)) state.selected.erase(std::remove(state.selected.begin(),state.selected.end(),hot_node),state.selected.end());
                else state.selected.push_back(hot_node);
            } else if (!GraphSelected(state,hot_node)) state.selected = {hot_node};
            state.before_drag = g; state.moving = GraphSelected(state,hot_node);
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) { state.moving = false; ImGui::OpenPopup("EditNode"); }
        } else if (hot_wire >= 0) {
            state.selected_wire = hot_wire;
            if (io.KeyAlt) {
                auto before = g; g.links.erase(g.links.begin()+hot_wire); state.selected_wire = -1; GraphCommit(material_index,before,"Break connection");
            } else if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                auto before = g; auto l = g.links[hot_wire]; auto at = local(mouse);
                int id = g.add(graph_kind::reroute,at.x-36,at.y-20);
                if (id) { g.disconnect(l.to,l.input,true); g.connect({l.from,l.output,id,0}); g.connect({id,0,l.to,l.input}); state.selected = {id}; }
                GraphCommit(material_index,before,"Insert reroute"); state.selected_wire = -1;
            }
        } else {
            graph_kind quick = graph_kind::count;
            if (!io.KeyCtrl && !io.KeyAlt) {
                if (ImGui::IsKeyDown(ImGuiKey_1)) quick = graph_kind::scalar;
                if (ImGui::IsKeyDown(ImGuiKey_3)) quick = graph_kind::vector;
                if (ImGui::IsKeyDown(ImGuiKey_T)) quick = graph_kind::texture_sample;
                if (ImGui::IsKeyDown(ImGuiKey_M)) quick = graph_kind::multiply;
                if (ImGui::IsKeyDown(ImGuiKey_A)) quick = graph_kind::add;
                if (ImGui::IsKeyDown(ImGuiKey_L)) quick = graph_kind::lerp;
            }
            if (quick != graph_kind::count) {
                auto before = g; auto at = local(mouse); int id = g.add(quick,at.x,at.y);
                if (id) state.selected = {id}; GraphCommit(material_index,before,"Add node");
            } else { if (!io.KeyCtrl) state.selected.clear(); state.marquee = true; state.marquee_start = local(mouse); }
        }
    }
    if (state.moving && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        for (int id : state.selected) if (auto n = g.find(id)) { n->x += io.MouseDelta.x/g.zoom; n->y += io.MouseDelta.y/g.zoom; }
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    }
    if (state.marquee) {
        ImVec2 start = screen(state.marquee_start), lo(std::min(start.x,mouse.x),std::min(start.y,mouse.y)), hi(std::max(start.x,mouse.x),std::max(start.y,mouse.y));
        draw->AddRectFilled(lo,hi,IM_COL32(109,200,177,22)); draw->AddRect(lo,hi,IM_COL32(109,200,177,170));
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            if (GraphDistance(start,mouse) > 16) for (const auto& n : g.nodes) {
                ImVec2 a = screen(ImVec2(n.x,n.y)), b = GraphAdd(a,GraphMul(GraphNodeSize(n,mat),g.zoom));
                if (a.x <= hi.x && b.x >= lo.x && a.y <= hi.y && b.y >= lo.y && !GraphSelected(state,n.id)) state.selected.push_back(n.id);
            }
            state.marquee = false;
        }
    }
    if (state.dragging) {
        ImVec2 anchor = pin_screen(state.dragging);
        ImU32 color = IM_COL32(205,231,224,255);
        if (hot_pin && hot_pin.input != state.dragging.input) color = GraphCanConnect(state,g,state.dragging,hot_pin) ? IM_COL32(142,233,185,255) : IM_COL32(230,119,113,255);
        bezier(state.dragging.input ? mouse : anchor,state.dragging.input ? anchor : mouse,color,2.5f,false);
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            if (hot_pin && hot_pin.input != state.dragging.input) {
                auto before = g; std::string why;
                if (state.rewire_pin) g.disconnect(state.rewire_pin.node,state.rewire_pin.slot,true);
                if (g.connect(GraphLink(state.dragging,hot_pin),&why)) GraphCommit(material_index,before,"Connect nodes");
                else { g = before; GraphMessage(state,why); }
            } else if (hovered && !hot_node) {
                state.popup_local = local(mouse); state.popup_pin = state.dragging; state.popup_rewire = state.rewire_pin; open_search = true;
            }
            state.dragging = {}; state.rewire_pin = {};
        }
    }
    if (state.moving && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        if (io.KeyShift) for (int id : state.selected) if (auto n = g.find(id)) { n->x = std::round(n->x/16)*16; n->y = std::round(n->y/16)*16; }
        GraphCommit(material_index,state.before_drag,"Move nodes",false); state.moving = false;
    }
    if (hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right) && io.MouseDragMaxDistanceSqr[ImGuiMouseButton_Right] < 25) {
        state.popup_local = local(mouse); state.popup_pin = {}; state.popup_rewire = {}; open_search = true;
        if (hot_node && !GraphSelected(state,hot_node)) state.selected = {hot_node};
    }
    if (focused && !io.WantTextInput && !ImGui::IsPopupOpen(nullptr,ImGuiPopupFlags_AnyPopupId)) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            state.dragging = {}; state.rewire_pin = {}; state.marquee = false;
            if (state.moving) g = state.before_drag;
            state.moving = false;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace)) {
            auto before = g;
            if (state.selected_wire >= 0 && state.selected_wire < (int)g.links.size()) g.links.erase(g.links.begin()+state.selected_wire);
            for (int id : state.selected) g.erase(id);
            state.selected.clear(); state.selected_wire = -1; GraphCommit(material_index,before,"Delete graph selection");
        }
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A)) { state.selected.clear(); for (const auto& n : g.nodes) state.selected.push_back(n.id); }
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C)) GraphCopy(g,state);
        if (io.KeyCtrl && (ImGui::IsKeyPressed(ImGuiKey_V) || ImGui::IsKeyPressed(ImGuiKey_D))) {
            auto before = g; ImVec2 at = hovered ? local(mouse) : ImVec2(80,80);
            if (ImGui::IsKeyPressed(ImGuiKey_D)) {
                GraphCopy(g,state);
                if (!g_graph_clipboard.nodes.empty()) at = ImVec2(g_graph_clipboard.nodes[0].x+32,g_graph_clipboard.nodes[0].y+32);
            }
            GraphPaste(g,state,at); GraphCommit(material_index,before,"Duplicate / paste nodes");
        }
    }
    const char* status = state.message_until > ImGui::GetTime() ? state.message.c_str() :
        state.dragging ? "Release on a compatible pin, or on the canvas to add a node.  Esc to cancel." :
        "RMB / MMB  Pan     Scroll  Zoom     F  Frame     Alt + click  Disconnect";
    draw->AddRectFilled(ImVec2(origin.x,end.y-29),end,IM_COL32(20,28,36,242));
    draw->AddText(ImVec2(origin.x+12,end.y-23),muted,status);
    draw->PopClipRect();

    if (open_search) {
        state.search[0] = 0; state.search_row = 0; state.popup_focus = true;
        ImGui::OpenPopup("AddGraphNode");
    }
    ImGui::SetNextWindowSize(ImVec2(360,0),ImGuiCond_Appearing);
    if (ImGui::BeginPopup("AddGraphNode")) {
        ImGui::TextUnformatted(state.popup_pin ? "ADD CONNECTED NODE" : "ADD NODE");
        if (state.popup_focus) { ImGui::SetKeyboardFocusHere(); state.popup_focus = false; }
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputTextWithHint("##SearchNodes","Search nodes or textures...",state.search,sizeof(state.search))) state.search_row = 0;
        std::string query = state.search;
        std::transform(query.begin(),query.end(),query.begin(),[](unsigned char c) { return (char)std::tolower(c); });
        auto matches = [&](std::string name) {
            std::transform(name.begin(),name.end(),name.begin(),[](unsigned char c) { return (char)std::tolower(c); });
            return name.find(query) != std::string::npos;
        };
        struct option { graph_kind kind; int texture; std::string label; };
        std::vector<option> options;
        auto compatible_kind = [&](graph_kind kind) {
            if (!state.popup_pin) return true;
            GraphAnalysis(state,g);
            if (state.compatibility_anchor.node != state.popup_pin.node || state.compatibility_anchor.slot != state.popup_pin.slot || state.compatibility_anchor.input != state.popup_pin.input) {
                state.compatibility_anchor = state.popup_pin; state.compatible_pins.clear(); state.compatible_kinds.fill(0);
            }
            int& cached = state.compatible_kinds[(int)kind];
            if (!cached) {
                auto candidate = g; int id = candidate.add(kind,0,0);
                cached = id && GraphAutoConnect(candidate,state.popup_pin,id) ? 1 : -1;
            }
            return cached > 0;
        };
        for (int i = 1; i < (int)graph_kind::count; ++i) {
            auto kind = static_cast<graph_kind>(i);
            std::string label = std::string(graph_name(kind))+"    / "+graph_category(kind);
            std::string aliases = kind == graph_kind::scalar ? " constant float value 1" : kind == graph_kind::vector ? " constant3vector vector3 color colour 3" : kind == graph_kind::lerp ? " mix linear interpolate" : kind == graph_kind::texcoord ? " uv texturecoordinate tiling" : kind == graph_kind::one_minus ? " invert 1-x" : "";
            if (matches(label+aliases) && compatible_kind(kind)) options.push_back({kind,-1,label});
        }
        if (compatible_kind(graph_kind::texture_sample)) for (int i = 0; i < (int)scn.textures.size(); ++i)
            if (matches(scn.textures[i].name)) options.push_back({graph_kind::texture_sample,i,scn.textures[i].name+"    / Texture"});
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) ++state.search_row;
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) --state.search_row;
        state.search_row = std::clamp(state.search_row,0,std::max(0,(int)options.size()-1));
        bool enter = ImGui::IsKeyPressed(ImGuiKey_Enter);
        ImGui::BeginChild("SearchResults",ImVec2(0,290),false);
        if (options.empty()) ImGui::TextDisabled("No compatible results.");
        int chosen = -1;
        for (int i = 0; i < (int)options.size(); ++i) {
            ImGui::PushID(i);
            if (ImGui::Selectable(options[i].label.c_str(),i == state.search_row) || (enter && i == state.search_row)) chosen = i;
            if (i == state.search_row && (ImGui::IsKeyPressed(ImGuiKey_UpArrow) || ImGui::IsKeyPressed(ImGuiKey_DownArrow))) ImGui::SetScrollHereY();
            ImGui::PopID();
        }
        ImGui::EndChild();
        if (chosen >= 0) {
            auto before = g; const auto& o = options[chosen];
            int id = o.texture >= 0 ? GraphAddTexture(g,scn,o.texture,state.popup_local) : g.add(o.kind,state.popup_local.x,state.popup_local.y);
            if (id) {
                if (GraphAutoConnect(g,state.popup_pin,id) && state.popup_rewire)
                    g.disconnect(state.popup_rewire.node,state.popup_rewire.slot,true);
                state.selected = {id}; GraphCommit(material_index,before,"Add graph node");
            }
            else GraphMessage(state,"This material has reached the 256 node limit.");
            ImGui::CloseCurrentPopup(); state.popup_pin = {}; state.popup_rewire = {};
        }
        if (!state.popup_pin && !state.selected.empty()) {
            ImGui::Separator();
            if (ImGui::MenuItem("Duplicate selection","Ctrl+D")) {
                auto before = g; GraphCopy(g,state); GraphPaste(g,state,state.popup_local); GraphCommit(material_index,before,"Duplicate nodes");
            }
            if (ImGui::MenuItem("Delete selection","Delete")) {
                auto before = g; for (int id : state.selected) g.erase(id); state.selected.clear(); GraphCommit(material_index,before,"Delete nodes");
            }
        }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("EditNode")) {
        if (state.selected.size() == 1) if (auto n = g.find(state.selected.front())) {
            ImGui::TextUnformatted(graph_name(n->kind)); ImGui::Separator(); GraphNodeProperties(mat,scn,material_index,*n);
        }
        ImGui::EndPopup();
    }
    // Color pickers can close without resubmitting the active inner widget.
    if (state.editing && !ImGui::IsAnyItemActive() && !ImGui::IsPopupOpen(nullptr,ImGuiPopupFlags_AnyPopupId)) {
        GraphCommit(material_index,state.before_edit,"Edit node value"); state.editing = false;
    }
    ImGui::EndChild(); ImGui::PopID();
}
