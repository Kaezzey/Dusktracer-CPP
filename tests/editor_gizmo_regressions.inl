namespace gizmo_checks {
using namespace transform_gizmo;
static editor_camera_state camera_fixture() {
    editor_camera_state cam; cam.vfov=45;
    cam.set_from_lookat(point3(4,3,6),point3()); return cam;
}
static frame fixture(mode value=mode::translate) {
    finish(true); tool=value; coordinates=space::world;
    g_scene={}; scene_object obj; obj.type=scene_object_type::cube; obj.material_index=0;
    g_scene.objects.push_back(obj); g_scene.materials.emplace_back();
    UndoManager::Instance().clear();
    return build_frame(g_scene.objects[0],camera_fixture(),{30,80},{1000,640});
}
static ImVec2 pixel(const frame& f,point3 p) {
    ImVec2 screen; check(project(f,p,screen),"test point projects"); return screen;
}
static bool near_vec(vec3 a,vec3 b,double tolerance=1e-5) { return (a-b).length()<tolerance; }
static void controller() {
    auto f=fixture(); check(f.visible,"front-facing widget visible");
    check(pick(f,pixel(f,f.pivot+f.axes[0]*f.length*.65))==x,"finite X shaft pick");
    check(pick(f,pixel(f,f.pivot+f.axes[0]*f.length*1.7))==none,"no infinite-axis picking beyond arrow");
    check(pick(f,f.pivot_screen)==center,"center handle picks free movement");
    auto big=g_scene.objects[0]; big.radius=100; big.scale=vec3(.001,90,40);
    auto same=build_frame(big,camera_fixture(),f.top_left,f.size);
    check(std::abs(same.length-f.length)<1e-12,"widget size independent of object size");
    auto far_camera=camera_fixture(); far_camera.position*=10;
    auto distant=build_frame(big,far_camera,f.top_left,f.size);
    check(std::abs(distant.length/f.length-10)<1e-9,"world widget size tracks distance");
    check(near_vec(vec3(pixel(f,f.pivot+f.axes[0]*f.length).x,pixel(f,f.pivot+f.axes[0]*f.length).y,0),
                   vec3(pixel(distant,distant.pivot+distant.axes[0]*distant.length).x,pixel(distant,distant.pivot+distant.axes[0]*distant.length).y,0)),
          "widget stays the same displayed pixel size");
    auto start=f.pivot+f.axes[0]*f.length*.6;
    check(begin(f,x,pixel(f,start),0),"begin X drag");
    auto end=pixel(f,start+vec3(.75,0,0)); update(end,false); update(end,false);
    check(near_vec(g_scene.objects[0].translation,vec3(.75,0,0)),"axis constraint and frozen drag anchor");
    finish(); check(UndoManager::Instance().undo(),"drag creates undo action");
    check(near_vec(g_scene.objects[0].translation,vec3()),"undo entire drag");
    check(!UndoManager::Instance().undo(),"one undo action per drag");
    check(UndoManager::Instance().redo() && near_vec(g_scene.objects[0].translation,vec3(.75,0,0)),"redo entire drag");
    f=fixture(); start=f.pivot+f.length*(f.axes[0]*.33+f.axes[1]*.33);
    check(begin(f,xy,pixel(f,start),0),"begin XY plane drag");
    update(pixel(f,start+vec3(.26,.44,0)),true);
    check(near_vec(g_scene.objects[0].translation,vec3(.3,.4,0)),"plane constraint and translation snap");
    finish(true); check(near_vec(g_scene.objects[0].translation,vec3()) && !UndoManager::Instance().can_undo(),"cancel restores transform without undo entry");
    check(begin(f,center,f.pivot_screen,0),"begin free move");
    update(pixel(f,f.pivot+f.camera.right*.4+f.camera.up*.6),false);
    check(near_vec(g_scene.objects[0].translation,f.camera.right*.4+f.camera.up*.6),"free movement stays on camera plane");
    finish(true);
    g_scene.objects[0].rotation_deg=vec3(0,0,90); coordinates=space::local;
    f=build_frame(g_scene.objects[0],camera_fixture(),{30,80},{1000,640});
    check(near_vec(f.axes[0],vec3(0,1,0)),"local translation frame follows object rotation");
    start=f.pivot+f.axes[0]*f.length*.6; check(begin(f,x,pixel(f,start),0),"begin local move");
    update(pixel(f,start+f.axes[0]*.5),false);
    check(near_vec(g_scene.objects[0].translation,vec3(0,.5,0)),"local X translates in world Y"); finish(true);
    f=fixture(mode::scale); g_scene.objects[0].scale=vec3(.5,2,-1);
    start=f.pivot+f.axes[0]*f.length;
    check(begin(f,x,pixel(f,start),0),"begin scale X"); update(pixel(f,start+f.axes[0]*f.length*.26),true);
    check(near_vec(g_scene.objects[0].scale,vec3(.65,2,-1)),"scale snap changes only selected axis and keeps mirrored scale"); finish(true);
    check(begin(f,center,f.pivot_screen,0),"begin uniform scale");
    update(add(f.pivot_screen,{float(160*std::log(2.)),0}),false);
    check(near_vec(g_scene.objects[0].scale,vec3(1,4,-2)),"uniform scale preserves proportions and signs"); finish(true);
    f=fixture(mode::rotate); start=f.pivot+f.axes[0]*f.length;
    check(begin(f,z,pixel(f,start),0),"begin rotation arc");
    update(pixel(f,f.pivot+f.length*(f.axes[0]*std::cos(.55)+f.axes[1]*std::sin(.55))),true);
    check(near_vec(g_scene.objects[0].rotation_deg,vec3(0,0,30),1e-3),"rotation angle and snapping"); finish(true);
    check(begin(f,z,pixel(f,start),0),"begin continuous rotation");
    for(double degrees:{90.,170.,190.,270.})
        update(pixel(f,f.pivot+f.length*(f.axes[0]*std::cos(degrees_to_radians(degrees))+f.axes[1]*std::sin(degrees_to_radians(degrees)))),false);
    check(std::abs(drag.amount-270)<.001,"rotation unwraps across 180 degrees"); finish(true);
    auto side=camera_fixture(); side.set_from_lookat(point3(0,0,6),point3());
    f=build_frame(g_scene.objects[0],side,{30,80},{1000,640});
    start=f.pivot+f.length*(vec3(0,1,0)+vec3(0,0,1))/std::sqrt(2.);
    check(begin(f,x,pixel(f,start),0) && drag.screen_rotation,"edge-on ring uses stable screen tangent");
    update(add(drag.mouse_start,{drag.rotation_tangent.x*float(drag.pixels_per_radian*.4),drag.rotation_tangent.y*float(drag.pixels_per_radian*.4)}),false);
    check(std::abs(drag.amount-.4*180/pi)<.001,"edge-on rotation remains draggable"); finish(true);
    side.set_from_lookat(point3(0,0,-6),point3(0,0,-7));
    check(!build_frame(g_scene.objects[0],side,{0,0},{1000,640}).visible,"behind-camera widget hidden");
    vec3 initial(25,89.999,40),axis=unit_vector(vec3(1,2,3));
    vec3 angles=rotated_euler(initial,axis,.7);
    double r[3][3],inverse[3][3],out[3][3]; make_trs_linear(initial,vec3(1,1,1),r,inverse); make_trs_linear(angles,vec3(1,1,1),out,inverse);
    for(int c=0;c<3;++c) {
        vec3 v(r[0][c],r[1][c],r[2][c]);
        auto expected=v*std::cos(.7)+cross(axis,v)*std::sin(.7)+axis*dot(axis,v)*(1-std::cos(.7));
        check(near_vec(expected,vec3(out[0][c],out[1][c],out[2][c]),1e-9),"rotation composition near gimbal lock");
    }
}
static int run() {
    controller();
    check(glfwInit()!=0,"gizmo GLFW init"); glfwWindowHint(GLFW_VISIBLE,GLFW_FALSE);
    auto* window=glfwCreateWindow(1200,760,"Gizmo regression",nullptr,nullptr);
    check(window!=nullptr,"hidden gizmo context"); glfwMakeContextCurrent(window); glewInit();
    while(glGetError()!=GL_NO_ERROR) {}
    ImGui::CreateContext(); auto& io=ImGui::GetIO(); io.IniFilename=nullptr; io.DisplaySize={1200,760}; io.DeltaTime=1.f/60;
    SetupEditorFonts(io); ApplyModernEditorTheme(); ImGui_ImplOpenGL3_Init("#version 130");
    g_viewport_mode=1; g_viewport_focused=true;
    auto press=[&](ImGuiKey key,bool ctrl=false,bool right_mouse=false,bool text=false) {
        io.AddKeyEvent(ImGuiMod_Ctrl,ctrl); io.AddKeyEvent(key,true); io.AddMouseButtonEvent(1,right_mouse);
        ImGui_ImplOpenGL3_NewFrame(); ImGui::NewFrame(); io.WantTextInput=text; shortcuts(); ImGui::EndFrame();
        io.AddKeyEvent(key,false); io.AddKeyEvent(ImGuiMod_Ctrl,false); io.AddMouseButtonEvent(1,false);
        ImGui_ImplOpenGL3_NewFrame(); ImGui::NewFrame(); io.WantTextInput=false; ImGui::EndFrame();
    };
    press(ImGuiKey_W); check(tool==mode::translate,"W selects Move");
    press(ImGuiKey_E); check(tool==mode::rotate,"E selects Rotate");
    press(ImGuiKey_R); check(tool==mode::scale,"R selects Scale");
    press(ImGuiKey_Space); check(tool==mode::translate,"Space cycles gizmo modes");
    press(ImGuiKey_Space,true); check(tool==mode::translate,"Ctrl Space does not switch tool");
    press(ImGuiKey_E,false,true); check(tool==mode::translate,"RMB camera navigation suspends transform shortcuts");
    press(ImGuiKey_R,false,false,true); check(tool==mode::translate,"text entry suspends transform shortcuts");
    auto cancel_frame=fixture();
    check(begin(cancel_frame,x,pixel(cancel_frame,cancel_frame.pivot+cancel_frame.axes[0]*cancel_frame.length*.6),0),"begin drag for Escape shortcut");
    update(add(drag.mouse_start,{40,0}),false); press(ImGuiKey_Escape);
    check(!drag.active && near_vec(g_scene.objects[0].translation,vec3()),"Escape cancels drag through input shortcut");
    BuildRasterShader(); BuildUnitSphereMesh(); BuildUnitCubeMesh();
    for(auto operation:{mode::translate,mode::rotate,mode::scale}) {
        fixture(operation); g_editor_cam=camera_fixture(); g_scene.objects[0].scale=vec3(.65,.65,.65);
        g_scene.materials[0].model=scene_material_model::pbr; g_scene.materials[0].base_color=vec3(.3,.35,.42);
        g_viewport_mode=1;
        for(int highlight:{none,x}) {
            for(int warm=0;warm<2;++warm) {
                ImGui_ImplOpenGL3_NewFrame(); ImGui::NewFrame();
                ImGui::SetNextWindowPos({0,0}); ImGui::SetNextWindowSize({1200,760});
                ImGui::Begin("Transform controls",nullptr,ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoCollapse);
                toolbar(); ImGui::TextDisabled("RMB + WASD fly | W / E / R transform | Ctrl snap | Esc cancel");
                auto origin=ImGui::GetCursorScreenPos(),size=ImGui::GetContentRegionAvail();
                RenderRasterToTexture(int(size.x),int(size.y));
                ImGui::Image((ImTextureID)(intptr_t)g_rasterColorTex,size,{0,1},{1,0});
                hovered=highlight; auto f=build_frame(g_scene.objects[0],g_editor_cam,origin,size); draw(ImGui::GetWindowDrawList(),f);
                ImGui::End(); ImGui::Render(); glViewport(0,0,1200,760); glClear(GL_COLOR_BUFFER_BIT);
                ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData()); glFinish();
            }
            std::vector<unsigned char> pixels(1200*760*4); glReadPixels(0,0,1200,760,GL_RGBA,GL_UNSIGNED_BYTE,pixels.data());
            std::string name=std::string(mode_name(operation))+(highlight==none?".png":"-hover.png");
            stbi_flip_vertically_on_write(1); check(stbi_write_png(name.c_str(),1200,760,4,pixels.data(),1200*4)!=0,"save gizmo screenshot");
            check(glGetError()==GL_NO_ERROR,"gizmo overlay renders without GL errors");
        }
    }
    ImGui_ImplOpenGL3_Shutdown(); ImGui::DestroyContext(); StopRasterPreview(); glfwDestroyWindow(window); glfwTerminate();
    std::printf("%d transform gizmo checks passed\n",checks); return 0;
}
} // namespace gizmo_checks
