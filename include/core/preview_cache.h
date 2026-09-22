#pragma once

#include "preview_worker.h"
#include "external/stb_image.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>

namespace preview_cache {
// A versioned, disposable disk cache. Each asset has one file per resolution;
// edits replace that file instead of leaving a thumbnail for every revision.
inline uint64_t hash(const std::string& text) {
    uint64_t value = 14695981039346656037ull;
    for (unsigned char c : text) {
        value ^= c;
        value *= 1099511628211ull;
    }
    return value;
}
inline std::string file_stamp(const std::string& path) {
    std::error_code error;
    const auto absolute = std::filesystem::absolute(path, error).lexically_normal();
    const auto size = std::filesystem::file_size(absolute, error);
    const auto modified = std::filesystem::last_write_time(absolute, error).time_since_epoch().count();
    return absolute.string() + "|" + std::to_string(size) + "|" + std::to_string(modified);
}
struct cached_image {
    preview_image image;
    int source_channels = 4;
};
inline std::mutex disk_mutex;
inline std::filesystem::path cache_path(const std::string& identity) {
    return std::filesystem::path(".dusk-cache/previews") / (std::to_string(hash(identity)) + ".bin");
}
inline cached_image read(const std::string& identity, uint64_t stamp) {
    std::lock_guard<std::mutex> lock(disk_mutex);
    std::ifstream in(cache_path(identity), std::ios::binary);
    uint64_t header[5]{};
    in.read(reinterpret_cast<char*>(header), sizeof(header));
    if (!in || header[0] != 0x4455534b50525602ull || header[1] != stamp || header[2] < 1 ||
        header[2] > 2048 || header[3] < 1 || header[3] > 2048 || header[4] < 1 || header[4] > 4) {
        return {};
    }
    cached_image result{{int(header[2]), int(header[3]), {}}, int(header[4])};
    result.image.pixels.resize(header[2] * header[3] * 4);
    in.read(reinterpret_cast<char*>(result.image.pixels.data()), result.image.pixels.size());
    return in ? result : cached_image{};
}
inline void write(const std::string& identity, uint64_t stamp, const cached_image& data) {
    if (data.image.pixels.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(disk_mutex);
    auto path = cache_path(identity);
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    const uint64_t header[] = {0x4455534b50525602ull, stamp, uint64_t(data.image.width),
                               uint64_t(data.image.height), uint64_t(data.source_channels)};
    out.write(reinterpret_cast<const char*>(header), sizeof(header));
    out.write(reinterpret_cast<const char*>(data.image.pixels.data()), data.image.pixels.size());
}
inline preview_image resize(const unsigned char* source, int width, int height, int limit) {
    const double scale = std::min(1.0, double(limit) / std::max(width, height));
    preview_image result{std::max(1, int(width * scale)), std::max(1, int(height * scale)), {}};
    result.pixels.resize(size_t(result.width) * result.height * 4);
    // Box filtering removes high-frequency aliasing in both thumbnails and
    // sphere previews; all channels remain encoded as in the original image.
    for (int y = 0; y < result.height; ++y) {
        for (int x = 0; x < result.width; ++x) {
            int x0 = x * width / result.width, x1 = (x + 1) * width / result.width;
            int y0 = y * height / result.height, y1 = (y + 1) * height / result.height;
            uint64_t sum[4]{};
            for (int sy = y0; sy < y1; ++sy) {
                for (int sx = x0; sx < x1; ++sx) {
                    for (int c = 0; c < 4; ++c) {
                        sum[c] += source[(size_t(sy) * width + sx) * 4 + c];
                    }
                }
            }
            for (int c = 0; c < 4; ++c) {
                result.pixels[(size_t(y) * result.width + x) * 4 + c] =
                    static_cast<unsigned char>(sum[c] / ((x1 - x0) * (y1 - y0)));
            }
        }
    }
    return result;
}
inline cached_image texture(const std::string& path, bool* cache_hit = nullptr, int limit = 1024) {
    limit = std::clamp(limit, 1, 2048);
    const auto identity = "texture" + std::to_string(limit) + ":" + std::filesystem::absolute(path).lexically_normal().string();
    const auto stamp = hash(file_stamp(path));
    auto result = read(identity, stamp);
    if (cache_hit) {
        *cache_hit = !result.image.pixels.empty();
    }
    if (!result.image.pixels.empty()) {
        return result;
    }
    int width = 0, height = 0, channels = 0;
    std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> pixels(
        stbi_load(path.c_str(), &width, &height, &channels, 4), stbi_image_free);
    if (!pixels || width <= 0 || height <= 0) {
        return {};
    }
    result = {resize(pixels.get(), width, height, limit), channels};
    write(identity, stamp, result);
    return result;
}
} // namespace preview_cache
