// Unreal-style transform handles. Drawing and picking share projected finite
// geometry in displayed viewport pixels, independent of render resolution.
namespace transform_gizmo {
enum class mode { select, translate, rotate, scale };
enum class space { world, local };
enum part { none=-1, x=0, y=1, z=2, xy=3, xz=4, yz=5, center=6 };
static mode tool = mode::translate;
static space coordinates = space::world;
static int hovered = none;
static bool snapping = false;
static float move_step = .1f, rotation_step = 15.f, scale_step = .1f;
constexpr double widget_pixels = 96;
constexpr double hit_pixels = 7;
struct triangle { ImVec2 a,b,c; double depth, shade; };
struct handle {
    int id = none;
    std::vector<ImVec2> path, polygon;
    std::vector<triangle> faces;
};
struct frame {
    editor_camera_state camera;
    ImVec2 top_left{}, size{}, pivot_screen{};
    point3 pivot;
    vec3 axes[3];
    double length = 0;
    bool visible = false;
    std::vector<handle> handles;
};
struct drag_state {
    bool active = false;
    int object = -1, handle = none;
    mode operation = mode::translate;
    ObjSnapshot before;
    frame basis;
    vec3 plane_normal, anchor, radial_start;
    ImVec2 mouse_start{};
    bool screen_rotation = false;
    ImVec2 rotation_tangent{};
    double pixels_per_radian = 1;
    double previous_angle = 0, angle = 0, amount = 0;
};
static drag_state drag;
static ImVec2 add(ImVec2 a, ImVec2 b) { return {a.x+b.x,a.y+b.y}; }
static ImVec2 subtract(ImVec2 a, ImVec2 b) { return {a.x-b.x,a.y-b.y}; }
static double length2(ImVec2 a) { return double(a.x)*a.x+double(a.y)*a.y; }
static double cross2(ImVec2 a, ImVec2 b) { return double(a.x)*b.y-double(a.y)*b.x; }
static int axes_mask(int id) {
    if (id < 0) return 0;
    if (id < 3) return 1<<id;
    return id == xy ? 3 : id == xz ? 5 : id == yz ? 6 : 7;
}
static const char* part_name(int id) {
    const char* names[] = {"X","Y","Z","XY","XZ","YZ","Free"};
    return id >= 0 && id <= center ? names[id] : "";
}
static const char* mode_name(mode value) {
    return value == mode::rotate ? "Rotate" : value == mode::scale ? "Scale" : "Move";
}
static double segment_distance(ImVec2 p, ImVec2 a, ImVec2 b) {
    auto delta = subtract(b,a), offset = subtract(p,a);
    double t = std::clamp((double(offset.x)*delta.x+double(offset.y)*delta.y)/std::max(1e-12,length2(delta)),0.,1.);
    return std::sqrt(length2(subtract(p,{float(a.x+t*delta.x),float(a.y+t*delta.y)})));
}
static bool inside_triangle(ImVec2 p, ImVec2 a, ImVec2 b, ImVec2 c) {
    if (std::abs(cross2(subtract(b,a),subtract(c,a))) < .5) return false;
    double u=cross2(subtract(b,a),subtract(p,a)), v=cross2(subtract(c,b),subtract(p,b)),
           w=cross2(subtract(a,c),subtract(p,c));
    return (u>=0 && v>=0 && w>=0) || (u<=0 && v<=0 && w<=0);
}
static bool project(const frame& f, const point3& p, ImVec2& screen, double* depth=nullptr) {
    vec3 offset = p-f.camera.position;
    double z = dot(offset,f.camera.forward);
    if (depth) *depth=z;
    if (z <= .01 || f.size.x <= 0 || f.size.y <= 0) return false;
    double focal = .5*f.size.y/std::tan(degrees_to_radians(f.camera.vfov)*.5);
    screen = {float(f.top_left.x+f.size.x*.5+focal*dot(offset,f.camera.right)/z),
              float(f.top_left.y+f.size.y*.5-focal*dot(offset,f.camera.up)/z)};
    return true;
}
static ray mouse_ray(const frame& f, ImVec2 mouse) {
    // Continuous pixel coordinates match projection, without a texel-center bias.
    double focal = .5*f.size.y/std::tan(degrees_to_radians(f.camera.vfov)*.5);
    vec3 direction = f.camera.forward + f.camera.right*((mouse.x-f.top_left.x-f.size.x*.5)/focal)
        - f.camera.up*((mouse.y-f.top_left.y-f.size.y*.5)/focal);
    return ray(f.camera.position,safe_unit_vector(direction),0);
}
static bool plane_hit(const frame& f, ImVec2 mouse, const vec3& normal, point3& p) {
    ray r=mouse_ray(f,mouse);
    double denominator=dot(r.direction(),normal);
    if (std::abs(denominator)<1e-6) return false;
    double t=dot(f.pivot-r.origin(),normal)/denominator;
    if (t<=0 || !std::isfinite(t)) return false;
    p=r.at(t); return true;
}
static void face(frame& f, handle& h, const point3& a, const point3& b, const point3& c) {
    triangle tri; double za,zb,zc;
    if (!project(f,a,tri.a,&za) || !project(f,b,tri.b,&zb) || !project(f,c,tri.c,&zc)) return;
    if(cross2(subtract(tri.b,tri.a),subtract(tri.c,tri.a))<0) std::swap(tri.b,tri.c);
    tri.depth=(za+zb+zc)/3;
    vec3 normal=safe_unit_vector(cross(b-a,c-a));
    tri.shade=.65+.35*std::abs(dot(normal,-f.camera.forward));
    h.faces.push_back(tri);
}
static void cube(frame& f, handle& h, const point3& p, double radius) {
    point3 corners[8];
    for(int i=0;i<8;++i) corners[i]=p+radius*((i&1?1:-1)*f.axes[0]+(i&2?1:-1)*f.axes[1]+(i&4?1:-1)*f.axes[2]);
    const int indices[][4]={{0,1,3,2},{4,6,7,5},{0,4,5,1},{2,3,7,6},{0,2,6,4},{1,5,7,3}};
    for(auto& q:indices) { face(f,h,corners[q[0]],corners[q[1]],corners[q[2]]); face(f,h,corners[q[0]],corners[q[2]],corners[q[3]]); }
}
static frame build_frame(const scene_object& object, const editor_camera_state& camera, ImVec2 origin, ImVec2 size) {
    frame f; f.camera=camera; f.top_left=origin; f.size=size;
    f.pivot=object.center+object.translation;
    double depth=dot(f.pivot-camera.position,camera.forward);
    if (tool==mode::select || depth<=.02 || !project(f,f.pivot,f.pivot_screen)) return f;
    f.length=widget_pixels*2*depth*std::tan(degrees_to_radians(camera.vfov)*.5)/size.y;
    if (f.length>=depth*.8) return f; // Do not cross the camera near plane.
    double rotation[3][3],inverse[3][3];
    make_trs_linear((coordinates==space::local || tool==mode::scale) ? object.rotation_deg : vec3(),vec3(1,1,1),rotation,inverse);
    for(int a=0;a<3;++a) f.axes[a]=vec3(rotation[0][a],rotation[1][a],rotation[2][a]);
    // Freeze the rotation widget's frame for the duration of a drag.
    if(drag.active && drag.operation==mode::rotate)
        for(int a=0;a<3;++a) f.axes[a]=drag.basis.axes[a];
    f.visible=true;
    if(tool==mode::rotate) {
        for(int a=0;a<3;++a) {
            if(drag.active && drag.handle!=a) continue;
            handle h; h.id=a;
            vec3 u=f.axes[(a+1)%3],v=f.axes[(a+2)%3],view=camera.position-f.pivot;
            double su=dot(view,u)>=0?1:-1, sv=dot(view,v)>=0?1:-1;
            const int steps=drag.active?128:48;
            for(int i=0;i<=steps;++i) {
                double angle=(drag.active?2*pi:pi*.5)*i/steps;
                point3 point=f.pivot+f.length*(u*(su*std::cos(angle))+v*(sv*std::sin(angle)));
                ImVec2 screen; if(project(f,point,screen)) h.path.push_back(screen);
            }
            f.handles.push_back(std::move(h));
        }
        return f;
    }
    for(int a=0;a<3;++a) {
        ImVec2 start,end;
        if(!project(f,f.pivot+f.axes[a]*f.length*.12,start) || !project(f,f.pivot+f.axes[a]*f.length,end)) continue;
        if(length2(subtract(end,f.pivot_screen))<14*14) continue;
        handle h; h.id=a; h.path={start,end};
        if(tool==mode::translate) {
            vec3 u=f.axes[(a+1)%3],v=f.axes[(a+2)%3];
            point3 base=f.pivot+f.axes[a]*f.length*.83, tip=f.pivot+f.axes[a]*f.length*1.07;
            for(int i=0;i<24;++i) {
                double p=2*pi*i/24, q=2*pi*(i+1)/24;
                point3 b=base+f.length*.065*(u*std::cos(p)+v*std::sin(p));
                point3 c=base+f.length*.065*(u*std::cos(q)+v*std::sin(q));
                face(f,h,tip,b,c); face(f,h,base,c,b);
            }
            project(f,base,h.path.back());
        } else cube(f,h,f.pivot+f.axes[a]*f.length,f.length*.067);
        f.handles.push_back(std::move(h));
    }
    const int pairs[][3]={{0,1,xy},{0,2,xz},{1,2,yz}};
    for(auto& pair:pairs) {
        handle h; h.id=pair[2]; vec3 u=f.axes[pair[0]],v=f.axes[pair[1]];
        std::vector<point3> points;
        if(tool==mode::translate) points={f.pivot+f.length*(u*.22+v*.22),f.pivot+f.length*(u*.44+v*.22),
            f.pivot+f.length*(u*.44+v*.44),f.pivot+f.length*(u*.22+v*.44)};
        else points={f.pivot+f.length*(u*.17+v*.17),f.pivot+f.length*(u*.48+v*.17),f.pivot+f.length*(u*.17+v*.48)};
        for(auto& point:points) { ImVec2 screen; if(project(f,point,screen)) h.polygon.push_back(screen); }
        if(h.polygon.size()>=3 && cross2(subtract(h.polygon[1],h.polygon[0]),subtract(h.polygon[2],h.polygon[0]))<0)
            std::reverse(h.polygon.begin(),h.polygon.end());
        if(h.polygon.size()>=3 && std::abs(cross2(subtract(h.polygon[1],h.polygon[0]),subtract(h.polygon[2],h.polygon[0])))>25)
            f.handles.push_back(std::move(h));
    }
    handle middle; middle.id=center;
    if(tool==mode::scale) cube(f,middle,f.pivot,f.length*.067);
    else for(int i=0;i<24;++i) {
        double angle=2*pi*i/24;
        middle.polygon.push_back(add(f.pivot_screen,{float(6*std::cos(angle)),float(6*std::sin(angle))}));
    }
    f.handles.push_back(std::move(middle));
    return f;
}
static int pick(const frame& f, ImVec2 mouse) {
    if(!f.visible) return none;
    double best=hit_pixels; int result=none;
    for(const auto& h:f.handles) {
        bool filled=false;
        for(const auto& tri:h.faces) if(inside_triangle(mouse,tri.a,tri.b,tri.c)) filled=true;
        for(size_t i=1;i+1<h.polygon.size();++i)
            if(inside_triangle(mouse,h.polygon[0],h.polygon[i],h.polygon[i+1])) filled=true;
        double distance=hit_pixels+1;
        for(size_t i=1;i<h.path.size();++i) distance=std::min(distance,segment_distance(mouse,h.path[i-1],h.path[i]));
        if(filled) distance=0;
        // Center and finite end caps win ties; do not select an infinite shaft.
        if(distance<best || (filled && h.id==center)) { best=distance; result=h.id; }
    }
    return result;
}
static ImU32 color(int axis, bool selected, float shade=1, int alpha=255) {
    ImVec4 rgb=selected?ImVec4(1,.82f,.12f,1):axis==0?ImVec4(.95f,.16f,.12f,1):
        axis==1?ImVec4(.25f,.85f,.12f,1):axis==2?ImVec4(.12f,.50f,1,1):ImVec4(.94f,.94f,.94f,1);
    return IM_COL32(int(rgb.x*shade*255),int(rgb.y*shade*255),int(rgb.z*shade*255),alpha);
}
static void draw(ImDrawList* list,const frame& f) {
    if(!f.visible) return;
    list->PushClipRect(f.top_left,add(f.top_left,f.size),true);
    int active=drag.active?drag.handle:hovered, mask=axes_mask(active);
    // Planes first, then shafts and shaded end caps; center is drawn last.
    for(const auto& h:f.handles) if(h.id>=xy && h.id<=yz) {
        bool selected=active==h.id;
        list->AddConvexPolyFilled(h.polygon.data(),int(h.polygon.size()),color(3,selected,1,selected?110:35));
        list->AddPolyline(h.polygon.data(),int(h.polygon.size()),color(3,selected,.8f,190),ImDrawFlags_Closed,1.5f);
    }
    for(const auto& h:f.handles) if(h.id<3 || h.id==center) {
        bool selected=h.id==center?active==center:(mask&(1<<h.id))!=0;
        if(!h.path.empty()) {
            list->AddPolyline(h.path.data(),int(h.path.size()),IM_COL32(15,17,20,150),0,selected?7.f:5.5f);
            list->AddPolyline(h.path.data(),int(h.path.size()),color(h.id,selected),0,selected?4.5f:3.2f);
        }
        auto faces=h.faces;
        std::stable_sort(faces.begin(),faces.end(),[](const triangle& a,const triangle& b){return a.depth>b.depth;});
        for(const auto& tri:faces) list->AddTriangleFilled(tri.a,tri.b,tri.c,color(h.id,selected,float(tri.shade)));
        if(h.id==center && !h.polygon.empty()) {
            list->AddCircleFilled(f.pivot_screen,6,color(3,selected));
            list->AddCircle(f.pivot_screen,6,IM_COL32(15,17,20,180),24,1);
        }
    }
    if(drag.active) {
        char text[96];
        if(drag.operation==mode::rotate) std::snprintf(text,sizeof(text),"%s  %.1f deg",part_name(drag.handle),drag.amount);
        else if(drag.operation==mode::scale) std::snprintf(text,sizeof(text),"%s  x %.3f",part_name(drag.handle),drag.amount);
        else std::snprintf(text,sizeof(text),"%s  %.3f",part_name(drag.handle),drag.amount);
        ImVec2 p=add(f.pivot_screen,{16,16}),size=ImGui::CalcTextSize(text);
        list->AddRectFilled(subtract(p,{6,4}),add(p,add(size,{6,4})),IM_COL32(18,21,27,235),4);
        list->AddText(p,color(3,true),text);
    }
    list->PopClipRect();
}
static ObjSnapshot snapshot(const scene_object& o) { return {o.center,o.translation,o.rotation_deg,o.scale,o.radius}; }
static void restore(int index,const ObjSnapshot& s) {
    if(index<0 || index>=int(g_scene.objects.size())) return;
    auto& o=g_scene.objects[index];
    o.center=s.center; o.translation=s.translation; o.rotation_deg=s.rotation_deg; o.scale=s.scale; o.radius=s.radius;
    g_world_dirty=true; g_cached_world.reset();
}
static void finish(bool cancel=false) {
    if(!drag.active) { hovered=none; return; }
    int index=drag.object;
    if(index>=0 && index<int(g_scene.objects.size())) {
        ObjSnapshot before=drag.before,after=snapshot(g_scene.objects[index]);
        if(cancel) restore(index,before);
        else if((after.translation-before.translation).length_squared()>1e-14 ||
                (after.rotation_deg-before.rotation_deg).length_squared()>1e-14 ||
                (after.scale-before.scale).length_squared()>1e-14) {
            std::string label=std::string(mode_name(drag.operation))+" Object";
            UndoManager::Instance().push(std::make_unique<LambdaAction>(
                [index,before]{restore(index,before);},[index,after]{restore(index,after);},label));
        }
    }
    drag={}; hovered=none;
}
static bool begin(const frame& f,int id,ImVec2 mouse,int object) {
    if(id==none || object<0 || object>=int(g_scene.objects.size())) return false;
    drag_state next; next.active=true; next.object=object; next.handle=id;
    next.operation=tool; next.basis=f; next.before=snapshot(g_scene.objects[object]); next.mouse_start=mouse;
    if(tool==mode::scale && id==center) { next.amount=1; drag=std::move(next); return true; }
    if(tool==mode::rotate) {
        next.plane_normal=f.axes[id];
        // An edge-on rotation plane has no stable ray intersection. Follow
        // the projected ring tangent instead, so side-view arcs still drag.
        if(std::abs(dot(f.camera.forward,next.plane_normal))<.12) {
            double best=1e30;
            vec3 u=f.axes[(id+1)%3],v=f.axes[(id+2)%3];
            for(int i=0;i<256;++i) {
                double angle=2*pi*i/256;
                vec3 radial=u*std::cos(angle)+v*std::sin(angle);
                ImVec2 p,q;
                if(!project(f,f.pivot+f.length*radial,p) ||
                   !project(f,f.pivot+f.length*(radial*std::cos(.01)+cross(next.plane_normal,radial)*std::sin(.01)),q)) continue;
                ImVec2 tangent=subtract(q,p);
                double rate=std::sqrt(length2(tangent))/.01, distance=length2(subtract(p,mouse));
                if(rate>10 && distance<best) {
                    best=distance; next.rotation_tangent={float(tangent.x/(rate*.01)),float(tangent.y/(rate*.01))};
                    next.pixels_per_radian=rate; next.screen_rotation=true;
                }
            }
            if(!next.screen_rotation) return false;
            drag=std::move(next); return true;
        }
    }
    else if(id<3) {
        vec3 axis=f.axes[id]; next.plane_normal=safe_unit_vector(f.camera.forward-dot(f.camera.forward,axis)*axis);
    } else if(id==center) next.plane_normal=f.camera.forward;
    else next.plane_normal=id==xy?f.axes[2]:id==xz?f.axes[1]:f.axes[0];
    if(!plane_hit(f,mouse,next.plane_normal,next.anchor)) return false;
    if(tool==mode::rotate) {
        vec3 radial=next.anchor-f.pivot;
        if(radial.length_squared()<1e-12) return false;
        next.radial_start=unit_vector(radial);
    }
    next.amount=tool==mode::scale?1:0;
    drag=std::move(next); return true;
}
static double nearest_degrees(double degrees,double reference) { return degrees+360*std::round((reference-degrees)/360); }
static vec3 rotated_euler(const vec3& initial,const vec3& axis,double angle) {
    double r[3][3],inv[3][3],out[3][3]; make_trs_linear(initial,vec3(1,1,1),r,inv);
    for(int col=0;col<3;++col) {
        vec3 v(r[0][col],r[1][col],r[2][col]);
        vec3 rotated=v*std::cos(angle)+cross(axis,v)*std::sin(angle)+axis*dot(axis,v)*(1-std::cos(angle));
        for(int row=0;row<3;++row) out[row][col]=rotated[row];
    }
    double ry=std::asin(std::clamp(-out[2][0],-1.,1.)),rx,rz;
    if(std::abs(std::cos(ry))>1e-7) { rx=std::atan2(out[2][1],out[2][2]); rz=std::atan2(out[1][0],out[0][0]); }
    else {
        rx=degrees_to_radians(initial.x());
        rz=std::atan2(-out[0][1],out[1][1])+(ry>0?rx:-rx);
    }
    vec3 a(rx*180/pi,ry*180/pi,rz*180/pi),b(a.x()+180,180-a.y(),a.z()+180);
    for(int i=0;i<3;++i) { a[i]=nearest_degrees(a[i],initial[i]); b[i]=nearest_degrees(b[i],initial[i]); }
    return (a-initial).length_squared()<=(b-initial).length_squared()?a:b;
}
static void update(ImVec2 mouse,bool snap) {
    if(!drag.active || drag.object<0 || drag.object>=int(g_scene.objects.size())) return;
    auto& o=g_scene.objects[drag.object]; const auto& f=drag.basis;
    if(drag.operation==mode::scale && drag.handle==center) {
        auto delta=subtract(mouse,drag.mouse_start);
        double factor=std::exp(std::clamp((double(delta.x)-delta.y)/160.,-6.,6.));
        if(snap) factor=std::max(.01,1+std::round((factor-1)/std::max(.001f,scale_step))*scale_step);
        o.scale=drag.before.scale*factor; drag.amount=factor;
    } else if(drag.operation==mode::rotate && drag.screen_rotation) {
        auto delta=subtract(mouse,drag.mouse_start);
        double degrees=(delta.x*drag.rotation_tangent.x+delta.y*drag.rotation_tangent.y)/drag.pixels_per_radian*180/pi;
        if(snap) degrees=std::round(degrees/std::max(.01f,rotation_step))*rotation_step;
        o.rotation_deg=rotated_euler(drag.before.rotation_deg,drag.plane_normal,degrees_to_radians(degrees));
        drag.amount=degrees;
    } else {
        point3 point; if(!plane_hit(f,mouse,drag.plane_normal,point)) return;
        vec3 delta=point-drag.anchor;
        if(drag.operation==mode::rotate) {
            vec3 radial=safe_unit_vector(point-f.pivot,drag.radial_start);
            double angle=std::atan2(dot(drag.plane_normal,cross(drag.radial_start,radial)),dot(drag.radial_start,radial));
            double step=angle-drag.previous_angle;
            if(step>pi) step-=2*pi; if(step<-pi) step+=2*pi;
            drag.angle+=step; drag.previous_angle=angle;
            double degrees=drag.angle*180/pi;
            if(snap) degrees=std::round(degrees/std::max(.01f,rotation_step))*rotation_step;
            o.rotation_deg=rotated_euler(drag.before.rotation_deg,drag.plane_normal,degrees_to_radians(degrees));
            drag.amount=degrees;
        } else if(drag.operation==mode::translate) {
            vec3 offset;
            if(drag.handle==center) {
                for(vec3 axis:{f.camera.right,f.camera.up}) {
                    double value=dot(delta,axis);
                    if(snap) value=std::round(value/std::max(.001f,move_step))*move_step;
                    offset+=axis*value;
                }
            } else for(int a=0;a<3;++a) if(axes_mask(drag.handle)&(1<<a)) {
                double value=dot(delta,f.axes[a]);
                if(snap) value=std::round(value/std::max(.001f,move_step))*move_step;
                offset+=f.axes[a]*value;
            }
            o.translation=drag.before.translation+offset;
            drag.amount=drag.handle<3?dot(offset,f.axes[drag.handle]):offset.length();
        } else {
            o.scale=drag.before.scale; double sum=0; int count=0;
            for(int a=0;a<3;++a) if(axes_mask(drag.handle)&(1<<a)) {
                double factor=1+dot(delta,f.axes[a])/f.length;
                if(snap) factor=1+std::round((factor-1)/std::max(.001f,scale_step))*scale_step;
                factor=std::clamp(factor,.01,100.);
                o.scale[a]=drag.before.scale[a]*factor; sum+=factor; ++count;
            }
            drag.amount=sum/std::max(1,count);
        }
    }
    g_world_dirty=true; g_cached_world.reset();
}
static void set_tool(mode value) { if(drag.active) return; tool=value; hovered=none; }
static void shortcuts() {
    auto& io=ImGui::GetIO();
    if(drag.active) { if(ImGui::IsKeyPressed(ImGuiKey_Escape,false)) finish(true); return; }
    if(g_viewport_mode!=1 || (!g_viewport_hovered && !g_viewport_focused) || io.WantTextInput ||
       ImGui::IsAnyItemActive() || io.KeyCtrl || io.KeyAlt || ImGui::IsMouseDown(1)) return;
    if(ImGui::IsKeyPressed(ImGuiKey_Q,false)) set_tool(mode::select);
    if(ImGui::IsKeyPressed(ImGuiKey_W,false)) set_tool(mode::translate);
    if(ImGui::IsKeyPressed(ImGuiKey_E,false)) set_tool(mode::rotate);
    if(ImGui::IsKeyPressed(ImGuiKey_R,false)) set_tool(mode::scale);
    if(ImGui::IsKeyPressed(ImGuiKey_Space,false))
        set_tool(tool==mode::translate?mode::rotate:tool==mode::rotate?mode::scale:mode::translate);
}
static void toolbar() {
    ImGui::BeginDisabled(g_viewport_mode!=1 || drag.active);
    auto next_control=[](float width) {
        float right=ImGui::GetWindowPos().x+ImGui::GetWindowContentRegionMax().x;
        if(ImGui::GetItemRectMax().x+ImGui::GetStyle().ItemSpacing.x+width<=right) ImGui::SameLine();
    };
    const char* labels[]={"Q Select","W Move","E Rotate","R Scale"};
    for(int i=0;i<4;++i) {
        if(i) next_control(ImGui::CalcTextSize(labels[i]).x+ImGui::GetStyle().FramePadding.x*2);
        bool selected = tool==mode(i);
        if(selected) ImGui::PushStyleColor(ImGuiCol_Button,ImVec4(.22f,.34f,.49f,1));
        if(ImGui::Button(labels[i])) set_tool(mode(i));
        if(selected) ImGui::PopStyleColor();
    }
    next_control(ImGui::CalcTextSize("World").x+ImGui::GetStyle().FramePadding.x*2);
    ImGui::BeginDisabled(tool==mode::scale);
    if(ImGui::Button(tool==mode::scale?"Local##GizmoSpace":coordinates==space::world?"World##GizmoSpace":"Local##GizmoSpace"))
        coordinates=coordinates==space::world?space::local:space::world;
    ImGui::EndDisabled();
    if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(tool==mode::scale?"Scale follows the object's local axes.":"Switch world / local axes.");
    next_control(ImGui::GetFrameHeight()+ImGui::GetStyle().ItemInnerSpacing.x+ImGui::CalcTextSize("Snap").x);
    ImGui::Checkbox("Snap",&snapping);
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Snap transforms to increments. Hold Ctrl while dragging to snap temporarily.");
    next_control(60); ImGui::SetNextItemWidth(60);
    float* step=tool==mode::rotate?&rotation_step:tool==mode::scale?&scale_step:&move_step;
    ImGui::DragFloat("##GizmoSnapStep",step,.01f,.001f,1000.f,tool==mode::rotate?"%.1f deg":"%.3f",ImGuiSliderFlags_AlwaysClamp);
    ImGui::EndDisabled();
}
} // namespace transform_gizmo
