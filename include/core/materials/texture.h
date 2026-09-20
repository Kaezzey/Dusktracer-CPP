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

        int i = 0, j = 0;
        resolve_texel(u, v, i, j);

        double r = 0.0;
        double g = 0.0;
        double b = 0.0;

        int ch = image->channels();
        if (ch <= 0) {
            return colour(0,1,1);
        }
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
    }

      double alpha_at(double u, double v, const point3& p) const override {
        if (image->height() <= 0) return 1.0;

        int i = 0, j = 0;
        resolve_texel(u, v, i, j);

        // If the image has an explicit alpha channel, use it.
        if (image->has_alpha()) {
          return std::clamp(image->channel_value(i, j, image->channels() == 2 ? 1 : 3), 0.0, 1.0);
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
        if (ch == 1) {
          return std::clamp(image->channel_value(i, j, 0), 0.0, 1.0);
        } else if (ch == 2) {
          // gray + alpha (second channel is alpha)
          return std::clamp(image->channel_value(i, j, 1), 0.0, 1.0);
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

        int i = 0, j = 0;
        resolve_texel(u, v, i, j);

        if (image->has_alpha()) {
            return std::clamp(image->channel_value(i, j, image->channels() == 2 ? 1 : 3), 0.0, 1.0);
        }

        int ch = image->channels();
        if (ch <= 0) return 1.0;
        if (ch == 1) return std::clamp(image->channel_value(i, j, 0), 0.0, 1.0);
        if (ch == 2) return std::clamp(image->channel_value(i, j, 1), 0.0, 1.0);

        // RGB or larger: compute perceived luminance as mask value.
        double r = image->channel_value(i, j, 0);
        double g = image->channel_value(i, j, 1);
        double b = image->channel_value(i, j, 2);
        return std::clamp(0.2126 * r + 0.7152 * g + 0.0722 * b, 0.0, 1.0);
      }

  private:
    static double srgb_to_linear(double c) {
        c = std::clamp(c, 0.0, 1.0);
        if (c <= 0.04045) return c / 12.92;
        return std::pow((c + 0.055) / 1.055, 2.4);
    }

    void resolve_texel(double u, double v, int& i, int& j) const {
        u = interval(0,1).clamp(u);
        v = 1.0 - interval(0,1).clamp(v);
        i = int(u * image->width());
        j = int(v * image->height());
    }

    std::shared_ptr<rtw_image> image;
    texture_sample_space sample_space = texture_sample_space::srgb_color;
};

#endif
