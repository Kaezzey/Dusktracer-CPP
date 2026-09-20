#pragma once

#include "vec3.h"
#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <cstdint>
#include <unordered_map>

class texture;
struct scene_material;
struct scene_texture;

// Stable IDs belong to the material, independently of texture asset indices.
enum class graph_kind { output, texture_sample, scalar, vector, texcoord,
    add, subtract, multiply, divide, lerp, one_minus, saturate, power, reroute, count };
enum class graph_space { automatic, srgb, linear };
struct graph_node {
    int id = 0;
    graph_kind kind = graph_kind::scalar;
    float x = 0, y = 0;
    vec3 value = vec3(1, 1, 1);
    std::array<double, 3> defaults = {0, 1, 0.5};
    int texture_index = -1;
    std::string texture_path;
    graph_space space = graph_space::automatic;
};
struct graph_link {
    int from = 0, output = 0, to = 0, input = 0;
};
struct material_graph {
    static constexpr int max_nodes = 256;
    int next_id = 1;
    float pan_x = 40, pan_y = 60, zoom = 1;
    std::vector<graph_node> nodes;
    std::vector<graph_link> links;

    graph_node* find(int id);
    const graph_node* find(int id) const;
    const graph_link* incoming(int id, int pin) const;
    int add(graph_kind kind, float x, float y);
    int output_id() const;
    int width(int id, int pin = 0) const;
    bool connect(graph_link link, std::string* reason = nullptr);
    bool can_connect(graph_link link, std::string* reason = nullptr) const;
    void disconnect(int id, int pin, bool input);
    void erase(int id);
};

// Reusable linear-time topology analysis. Contains no editor positions, values,
// texture paths, or copies of nodes. A replacement is validated transactionally.
struct graph_analysis {
    bool valid = true;
    const char* error = "";
    std::unordered_map<int,int> indices;
    std::vector<std::array<int,5>> sources, channels;
    std::vector<int> widths, order;
    std::vector<unsigned char> output_mask;
    explicit graph_analysis(const material_graph& graph, const graph_link* replacement = nullptr);
    int width(int id, int pin = 0) const;
    std::vector<bool> reachable(const material_graph& graph, int output_slots = 5) const;
};
uint64_t graph_topology_key(const material_graph& graph);
uint64_t graph_surface_key(const material_graph& graph, int output_slots = 5);
bool graph_equal(const material_graph& a, const material_graph& b);

const char* graph_name(graph_kind kind);
const char* graph_category(graph_kind kind);
int graph_inputs(graph_kind kind);
int graph_outputs(graph_kind kind);
const char* graph_input_name(graph_kind kind, int pin);
const char* graph_output_name(graph_kind kind, int pin);
void initialize_material_graph(scene_material& material, const std::vector<scene_texture>& textures);
std::string serialize_material_graph(const material_graph& graph);
bool deserialize_material_graph(const std::string& data, material_graph& graph);

// Compiles the reachable DAG once, then evaluates without allocations per hit.
// The loader's bool is true for sRGB color sampling, false for linear data.
using graph_texture_loader = std::function<std::shared_ptr<texture>(const graph_node&, bool)>;
std::shared_ptr<texture> compile_material_graph(const material_graph& graph, int slot,
    const graph_texture_loader& loader, std::string* error = nullptr);
