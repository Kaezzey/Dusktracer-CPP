#ifndef DUSK_IMAGE_H
#define DUSK_IMAGE_H

#ifdef _MSC_VER
    #pragma warning (push, 0)
#endif

// Correct: include stb_image.h WITHOUT defining STB_IMAGE_IMPLEMENTATION
#define STBI_FAILURE_USERMSG
#include "../../external/stb_image.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>
#include <stdexcept>

class rtw_image {
public:
    rtw_image() {}
    rtw_image(const rtw_image&) = delete;
    rtw_image& operator=(const rtw_image&) = delete;

    // Owned, reduced pixels for editor previews. Preserve the original channel
    // count so RGB opacity masks and RGBA alpha retain their normal semantics.
    rtw_image(int width, int height, int channels, const std::vector<unsigned char>& rgba)
        : bytes_per_pixel(channels), image_width(width), image_height(height),
          bytes_per_scanline(width * channels) {
        if (width <= 0 || height <= 0 || channels < 1 || channels > 4 ||
            rgba.size() != size_t(width) * height * 4) {
            throw std::invalid_argument("Invalid preview image dimensions or channels.");
        }
        owned_pixels.resize(size_t(width) * height * channels);
        for (size_t p = 0; p < size_t(width) * height; ++p) {
            for (int c = 0; c < channels; ++c) {
                owned_pixels[p * channels + c] = rgba[p * 4 + (channels == 2 && c == 1 ? 3 : c)];
            }
        }
        bdata = owned_pixels.data();
    }

    explicit rtw_image(const char* image_filename) {
        auto filename = std::string(image_filename);
        auto imagedir = getenv("RTW_IMAGES");
        if (imagedir && load(std::string(imagedir) + "/" + filename)) {
            std::printf("Loaded image from RTW_IMAGES: %s\n", (std::string(imagedir) + "/" + filename).c_str());
            return;
        }

        // Try the exact path first (handles paths like "models/.." or absolute paths)
        if (load(filename)) {
            std::printf("Loaded image: %s\n", filename.c_str());
            return;
        }

        // Common asset folders to try (images, textures, models) with a few parent levels
        const char* search_folders[] = { "images/", "textures/", "models/" };
        const int max_up = 6;

        for (int up = 0; up <= max_up; ++up) {
            std::string prefix;
            for (int k = 0; k < up; ++k) prefix += "../";
            for (const char* folder : search_folders) {
                std::string try_path = prefix + folder + filename;
                if (load(try_path)) {
                    std::printf("Loaded image: %s\n", try_path.c_str());
                    return;
                }
            }
        }
        std::cerr << "ERROR: Could not load image file '" << image_filename << "'.\n";
    }

    // Alpha helpers: support images with an alpha channel (RGBA / GA).
    bool has_alpha() const { return bytes_per_pixel == 2 || bytes_per_pixel >= 4; }
    bool is_hdr() const { return hdr; }
    unsigned char pixel_alpha_byte(int x, int y) const {
        if (!has_alpha()) return 255;
        double a = channel_value(x, y, alpha_channel_index());
        a = std::clamp(a, 0.0, 1.0);
        return static_cast<unsigned char>(255.0 * a + 0.5);
    }

    ~rtw_image() {
        if (bdata != owned_pixels.data()) stbi_image_free(bdata);
        stbi_image_free(fdata);
    }

    bool load(const std::string& filename) {
        int n = 0;
        if (stbi_is_hdr(filename.c_str())) {
            hdr = true;
            fdata = stbi_loadf(filename.c_str(), &image_width, &image_height, &n, 0);
            if (!fdata) return false;
        } else {
            hdr = false;
            bdata = stbi_load(filename.c_str(), &image_width, &image_height, &n, 0);
            if (!bdata) return false;
        }

        bytes_per_pixel = (n > 0) ? n : 3;
        bytes_per_scanline = image_width * bytes_per_pixel;
        return true;
    }

    int width()  const { return image_width; }
    int height() const { return image_height; }

    // Number of channels per pixel (1 = grey, 2 = grey+alpha, 3 = RGB, 4 = RGBA)
    int channels() const { return bytes_per_pixel; }

    const unsigned char* pixel_data(int x, int y) const {
        static unsigned char magenta[] = { 255, 0, 255 };
        if (!bdata || hdr) return magenta;

        x = clamp(x, 0, image_width);
        y = clamp(y, 0, image_height);

        return bdata + y * bytes_per_scanline + x * bytes_per_pixel;
    }

    double channel_value(int x, int y, int channel) const {
        if (image_width <= 0 || image_height <= 0 || bytes_per_pixel <= 0) return 0.0;

        x = clamp(x, 0, image_width);
        y = clamp(y, 0, image_height);
        channel = std::clamp(channel, 0, bytes_per_pixel - 1);

        size_t offset = (static_cast<size_t>(y) * static_cast<size_t>(image_width) + static_cast<size_t>(x))
                      * static_cast<size_t>(bytes_per_pixel)
                      + static_cast<size_t>(channel);

        if (hdr && fdata) {
            return static_cast<double>(fdata[offset]);
        }
        if (bdata) {
            return static_cast<double>(bdata[offset]) / 255.0;
        }
        return 0.0;
    }

private:
    std::vector<unsigned char> owned_pixels;
    int            bytes_per_pixel = 3;
    bool           hdr = false;
    float         *fdata = nullptr;
    unsigned char *bdata = nullptr;
    int            image_width = 0;
    int            image_height = 0;
    int            bytes_per_scanline = 0;

    static int clamp(int x, int low, int high) {
        if (x < low) return low;
        if (x < high) return x;
        return high - 1;
    }

    int alpha_channel_index() const {
        if (bytes_per_pixel == 2) return 1;
        if (bytes_per_pixel >= 4) return 3;
        return 0;
    }
};

#ifdef _MSC_VER
    #pragma warning (pop)
#endif

#endif // DUSK_IMAGE_H
