#include "core/dusktracer.h"
#include "core/scene.h"
#include "core/materials/texture.h"
#include <iomanip>
#include <cstring>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

const char* graph_name(graph_kind k) {
    static const char* names[] = {"Material Output", "Texture Sample", "Scalar", "Color",
        "Texture Coordinate", "Add", "Subtract", "Multiply", "Divide", "Lerp",
        "One Minus", "Saturate", "Power", "Reroute"};
    return names[static_cast<int>(k)];
}
const char* graph_category(graph_kind k) {
    switch (k) {
        case graph_kind::texture_sample: case graph_kind::texcoord: return "Textures";
        case graph_kind::scalar: case graph_kind::vector: return "Parameters";
        case graph_kind::output: case graph_kind::reroute: return "Utility";
        default: return "Math";
    }
}
int graph_inputs(graph_kind k) {
    switch (k) {
        case graph_kind::output: return 5;
        case graph_kind::scalar: case graph_kind::vector: case graph_kind::texcoord: return 0;
        case graph_kind::texture_sample: case graph_kind::one_minus:
        case graph_kind::saturate: case graph_kind::reroute: return 1;
        case graph_kind::lerp: return 3;
        default: return 2;
    }
}
int graph_outputs(graph_kind k) {
    switch (k) {
        case graph_kind::output: return 0;
        case graph_kind::texture_sample: return 6;
        case graph_kind::vector: return 4;
        case graph_kind::texcoord: return 3;
        default: return 1;
    }
}
const char* graph_input_name(graph_kind k, int pin) {
    static const char* slots[] = {"Base Color", "Metallic", "Roughness", "Normal", "Opacity"};
    static const char* math[] = {"A", "B", "Alpha"};
    if (k == graph_kind::output) return slots[pin];
    if (k == graph_kind::texture_sample) return "UV";
    if (graph_inputs(k) == 1) return "In";
    return math[pin];
}
const char* graph_output_name(graph_kind k, int pin) {
    static const char* channels[] = {"RGB", "R", "G", "B", "A", "Mask"};
    static const char* uv[] = {"UV", "U", "V"};
    if (k == graph_kind::texture_sample || k == graph_kind::vector) return channels[pin];
    if (k == graph_kind::texcoord) return uv[pin];
    return "Out";
}
graph_node* material_graph::find(int id) {
    for (auto& n : nodes) if (n.id == id) return &n;
    return nullptr;
}
const graph_node* material_graph::find(int id) const {
    for (const auto& n : nodes) if (n.id == id) return &n;
    return nullptr;
}
const graph_link* material_graph::incoming(int id, int pin) const {
    for (const auto& l : links) if (l.to == id && l.input == pin) return &l;
    return nullptr;
}
int material_graph::add(graph_kind k, float x, float y) {
    if (nodes.size() >= max_nodes || (k == graph_kind::output && output_id())) return 0;
    graph_node n;
    n.id = next_id++; n.kind = k; n.x = x; n.y = y;
    if (k == graph_kind::add || k == graph_kind::subtract) n.defaults[1] = 0;
    nodes.push_back(n);
    return n.id;
}
int material_graph::output_id() const {
    for (const auto& n : nodes) if (n.kind == graph_kind::output) return n.id;
    return 0;
}
graph_analysis::graph_analysis(const material_graph& g, const graph_link* replacement) {
    const int count = (int)g.nodes.size();
    auto fail = [&](const char* reason) { valid = false; error = reason; };
    if (count > material_graph::max_nodes) { fail("Too many graph nodes."); return; }
    indices.reserve(count);
    sources.resize(count, {-1,-1,-1,-1,-1}); channels.resize(count); widths.resize(count,1);
    output_mask.resize(count,0);
    for (int i = 0; i < count; ++i) {
        const auto& n = g.nodes[i];
        if (n.id <= 0 || (int)n.kind < 0 || n.kind >= graph_kind::count || !indices.emplace(n.id,i).second) {
            fail("Invalid graph node."); return;
        }
    }
    auto add_link = [&](const graph_link& l) {
        auto a = indices.find(l.from), b = indices.find(l.to);
        if (a == indices.end() || b == indices.end() || l.from == l.to || l.output < 0
            || l.output >= graph_outputs(g.nodes[a->second].kind) || l.input < 0
            || l.input >= graph_inputs(g.nodes[b->second].kind) || sources[b->second][l.input] >= 0) {
            fail("Connect an output to a different node's input."); return;
        }
        sources[b->second][l.input] = a->second; channels[b->second][l.input] = l.output;
        output_mask[a->second] |= 1 << l.output;
    };
    for (const auto& l : g.links) {
        if (replacement && l.to == replacement->to && l.input == replacement->input) continue;
        add_link(l); if (!valid) return;
    }
    if (replacement) { add_link(*replacement); if (!valid) return; }
    std::vector<std::vector<int>> users(count);
    std::vector<int> degree(count);
    for (int i = 0; i < count; ++i) for (int p = 0; p < graph_inputs(g.nodes[i].kind); ++p)
        if (sources[i][p] >= 0) { ++degree[i]; users[sources[i][p]].push_back(i); }
    order.reserve(count);
    for (int i = 0; i < count; ++i) if (!degree[i]) order.push_back(i);
    for (size_t at = 0; at < order.size(); ++at) {
        int i = order[at]; auto kind = g.nodes[i].kind;
        if (kind == graph_kind::texture_sample || kind == graph_kind::vector) widths[i] = 3;
        else if (kind == graph_kind::texcoord) widths[i] = 2;
        else for (int p = 0; p < graph_inputs(kind); ++p)
            if (sources[i][p] >= 0 && channels[i][p] == 0) widths[i] = std::max(widths[i],widths[sources[i][p]]);
        for (int target : users[i]) if (--degree[target] == 0) order.push_back(target);
    }
    if ((int)order.size() != count) { fail("This connection would create a cycle."); return; }
    for (int i = 0; i < count; ++i) for (int p = 0; p < graph_inputs(g.nodes[i].kind); ++p) {
        int source = sources[i][p]; if (source < 0) continue;
        int w = channels[i][p] ? 1 : widths[source]; auto kind = g.nodes[i].kind;
        bool compatible = true;
        if (kind == graph_kind::output) {
            int expected = (p == 0 || p == 3) ? 3 : 1;
            compatible = w == expected || (w == 1 && p != 3);
        } else if (kind == graph_kind::texture_sample) compatible = w == 2;
        else if (kind == graph_kind::lerp && p == 2) compatible = w == 1;
        if (!compatible) { fail("Pin types do not match. Use R / G / B / A for scalar inputs, or UV for coordinates."); return; }
    }
}
int graph_analysis::width(int id, int pin) const {
    auto it = indices.find(id); return it == indices.end() ? 0 : pin ? 1 : widths[it->second];
}
std::vector<bool> graph_analysis::reachable(const material_graph& g, int output_slots) const {
    std::vector<bool> used(g.nodes.size(),false);
    auto out = indices.find(g.output_id()); if (!valid || out == indices.end()) return used;
    for (int p = 0; p < std::min(output_slots,5); ++p) if (sources[out->second][p] >= 0) used[sources[out->second][p]] = true;
    for (auto it = order.rbegin(); it != order.rend(); ++it) if (used[*it])
        for (int source : sources[*it]) if (source >= 0) used[source] = true;
    return used;
}
namespace {
void hash_word(uint64_t& key, uint64_t value) { key ^= value; key *= 1099511628211ull; }
void hash_double(uint64_t& key, double value) { uint64_t bits; std::memcpy(&bits,&value,sizeof(bits)); hash_word(key,bits); }
void hash_node(uint64_t& key, const graph_node& n) {
    hash_word(key,n.id); hash_word(key,(int)n.kind); hash_word(key,n.texture_index); hash_word(key,(int)n.space);
    for (unsigned char c : n.texture_path) hash_word(key,c);
    for (int c = 0; c < 3; ++c) { hash_double(key,n.value[c]); hash_double(key,n.defaults[c]); }
}
}
uint64_t graph_topology_key(const material_graph& g) {
    uint64_t key = 1469598103934665603ull;
    for (const auto& n : g.nodes) { hash_word(key,n.id); hash_word(key,(int)n.kind); }
    for (const auto& l : g.links) { hash_word(key,l.from); hash_word(key,l.output); hash_word(key,l.to); hash_word(key,l.input); }
    return key;
}
uint64_t graph_surface_key(const material_graph& g, int output_slots) {
    graph_analysis analysis(g); auto used = analysis.reachable(g,output_slots);
    uint64_t key = 1469598103934665603ull;
    for (int i : analysis.order) if (used[i]) {
        hash_node(key,g.nodes[i]);
        for (int p = 0; p < graph_inputs(g.nodes[i].kind); ++p) {
            int source = analysis.sources[i][p]; hash_word(key,source < 0 ? 0 : g.nodes[source].id);
            hash_word(key,analysis.channels[i][p]);
        }
    }
    auto out = analysis.indices.find(g.output_id());
    if (out != analysis.indices.end()) for (int p = 0; p < std::min(output_slots,5); ++p) {
        int source = analysis.sources[out->second][p]; hash_word(key,source < 0 ? 0 : g.nodes[source].id);
        hash_word(key,analysis.channels[out->second][p]);
    }
    return key;
}
bool graph_equal(const material_graph& a, const material_graph& b) {
    if (a.next_id != b.next_id || a.pan_x != b.pan_x || a.pan_y != b.pan_y || a.zoom != b.zoom
        || a.nodes.size() != b.nodes.size() || a.links.size() != b.links.size()) return false;
    for (size_t i = 0; i < a.nodes.size(); ++i) {
        const auto& x = a.nodes[i]; const auto& y = b.nodes[i];
        if (x.id != y.id || x.kind != y.kind || x.x != y.x || x.y != y.y || x.texture_index != y.texture_index
            || x.texture_path != y.texture_path || x.space != y.space || x.defaults != y.defaults) return false;
        for (int c = 0; c < 3; ++c) if (x.value[c] != y.value[c]) return false;
    }
    for (size_t i = 0; i < a.links.size(); ++i) {
        const auto& x = a.links[i]; const auto& y = b.links[i];
        if (x.from != y.from || x.output != y.output || x.to != y.to || x.input != y.input) return false;
    }
    return true;
}
int material_graph::width(int id, int pin) const { return graph_analysis(*this).width(id,pin); }
bool material_graph::can_connect(graph_link l, std::string* reason) const {
    graph_analysis analysis(*this,&l);
    if (!analysis.valid && reason) *reason = analysis.error;
    return analysis.valid;
}
bool material_graph::connect(graph_link l, std::string* reason) {
    if (!can_connect(l, reason)) return false;
    disconnect(l.to, l.input, true);
    links.push_back(l); return true;
}
void material_graph::disconnect(int id, int pin, bool input) {
    links.erase(std::remove_if(links.begin(), links.end(), [&](const graph_link& l) {
        return input ? l.to == id && l.input == pin : l.from == id && l.output == pin;
    }), links.end());
}
void material_graph::erase(int id) {
    if (id == output_id()) return;
    links.erase(std::remove_if(links.begin(), links.end(), [=](const graph_link& l) { return l.from == id || l.to == id; }), links.end());
    nodes.erase(std::remove_if(nodes.begin(), nodes.end(), [=](const graph_node& n) { return n.id == id; }), nodes.end());
}
void initialize_material_graph(scene_material& m, const std::vector<scene_texture>& textures) {
    if (!m.graph.nodes.empty()) return;
    auto& g = m.graph;
    int out = g.add(graph_kind::output, 440, 90);
    const int indices[] = {m.albedo_tex, m.metallic_tex, m.roughness_tex, m.normal_tex, m.alpha_tex};
    std::unordered_map<int, int> samples;
    for (int s = 0; s < 5; ++s) {
        int index = indices[s];
        // Preserve the old packed workflow when opening an existing material.
        int packed = m.metallic_tex >= 0 ? m.metallic_tex : m.roughness_tex;
        int channel = (s == 1 || s == 2) ? 1 : s == 4 ? 5 : 0;
        if (m.unreal_pbr && (s == 1 || s == 2) && packed >= 0) {
            if (index < 0 || index == packed) { index = packed; channel = s == 1 ? 3 : 2; }
        }
        if (index < 0 || index >= static_cast<int>(textures.size())) continue;
        int id;
        auto it = samples.find(index);
        if (it != samples.end()) id = it->second;
        else {
            id = g.add(graph_kind::texture_sample, 20, static_cast<float>(samples.size()) * 300);
            auto n = g.find(id); n->texture_index = index; n->texture_path = textures[index].path;
            samples[index] = id;
        }
        g.connect({id, channel, out, s});
    }
}

// A versioned, hex-encoded field keeps paths (including spaces and '|') out of
// the legacy manifest's delimiter grammar. Parse transactionally and bound sizes.
std::string serialize_material_graph(const material_graph& g) {
    std::ostringstream out; out << std::setprecision(17);
    out << "1 " << g.pan_x << ' ' << g.pan_y << ' ' << g.zoom << ' ' << g.nodes.size() << ' ' << g.links.size() << '\n';
    for (const auto& n : g.nodes) {
        out << n.id << ' ' << static_cast<int>(n.kind) << ' ' << n.x << ' ' << n.y << ' '
            << n.value.x() << ' ' << n.value.y() << ' ' << n.value.z() << ' '
            << n.defaults[0] << ' ' << n.defaults[1] << ' ' << n.defaults[2] << ' '
            << n.texture_index << ' ' << static_cast<int>(n.space) << ' ' << std::quoted(n.texture_path) << '\n';
    }
    for (const auto& l : g.links) out << l.from << ' ' << l.output << ' ' << l.to << ' ' << l.input << '\n';
    const char* hex = "0123456789abcdef";
    std::string result;
    for (unsigned char c : out.str()) { result += hex[c >> 4]; result += hex[c & 15]; }
    return result;
}
bool deserialize_material_graph(const std::string& text, material_graph& graph) {
    if (text.size() > 4 * 1024 * 1024 || text.size() % 2) return false;
    std::string decoded;
    auto digit = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
    for (size_t i = 0; i < text.size(); i += 2) {
        int a = digit(text[i]), b = digit(text[i+1]);
        if (a < 0 || b < 0) return false;
        decoded += static_cast<char>(a * 16 + b);
    }
    std::istringstream in(decoded);
    material_graph g; int version, count, links;
    if (!(in >> version >> g.pan_x >> g.pan_y >> g.zoom >> count >> links) || version != 1
        || count < 0 || count > material_graph::max_nodes || links < 0 || links > count * 5
        || !std::isfinite(g.pan_x) || !std::isfinite(g.pan_y) || !std::isfinite(g.zoom)) return false;
    g.zoom = std::clamp(g.zoom, 0.35f, 1.65f);
    int outputs = 0;
    for (int i = 0; i < count; ++i) {
        graph_node n; int kind, space;
        if (!(in >> n.id >> kind >> n.x >> n.y >> n.value[0] >> n.value[1] >> n.value[2]
            >> n.defaults[0] >> n.defaults[1] >> n.defaults[2] >> n.texture_index >> space >> std::quoted(n.texture_path))) return false;
        if (n.id < 1 || n.id > 1000000000 || g.find(n.id) || kind < 0 || kind >= static_cast<int>(graph_kind::count)
            || space < 0 || space > 2 || !std::isfinite(n.x) || !std::isfinite(n.y)) return false;
        for (int c = 0; c < 3; ++c) if (!std::isfinite(n.value[c]) || !std::isfinite(n.defaults[c])) return false;
        n.kind = static_cast<graph_kind>(kind); n.space = static_cast<graph_space>(space);
        if (n.kind == graph_kind::output) ++outputs;
        g.next_id = std::max(g.next_id, n.id + 1); g.nodes.push_back(n);
    }
    if (count && outputs != 1) return false;
    for (int i = 0; i < links; ++i) {
        graph_link l;
        if (!(in >> l.from >> l.output >> l.to >> l.input) || g.incoming(l.to, l.input)) return false;
        g.links.push_back(l);
    }
    if (!graph_analysis(g).valid) return false;
    in >> std::ws;
    if (!in.eof()) return false;
    graph = std::move(g); return true;
}

namespace {
struct graph_value { vec3 rgb; vec3 raw; double alpha = 1, mask = 1; };
struct instruction {
    graph_node node;
    std::array<int, 3> input = {-1, -1, -1};
    std::array<int, 3> channel = {0, 0, 0};
    std::shared_ptr<texture> color, data;
};
graph_value channel_value(graph_value v, int c) {
    if (!c) return v;
    double scalar = c <= 3 ? v.raw[c-1] : c == 4 ? v.alpha : v.mask;
    v.rgb = v.raw = vec3(scalar, scalar, scalar); v.alpha = 1; return v;
}
class evaluated_graph_texture final : public texture {
public:
    std::vector<instruction> code;
    int root = 0, channel = 0;
    graph_value evaluate(double u, double v, const point3& p) const {
        // POD scratch is intentionally uninitialized. Only emitted instructions
        // are written/read, so a one-node graph doesn't clear 256 values per ray.
        std::array<std::array<double, 8>, material_graph::max_nodes> values;
        auto read = [&](int i) {
            const auto& s = values[i];
            return graph_value{vec3(s[0],s[1],s[2]),vec3(s[3],s[4],s[5]),s[6],s[7]};
        };
        for (size_t i = 0; i < code.size(); ++i) {
            const auto& op = code[i]; graph_value result;
            auto write = [&]() { values[i] = {result.rgb.x(),result.rgb.y(),result.rgb.z(),
                result.raw.x(),result.raw.y(),result.raw.z(),result.alpha,result.mask}; };
            std::array<graph_value, 3> args;
            for (int a = 0; a < 3; ++a) {
                double d = op.node.defaults[a]; args[a].rgb = args[a].raw = vec3(d, d, d);
                if (op.input[a] >= 0) args[a] = channel_value(read(op.input[a]), op.channel[a]);
            }
            auto kind = op.node.kind;
            if (kind == graph_kind::texture_sample) {
                double tu = u, tv = v;
                if (op.input[0] >= 0) { tu = args[0].rgb.x(); tv = args[0].rgb.y(); tu -= std::floor(tu); tv -= std::floor(tv); }
                result.rgb = op.color ? op.color->value(tu, tv, p) : vec3(1, 0, 1);
                result.raw = op.data && op.data != op.color ? op.data->value(tu, tv, p) : result.rgb;
                result.alpha = op.data ? op.data->alpha_at(tu, tv, p) : 1;
                result.mask = op.data ? op.data->mask_alpha_at(tu, tv, p) : 1;
                write();
                continue;
            }
            if (kind == graph_kind::scalar) result.rgb = vec3(op.node.value.x(), op.node.value.x(), op.node.value.x());
            else if (kind == graph_kind::vector) result.rgb = op.node.value;
            else if (kind == graph_kind::texcoord) result.rgb = vec3(u * op.node.value.x(), v * op.node.value.y(), 0);
            else {
                result.alpha = args[0].alpha;
                if (kind == graph_kind::multiply) result.alpha *= args[1].alpha;
                if (kind == graph_kind::lerp) result.alpha = args[0].alpha * (1 - args[2].rgb.x()) + args[1].alpha * args[2].rgb.x();
                for (int c = 0; c < 3; ++c) {
                    double a = args[0].rgb[c], b = args[1].rgb[c], t = args[2].rgb[c], r = a;
                    switch (kind) {
                        case graph_kind::add: r = a + b; break;
                        case graph_kind::subtract: r = a - b; break;
                        case graph_kind::multiply: r = a * b; break;
                        case graph_kind::divide: r = std::abs(b) < 1e-8 ? 0 : a / b; break;
                        case graph_kind::lerp: r = a * (1 - t) + b * t; break;
                        case graph_kind::one_minus: r = 1 - a; break;
                        case graph_kind::saturate: r = std::clamp(a, 0.0, 1.0); break;
                        case graph_kind::power: r = std::pow(std::max(0.0, a), b); break;
                        default: break;
                    }
                    result.rgb[c] = std::isfinite(r) ? r : 0;
                }
            }
            result.raw = result.rgb; result.mask = result.rgb.x();
            write();
        }
        return channel_value(read(root), channel);
    }
    colour value(double u, double v, const point3& p) const override { return evaluate(u, v, p).rgb; }
    double alpha_at(double u, double v, const point3& p) const override { return std::clamp(evaluate(u, v, p).alpha, 0.0, 1.0); }
    double mask_alpha_at(double u, double v, const point3& p) const override { return std::clamp(evaluate(u, v, p).rgb.x(), 0.0, 1.0); }
};
class uniform_graph_texture final : public texture {
    graph_value sample;
public:
    explicit uniform_graph_texture(graph_value value) : sample(value) {}
    colour value(double,double,const point3&) const override { return sample.rgb; }
    double alpha_at(double,double,const point3&) const override { return std::clamp(sample.alpha,0.0,1.0); }
    double mask_alpha_at(double,double,const point3&) const override { return std::clamp(sample.rgb.x(),0.0,1.0); }
};
}
std::shared_ptr<texture> compile_material_graph(const material_graph& g, int slot,
    const graph_texture_loader& loader, std::string* error) {
    const auto* root = g.incoming(g.output_id(), slot);
    if (!root) return nullptr;
    graph_analysis analysis(g);
    if (!analysis.valid) { if (error) *error = analysis.error; return nullptr; }
    auto compiled = std::make_shared<evaluated_graph_texture>();
    std::unordered_map<int, int> indices;
    std::function<int(int)> emit = [&](int id) {
        auto found = indices.find(id); if (found != indices.end()) return found->second;
        instruction op; op.node = *g.find(id);
        for (int p = 0; p < graph_inputs(op.node.kind); ++p) if (auto l = g.incoming(id, p)) {
            op.input[p] = emit(l->from); op.channel[p] = l->output;
        }
        if (op.node.kind == graph_kind::texture_sample) {
            bool srgb = op.node.space == graph_space::srgb || (op.node.space == graph_space::automatic && slot == 0);
            op.color = loader(op.node, srgb);
            op.data = op.node.space == graph_space::srgb ? op.color : loader(op.node, false);
        }
        int index = static_cast<int>(compiled->code.size());
        indices[id] = index; compiled->code.push_back(std::move(op)); return index;
    };
    compiled->root = emit(root->from); compiled->channel = root->output;
    bool uniform = std::none_of(compiled->code.begin(),compiled->code.end(),[](const instruction& op) {
        return op.node.kind == graph_kind::texture_sample || op.node.kind == graph_kind::texcoord;
    });
    if (uniform) return std::make_shared<uniform_graph_texture>(compiled->evaluate(0,0,{}));
    return compiled;
}
