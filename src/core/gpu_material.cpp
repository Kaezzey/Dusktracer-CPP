#include "core/gpu_scene.h"
#include <cmath>
#include <stdexcept>

namespace dusk_gpu {
namespace {
f4 pack(const vec3& v, float w = 0) {
    for (int c = 0; c < 3; ++c) {
        if (!std::isfinite(v[c]) || std::abs(v[c]) > 1e20) {
            throw std::runtime_error("GPU scene contains a non-finite or out-of-range value.");
        }
    }
    return {float(v.x()), float(v.y()), float(v.z()), w};
}
struct material_compiler {
    const scene& source;
    const std::function<int(const graph_node&)>& texture_id;
    std::vector<instruction>& instructions;
    const std::function<int(int)>& texture_channels;
    i4 program(const material_graph& graph, int slot) {
        auto root = graph.incoming(graph.output_id(), slot);
        if (!root) {
            return {};
        }
        graph_analysis analysis(graph);
        if (!analysis.valid) {
            throw std::runtime_error(std::string("Invalid GPU material graph: ") + analysis.error);
        }
        std::vector<const graph_node*> order;
        std::unordered_map<int, int> indices;
        std::function<void(int)> emit = [&](int id) {
            if (indices.count(id)) {
                return;
            }
            const auto* n = graph.find(id);
            for (int p = 0; p < graph_inputs(n->kind); ++p) {
                if (auto link = graph.incoming(id, p)) {
                    emit(link->from);
                }
            }
            indices[id] = int(order.size());
            order.push_back(n);
        };
        emit(root->from);
        std::vector<int> uses(order.size(), 0), registers(order.size(), -1);
        for (auto n : order) {
            for (int p = 0; p < graph_inputs(n->kind); ++p) {
                if (auto l = graph.incoming(n->id, p)) {
                    ++uses[indices.at(l->from)];
                }
            }
        }
        ++uses[indices.at(root->from)];
        bool busy[graph_registers] = {};
        i4 span{int(instructions.size()), int(order.size()), 0, root->output};
        for (size_t k = 0; k < order.size(); ++k) {
            const auto& n = *order[k];
            instruction op{};
            op.code.x = int(n.kind);
            op.code.z = -1;
            op.inputs = {-1, -1, -1, 0};
            for (int p = 0; p < graph_inputs(n.kind); ++p) {
                if (auto link = graph.incoming(n.id, p)) {
                    int index = indices.at(link->from);
                    op.inputs[p] = registers[index];
                    op.channels[p] = link->output;
                    if (--uses[index] == 0) {
                        busy[registers[index]] = false;
                    }
                }
            }
            int reg = 0;
            while (reg < graph_registers && busy[reg]) {
                ++reg;
            }
            if (reg == graph_registers) {
                throw std::runtime_error("Material graph needs more than 32 simultaneous GPU values.");
            }
            busy[reg] = true;
            registers[k] = reg;
            op.code.y = reg;
            op.value = pack(n.value);
            op.defaults = {float(n.defaults[0]), float(n.defaults[1]), float(n.defaults[2]), 0};
            if (n.kind == graph_kind::texture_sample) {
                op.code.z = texture_id(n);
                bool srgb = n.space == graph_space::srgb || (n.space == graph_space::automatic && slot == 0);
                op.code.w = (srgb ? 1 : 0) | (n.space == graph_space::srgb ? 2 : 0);
            }
            instructions.push_back(op);
        }
        span.z = registers[indices.at(root->from)];
        return span;
    }
    material_record material(const scene_material& m) {
        material_record record{};
        record.base = pack(m.base_color, float(m.model));
        record.parameters = {float(m.metallic), float(m.roughness), float(m.normal_strength), float(m.ior)};
        record.f0 = pack(m.dielectric_F0, float(m.fuzz));
        record.emission =
            pack(m.emission.length_squared() == 0 ? m.base_color : m.emission, float(m.emission_intensity));
        record.alpha = {float(m.alpha_cutoff), m.alpha_double_sided ? 1.f : 0.f, 0, 0};
        material_graph graph = m.graph;
        if (graph.nodes.empty()) {
            int output = graph.add(graph_kind::output, 0, 0);
            auto bind = [&](int slot, int texture, int channel = 0) {
                if (texture < 0 || texture >= int(source.textures.size())) {
                    return;
                }
                int id = graph.add(graph_kind::texture_sample, 0, 0);
                graph.find(id)->texture_index = texture;
                graph.connect({id, channel, output, slot});
            };
            bind(0, m.albedo_tex);
            bind(3, m.normal_tex);
            bind(4, m.alpha_tex, 5);
            if (m.unreal_pbr && m.model == scene_material_model::pbr) {
                auto valid = [&](int index) { return index >= 0 && index < int(source.textures.size()); };
                int packed = valid(m.metallic_tex) ? m.metallic_tex : m.roughness_tex;
                bool separate_roughness =
                    valid(m.roughness_tex) && valid(packed) &&
                    source.textures[m.roughness_tex].path != source.textures[packed].path;
                bind(1, packed, 3);
                bind(2, separate_roughness ? m.roughness_tex : packed, separate_roughness ? 1 : 2);
            } else {
                bind(1, m.metallic_tex);
                bind(2, m.roughness_tex);
            }
        } else {
            record.alpha.y = 1;
        }
        int slots = m.model == scene_material_model::pbr ? 5 : 1;
        for (int s = 0; s < slots; ++s) {
            record.programs[s] = program(graph, s);
        }
        if (record.programs[4].y) {
            record.alpha.y = 1;
        }
        if (m.albedo_tex >= 0 && m.albedo_tex < int(source.textures.size()) && m.graph.nodes.empty()) {
            graph_node n;
            n.texture_index = m.albedo_tex;
            int id = texture_id(n);
            int channels = texture_channels && id >= 0 ? texture_channels(id) : 4;
            if (channels == 2 || channels == 4) {
                record.alpha.y = 1;
            }
        }
        return record;
    }
};
} // namespace
material_record compile_material(const scene& source, const scene_material& material,
                                 const std::function<int(const graph_node&)>& texture_id,
                                 std::vector<instruction>& instructions,
                                 const std::function<int(int)>& texture_channels) {
    return material_compiler{source, texture_id, instructions, texture_channels}.material(material);
}
} // namespace dusk_gpu
