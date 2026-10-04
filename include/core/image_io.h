#ifndef DUSK_IMAGE_IO_H
#define DUSK_IMAGE_IO_H

#include <string>
#include <fstream>
#include <filesystem>
#include "renderer.h"   // for render_result

inline bool write_ppm(const std::string& filename, const render_result& img)
{
    if (img.width <= 0 || img.height <= 0 || img.pixels.empty())
        return false;

    std::ofstream out(std::filesystem::u8path(filename));
    if (!out.is_open())
        return false;

    out << "P3\n" << img.width << ' ' << img.height << "\n255\n";

    const int W = img.width;
    const int H = img.height;

    for (int j = 0; j < H; ++j) {
        for (int i = 0; i < W; ++i) {
            int idx = 3 * (j * W + i);
            out << int(img.pixels[idx+0]) << ' '
                << int(img.pixels[idx+1]) << ' '
                << int(img.pixels[idx+2]) << '\n';
        }
    }

    out.flush();
    return bool(out);
}

#endif // DUSK_IMAGE_IO_H
