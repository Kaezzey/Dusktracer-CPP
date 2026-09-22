#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>
#include <cstring>
#include <vector>
#include <filesystem>
#include <fstream>

namespace dusk_gpu::pipeline_cache {
constexpr size_t max_bytes = 64 * 1024 * 1024;
inline uint64_t fingerprint(const void* data, size_t bytes) {
    uint64_t hash = 14695981039346656037ull;
    const auto* values = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < bytes; ++i) {
        hash ^= values[i];
        hash *= 1099511628211ull;
    }
    return hash;
}
inline std::filesystem::path path(const VkPhysicalDeviceProperties& device) {
    return std::filesystem::path(".dusk-cache/vulkan") /
           (std::to_string(device.vendorID) + "-" + std::to_string(device.deviceID) + "-" +
            std::to_string(device.driverVersion) + ".bin");
}
inline std::vector<uint8_t> read(const VkPhysicalDeviceProperties& device, uint64_t shader) {
    std::ifstream input(path(device), std::ios::binary);
    uint64_t envelope[4]{};
    input.read(reinterpret_cast<char*>(envelope), sizeof(envelope));
    if (!input || envelope[0] != 0x4455534b564b4301ull || envelope[1] != shader || envelope[2] < 32 ||
        envelope[2] > max_bytes) {
        return {};
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(envelope[2]));
    input.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
    if (!input || fingerprint(bytes.data(), bytes.size()) != envelope[3]) {
        return {};
    }
    // Windows is little endian, as required by the Vulkan cache header format.
    VkPipelineCacheHeaderVersionOne header{};
    static_assert(sizeof(header) == 32);
    std::memcpy(&header, bytes.data(), sizeof(header));
    if (header.headerSize != 32 || header.headerVersion != VK_PIPELINE_CACHE_HEADER_VERSION_ONE ||
        header.vendorID != device.vendorID || header.deviceID != device.deviceID ||
        std::memcmp(header.pipelineCacheUUID, device.pipelineCacheUUID, VK_UUID_SIZE) != 0) {
        return {};
    }
    return bytes;
}
inline void write(const VkPhysicalDeviceProperties& device, uint64_t shader,
                  const std::vector<uint8_t>& bytes) {
    // A failed or interrupted write is harmless: size/checksum validation makes
    // the next run rebuild the cache. Cache I/O never decides render success.
    const auto destination = path(device);
    std::error_code error;
    std::filesystem::create_directories(destination.parent_path(), error);
    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    const uint64_t envelope[] = {0x4455534b564b4301ull, shader, bytes.size(),
                                 fingerprint(bytes.data(), bytes.size())};
    output.write(reinterpret_cast<const char*>(envelope), sizeof(envelope));
    output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}
} // namespace dusk_gpu::pipeline_cache
