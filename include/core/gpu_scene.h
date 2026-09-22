#pragma once
#include "scene.h"
#include <array>
#include <cstdint>
#include <atomic>

// This immutable upload format contains no Vulkan objects or editor pointers.
// All records use explicit 16-byte lanes, mirrored by shaders/pathtrace.comp.
namespace dusk_gpu {
struct f4 {
    float x = 0, y = 0, z = 0, w = 0;
    float operator[](int index) const {
        return index == 0 ? x : index == 1 ? y : index == 2 ? z : w;
    }
};
struct i4 {
    int32_t x = 0, y = 0, z = 0, w = 0;
    int32_t& operator[](int index) {
        return index == 0 ? x : index == 1 ? y : index == 2 ? z : w;
    }
};
struct vertex {
    f4 position, normal, tangent, bitangent, uv;
};
struct geometry {
    i4 data;
}; // first vertex, first slot, primitive count, sphere
struct instance {
    f4 rows[3];
    i4 data;
}; // geometry, first material mapping, first light, light count
// code: node kind, destination register, texture index, color-space flags.
// inputs/channels: up to three source registers and selected output channels.
struct instruction {
    i4 code, inputs, channels;
    f4 value, defaults;
};
struct material_record {
    f4 base;        // RGB base color; W material model
    f4 parameters;  // metallic, roughness, normal strength, IOR
    f4 f0;          // RGB dielectric F0; W legacy metal fuzz
    f4 emission;    // RGB emission; W intensity
    f4 alpha;       // cutoff, double-sided masking, reserved, reserved
    i4 programs[5]; // start, count, result register, result channel
};
struct texture_record {
    i4 data;
}; // word offset, width, height, channels | HDR flag
struct area_light {
    i4 data;
    f4 distribution;
}; // instance, primitive; CDF, probability, world area
struct snapshot {
    std::vector<vertex> vertices;
    std::vector<uint32_t> slots, material_map, texels;
    std::vector<geometry> geometries;
    std::vector<instance> instances;
    std::vector<material_record> materials;
    std::vector<instruction> instructions;
    std::vector<texture_record> textures;
    std::vector<area_light> lights;
};
constexpr int graph_registers = 32;
// Shared, bounded material bytecode. The caller owns texture loading and IDs.
material_record compile_material(const scene& source, const scene_material& material,
                                 const std::function<int(const graph_node&)>& texture_id,
                                 std::vector<instruction>& instructions,
                                 const std::function<int(int)>& texture_channels = {});
snapshot compile_scene(const scene& source, const std::atomic<bool>* cancel = nullptr);
static_assert(sizeof(vertex) == 80 && sizeof(instance) == 64 && sizeof(instruction) == 80 &&
              sizeof(material_record) == 160);
} // namespace dusk_gpu
