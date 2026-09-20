#include "core/dusktracer.h"
#include "core/scene.h"
#include "core/materials/material.h"
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <sstream>

static int checks = 0;
static void require(bool ok, const char* message) {
    ++checks; if (!ok) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
static void near(double a, double b, const char* message) { require(std::abs(a-b) < 1e-7,message); }
class sample_texture : public texture {
    bool srgb;
public:
    explicit sample_texture(bool color) : srgb(color) {}
    colour value(double u,double v,const point3&) const override { return srgb ? vec3(u*u,v*v,0.25) : vec3(u,v,0.5); }
    double alpha_at(double,double,const point3&) const override { return 0.3; }
    double mask_alpha_at(double,double,const point3&) const override { return 0.7; }
};
int main() {
    scene scn; scene_material m; m.model = scene_material_model::pbr;
    initialize_material_graph(m,scn.textures);
    auto& g = m.graph; int out = g.output_id();
    auto loader = [](const graph_node&,bool srgb) -> std::shared_ptr<texture> { return std::make_shared<sample_texture>(srgb); };
    int color = g.add(graph_kind::vector,0,0); g.find(color)->value = vec3(0.2,0.4,0.8);
    int scalar = g.add(graph_kind::scalar,0,150); g.find(scalar)->value = vec3(0.5,0,0);
    int mult = g.add(graph_kind::multiply,250,0);
    require(g.connect({color,0,mult,0}),"connect color to multiply");
    require(g.connect({scalar,0,mult,1}),"scalar broadcast");
    require(g.connect({mult,0,out,0}),"connect expression to surface");
    auto compiled = compile_material_graph(g,0,loader);
    auto result = compiled->value(0.2,0.6,{});
    near(result.x(),0.1,"multiply R"); near(result.z(),0.4,"multiply B");
    near(compiled->alpha_at(.9,.1,{}),1,"folded color keeps opaque alpha");
    near(compiled->mask_alpha_at(.9,.1,{}),.1,"folded color keeps scalar mask semantics");
    require(!g.connect({mult,0,out,2}),"RGB cannot silently feed a scalar");
    require(!g.connect({mult,0,mult,1}),"reject self cycle");
    require(!g.connect({999,0,mult,0}),"reject missing node");
    require(!g.connect({color,9,mult,0}),"reject invalid channel");
    int reroute = g.add(graph_kind::reroute,400,0);
    require(g.connect({mult,0,reroute,0}),"reroute type follows expression");
    require(!g.connect({reroute,0,mult,0}),"reject multi-node cycle");
    require(g.incoming(mult,0)->from == color,"rejected link preserves original connection");
    require(!g.connect({color,0,out,4}),"opacity requires explicit scalar");
    require(g.connect({color,2,out,2}),"extract green scalar");
    near(compile_material_graph(g,2,loader)->value(0,0,{}).x(),0.4,"channel evaluation");
    g.disconnect(out,2,true); require(!compile_material_graph(g,2,loader),"unconnected slot falls back");
    g.find(color)->value = vec3(1,1,1);
    near(compiled->value(0,0,{}).x(),0.1,"compiled snapshot independent of editor mutation");

    int tex = g.add(graph_kind::texture_sample,0,300);
    require(g.connect({tex,0,out,0}),"texture RGB to base color");
    near(compile_material_graph(g,0,loader)->value(0.2,0.6,{}).x(),0.04,"automatic sRGB color");
    near(compile_material_graph(g,0,loader)->alpha_at(0,0,{}),0.3,"texture alpha propagated");
    require(g.connect({tex,2,out,2}),"packed green to roughness");
    near(compile_material_graph(g,2,loader)->value(0.2,0.6,{}).x(),0.6,"data map remains linear");
    g.connect({tex,4,out,4}); near(compile_material_graph(g,4,loader)->mask_alpha_at(0,0,{}),0.3,"explicit alpha channel");
    g.connect({tex,5,out,4}); near(compile_material_graph(g,4,loader)->mask_alpha_at(0,0,{}),0.7,"legacy RGB mask semantics");
    int uv = g.add(graph_kind::texcoord,-200,300); g.find(uv)->value = vec3(2,3,1);
    require(g.connect({uv,0,tex,0}),"UV to texture");
    auto tiled = compile_material_graph(g,2,loader);
    near(tiled->value(0.2,0.6,{}).x(),0.8,"UV tiling and repeat");
    require(!g.connect({color,0,tex,0}),"reject RGB texture coordinates");
    int divide = g.add(graph_kind::divide,0,600); g.find(divide)->defaults = {1,0,0};
    g.connect({divide,0,out,1}); near(compile_material_graph(g,1,loader)->value(0,0,{}).x(),0,"safe divide by zero");
    int power = g.add(graph_kind::power,0,700); g.find(power)->defaults = {0,-1,0};
    g.connect({power,0,out,1}); require(std::isfinite(compile_material_graph(g,1,loader)->value(0,0,{}).x()),"safe nonfinite power");
    int mix = g.add(graph_kind::lerp,0,800); g.find(mix)->defaults = {0.2,0.8,0.25};
    g.connect({mix,0,out,1}); near(compile_material_graph(g,1,loader)->value(0,0,{}).x(),0.35,"lerp semantics");

    // Validate downstream pin types when editing a dynamically typed branch.
    int dynamic = g.add(graph_kind::multiply,0,900); g.connect({dynamic,0,out,2});
    require(!g.connect({color,0,dynamic,0}),"upstream edit cannot invalidate scalar destination");
    g.erase(scalar); require(!g.incoming(mult,1),"delete removes dependent wires");
    g.erase(out); require(g.output_id() == out,"surface output cannot be deleted");
    g.pan_x = -178.25f; g.pan_y = 60.5f; g.zoom = 0.72f;
    g.find(tex)->texture_path = "textures/a | b \\\"quoted\\\".png";
    auto encoded = serialize_material_graph(g); material_graph loaded;
    require(deserialize_material_graph(encoded,loaded),"round-trip deserialize");
    require(serialize_material_graph(loaded) == encoded,"round-trip layout, values, paths, topology");
    require(!deserialize_material_graph("zz",loaded),"reject corrupt encoding");
    require(serialize_material_graph(loaded) == encoded,"failed parse is transactional");
    auto cyclic = g; cyclic.links.push_back({reroute,0,mult,1});
    require(!deserialize_material_graph(serialize_material_graph(cyclic),loaded),"reject serialized cycle");
    require(!compile_material_graph(cyclic,0,loader),"runtime rejects cyclic data");
    auto invalid = g; invalid.links.push_back({999,0,out,2});
    require(!deserialize_material_graph(serialize_material_graph(invalid),loaded),"reject malformed link");
    material_graph empty; require(deserialize_material_graph(serialize_material_graph(empty),loaded),"legacy empty graph round-trip");

    scene_material legacy; legacy.model = scene_material_model::pbr; legacy.unreal_pbr = true;
    legacy.metallic_tex = 0; legacy.alpha_tex = 1;
    std::vector<scene_texture> textures = {{"ORM","packed.png"},{"Mask","mask.png"}};
    initialize_material_graph(legacy,textures);
    require(legacy.graph.nodes.size() == 3,"migration shares existing texture sample");
    require(legacy.graph.incoming(legacy.graph.output_id(),1)->output == 3,"packed metallic B migration");
    require(legacy.graph.incoming(legacy.graph.output_id(),2)->output == 2,"packed roughness G migration");
    require(legacy.graph.incoming(legacy.graph.output_id(),4)->output == 5,"mask migration preserves semantics");

    // Exercise the actual editor-to-renderer bridge, not just the evaluator.
    scene_material runtime; runtime.model = scene_material_model::pbr;
    initialize_material_graph(runtime,{}); auto& rg = runtime.graph;
    int tint = rg.add(graph_kind::vector,0,0); rg.find(tint)->value = vec3(0.17,0.29,0.63);
    rg.connect({tint,0,rg.output_id(),0});
    auto material = build_rt_material(scn,runtime); hit_record hit; hit.u = 0.25; hit.v = 0.5; hit.p = {};
    near(material->albedo(hit).y(),0.29,"PBR consumes graph output");
    for (auto model : {scene_material_model::lambert,scene_material_model::metal,scene_material_model::isotropic}) {
        runtime.model = model; near(build_rt_material(scn,runtime)->albedo(hit).z(),0.63,"other material models consume graph");
    }
    runtime.model = scene_material_model::dielectric; runtime.ior = 1; // deterministic refraction at normal incidence
    hit.normal = vec3(0,0,1); hit.front_face = true;
    colour attenuation; ray scattered;
    build_rt_material(scn,runtime)->scatter(ray(vec3(0,0,1),vec3(0,0,-1),0),hit,attenuation,scattered);
    near(attenuation.z(),0.63,"dielectric graph tints transmitted rays");
    runtime.model = scene_material_model::diffuse_light;
    near(build_rt_material(scn,runtime)->emitted(0,0,{}).x(),0.17,"graph drives light emission");

    material_graph large; int output = large.add(graph_kind::output,0,0);
    int value = large.add(graph_kind::scalar,0,0), last = value;
    large.find(value)->value = vec3(.25,0,0);
    for (int i = 0; i < 200; ++i) {
        int next = large.add(graph_kind::multiply,0,0);
        large.find(next)->defaults[1] = .99; large.connect({last,0,next,0}); last = next;
    }
    large.connect({last,0,output,0});
    require(graph_analysis(large).valid,"deep graph has valid cached topology");
    auto folded = compile_material_graph(large,0,loader);
    near(folded->value(.8,.7,{}).x(),.25*std::pow(.99,200),"deep constant graph folds without changing output");
    uint64_t surface = graph_surface_key(large), topology = graph_topology_key(large);
    large.find(value)->x = 123; large.pan_y = -500; large.zoom = .5f;
    require(graph_topology_key(large) == topology && graph_surface_key(large) == surface,"layout never changes shader or pin topology");
    int unused = large.add(graph_kind::scalar,0,0); large.find(unused)->value = vec3(.8,0,0);
    require(graph_surface_key(large) == surface,"disconnected nodes do not invalidate shading");
    large.connect({unused,0,output,2});
    uint64_t lambert_surface = graph_surface_key(large,1), pbr_surface = graph_surface_key(large,5);
    large.find(unused)->value = vec3(.9,0,0);
    require(graph_surface_key(large,1) == lambert_surface && graph_surface_key(large,5) != pbr_surface,
        "only inputs used by the active material model invalidate shading");
    std::cout << checks << " graph checks passed\n";
}
