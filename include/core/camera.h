#ifndef CAMERA_H
#define CAMERA_H

#include "hittable.h"
#include "materials/material.h"
#include <cmath>
#include <algorithm>
#include <chrono>
#include "caustics.h"
#include "mnee.h"
#include <vector>
#include <atomic>
#include <limits>

class renderer;

class camera {

  public:
    friend class renderer;

    //ratio of image width over height
    double aspect_ratio = 1.0; 
    
    //rendered image width in pixel count
    int image_width  = 100;
    int samples_per_pixel = 10;
    int max_depth = 10; //max recursion depth for ray tracing

    colour background = colour(0.0, 0.0, 0.0); //default sky color

    //vertical field of view in degrees
    double vfov = 90.0; 

    //camera position and orientation
    point3 lookfrom = point3(0,0,0);

    //point the camera is looking at
    point3 lookat   = point3(0,0,-1);

    //"up" direction of the camera
    vec3   vup      = vec3(0,1,0);

    //lens parameters for depth of field
    double defocus_angle = 0;  
    double focus_dist = 10;   
    
    int image_height;

    // sampling method (choose HALTON for high-quality, efficient AA)
    enum sampling_method_e { RANDOM_SAMPLES = 0, HALTON_SAMPLES = 1 };
    sampling_method_e sampling_method = HALTON_SAMPLES;

    // Camera-side directional sun fields (can be mirrored from scene directional light)
    bool   use_sun = false;
    vec3   sun_dir = vec3(0,0,0);
    colour sun_radiance = colour(0,0,0);
    double sun_angular_radius = 0.0; // degrees; zero is a delta directional light
    int    sun_shadow_samples = 8; // number of shadow samples for soft sun (0 = off)

    // Point lights copied from the editor scene. Simple point lights with
    // position, radiance, and optional range (range<=0 => infinite).
    struct point_light {
        point3 position = point3(0,0,0);
        colour  radiance = colour(1,1,1);
        double  range = 0.0; // if >0, max influence distance
    };

    std::vector<point_light> point_lights;

    // Emissive area lights (surfaces with emissive materials)
    struct emissive_surface {
        std::shared_ptr<hittable> geometry;
        double area = 0;
        colour emission; // Power estimate for selection; samples evaluate the material.
    };
    std::vector<emissive_surface> emissive_surfaces;
    std::vector<double> emissive_cdf; // Importance sampling CDF (by power = emission*area)

    // MNEE single-sphere caustics toggle and parameters (populated by editor)
    bool   enable_mnee = false;
    bool   mnee_has_sphere = false;
    point3 mnee_sphere_center = point3(0,0,0);
    double mnee_sphere_radius = 0.0;
    double mnee_sphere_ior    = 1.5;
    // MNEE solver tuning
    int    mnee_per_thread_budget = 16;  // Low default for point lights (1024 for sun caustics)
    int    mnee_newton_max_iters  = 8;
    double mnee_newton_tol        = 1e-5;
    double mnee_step_eps          = 1e-3;
    // Sun disc sampling and gain control for broader/brighter caustics
    int    mnee_sun_samples       = 4;
    double mnee_gain_scale        = 1.0;

    // NEE + MIS for continuous BSDF lobes (diffuse and glossy).
    int    direct_light_samples   = 1;  // samples per diffuse hit (0 = disabled)
    bool   enable_mis             = true; // Multiple Importance Sampling

    void render(const hittable& world, std::ostream& out = std::cout){
        
        initialize();

        out << "P3\n" << image_width << ' '
            << image_height << "\n255\n";

        using clock = std::chrono::steady_clock;
        auto render_start = clock::now();

        // ---------- FRAMEBUFFER ----------
        std::vector<colour> framebuffer(image_width * image_height);

        // ---------- PROGRESS + ETA ----------
        std::clog << "\x1b[?25l" << std::flush; //hide cursor
        int total_scanlines = image_height;
        std::atomic<int> completed_scanlines{0};

        const int bar_width = 40;

        // Rolling ETA (shared, updated in critical section)
        const int chunk_size = 4;
        int accumulated_lines = 0;
        double accumulated_time = 0.0;
        double eta_longterm = 0.0;
        double eta_shortterm = 0.0;

        auto chunk_start = clock::now();

        // ---------- PARALLEL RENDER ----------
        #pragma omp parallel for schedule(dynamic)
        for (int j = 0; j < image_height; j++)
        {
            for (int i = 0; i < image_width; i++)
            {
                colour pixel_colour(0,0,0);

                for (int s = 0; s < samples_per_pixel; s++) {
                    ray r = get_ray(i, j, s);
                    pixel_colour += ray_colour(r, max_depth, world);
                }

                framebuffer[j * image_width + i] =
                    pixel_samples_scale * pixel_colour;
            }

            // ONE scanline finished
            int done = ++completed_scanlines;

            // Progress + ETA update (guarded with critical)
            if (done % chunk_size == 0) {
                #pragma omp critical
                {
                    auto now = clock::now();
                    double chunk_seconds =
                        std::chrono::duration<double>(now - chunk_start).count();

                    chunk_start = now;

                    // short-term ETA
                    double short_avg = chunk_seconds / chunk_size;
                    eta_shortterm = short_avg * (total_scanlines - done);

                    // long-term ETA
                    accumulated_time += chunk_seconds;
                    accumulated_lines += chunk_size;

                    double long_avg = accumulated_time / accumulated_lines;
                    eta_longterm = long_avg * (total_scanlines - done);

                    // blended ETA
                    double eta = 0.75 * eta_longterm + 0.25 * eta_shortterm;

                    double elapsed =
                        std::chrono::duration<double>(now - render_start).count();

                    double pct = double(done) / total_scanlines;

                    // ---------- PROGRESS BAR ----------
                    std::clog << "\r[";

                    int pos = int(bar_width * pct);
                    for (int k = 0; k < bar_width; k++) {
                        if (k < pos) std::clog << "#";
                        else std::clog << "-";
                    }

                    int rem = int(eta);
                    int rem_min = rem / 60;
                    int rem_sec = rem % 60;

                    std::clog << "] "
                            << int(pct * 100.0) << "% "
                            << "| Elapsed: " << int(elapsed) << "s "
                            << "| Remaining: " << rem_min << ":"
                            << (rem_sec < 10 ? "0" : "") << rem_sec
                            << std::flush;
                }
            }
        }

        // ---------- OUTPUT THE FRAMEBUFFER ----------
        for (int j = 0; j < image_height; j++) {
            for (int i = 0; i < image_width; i++) {
                write_colour(out, framebuffer[j * image_width + i]);
            }
        }

        // ---------- FINAL TIME ----------
        double total_time =
            std::chrono::duration<double>(clock::now() - render_start).count();

        int tm = int(total_time) / 60;
        int ts = int(total_time) % 60;

        std::clog << "\nDone. Total time: "
                << tm << ":" << (ts < 10 ? "0" : "") << ts << "\n";

        std::clog << "\x1b[?25h" << std::flush; //show cursor again
    }

  private:
    
    
    //camera parameters
    point3 center;
    double pixel_samples_scale;         
    point3 pixel00_loc;    
    vec3 pixel_delta_u;  
    vec3 pixel_delta_v;  

    //camera coordinate system basis vectors
    vec3 u, v, w;
    
    vec3 defocus_disk_u;
    vec3 defocus_disk_v;

    void initialize() {
        // If the caller explicitly set an image_height (e.g. via the Editor UI),
        // honour that value. Otherwise compute height from width and aspect ratio.
        if (image_height <= 0) {
            image_height = int(image_width / aspect_ratio);
        } else {
            // Keep aspect ratio in sync with the explicitly requested resolution.
            aspect_ratio = double(image_width) / double(image_height);
        }
        image_height = (image_height < 1) ? 1 : image_height;

        pixel_samples_scale = 1.0 / samples_per_pixel;

        center = lookfrom;

        //determine viewport dimensions.
        auto theta = degrees_to_radians(vfov);
        auto h = tan(theta/2);
        auto viewport_height = 2.0 * h * focus_dist;
        auto viewport_width = viewport_height * (double(image_width)/image_height);

        //calculate the u, v, w basis vectors for the camera coordinate system.
        w = unit_vector(lookfrom - lookat);
        u = unit_vector(cross(vup, w));
        v = cross(w, u);

        //calculate the vectors spanning the viewport.
        vec3 viewport_u = viewport_width * u;
        vec3 viewport_v = viewport_height * -v;

        //calculate the horizontal and vertical delta vectors from pixel to pixel.
        pixel_delta_u = viewport_u / image_width;
        pixel_delta_v = viewport_v / image_height;

        //calculate the location of the upper left pixel.
        auto viewport_upper_left = center - (focus_dist * w) - viewport_u / 2 - viewport_v / 2;
            
        pixel00_loc = viewport_upper_left + 0.5 * (pixel_delta_u + pixel_delta_v);

        auto defocus_radius = focus_dist * tan(degrees_to_radians(defocus_angle) / 2.0);
        defocus_disk_u = defocus_radius * u;
        defocus_disk_v = defocus_radius * v;
    }

    ray get_ray(int i, int j, int sample) const {
        auto offset = sample_square(i, j, sample);
        auto pixel_sample = pixel00_loc
            + (i + offset.x()) * pixel_delta_u
            + (j + offset.y()) * pixel_delta_v;

        auto ray_origin =  (defocus_angle <= 0) ? center : defocus_disk_sample();
        auto ray_direction = pixel_sample - ray_origin;
        auto ray_time = random_double();

        return ray(ray_origin, ray_direction, ray_time);

    }

    vec3 sample_square(int i, int j, int sample) const {
        if (sampling_method == RANDOM_SAMPLES) {
            return vec3(random_double() - 0.5, random_double() - 0.5, 0);
        }

        // HALTON_SAMPLES (fast, low-discrepancy 2D sampling)
        double u = halton(sample + 1, 2); // base 2
        double v = halton(sample + 1, 3); // base 3

        // deterministic per-pixel scramble to avoid visible correlation
        double scr_x = pixel_hash_double(i, j);
        double scr_y = pixel_hash_double(j, i);

        u = u + scr_x;
        u = u - std::floor(u); // wrap into [0,1)
        v = v + scr_y;
        v = v - std::floor(v);

        return vec3(u - 0.5, v - 0.5, 0);
    }

    point3 defocus_disk_sample() const {
        double r = sqrt(random_double());
        double theta = 2.0 * pi * random_double();
        return center + r * cos(theta) * defocus_disk_u + r * sin(theta) * defocus_disk_v;
    }

    static double halton(int index, int base) {
        double result = 0.0;
        double f = 1.0;
        int i = index;
        while (i > 0) {
            f /= (double)base;
            result += f * double(i % base);
            i /= base;
        }
        return result;
    }

    public:
    // Public setter for caustics photon map
    void set_caustics(const photon_map& pm, double radius) {
        caustics = pm;
        caustics_radius = radius;
    }

    // Public setter for emissive area lights
    void set_emissive_surfaces(const std::vector<emissive_surface>& surfaces) {
        emissive_surfaces = surfaces;
        
        // Build importance sampling CDF by power (emission * area)
        emissive_cdf.clear();
        if (surfaces.empty()) return;
        
        emissive_cdf.resize(surfaces.size());
        double total_power = 0.0;
        
        for (size_t i = 0; i < surfaces.size(); ++i) {
            const auto& surf = surfaces[i];
            double lum = 0.2126 * surf.emission.x() + 0.7152 * surf.emission.y() + 0.0722 * surf.emission.z();
            double power = std::max(1e-3,lum) * surf.area;
            total_power += power;
            emissive_cdf[i] = total_power;
        }
        
        // Normalize CDF
        if (total_power > 1e-10) {
            for (auto& val : emissive_cdf) {
                val /= total_power;
            }
        }
    }

    // simple integer hash -> double in [0,1)
    static double pixel_hash_double(int a, int b) {
        unsigned int n = (unsigned int)(a * 73856093u ^ b * 19349663u);
        n = (n << 13) ^ n;
        unsigned int nn = (n * (n * n * 15731u + 789221u) + 1376312589u) & 0x7fffffffu;
        return double(nn) / double(0x7fffffffu);
    }

    static double colour_luminance(const colour& c) {
        return 0.2126 * c.x() + 0.7152 * c.y() + 0.0722 * c.z();
    }

    double point_light_sampling_weight(const point_light& pl, const point3& p, const vec3& normal) const {
        vec3 to_light = pl.position - p;
        double dist = to_light.length();
        if (dist <= 1e-6) return 0.0;
        if (pl.range > 0.0 && dist > pl.range) return 0.0;

        vec3 Ldir = unit_vector(to_light);
        // Retain support for normal maps and thin transmission on either side.
        double NdotL = std::max(0.05,std::abs(dot(normal,Ldir)));

        double att = 1.0 / std::max(1e-4, dist * dist);
        return att * NdotL * colour_luminance(pl.radiance);
    }

    double total_point_light_sampling_weight(const point3& p, const vec3& normal) const {
        double total = 0.0;
        for (const auto& pl : point_lights) {
            total += point_light_sampling_weight(pl, p, normal);
        }
        return total;
    }

    bool sample_point_light(const point3& p, const vec3& normal, double total_weight,
                            const point_light*& out_light, double& out_pick_prob) const {
        out_light = nullptr;
        out_pick_prob = 0.0;
        if (point_lights.empty() || total_weight <= 1e-10) return false;

        double target = random_double() * total_weight;
        double accum = 0.0;
        const point_light* fallback_light = nullptr;
        double fallback_weight = 0.0;

        for (const auto& pl : point_lights) {
            double weight = point_light_sampling_weight(pl, p, normal);
            if (weight <= 1e-10) continue;

            fallback_light = &pl;
            fallback_weight = weight;
            accum += weight;
            if (target <= accum) {
                out_light = &pl;
                out_pick_prob = weight / total_weight;
                return true;
            }
        }

        if (!fallback_light) return false;
        out_light = fallback_light;
        out_pick_prob = fallback_weight / total_weight;
        return true;
    }

    static double power_weight(double a, double b) {
        if (a <= 0) return 0;
        double ratio = b/a; return 1/(1+ratio*ratio);
    }
    double sun_solid_angle() const {
        double angle = degrees_to_radians(std::clamp(sun_angular_radius,0.0,90.0));
        return 4*pi*std::pow(std::sin(.5*angle),2);
    }
    double sun_pdf(const vec3& direction) const {
        double omega = sun_solid_angle();
        if (!use_sun || omega <= 1e-12) return 0;
        return dot(safe_unit_vector(direction),safe_unit_vector(sun_dir)) >= 1-omega/(2*pi) ? 1/omega : 0;
    }
    // PDF of selecting and sampling this visible emitter point, in solid angle.
    double emitter_pdf(const point3& origin, const hit_record& hit, double time) const {
        vec3 delta = hit.p-origin; double distance2 = delta.length_squared();
        if (distance2 <= 1e-12) return 0;
        vec3 direction = delta/std::sqrt(distance2); double pdf = 0;
        for (size_t i = 0; i < emissive_surfaces.size(); ++i) {
            const auto& light = emissive_surfaces[i]; if (!light.geometry) continue;
            hit_record sampled;
            if (!light.geometry->hit(ray(origin,direction,time),interval(.001,std::sqrt(distance2)+.002),sampled)) continue;
            if ((sampled.p-hit.p).length_squared() > 1e-6) continue;
            double cosine = std::abs(dot(sampled.geometry_normal(),-direction));
            double pick = emissive_cdf[i]-(i ? emissive_cdf[i-1] : 0);
            if (cosine > 1e-12) pdf += pick*light.geometry->surface_pdf(sampled)*distance2/cosine;
        }
        return pdf;
    }
    // The editor's directional-light strength is integrated incident radiance.
    // Finite sun discs divide it by their solid angle, consistently for NEE and
    // BSDF hits, so changing the angular size does not change total brightness.
    colour ray_colour(const ray& r0, int max_depth, const hittable& world, colour* out_albedo = nullptr,
                      vec3* out_normal = nullptr, const hit_record* prehit = nullptr,
                      const colour* precomputed_direct = nullptr) const {
        (void)precomputed_direct; // The retired ISPC prepass cannot supply MIS-aware lighting.
        ray current_ray = r0;
        colour throughput(1,1,1), result(0,0,0);
        point3 previous_point;
        double previous_pdf = 0;
        int previous_samples = 0, previous_sun_samples = 0, null_events = 0;
        bool previous_delta = true, first_intersection = true;
        auto hit_weight = [&](double light_pdf, int samples) {
            if (previous_delta || samples == 0 || light_pdf <= 0) return 1.0;
            return enable_mis ? power_weight(previous_pdf,samples*light_pdf) : 0.0;
        };
        for (int depth = 0; depth <= max_depth; ++depth) {
            hit_record rec;
            bool did_hit;
            if (prehit && first_intersection) { rec = *prehit; did_hit = true; }
            else did_hit = world.hit(current_ray,interval(.001,infinity),rec);
            first_intersection = false;
            if (!did_hit) {
                result += throughput*background;
                double pdf = sun_pdf(current_ray.direction());
                if (pdf > 0) result += throughput*sun_radiance*(pdf*hit_weight(pdf,previous_sun_samples));
                break;
            }
            if (!rec.mat) break;
            bsdf_sample sample;
            bool scattered = rec.mat->sample_bsdf(current_ray,rec,sample);
            if (scattered && sample.passthrough) {
                if (++null_events > 1024) break;
                current_ray = sample.scattered; --depth; continue;
            }
            if (depth == 0) {
                if (out_albedo) *out_albedo = rec.mat->albedo(rec);
                if (out_normal) *out_normal = rec.normal;
            }
            colour emitted = rec.mat->emitted(rec.u,rec.v,rec.p);
            if (emitted.length_squared() > 0) {
                double pdf = previous_delta || previous_samples == 0 ? 0 : emitter_pdf(previous_point,rec,current_ray.time());
                result += throughput*emitted*hit_weight(pdf,previous_samples);
            }
            if (depth == max_depth || (!scattered && emitted.length_squared() > 0)) break;

            // Caustics lookup on diffuse receivers.
            // Since the photon map stores only caustic photons (paths that
            // went through a dielectric), we can query unconditionally on
            // non-specular hits.
            if (!rec.mat->is_specular() && depth <= 1) {
                if (caustics.size() > 0) {
                    colour Lc = caustics.query(rec.p, caustics_radius);
                    result += throughput * Lc;
                }

                if (enable_mnee && mnee_has_sphere) {
                    mnee_config mc;
                    mc.newton_max_iters = mnee_newton_max_iters;
                    mc.newton_tol       = mnee_newton_tol;
                    mc.step_eps         = mnee_step_eps;
                    mc.per_thread_budget= mnee_per_thread_budget;
                    mc.sun_ang_radius   = sun_angular_radius * (pi / 180.0);
                    mc.sun_samples      = mnee_sun_samples;
                    mc.gain_scale       = mnee_gain_scale;
                    // Sun-directed MNEE (if sun enabled)
                    if (use_sun) {
                        colour Lm = mnee_single_sphere_estimate(
                            rec.p, rec.normal,
                            unit_vector(sun_dir), sun_radiance,
                            mnee_sphere_center, mnee_sphere_radius, mnee_sphere_ior,
                            world, mc
                        );
                        result += throughput * Lm;
                    }

                    // Point-light MNEE: DISABLED - creates circular banding artifacts
                    // with low budgets and is extremely expensive. Point light caustics
                    // are better handled by regular path tracing with sufficient SPP.
                    // Sun caustics use Newton solver which is much more accurate.
                    // To re-enable: uncomment code and set mnee_per_thread_budget > 256
                }
            }

            vec3 view = safe_unit_vector(-current_ray.direction());
            int count = std::max(0,direct_light_samples);
            // Surface and subsurface hooks are conditional on an accepted opaque
            // hit. A null-opacity event above receives no direct surface lighting.
            auto add_light = [&](const vec3& direction, const colour& radiance, double max_distance,
                                 double weight, double surface_mis) {
                vec3 origin = rec.p+rec.geometry_normal()*(dot(direction,rec.geometry_normal()) >= 0 ? .001 : -.001);
                double visibility = compute_transmittance(ray(origin,direction,current_ray.time()),max_distance,world);
                if (visibility <= 0) return;
                colour direct = rec.mat->shade_direct(rec,view,direction,radiance,world,visibility);
                colour subsurface = rec.mat->shade_sss(rec,view,direction,radiance,world,visibility);
                // The approximate SSS hook has no competing BSDF sampler.
                result += throughput*(direct*surface_mis+subsurface)*weight;
            };
            if (use_sun) {
                double omega = sun_solid_angle();
                int samples = omega <= 1e-12 ? 1 : count > 0 ? std::max(1,sun_shadow_samples) : 0;
                pbr::frame sun_frame(safe_unit_vector(sun_dir));
                for (int i = 0; i < samples; ++i) {
                    vec3 direction = sun_frame.n;
                    double weight = 1.0/samples, mis = 1;
                    colour radiance = sun_radiance;
                    if (omega > 1e-12) {
                        double z = 1-random_double()*omega/(2*pi), phi = 2*pi*random_double();
                        double radius = std::sqrt(std::max(0.0,1-z*z));
                        direction = sun_frame.world(vec3(radius*std::cos(phi),radius*std::sin(phi),z));
                        double pdf = 1/omega;
                        mis = enable_mis ? power_weight(samples*pdf,rec.mat->bsdf_pdf(rec,view,direction)) : 1;
                        radiance *= pdf; weight /= pdf;
                    }
                    add_light(direction,radiance,infinity,weight,mis);
                }
            }
            // Delta lights cannot be reached by BSDF sampling: no MIS weight.
            double total = total_point_light_sampling_weight(rec.p,rec.geometry_normal());
            int point_samples = std::max(1,count);
            for (int i = 0; total > 0 && i < point_samples; ++i) {
                const point_light* light; double pick;
                if (!sample_point_light(rec.p,rec.geometry_normal(),total,light,pick)) continue;
                vec3 delta = light->position-rec.p; double distance = delta.length();
                if (distance <= .002) continue;
                add_light(delta/distance,light->radiance/(distance*distance),distance-.002,1/(pick*point_samples),1);
            }
            for (int i = 0; i < count && !emissive_cdf.empty(); ++i) {
                auto it = std::lower_bound(emissive_cdf.begin(),emissive_cdf.end(),random_double());
                size_t index = std::min((size_t)(it-emissive_cdf.begin()),emissive_surfaces.size()-1);
                const auto& light = emissive_surfaces[index];
                double pick = emissive_cdf[index]-(index ? emissive_cdf[index-1] : 0), area_pdf;
                hit_record light_hit;
                if (!light.geometry || pick <= 0 || !light.geometry->sample_surface(random_double(),random_double(),current_ray.time(),light_hit,area_pdf)) continue;
                vec3 delta = light_hit.p-rec.p; double distance2 = delta.length_squared(), distance = std::sqrt(distance2);
                if (distance <= .002) continue;
                vec3 direction = delta/distance;
                double cosine = std::abs(dot(light_hit.geometry_normal(),-direction));
                if (cosine <= 1e-12 || area_pdf <= 0) continue;
                double pdf = pick*area_pdf*distance2/cosine;
                double mis = enable_mis ? power_weight(count*pdf,rec.mat->bsdf_pdf(rec,view,direction)) : 1;
                colour radiance = light_hit.mat ? light_hit.mat->emitted(light_hit.u,light_hit.v,light_hit.p) : colour(0,0,0);
                add_light(direction,radiance,distance-.002,1/(count*pdf),mis);
            }
            // A rejected BSDF sample must not discard the independent NEE estimate.
            if (!scattered) break;
            previous_point = rec.p; previous_pdf = sample.pdf;
            previous_delta = sample.delta; previous_samples = count;
            previous_sun_samples = count > 0 ? std::max(1,sun_shadow_samples) : 0;
            throughput = throughput*sample.weight;
            current_ray = sample.scattered;
            if (!sample.passthrough) {
                vec3 direction = safe_unit_vector(current_ray.direction());
                double side = dot(direction,rec.geometry_normal()) >= 0 ? 1 : -1;
                current_ray = ray(rec.p+side*.001*rec.geometry_normal(),direction,current_ray.time());
            }
            if (depth >= 8) {
                double survival = std::clamp(std::max({throughput.x(),throughput.y(),throughput.z()}),.05,.95);
                if (random_double() >= survival) break;
                throughput /= survival;
            }
        }
        return result;
    }

    public:
        // Caustics photon map (built externally before rendering); queried on diffuse hits
        photon_map caustics;
        double     caustics_radius = 0.08; // gather radius

    // Compute deterministic transmittance along a ray up to max_t by
    // accumulating per-hit material opacity (alpha). Returns value in
    // [0,1], where 0 = fully blocked, 1 = fully clear.
    double compute_transmittance(const ray& r, double max_t, const hittable& world) const {
        double T = 1.0;
        double tmin = 0.001;
        double tmax = max_t;
        hit_record rec;

        while (tmin < tmax && world.hit(r, interval(tmin, tmax), rec)) {
            double opacity = 1.0;
            if (rec.mat) opacity = rec.mat->opacity_at(rec);

            // transparency is (1 - opacity)
            double trans = 1.0 - clamp01(opacity);

            // If fully opaque, blocked
            if (trans <= 1e-6) return 0.0;

            T *= trans;
            if (T <= 1e-6) return 0.0;

            // Advance min t to just beyond this hit and continue
            tmin = rec.t + 0.001;
        }

        return T;
    }

};

#endif
