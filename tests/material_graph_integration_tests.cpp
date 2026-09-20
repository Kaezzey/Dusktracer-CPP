// Exercise persistence, native-drop import and preview through production editor
// functions. Use an isolated working directory: never touch the user's assets.
#define main dusk_editor_entry_for_test
#include "../src/core/editor_main.cpp"
#undef main
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "external/stb_image_write.h"
#include <future>

static int checks = 0;
static void check(bool result,const char* what) {
    ++checks; if (!result) { std::fprintf(stderr,"FAIL: %s\n",what); std::exit(1); }
}
static GLuint wait_for_preview(int index) {
    auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        GLuint texture = GetOrCreateMaterialThumbnail(index);
        auto& state = g_material_preview_states[index];
        if (!state.dirty && state.requested && state.displayed == state.requested && state.size == MATERIAL_THUMB_SIZE) return texture;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    check(false,"preview worker completes latest revision"); return 0;
}
static void check_edit_scheduling(int scalar) {
    auto& graph = g_scene.materials[0].graph;
    auto original = graph;
    auto world = std::make_shared<hittable_list>(); g_cached_world = world;
    GraphDirty(0); wait_for_preview(0); g_world_dirty = false;
    auto ticket = g_material_preview_states[0].requested;
    auto before = graph; int detached = graph.add(graph_kind::scalar,0,0);
    GraphCommit(0,before,"Detached test node"); GetOrCreateMaterialThumbnail(0);
    check(!g_world_dirty && !g_material_preview_states[0].dirty && g_material_preview_states[0].requested == ticket,
        "adding a disconnected node schedules no shading work");
    graph.find(detached)->value = vec3(.25,0,0); GraphDirty(0);
    check(!g_world_dirty && !g_material_preview_states[0].dirty,"editing an unused branch schedules no shading work");
    before = graph; graph.find(scalar)->x += 10; GraphCommit(0,before,"Move test node",false);
    check(!g_world_dirty && !g_material_preview_states[0].dirty,"moving a connected node schedules no shading work");
    auto read_manifest = [] {
        std::ifstream stream("materials.txt"); return std::string(std::istreambuf_iterator<char>(stream),{});
    };
    SaveMaterialsManifest(); auto persisted = read_manifest();
    for (int i = 0; i < 30; ++i) {
        graph.find(scalar)->value = vec3(.2 + i*.01,0,0); GraphDirty(0); TickMaterialPersistence();
    }
    check(g_world_dirty && g_material_preview_states[0].dirty,"connected edits invalidate the runtime and preview");
    check(g_cached_world == world,"editing retains the cached runtime world until render rebuilds it");
    check(read_manifest() == persisted && g_materials_dirty,"rapid edits do not serialize the material manifest");
    g_materials_last_edit -= std::chrono::seconds(1); TickMaterialPersistence();
    check(read_manifest() != persisted && !g_materials_dirty,"settled edits persist the latest graph once");
    wait_for_preview(0);
    // A completed job can outlive a material insertion/deletion. Such results
    // must not update the GL texture now owned by a different material index.
    uint64_t stale = g_material_preview_states[0].requested;
    InvalidateAllMaterialThumbnails();
    auto current = g_material_preview_states[0].displayed;
    std::promise<void> completed; auto completion = completed.get_future();
    g_editor_previews.request("M0",stale,[&] {
        preview_image image{1,1,{255,0,255,255}}; completed.set_value(); return image;
    });
    check(completion.wait_for(std::chrono::seconds(5)) == std::future_status::ready,"stale preview fixture completes");
    auto id = wait_for_preview(0);
    check(g_material_preview_states[0].displayed > current && g_material_preview_states[0].size == 128,
        "material reindex rejects stale previews and completes the new revision");
    auto expected_image = RenderMaterialPreview(g_scene,g_scene.materials[0],128);
    std::vector<unsigned char> actual(expected_image.pixels.size());
    glBindTexture(GL_TEXTURE_2D,id); glGetTexImage(GL_TEXTURE_2D,0,GL_RGBA,GL_UNSIGNED_BYTE,actual.data());
    check(actual == expected_image.pixels,"coalesced preview matches the newest material values exactly");
    RestoreGraph(0,original); wait_for_preview(0);
    g_cached_world.reset();
}
static void benchmark_graph_edits() {
    auto& graph = g_scene.materials[0].graph;
    int color = graph.add(graph_kind::vector,0,0), last = color;
    for (int i = 0; i < 32; ++i) {
        int node = graph.add(graph_kind::multiply,0,0);
        graph.find(node)->defaults[1] = .99;
        graph.connect({last,0,node,0}); last = node;
    }
    graph.connect({last,0,graph.output_id(),0});
    for (int i = 0; i < 80; ++i) graph.add(graph_kind::scalar,0,0);
    GraphDirty(0); wait_for_preview(0);
    auto run = [&](const char* name,bool add) {
        std::vector<double> times;
        for (int i = 0; i < 24; ++i) {
            auto start = std::chrono::steady_clock::now();
            if (add) {
                auto before = graph; graph.add(graph_kind::scalar,0,0);
                GraphCommit(0,before,"Benchmark add disconnected node");
            } else {
                graph.find(color)->value = vec3(.1+.02*i,.3,.5); GraphDirty(0);
            }
            GetOrCreateMaterialThumbnail(0);
            TickMaterialPersistence();
            times.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        std::sort(times.begin(),times.end());
        double total = 0; for (double time : times) total += time;
        std::printf("BENCH %s: mean %.3f ms, p95 %.3f ms, max %.3f ms (24 edits)\n",name,total/times.size(),times[22],times.back());
    };
    run("add_disconnected_node",true); run("drag_connected_color",false);
    wait_for_preview(0);
}
int main(int argc,char** argv) {
    check(argc >= 2,"isolated working directory supplied");
    std::filesystem::create_directories(argv[1]); std::filesystem::current_path(argv[1]);
    check(glfwInit() != 0,"GLFW init"); glfwWindowHint(GLFW_VISIBLE,GLFW_FALSE);
    auto window = glfwCreateWindow(1440,960,"Graph integration test",nullptr,nullptr);
    check(window != nullptr,"hidden preview window"); glfwMakeContextCurrent(window); glewInit();
    ImGui::CreateContext(); auto& io = ImGui::GetIO(); io.IniFilename = nullptr;
    SetupEditorFonts(io); ApplyModernEditorTheme(); ImGui_ImplOpenGL3_Init("#version 130");
    scene_material material; material.name = "Brushed copper"; material.model = scene_material_model::pbr; material.metallic = 0.85; material.roughness = 0.3;
    g_scene.materials.push_back(material); g_selected_material = 0;
    initialize_material_graph(g_scene.materials[0],g_scene.textures);
    std::vector<unsigned char> pixels(128*128*4);
    for (int y = 0; y < 128; ++y) for (int x = 0; x < 128; ++x) {
        int at = (y*128+x)*4, grain = (x*37+y*7)%29;
        pixels[at] = (unsigned char)(165+grain); pixels[at+1] = (unsigned char)(94+grain); pixels[at+2] = (unsigned char)(48+grain); pixels[at+3] = 255;
    }
    check(stbi_write_png("Copper grain.png",128,128,4,pixels.data(),128*4) != 0,"create texture fixture");
    g_dropped_files = {std::filesystem::absolute("Copper grain.png").string()};
    g_graph_file_drop_material = 0; g_graph_file_drop_local = ImVec2(30,240);
    process_dropped_files();
    auto& g = g_scene.materials[0].graph; int out = g.output_id(), sample = g.nodes.back().id;
    check(g.nodes.back().kind == graph_kind::texture_sample && g_scene.textures.size() == 1,"file import creates sample and asset");
    check(g.find(sample)->x == 30 && g.find(sample)->y == 240,"file drop preserves graph coordinates");
    check(std::filesystem::exists(g.find(sample)->texture_path),"imported texture path exists");
    g.find(out)->x = 650; g.find(out)->y = 180;
    int scalar = g.add(graph_kind::scalar,30,30); g.find(scalar)->value = vec3(.7,0,0);
    int invert = g.add(graph_kind::one_minus,350,30);
    int uv = g.add(graph_kind::texcoord,-240,250); g.find(uv)->value = vec3(3,3,1);
    g.connect({uv,0,sample,0}); g.connect({sample,0,out,0}); g.connect({scalar,0,invert,0}); g.connect({invert,0,out,2});
    auto graph_data = serialize_material_graph(g);
    SaveMaterialsManifest(); g_scene.materials.clear(); LoadMaterialsManifest();
    check(g_scene.materials.size() == 1 && g_scene.materials[0].name == "Brushed copper","materials load into original indices");
    check(serialize_material_graph(g_scene.materials[0].graph) == graph_data,"production manifest preserves complete graph");
    auto texture_pixels = [&](GLuint id) {
        std::vector<unsigned char> data(MATERIAL_THUMB_SIZE*MATERIAL_THUMB_SIZE*4);
        glBindTexture(GL_TEXTURE_2D,id); glGetTexImage(GL_TEXTURE_2D,0,GL_RGBA,GL_UNSIGNED_BYTE,data.data()); return data;
    };
    auto preview_id = wait_for_preview(0);
    auto first = texture_pixels(preview_id);
    check(first.size() > 0 && first[0] != first[(64*128+64)*4],"production material preview draws sphere");
    g_scene.materials[0].graph.find(scalar)->value = vec3(.15,0,0); GraphDirty(0);
    auto changed_id = wait_for_preview(0);
    auto changed = texture_pixels(changed_id);
    check(preview_id == changed_id,"preview refresh reuses its GL texture");
    check(first != changed,"roughness graph change refreshes actual preview");
    if (argc == 3) benchmark_graph_edits();
    check_edit_scheduling(scalar);
    std::vector<unsigned char> large_pixels(512*256*4,128);
    check(stbi_write_png("large-texture.png",512,256,4,large_pixels.data(),512*4) != 0,"large thumbnail fixture");
    GLuint thumb = 0; auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while (!thumb && std::chrono::steady_clock::now() < deadline) {
        thumb = GetOrCreateTextureThumbnail("large-texture.png");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    int width = 0, height = 0; glBindTexture(GL_TEXTURE_2D,thumb);
    glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_WIDTH,&width);
    glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_HEIGHT,&height);
    check(thumb && width == 128 && height == 64,"background texture thumbnail is bounded and preserves aspect ratio");
    g_scene.materials[0].graph.find(scalar)->value = vec3(.7,0,0); GraphDirty(0);
    wait_for_preview(0);
    g_graph_states[0].selected.clear(); g_graph_states[0].fit = true;
    for (int i = 0; i < 4; ++i) {
        io.DisplaySize = ImVec2(1440,960); io.DeltaTime = 1.f/60; g_thumbs_created_this_frame = 0;
        ImGui_ImplOpenGL3_NewFrame(); ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0,0)); ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("Material Editor",nullptr,ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        DrawPanelTitle("Material Editor","Brushed copper / PBR surface"); ImGui::Separator();
        ImGui::BeginChild("Details",ImVec2(330,0));
        DrawMaterialGraphDetails(g_scene.materials[0],g_scene,0);
        DrawMaterialInspector(g_scene.materials[0],g_scene,0,false);
        ImGui::EndChild(); ImGui::SameLine(); ImGui::BeginChild("Graph");
        DrawMaterialGraphEditor(g_scene.materials[0],g_scene,0);
        ImGui::EndChild(); ImGui::End(); ImGui::Render();
        glBindFramebuffer(GL_FRAMEBUFFER,0); glViewport(0,0,1440,960); glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData()); glFinish();
    }
    pixels.resize(1440*960*4); glReadPixels(0,0,1440,960,GL_RGBA,GL_UNSIGNED_BYTE,pixels.data());
    stbi_flip_vertically_on_write(1);
    check(stbi_write_png("material-editor.png",1440,960,4,pixels.data(),1440*4) != 0,"production editor screenshot");
    g_editor_previews.stop();
    for (const auto& item : g_material_thumb_cache) glDeleteTextures(1,&item.second);
    for (const auto& item : g_texture_thumb_cache) glDeleteTextures(1,&item.second);
    ImGui_ImplOpenGL3_Shutdown(); ImGui::DestroyContext(); glfwDestroyWindow(window); glfwTerminate();
    std::printf("%d editor integration checks passed\n",checks);
}
