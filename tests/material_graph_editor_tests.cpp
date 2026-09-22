// Feed real ImGui input events into the production canvas in a hidden GLFW
// window. This exercises hit testing and popup/drag timing without desktop input.
#include "core/dusktracer.h"
#include "core/scene.h"
#include "core/undo.h"
#include "GL/glew.h"
#include "GLFW/glfw3.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_opengl3.h"
#include <unordered_map>
#include <cstdio>
#include <cctype>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "external/stb_image_write.h"

static scene g_scene;
static int g_selected_material = 0;
static bool g_world_dirty = false, g_materials_dirty = false;
static std::shared_ptr<int> g_cached_world;
static void MarkMaterialsDirty() { g_materials_dirty = true; }
static void InvalidateMaterialThumbnail(int) {}
static void DrawPanelTitle(const char* title,const char*) { ImGui::TextUnformatted(title); }
static GLuint test_texture = 0;
static GLuint GetOrCreateTextureThumbnail(const std::string&) { return test_texture; }
static void DrawMaterialSpherePreview(int) {}
#include "../src/core/material_graph_editor.inl"

static int checks = 0;
static void require(bool value,const char* message) {
    ++checks; if (!value) { std::fprintf(stderr,"FAIL: %s\n",message); std::exit(1); }
}
static ImVec2 asset_pos;
static void frame() {
    auto& io = ImGui::GetIO(); io.DisplaySize = ImVec2(1360,900); io.DeltaTime = 1.0f/60;
    ImGui_ImplOpenGL3_NewFrame(); ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0,0)); ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("Material Editor",nullptr,ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextUnformatted("MATERIAL EDITOR     /     Brushed copper");
    ImGui::Separator();
    ImGui::BeginChild("Details",ImVec2(260,0));
    DrawMaterialGraphDetails(g_scene.materials[0],g_scene,0);
    ImGui::TextUnformatted("Surface / PBR");
    ImGui::TextDisabled("Metallic  0.85\nRoughness  0.28");
    ImGui::Separator(); ImGui::TextUnformatted("TEXTURE ASSETS");
    ImGui::Button("Copper grain",ImVec2(230,70)); asset_pos = GraphAdd(ImGui::GetItemRectMin(),ImVec2(70,30));
    if (ImGui::BeginDragDropSource()) {
        int index = 0; ImGui::SetDragDropPayload("TEXTURE_ASSET_ID",&index,sizeof(index));
        ImGui::TextUnformatted("Copper grain"); ImGui::EndDragDropSource();
    }
    ImGui::EndChild(); ImGui::SameLine(); ImGui::BeginChild("Graph",ImVec2(0,0));
    DrawMaterialGraphEditor(g_scene.materials[0],g_scene,0);
    ImGui::EndChild(); ImGui::End();
    ImGui::Render(); glViewport(0,0,1360,900); glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData()); glFinish();
}
static void move(ImVec2 p) { ImGui::GetIO().AddMousePosEvent(p.x,p.y); frame(); }
static void button(int b,bool down) { ImGui::GetIO().AddMouseButtonEvent(b,down); frame(); }
static void click(ImVec2 p,int b = 0) { move(p); button(b,true); button(b,false); frame(); }
static void key(ImGuiKey k,bool down) { ImGui::GetIO().AddKeyEvent(k,down); frame(); }
static void press(ImGuiKey k) { key(k,true); key(k,false); }
static void drag(ImVec2 a,ImVec2 b) { move(a); button(0,true); move(GraphAdd(a,ImVec2(8,8))); move(b); frame(); button(0,false); frame(); }
static ImVec2 screen_point(ImVec2 p) {
    const auto& g = g_scene.materials[0].graph;
    return GraphAdd(g_graph_canvas_min,GraphAdd(ImVec2(g.pan_x,g.pan_y),GraphMul(p,g.zoom)));
}
static ImVec2 pin(int id,int slot,bool input) {
    const auto& m = g_scene.materials[0]; return screen_point(GraphPinLocal(*m.graph.find(id),{id,slot,input},m));
}
int main(int argc,char** argv) {
    require(glfwInit() != 0,"GLFW initializes");
    glfwWindowHint(GLFW_VISIBLE,GLFW_FALSE);
    auto window = glfwCreateWindow(1360,900,"Graph interaction test",nullptr,nullptr);
    require(window != nullptr,"hidden OpenGL window"); glfwMakeContextCurrent(window); glewInit();
    ImGui::CreateContext(); auto& io = ImGui::GetIO(); io.IniFilename = nullptr;
    io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/segoeui.ttf",17);
    ImGui::StyleColorsDark(); auto& style = ImGui::GetStyle();
    style.WindowPadding = ImVec2(14,12); style.FramePadding = ImVec2(10,7); style.ItemSpacing = ImVec2(10,8);
    style.Colors[ImGuiCol_WindowBg] = ImVec4(.08f,.10f,.13f,1);
    style.Colors[ImGuiCol_ChildBg] = ImVec4(.10f,.12f,.16f,1);
    style.Colors[ImGuiCol_FrameBg] = ImVec4(.13f,.16f,.20f,1);
    style.Colors[ImGuiCol_Button] = ImVec4(.16f,.25f,.29f,1);
    style.Colors[ImGuiCol_Header] = ImVec4(.20f,.40f,.39f,1);
    ImGui_ImplOpenGL3_Init("#version 130");
    std::vector<unsigned char> pixels(128*128*4);
    for (int y = 0; y < 128; ++y) for (int x = 0; x < 128; ++x) {
        int i = (y*128+x)*4, grain = (x*37+y*11)%23;
        pixels[i] = (unsigned char)(155+grain); pixels[i+1] = (unsigned char)(86+grain); pixels[i+2] = (unsigned char)(45+grain); pixels[i+3] = 255;
    }
    glGenTextures(1,&test_texture); glBindTexture(GL_TEXTURE_2D,test_texture);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,128,128,0,GL_RGBA,GL_UNSIGNED_BYTE,pixels.data());
    scene_material m; m.name = "Brushed copper"; m.model = scene_material_model::pbr; m.metallic = .85; m.roughness = .28;
    g_scene.materials.push_back(m); g_scene.textures.push_back({"Copper grain","fixture.png"});
    initialize_material_graph(g_scene.materials[0],g_scene.textures);
    auto& g = g_scene.materials[0].graph; auto& state = g_graph_states[0];
    g.find(g.output_id())->x = 640; g.find(g.output_id())->y = 180;
    g.pan_x = 40; g.pan_y = 40; g.zoom = 1; state.fit = false;
    int scalar = g.add(graph_kind::scalar,40,30); g.find(scalar)->value = vec3(.35,0,0);
    int out = g.output_id(); frame(); frame();
    drag(pin(scalar,0,false),pin(out,2,true));
    require(g.incoming(out,2) && g.incoming(out,2)->from == scalar,"output-to-input drag connects");
    drag(pin(out,1,true),pin(scalar,0,false));
    require(g.incoming(out,1) && g.incoming(out,1)->from == scalar,"input-to-output reverse wiring");
    key(ImGuiMod_Alt,true); click(pin(out,1,true)); key(ImGuiMod_Alt,false);
    require(!g.incoming(out,1),"Alt-click disconnects pin");
    require(UndoManager::Instance().undo(),"undo connection break"); frame();
    require(g.incoming(out,1) != nullptr,"undo restores actual topology");
    require(UndoManager::Instance().redo(),"redo connection break"); frame();
    require(!g.incoming(out,1),"redo removes actual link");
    key(ImGuiMod_Ctrl,true); drag(pin(out,2,true),pin(out,1,true)); key(ImGuiMod_Ctrl,false);
    require(!g.incoming(out,2) && g.incoming(out,1),"Ctrl-drag moves an existing wire to another input");
    require(UndoManager::Instance().undo(),"rewire can be undone atomically"); frame();
    require(g.incoming(out,2) && !g.incoming(out,1),"undo rewiring restores both sockets");
    float x = g.find(scalar)->x;
    drag(screen_point(ImVec2(x+50,45)),screen_point(ImVec2(x+110,105)));
    require(g.find(scalar)->x > x+40,"node drag moves the node");

    state.selected = {scalar}; frame();
    ImVec2 value_widget;
    for (auto window : ImGui::GetCurrentContext()->Windows)
        if (std::strstr(window->Name,"/Details_")) value_widget = GraphAdd(window->DC.CursorStartPos,ImVec2(65,40));
    double original_value = g.find(scalar)->value.x();
    drag(value_widget,GraphAdd(value_widget,ImVec2(90,0)));
    require(g.find(scalar)->value.x() != original_value && !state.editing,"property drag updates its value and finishes editing");
    double edited_value = g.find(scalar)->value.x();
    require(UndoManager::Instance().undo(),"continuous property edit produces an undo action"); frame();
    require(g.find(scalar)->value.x() == original_value,"one undo restores the value before the entire drag");
    require(UndoManager::Instance().redo(),"property drag can be redone"); frame();
    require(g.find(scalar)->value.x() == edited_value,"redo restores the final dragged value");

    // Search is opened at the mouse location and keyboard Enter inserts a node.
    size_t count = g.nodes.size();
    click(screen_point(ImVec2(360,430)),1);
    ImGui::GetIO().AddInputCharactersUTF8("Multiply"); frame(); frame(); press(ImGuiKey_Enter); frame();
    require(g.nodes.size() == count+1 && g.nodes.back().kind == graph_kind::multiply,"right-click search and Enter creates node");
    int mult = g.nodes.back().id;
    require(std::abs(g.find(mult)->x-360) < 3,"search insertion uses click position");

    // Releasing a wire over empty canvas creates a filtered, connected node.
    count = g.nodes.size();
    drag(pin(scalar,0,false),screen_point(ImVec2(350,100)));
    ImGui::GetIO().AddInputCharactersUTF8("One Minus"); frame(); frame(); press(ImGuiKey_Enter); frame();
    require(g.nodes.size() == count+1 && g.nodes.back().kind == graph_kind::one_minus,"wire-release opens contextual search");
    require(g.incoming(g.nodes.back().id,0)->from == scalar,"created node auto-connects to dragged wire");

    // Escape must abandon a pending wire without losing the old connection.
    move(pin(out,2,true)); button(0,true); move(screen_point(ImVec2(400,350))); press(ImGuiKey_Escape); button(0,false);
    require(!state.dragging && g.incoming(out,2),"Escape cancels wiring without disconnecting");
    count = g.nodes.size();
    drag(asset_pos,screen_point(ImVec2(30,370)));
    require(g.nodes.size() == count+1 && g.nodes.back().kind == graph_kind::texture_sample,"Content Drawer payload creates texture sample");
    int texture = g.nodes.back().id;
    require(g.find(texture)->texture_path == "fixture.png","asset identity assigned to dropped node");
    drag(pin(texture,0,false),pin(out,0,true));
    require(g.incoming(out,0) && g.incoming(out,0)->from == texture,"dropped sample can wire to material");
    drag(pin(texture,0,false),pin(out,1,true));
    require(!g.incoming(out,1),"incompatible mouse connection rejected");
    drag(pin(texture,2,false),pin(out,1,true));
    require(g.incoming(out,1) && g.incoming(out,1)->output == 2,"channel output wires to scalar");

    // Node duplication must retain an independent sample, then delete cleanly.
    click(screen_point(ImVec2(g.find(texture)->x+45,g.find(texture)->y+17)));
    count = g.nodes.size(); key(ImGuiMod_Ctrl,true); press(ImGuiKey_D); key(ImGuiMod_Ctrl,false);
    require(g.nodes.size() == count+1 && state.selected.size() == 1,"Ctrl+D duplicates selection");
    int duplicate = state.selected.front();
    require(duplicate != texture && g.find(duplicate)->texture_path == "fixture.png","duplicate has stable independent identity");
    press(ImGuiKey_Delete); require(!g.find(duplicate) && g.find(texture),"Delete removes only selected node");
    require(g.incoming(out,0) && g.incoming(out,0)->from == texture,"deleting duplicate preserves original links");

    // Panning changes the camera only; right-drag must not open search.
    float pan = g.pan_x; ImVec2 blank = screen_point(ImVec2(500,620));
    move(blank); button(1,true); move(GraphAdd(blank,ImVec2(-30,-20))); button(1,false);
    require(g.pan_x < pan-20 && !ImGui::IsPopupOpen(nullptr,ImGuiPopupFlags_AnyPopupId),"right-drag pans without opening menu");

    float zoom = g.zoom; ImVec2 pivot = screen_point(ImVec2(500,400)); move(pivot);
    ImVec2 before = GraphMul(GraphSub(GraphSub(pivot,g_graph_canvas_min),ImVec2(g.pan_x,g.pan_y)),1/g.zoom);
    io.AddMouseWheelEvent(0,1); frame();
    ImVec2 after = GraphMul(GraphSub(GraphSub(pivot,g_graph_canvas_min),ImVec2(g.pan_x,g.pan_y)),1/g.zoom);
    require(g.zoom > zoom && GraphDistance(before,after) < 0.01,"wheel zoom stays under cursor");
    auto serialized = serialize_material_graph(g); material_graph saved;
    require(deserialize_material_graph(serialized,saved) && serialize_material_graph(saved) == serialized,"edited graph and layout persist");

    // Compose an actual canvas screenshot for visual QA.
    g.erase(mult); state.selected.clear();
    g.find(texture)->x = 40; g.find(texture)->y = 210;
    g.find(scalar)->x = 40; g.find(scalar)->y = 40;
    int invert = 0; for (const auto& n : g.nodes) if (n.kind == graph_kind::one_minus) invert = n.id;
    g.find(invert)->x = 360; g.find(invert)->y = 60;
    g.connect({invert,0,out,2}); g.connect({texture,0,out,0});
    int uv = g.add(graph_kind::texcoord,-230,220); g.find(uv)->value = vec3(2,2,1); g.connect({uv,0,texture,0});
    state.selected = {texture}; state.message_until = 0; state.fit = true;
    state.selected.clear(); frame(); frame();
    // Marquee selects nodes using the same transform as the zoomed canvas.
    drag(screen_point(ImVec2(20,20)),screen_point(ImVec2(280,190)));
    require(GraphSelected(state,scalar) && !GraphSelected(state,out),"marquee selects intersecting nodes after zoom");
    state.selected.clear(); frame();
    pixels.resize(1360*900*4); glReadPixels(0,0,1360,900,GL_RGBA,GL_UNSIGNED_BYTE,pixels.data());
    stbi_flip_vertically_on_write(1);
    if (argc > 1) require(stbi_write_png(argv[1],1360,900,4,pixels.data(),1360*4) != 0,"save visual QA screenshot");
    std::printf("%d ImGui interaction checks passed\n",checks);
    glDeleteTextures(1,&test_texture); ImGui_ImplOpenGL3_Shutdown(); ImGui::DestroyContext(); glfwDestroyWindow(window); glfwTerminate();
}
