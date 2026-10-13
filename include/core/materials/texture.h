#ifndef TEXTURE_H
#define TEXTURE_H

#include "dusk_image.h"
#include "perlin.h"
#include "dusk_image.h"
#include <algorithm>
#include <memory>
#include <cmath>

class texture {
  public:
    virtual ~texture() = default;
    virtual colour value(double u, double v, const point3& p) const = 0;
    // Alpha sampling: return alpha in [0,1]. Default = 1 (opaque)
    virtual double alpha_at(double u, double v, const point3& p) const { return 1.0; }
    // Mask-aware alpha sampling: when a texture is explicitly used as an
    // opacity mask, `mask_alpha_at` should be used so implementations can
    // interpret RGB images as luminance masks if desired. Default forwards
    // to `alpha_at` for backwards compatibility.
    virtual double mask_alpha_at(double u, double v, const point3& p) const { return alpha_at(u,v,p); }
};

enum class texture_sample_space {
    srgb_color,
    linear_data
};

class solid_colour : public texture {
  public:
    solid_colour(const colour& albedo) : albedo(albedo) {}

    solid_colour(double red, double green, double blue) : solid_colour(colour(red,green,blue)) {}

    colour value(double u, double v, const point3& p) const override {
        return albedo;
    }
    double mask_alpha_at(double, double, const point3&) const override {
        return std::clamp(albedo.x(),0.0,1.0);
    }

  private:
    colour albedo;
};

class checker_texture : public texture {
  public:
    checker_texture(double scale, shared_ptr<texture> even, shared_ptr<texture> odd)
      : inv_scale(1.0 / scale), even(even), odd(odd) {}

    checker_texture(double scale, const colour& c1, const colour& c2)
      : checker_texture(scale, make_shared<solid_colour>(c1), make_shared<solid_colour>(c2)) {}

    colour value(double u, double v, const point3& p) const override {
        auto xInteger = int(std::floor(inv_scale * p.x()));
        auto yInteger = int(std::floor(inv_scale * p.y()));
        auto zInteger = int(std::floor(inv_scale * p.z()));

        bool isEven = (xInteger + yInteger + zInteger) % 2 == 0;

        return isEven ? even->value(u, v, p) : odd->value(u, v, p);
    }

  private:
    double inv_scale;
    shared_ptr<texture> even;
    shared_ptr<texture> odd;
};

class noise_texture : public texture {
  public:
    noise_texture(double scale) : scale(scale) {}

    colour value(double u, double v, const point3& p) const override {
        return colour(.5, .5, .5) * (1 + std::sin(scale * p.z() + 10 * noise.turb(p, 7)));
    }

  private:
    perlin noise;
    double scale;
};

class image_texture : public texture {
  public:
    image_texture(const char* filename, texture_sample_space sample_space = texture_sample_space::srgb_color)
        : image(std::make_shared<rtw_image>(filename)), sample_space(sample_space) {}
    image_texture(std::shared_ptr<rtw_image> decoded, texture_sample_space space)
        : image(std::move(decoded)), sample_space(space) {}

    // Expose whether the loaded image contains an alpha channel.
    bool has_alpha() const { return image->has_alpha(); }

    colour value(double u, double v, const point3& p) const override {
        //no texture data, then return solid cyan as a debugging aid
        if (image->height() <= 0) return colour(0,1,1);

        int ch = image->channels();
        if (ch <= 0) {
            return colour(0,1,1);
        }
        // Texels are decoded before filtering, so sRGB blends in linear light.
        return filter(u, v, [&](int i, int j) {
            double r, g, b;
            if (ch == 1 || ch == 2) {
                r = g = b = image->channel_value(i, j, 0);
            } else {
                r = image->channel_value(i, j, 0);
                g = image->channel_value(i, j, 1);
                b = image->channel_value(i, j, 2);
            }
            if (sample_space == texture_sample_space::srgb_color && !image->is_hdr()) {
                r = srgb_to_linear(r);
                g = srgb_to_linear(g);
                b = srgb_to_linear(b);
            }
            return colour(r, g, b);
        });
    }

      double alpha_at(double u, double v, const point3& p) const override {
        if (image->height() <= 0) return 1.0;

        // If the image has an explicit alpha channel, use it.
        if (image->has_alpha()) {
          const int channel = image->channels() == 2 ? 1 : 3;
          return std::clamp(filter(u, v, [&](int i, int j) { return image->channel_value(i, j, channel); }), 0.0, 1.0);
        }

        // No explicit alpha channel: only treat true mask images as alpha.
        // Behaviour:
        //  - 1-channel image: use that channel as alpha (common for masks)
        //  - 2-channel image: treat second channel as alpha (grey+alpha)
        //  - 3-channel (RGB): do NOT derive alpha from luminance here because
        //    RGB albedo textures should not be interpreted as opacity masks.
        //    Return fully opaque so PBR fallback doesn't accidentally mask geometry.
        int ch = image->channels();
        if (ch <= 0) return 1.0;
        if (ch == 1 || ch == 2) {
          // A grey mask, or grey + alpha whose second channel is alpha.
          const int channel = ch - 1;
          return std::clamp(filter(u, v, [&](int i, int j) { return image->channel_value(i, j, channel); }), 0.0, 1.0);
        } else {
          // 3 or more channels but no explicit alpha -> treat as opaque
          return 1.0;
        }

      }

      // When this texture is being used specifically as an opacity mask,
      // interpret RGB images as luminance masks so artists can supply RGB
      // mask files. If the image contains an alpha channel, prefer that.
      double mask_alpha_at(double u, double v, const point3& p) const override {
        if (image->height() <= 0) return 1.0;

        int ch = image->channels();
        if (ch <= 0) return 1.0;
        // Image alpha where present, a grey mask, or RGB luminance.
        const int channel = image->has_alpha() ? (ch == 2 ? 1 : 3) : ch <= 2 ? ch - 1 : -1;
        return std::clamp(filter(u, v, [&](int i, int j) {
            if (channel >= 0) return image->channel_value(i, j, channel);
            return 0.2126 * image->channel_value(i, j, 0) + 0.7152 * image->channel_value(i, j, 1) +
                   0.0722 * image->channel_value(i, j, 2);
        }), 0.0, 1.0);
      }

  private:
    static double srgb_to_linear(double c) {
        c = std::clamp(c, 0.0, 1.0);
        if (c <= 0.04045) return c / 12.92;
        return std::pow((c + 0.055) / 1.055, 2.4);
    }

    // Bilinear filtering between texel centres, clamped to the edge texels,
    // as the Vulkan tracer and the raster viewport sample. Without it, a
    // texture magnified across many pixels shows each texel as a block:
    // alpha-tested hair cards break their strands into jagged chunks.
    template <class F> auto filter(double u, double v, F texel) const {
        const int w = image->width(), h = image->height();
        const double x = interval(0,1).clamp(u) * w - .5, y = (1.0 - interval(0,1).clamp(v)) * h - .5;
        int x0 = int(std::floor(x)), y0 = int(std::floor(y));
        const double fx = x - x0, fy = y - y0;
        const int x1 = std::clamp(x0 + 1, 0, w - 1), y1 = std::clamp(y0 + 1, 0, h - 1);
        x0 = std::clamp(x0, 0, w - 1);
        y0 = std::clamp(y0, 0, h - 1);
        return (texel(x0, y0) * (1 - fx) + texel(x1, y0) * fx) * (1 - fy) +
               (texel(x0, y1) * (1 - fx) + texel(x1, y1) * fx) * fy;
    }

    std::shared_ptr<rtw_image> image;
    texture_sample_space sample_space = texture_sample_space::srgb_color;
};

#endif
