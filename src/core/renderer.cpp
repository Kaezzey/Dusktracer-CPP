#include "../../include/core/dusktracer.h"
#include "../../include/core/renderer.h"
#include "../../include/core/interval.h"

#include <cmath>
#include <atomic>
#include <chrono>
#include <algorithm>
#include <cstring>
#include <deque>
#include <mutex>
#ifdef _OPENMP
#include <omp.h>
#endif
#ifdef HAVE_OIDN
#include <OpenImageDenoise/oidn.hpp>
#endif
#include "../../include/core/image_io.h"
#include "ispc_brdf.h"
#include "ispc_brdf_spec.h"
#include "../../include/core/BVH.h"
#include "../../include/core/packet.h"
#include "../../include/core/materials/material.h"

// 1) Simple overload: forwards to the full version with progress = nullptr
render_result renderer::render(
    const hittable& world,
    camera&         cam,
    std::atomic<bool>* cancel_flag
) const
{
    return render(world, cam, cancel_flag, nullptr, nullptr);
}

// 2) Full version with progress + ETA
render_result renderer::render(
    const hittable&        world,
    camera&                cam,
    std::atomic<bool>*     cancel_flag,
    render_progress_state* progress,
    std::function<void(const render_result&)> progress_callback
) const
{
    using clock = std::chrono::steady_clock;

    cam.initialize();

    render_result out;
    out.width  = cam.image_width;
    out.height = cam.image_height;
    out.pixels.resize(out.width * out.height * 3);

    const int w  = out.width;
    const int h = out.height;
    const int spp    = cam.samples_per_pixel;
    const int depth  = cam.max_depth;
    const bool need_aux_aovs = this->use_denoiser;
    const bool enable_progressive_updates = (bool)progress_callback;
    const bool use_primary_prehit = false;

    // HDR linear float buffer (3 floats per pixel) used for optional denoising
    std::vector<float> hdr_buffer((size_t)w * (size_t)h * 3, 0.0f);
    // Albedo (base color) and normal AOVs for OIDN guidance
    std::vector<float> albedo_buffer;
    std::vector<float> normal_buffer;
    if (need_aux_aovs) {
        albedo_buffer.resize((size_t)w * (size_t)h * 3, 0.0f);
        normal_buffer.resize((size_t)w * (size_t)h * 3, 0.0f);
    }

    const int total_scanlines = h;
    const int progress_min_interval_ms = 1500; // throttle partial updates aggressively
    const int progress_denoise_interval_ms = 30000; // OIDN during render is expensive; keep it rare
    const int progress_denoise_hold_ms = 25000; // hold cached denoised result to avoid flicker/churn

    // Rolling average per-tile time (seconds). Updated under critical section.
    std::atomic<double> avg_tile_time{0.0};
    // Track which scanlines are fully written to HDR buffer
    std::vector<char> row_done(enable_progressive_updates ? (size_t)h : 0, 0);

    // Throttle progress callbacks across all OMP workers
    static std::atomic<long long> s_last_progress_ms{0};
    static std::atomic<long long> s_last_denoise_ms{0};
    // Cached denoised HDR result + timestamp to provide a stable denoised display
    static std::atomic<long long> s_cached_denoised_ts{0};
    static std::vector<float> s_cached_denoised_hdr; // resized on first use to match render size
    static std::mutex s_cached_denoised_mutex;

    if (progress) {
        progress->total_scanlines           = total_scanlines;
        progress->completed_scanlines.store(0);
        progress->eta_seconds.store(0.0);
        progress->elapsed_seconds.store(0.0);
    }

    auto render_start = clock::now();
    static const interval intensity(0.000, 0.999);

    // Tile-based parallel rendering: reduces OpenMP scheduling overhead
    const int tile_w = 32;
    const int tile_h = 32;
    const int num_tiles_x = (w + tile_w - 1) / tile_w;
    const int num_tiles_y = (h + tile_h - 1) / tile_h;
    const int num_tiles = num_tiles_x * num_tiles_y;
    // Tile progress bookkeeping (used for ETA calculation)
    std::atomic<int> completed_tiles{0};

    // Recent per-tile timings (protected by the critical section below).
    // We'll keep a short sliding window and use its median to compute ETA
    // which avoids bias from slow startup tiles or occasional outliers.
    std::deque<double> recent_tile_times;
    const int RECENT_TILE_WINDOW = 64;

    if (progress) {
        progress->total_tiles = num_tiles;
        progress->completed_tiles.store(0);
    }

    // Preallocate per-thread reusable buffers to avoid allocations inside the tile loop.
    int max_tile_pixels = tile_w * tile_h;
    int max_first_size = spp * max_tile_pixels;
    int worker_count = 1;
#ifdef _OPENMP
    worker_count = std::max(1, omp_get_max_threads());
#endif

    // Per-thread storage
    std::vector<std::vector<hit_record>> thread_first_rec(worker_count);
    std::vector<std::vector<char>>       thread_first_hit(worker_count);
    std::vector<std::vector<colour>>     thread_tile_direct(worker_count);
    std::vector<std::vector<float>>      thread_tile_normals(worker_count);
    std::vector<std::vector<int>>        thread_tile_idxf(worker_count);

    for (int t = 0; t < worker_count; ++t) {
        if (use_primary_prehit) {
            thread_first_rec[t].resize(max_first_size);
            thread_first_hit[t].resize(max_first_size);
            thread_tile_direct[t].resize(max_first_size, colour(0,0,0));
        }
        if (need_aux_aovs) {
            thread_tile_normals[t].reserve(max_tile_pixels * 3);
            thread_tile_idxf[t].reserve(max_tile_pixels);
        }
    }

    // Force a consistent number of threads to match preallocated buffers
    #pragma omp parallel for schedule(dynamic,1) num_threads(worker_count)
    for (int ti = 0; ti < num_tiles; ++ti) {
        if (cancel_flag && cancel_flag->load()) continue;

        int tid = 0;
#ifdef _OPENMP
        tid = omp_get_thread_num();
#endif
        // Safety: ensure tid maps into the preallocated per-thread buffers
        if (tid < 0 || tid >= worker_count) {
            tid = tid % std::max(1, worker_count);
        }

        int tx = ti % num_tiles_x;
        int ty = ti / num_tiles_x;
        int x0 = tx * tile_w;
        int y0 = ty * tile_h;
        int x1 = std::min(w, x0 + tile_w);
        int y1 = std::min(h, y0 + tile_h);

        // Reusable per-thread buffers for this tile (avoid allocations)
        int tile_w_pixels = (x1 - x0) * (y1 - y0);
        size_t needed_first = (size_t)spp * (size_t)tile_w_pixels;

        auto& tile_normals = thread_tile_normals[tid];
        auto& tile_idxf = thread_tile_idxf[tid];
        if (need_aux_aovs) {
            tile_normals.clear();
            tile_idxf.clear();
            tile_normals.reserve((x1 - x0) * (y1 - y0) * 3);
            tile_idxf.reserve((x1 - x0) * (y1 - y0));
        }

        auto& tile_first_rec = thread_first_rec[tid];
        auto& tile_first_hit = thread_first_hit[tid];
        if (use_primary_prehit) {
            if (tile_first_rec.size() < needed_first) {
                tile_first_rec.resize(needed_first);
                tile_first_hit.resize(needed_first);
            }
            // reset hit flags for the active range
            std::fill_n(tile_first_hit.data(), (size_t)needed_first, (char)0);
        }

        // Try to obtain a pointer to a top-level BVH to use packet traversal.
        const bvh_node* top_bvh = nullptr;
        if (use_primary_prehit) {
            if (auto wl = dynamic_cast<const hittable_list*>(&world)) {
                if (!wl->objects.empty()) {
                    // Object[0] is expected to be the top-level BVH node constructed in scene builder
                    top_bvh = dynamic_cast<const bvh_node*>(wl->objects[0].get());
                }
            }
        }
        
        if (top_bvh) {
            // For each primary sample index, packet-trace primary rays and store per-sample prehits.
            for (int s = 0; s < spp; ++s) {
                for (int j = y0; j < y1; ++j) {
                    for (int i = x0; i < x1; i += 4) {
                        RayPacket4 pk;
                        pk.active_mask = 0;
                        for (int l = 0; l < 4; ++l) {
                            int px = i + l;
                            if (px >= x1) continue;
                            pk.r[l] = cam.get_ray(px, j, s);
                            pk.active_mask |= (1u << l);
                            pk.tmin[l] = 0.001;
                            pk.tmax[l] = 1e30;
                        }

                        hit_record out_recs[4];
                        unsigned int hitmask = top_bvh->hit_packet(pk, out_recs);

                        for (int l = 0; l < 4; ++l) {
                            int px = i + l;
                            if (px >= x1) continue;
                            int local_idx = (j - y0) * (x1 - x0) + (px - x0);
                            int idx = s * tile_w_pixels + local_idx;
                            if (hitmask & (1u << l)) {
                                tile_first_hit[idx] = 1;
                                tile_first_rec[idx] = out_recs[l];
                            }
                        }
                    }
                }
            }
        }

        // Prepare per-tile precomputed direct lighting (for first-hit only)
        auto& tile_direct = thread_tile_direct[tid];
        if (use_primary_prehit) {
            if (tile_direct.size() < needed_first) tile_direct.resize(needed_first);
            std::fill_n(tile_direct.data(), needed_first, colour(0,0,0));
        }

        // Per-tile timer (start before any work)
        auto tile_start = clock::now();

        // DISABLED: Direct lighting precompute causing crashes
        // The ray_colour function will handle direct lighting via NEE/MIS instead
        /*
        if (ti == 0) { std::fprintf(stderr, "[TILE0] Starting direct lighting precompute, top_bvh=%p\n", (void*)top_bvh); std::fflush(stderr); }

        // If we have a top-level BVH, attempt to batch direct-lighting for
        // the first-hit points using packetized shadow queries and ISPC
        // specular evaluation. This precomputes a direct-light colour per
        // (sample,pixel) which we later pass into `camera::ray_colour`.
        if (top_bvh) {
            if (ti == 0) { std::fprintf(stderr, "[TILE0] Entering direct lighting loop\n"); std::fflush(stderr); }
            // Precompute per-sample first-hit direct lighting
            for (int s = 0; s < spp; ++s) {
                if (ti == 0 && s == 0) { std::fprintf(stderr, "[TILE0] Direct light sample s=0\n"); std::fflush(stderr); }
                for (int j = y0; j < y1; ++j) {
                    for (int i = x0; i < x1; i += 4) {
                        if (ti == 0 && s == 0 && j == y0 && i == x0) { std::fprintf(stderr, "[TILE0] First direct packet\n"); std::fflush(stderr); }
                        // Build a small 4-wide packet for up to 4 lanes
                        RayPacket4 pk;
                        pk.active_mask = 0;
                        hit_record primary_recs[4];
                        int lane_px[4];
                        int lane_local_idx[4];
                        int lane_idx_global[4];
                        if (ti == 0 && s == 0 && j == y0 && i == x0) { std::fprintf(stderr, "[TILE0] Arrays initialized\n"); std::fflush(stderr); }
                        for (int l = 0; l < 4; ++l) {
                            int px = i + l;
                            lane_px[l] = px;
                            if (px >= x1) continue;
                            int local_idx = (j - y0) * (x1 - x0) + (px - x0);
                            int idx = s * tile_w_pixels + local_idx;
                            if (ti == 0 && s == 0 && j == y0 && i == x0 && l == 0) { 
                                std::fprintf(stderr, "[TILE0] Lane 0: px=%d local_idx=%d idx=%d tile_first_hit.size()=%zu tile_first_rec.size()=%zu\n", 
                                    px, local_idx, idx, tile_first_hit.size(), tile_first_rec.size()); 
                                std::fflush(stderr); 
                            }
                            lane_local_idx[l] = local_idx;
                            lane_idx_global[l] = idx;
                            if (ti == 0 && s == 0 && j == y0 && i == x0 && l == 0) { 
                                std::fprintf(stderr, "[TILE0] About to check tile_first_hit[%d]\n", idx); 
                                std::fflush(stderr); 
                            }
                            bool has_hit = (idx < tile_first_hit.size()) && tile_first_hit[idx];
                            if (ti == 0 && s == 0 && j == y0 && i == x0 && l == 0) { 
                                std::fprintf(stderr, "[TILE0] tile_first_hit[%d]=%d\n", idx, (int)has_hit); 
                                std::fflush(stderr); 
                            }
                            if (has_hit) {
                                if (ti == 0 && s == 0 && j == y0 && i == x0 && l == 0) { std::fprintf(stderr, "[TILE0] Lane 0 has hit, loading rec\n"); std::fflush(stderr); }
                                primary_recs[l] = tile_first_rec[idx];
                                pk.r[l] = cam.get_ray(px, j, s);
                                pk.tmin[l] = 0.001;
                                pk.tmax[l] = 1e30;
                                pk.active_mask |= (1u << l);
                            }
                            if (ti == 0 && s == 0 && j == y0 && i == x0 && l == 0) { 
                                std::fprintf(stderr, "[TILE0] Lane 0 complete, has_hit=%d\n", (int)has_hit); 
                                std::fflush(stderr); 
                            }
                        }
                        if (ti == 0 && s == 0 && j == y0 && i == x0) { 
                            std::fprintf(stderr, "[TILE0] All 4 lanes processed, checking active_mask\n"); 
                            std::fflush(stderr); 
                        }

                        if (pk.active_mask == 0) continue;
                        
                        if (ti == 0 && s == 0 && j == y0 && i == x0) { std::fprintf(stderr, "[TILE0] Active mask=%u, processing direct light\n", pk.active_mask); std::fflush(stderr); }

                        // For each directional sun (if enabled) and point lights,
                        // perform a packet shadow query and accumulate direct light.
                        // We'll use ISPC specular for the microfacet reflection term
                        // for visible lanes and a scalar diffuse term.

                        // Helper buffers (max 4 lanes) - expanded to include tangent-space and material params
                        float Ngeomx[4], Ngeomy[4], Ngeomz[4];
                        float Tx[4], Ty[4], Tz[4];
                        float Bx[4], By[4], Bz[4];
                        float ntr[4], ntg[4], ntb[4];
                        float normal_strength_arr[4];
                        float Vx[4], Vy[4], Vz[4];
                        float Lx[4], Ly[4], Lz[4];
                        float baser[4], baseg[4], baseb[4];
                        float metallic_arr[4];
                        float dielr[4], dielg[4], dielb[4];
                        float alpha[4];
                        float out_r[4], out_g[4], out_b[4];

                        // Process sun (directional) first
                        if (cam.use_sun && !cam.sun_dir.near_zero()) {
                            RayPacket4 spk = pk; // copy base packet
                            vec3 Ldir = unit_vector(cam.sun_dir);
                            // Set packet ray directions toward sun
                            for (int l = 0; l < 4; ++l) {
                                if (!(spk.active_mask & (1u << l))) continue;
                                hit_record& rec = primary_recs[l];
                                vec3 origin = rec.p + rec.normal * 0.001f;
                                spk.r[l] = ray(origin, Ldir, 0.0);
                                spk.tmin[l] = 0.001;
                                spk.tmax[l] = 1e30;
                            }

                            hit_record shadow_recs[4];
                            unsigned int shadow_mask = top_bvh->hit_packet(spk, shadow_recs);

                            // Build arrays for visible lanes and call ISPC specular
                            int k = 0;
                            int lane_map[4];
                            for (int l = 0; l < 4; ++l) {
                                if (!(spk.active_mask & (1u << l))) continue;
                                bool occluded = (shadow_mask & (1u << l));
                                double vis = 1.0;
                                if (occluded) {
                                    double op = 1.0;
                                    if (shadow_recs[l].mat) op = shadow_recs[l].mat->opacity_at(shadow_recs[l]);
                                    vis = 1.0 - std::clamp(op, 0.0, 1.0);
                                }
                                if (vis <= 0.0) continue;

                                hit_record& rec = primary_recs[l];
                                // include specular materials here so dielectrics receive
                                // direct specular response from lights. For dielectric
                                // materials we prefer to call their material-specific
                                // `shade_direct` (which models thin transmission/refraction)
                                // instead of the generic PBR ISPC kernel to avoid
                                // introducing a diffuse term that makes glass look
                                // opaque.
                                // Gather shading inputs (defer normal-map transform to ISPC)
                                vec3 Ngeom = rec.normal;
                                vec3 N = Ngeom;
                                ray view_ray = cam.get_ray(lane_px[l], j, s);
                                vec3 V = -unit_vector(view_ray.direction());
                                colour base = rec.mat ? rec.mat->albedo(rec) : colour(1,1,1);

                                const material* mptr = rec.mat.get();
                                // If this material is dielectric, compute direct using
                                // its `shade_direct` implementation and skip ISPC.
                                if (mptr && mptr->is_dielectric()) {
                                    // deterministic visibility already applied via 'vis'
                                    colour direct = mptr->shade_direct(rec, V, Ldir, cam.sun_radiance, world, vis);
                                    int global_idx = lane_idx_global[l];
                                    tile_direct[global_idx] += direct;
                                    continue;
                                }
                                const pbr_material* pbr = dynamic_cast<const pbr_material*>(mptr);
                                float metallic_v = 0.0f;
                                float rough_v = 0.5f;
                                colour dielectricF0 = colour(0.04f, 0.04f, 0.04f);
                                // default tangent/bitangent/normal-texture values (may be overridden by pbr)
                                vec3 T = rec.tangent;
                                vec3 B = rec.bitangent;
                                colour n_tex = colour(0.5f, 0.5f, 1.0f);
                                float nstrength = 0.0f;
                                if (pbr) {
                                    if (pbr->metallic_tex) {
                                        // Sample metallic/roughness textures. Default: R channel = metallic, G = roughness.
                                        // If Unreal packing is enabled, interpret G=roughness, B=metallic.
                                        colour mr = pbr->metallic_tex->value(rec.u, rec.v, rec.p);
                                        if (pbr->use_unreal_pbr) {
                                            rough_v = (float)mr.g();
                                            metallic_v = (float)mr.b();
                                        } else {
                                            metallic_v = (float)mr.x();
                                        }
                                    }
                                    if (pbr->roughness_tex) rough_v = (float)pbr->roughness_tex->value(rec.u, rec.v, rec.p).x();
                                    dielectricF0 = pbr->dielectric_F0;
                                    base = pbr->albedo(rec);
                                    if (pbr->normal_tex) {
                                        n_tex = pbr->normal_tex->value(rec.u, rec.v, rec.p);
                                        nstrength = pbr->normal_strength;
                                    }
                                }

                                // populate arrays
                                Ngeomx[k] = (float)Ngeom.x(); Ngeomy[k] = (float)Ngeom.y(); Ngeomz[k] = (float)Ngeom.z();
                                Tx[k] = (float)T.x(); Ty[k] = (float)T.y(); Tz[k] = (float)T.z();
                                Bx[k] = (float)B.x(); By[k] = (float)B.y(); Bz[k] = (float)B.z();
                                ntr[k] = (float)n_tex.x(); ntg[k] = (float)n_tex.y(); ntb[k] = (float)n_tex.z();
                                normal_strength_arr[k] = (float)nstrength;

                                Vx[k] = (float)V.x(); Vy[k] = (float)V.y(); Vz[k] = (float)V.z();
                                Lx[k] = (float)Ldir.x(); Ly[k] = (float)Ldir.y(); Lz[k] = (float)Ldir.z();

                                baser[k] = (float)base.x(); baseg[k] = (float)base.y(); baseb[k] = (float)base.z();
                                metallic_arr[k] = (float)metallic_v;
                                dielr[k] = (float)dielectricF0.x(); dielg[k] = (float)dielectricF0.y(); dielb[k] = (float)dielectricF0.z();
                                alpha[k] = (float)perceptual_to_alpha((double)rough_v);

                                lane_map[k] = l;
                                ++k;
                            }

                                if (k > 0) {
                                // Compute full shaded BRDF per lane using ISPC helper
                                ispc_compute_shade(Ngeomx, Ngeomy, Ngeomz,
                                                   Tx, Ty, Tz,
                                                   Bx, By, Bz,
                                                   ntr, ntg, ntb,
                                                   normal_strength_arr,
                                                   Vx, Vy, Vz,
                                                   Lx, Ly, Lz,
                                                   baser, baseg, baseb,
                                                   metallic_arr,
                                                   dielr, dielg, dielb,
                                                   alpha, k,
                                                   out_r, out_g, out_b);
                                // Scatter results back to tile_direct entries (sun radiance)
                                for (int pk_i = 0; pk_i < k; ++pk_i) {
                                    int l = lane_map[pk_i];
                                    int global_idx = lane_idx_global[l];
                                    colour Li = cam.sun_radiance;
                                    colour brdf_col(out_r[pk_i] * (float)Li.x(),
                                                     out_g[pk_i] * (float)Li.y(),
                                                     out_b[pk_i] * (float)Li.z());
                                    tile_direct[global_idx] += brdf_col;

                                    // Add SSS contribution from the concrete material if present
                                    hit_record& rec = primary_recs[l];
                                    const material* mptr = rec.mat.get();
                                    const pbr_material* pbr = dynamic_cast<const pbr_material*>(mptr);
                                    if (pbr && pbr->sss_strength > 0.0) {
                                        colour sss = pbr->shade_sss(rec, V, Ldir, cam.sun_radiance, world, 1.0);
                                        tile_direct[global_idx] += sss;
                                    }
                                }
                            }
                        }

                        // Process point lights: loop over all (precompute direct lighting is deterministic)
                        // Note: Path tracer uses importance sampling, but precompute needs all lights
                        // for consistent direct lighting. This is acceptable since it's only first-hit.
                        for (const auto& pl : cam.point_lights) {
                            RayPacket4 spk = pk;
                            // Setup each lane's shadow ray toward the point light
                                for (int l = 0; l < 4; ++l) {
                                if (!(spk.active_mask & (1u << l))) continue;
                                hit_record& rec = primary_recs[l];
                                vec3 origin = rec.p + rec.normal * 0.001f;
                                vec3 Ldir = unit_vector(pl.position - rec.p);
                                double dist = (pl.position - rec.p).length();
                                spk.r[l] = ray(origin, Ldir, 0.0);
                                spk.tmin[l] = 0.001;
                                spk.tmax[l] = (float)(dist - 0.001);
                            }

                            hit_record shadow_recs[4];
                            unsigned int shadow_mask = top_bvh->hit_packet(spk, shadow_recs);

                            int k = 0;
                            int lane_map[4];
                            for (int l = 0; l < 4; ++l) {
                                if (!(spk.active_mask & (1u << l))) continue;
                                bool occluded = (shadow_mask & (1u << l));
                                double vis = 1.0;
                                if (occluded) {
                                    double op = 1.0;
                                    if (shadow_recs[l].mat) op = shadow_recs[l].mat->opacity_at(shadow_recs[l]);
                                    vis = 1.0 - std::clamp(op, 0.0, 1.0);
                                }
                                if (vis <= 0.0) continue;

                                hit_record& rec = primary_recs[l];
                                // Precompute geometry, view and light directions used by both
                                // dielectric and PBR branches.
                                vec3 Ngeom = rec.normal;
                                vec3 N = Ngeom;
                                ray view_ray = cam.get_ray(lane_px[l], j, s);
                                vec3 V = -unit_vector(view_ray.direction());
                                vec3 Ldir = unit_vector(pl.position - rec.p);
                                colour base = rec.mat ? rec.mat->albedo(rec) : colour(1,1,1);

                                // For dielectrics, call material's shade_direct
                                // (handles transmission/refraction) and skip ISPC.
                                const material* mptr2 = rec.mat.get();
                                if (mptr2 && mptr2->is_dielectric()) {
                                    // compute attenuated radiance toward point light
                                    double dist = (pl.position - rec.p).length();
                                    double att = 1.0 / std::max(1e-4, dist * dist);
                                    // visibility already folded into 'vis'
                                    colour Li = pl.radiance * (float)att * (float)vis;
                                    colour direct = mptr2->shade_direct(rec, V, Ldir, Li, world, vis);
                                    int global_idx = lane_idx_global[l];
                                    tile_direct[global_idx] += direct;
                                    continue;
                                }

                                // Compute F0 and alpha using pbr_material when present
                                const material* mptr = rec.mat.get();
                                const pbr_material* pbr = dynamic_cast<const pbr_material*>(mptr);
                                float metallic_v = 0.0f;
                                float rough_v = 0.5f;
                                colour dielectricF0 = colour(0.04f, 0.04f, 0.04f);
                                // default tangent/bitangent/normal-texture values (may be overridden by pbr)
                                vec3 T = rec.tangent;
                                vec3 B = rec.bitangent;
                                colour n_tex = colour(0.5f, 0.5f, 1.0f);
                                float nstrength = 0.0f;
                                if (pbr) {
                                    if (pbr->metallic_tex) metallic_v = (float)pbr->metallic_tex->value(rec.u, rec.v, rec.p).x();
                                    if (pbr->roughness_tex) rough_v = (float)pbr->roughness_tex->value(rec.u, rec.v, rec.p).x();
                                    dielectricF0 = pbr->dielectric_F0;
                                    base = pbr->albedo(rec);
                                    
                                    // Apply normal map if present
                                    if (pbr->normal_tex) {
                                        // override defaults with texture values
                                        n_tex = pbr->normal_tex->value(rec.u, rec.v, rec.p);
                                        vec3 n_tan_raw(2.0 * n_tex.x() - 1.0, 2.0 * n_tex.y() - 1.0, 2.0 * n_tex.z() - 1.0);
                                        vec3 n_tan(n_tan_raw.x() * pbr->normal_strength, n_tan_raw.y() * pbr->normal_strength, n_tan_raw.z());
                                        n_tan = unit_vector(n_tan);
                                        N = unit_vector(n_tan.x() * T + n_tan.y() * B + n_tan.z() * Ngeom);
                                        // Fade normal map at grazing angles
                                        double NdotV_geo = dot(Ngeom, V);
                                        if (NdotV_geo < 0.0) NdotV_geo = 0.0;
                                        double strength = std::clamp(NdotV_geo * 5.0, 0.0, 1.0);
                                        nstrength = pbr->normal_strength;
                                        N = unit_vector(N * strength + Ngeom * (1.0 - strength));
                                    }
                                }
                                // populate arrays for ISPC shaded compute
                                Ngeomx[k] = (float)Ngeom.x(); Ngeomy[k] = (float)Ngeom.y(); Ngeomz[k] = (float)Ngeom.z();
                                Tx[k] = (float)T.x(); Ty[k] = (float)T.y(); Tz[k] = (float)T.z();
                                Bx[k] = (float)B.x(); By[k] = (float)B.y(); Bz[k] = (float)B.z();
                                ntr[k] = (float)n_tex.x(); ntg[k] = (float)n_tex.y(); ntb[k] = (float)n_tex.z();
                                normal_strength_arr[k] = (float)nstrength;

                                Vx[k] = (float)V.x(); Vy[k] = (float)V.y(); Vz[k] = (float)V.z();
                                Lx[k] = (float)Ldir.x(); Ly[k] = (float)Ldir.y(); Lz[k] = (float)Ldir.z();

                                baser[k] = (float)base.x(); baseg[k] = (float)base.y(); baseb[k] = (float)base.z();
                                metallic_arr[k] = (float)metallic_v;
                                dielr[k] = (float)dielectricF0.x(); dielg[k] = (float)dielectricF0.y(); dielb[k] = (float)dielectricF0.z();
                                alpha[k] = (float)perceptual_to_alpha((double)rough_v);
                                lane_map[k] = l;
                                ++k;
                            }

                            if (k > 0) {
                                // Compute full shaded BRDF per lane using ISPC helper
                                ispc_compute_shade(Ngeomx, Ngeomy, Ngeomz,
                                                   Tx, Ty, Tz,
                                                   Bx, By, Bz,
                                                   ntr, ntg, ntb,
                                                   normal_strength_arr,
                                                   Vx, Vy, Vz,
                                                   Lx, Ly, Lz,
                                                   baser, baseg, baseb,
                                                   metallic_arr,
                                                   dielr, dielg, dielb,
                                                   alpha, k,
                                                   out_r, out_g, out_b);
                                for (int pk_i = 0; pk_i < k; ++pk_i) {
                                    int l = lane_map[pk_i];
                                    int global_idx = lane_idx_global[l];
                                    colour Li = pl.radiance * (float)(1.0 / std::max(1e-4, (pl.position - primary_recs[l].p).length_squared()));
                                    colour brdf_col(out_r[pk_i] * (float)Li.x(),
                                                     out_g[pk_i] * (float)Li.y(),
                                                     out_b[pk_i] * (float)Li.z());
                                    tile_direct[global_idx] += brdf_col;

                                    // Add SSS from material if set
                                    hit_record& rec = primary_recs[l];
                                    const material* mptr = rec.mat.get();
                                    const pbr_material* pbr = dynamic_cast<const pbr_material*>(mptr);
                                    if (pbr && pbr->sss_strength > 0.0) {
                                        colour sss = pbr->shade_sss(rec, V, Ldir, Li, world, 1.0);
                                        tile_direct[global_idx] += sss;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        */

        // Main per-pixel rendering loop
        for (int j = y0; j < y1; ++j) {
            for (int i = x0; i < x1; ++i) {
            // We'll perform per-pixel sampling with optional adaptive stopping.
            // Welford accumulators for mean & M2 (per-channel radiance)
            double mean_r = 0.0, mean_g = 0.0, mean_b = 0.0;
            double m2_r = 0.0, m2_g = 0.0, m2_b = 0.0;
            // Welford accumulators for albedo (per-channel)
            double mean_ar = 0.0, mean_ag = 0.0, mean_ab = 0.0;
            double m2_ar = 0.0, m2_ag = 0.0, m2_ab = 0.0; // not used for now but reserved
            // Normal accumulator (vector sum)
            double normal_sum_x = 0.0, normal_sum_y = 0.0, normal_sum_z = 0.0;

            int n = 0;

            // Convenience aliases for adaptive settings
            bool adaptive = this->adaptive_sampling;
            int min_samples = this->adaptive_min_samples;
            int check_interval = this->adaptive_check_interval;
            double rel_thresh = this->adaptive_rel_threshold;
            double abs_thresh = this->adaptive_abs_threshold;

            // Per-sample sanitizer helper (moved outside the tight loop)
            auto samp_sanitize = [](double v) -> double {
                if (!std::isfinite(v) || v <= 0.0) return 0.0;
                return v;
            };

            for (int s = 0; s < spp; ++s) {
                ray r = cam.get_ray(i, j, s);
                // Get one sample's contribution and the first-hit albedo/normal
                colour samp;
                colour samp_albedo(0,0,0);
                vec3   samp_normal(0,0,0);
                const hit_record* prehit_ptr = nullptr;
                const colour* precomputed_direct_ptr = nullptr;
                if (use_primary_prehit && top_bvh) {
                    int local_idx = (j - y0) * (x1 - x0) + (i - x0);
                    int idx = s * tile_w_pixels + local_idx;
                    if (local_idx >= 0 && idx >= 0 && idx < (int)tile_first_hit.size() && tile_first_hit[idx]) {
                        prehit_ptr = &tile_first_rec[idx];
                        precomputed_direct_ptr = &tile_direct[idx];
                    }
                }
                colour* albedo_out = need_aux_aovs ? &samp_albedo : nullptr;
                vec3* normal_out = need_aux_aovs ? &samp_normal : nullptr;
                samp = cam.ray_colour(r, depth, world, albedo_out, normal_out, prehit_ptr, precomputed_direct_ptr);

                double sr = samp_sanitize(samp.x());
                double sg = samp_sanitize(samp.y());
                double sb = samp_sanitize(samp.z());

                // Preserve finite HDR samples. Clipping rare bright paths biases
                // GGX highlights and makes lighting depend on sample ordering.

                // Welford update for each channel (radiance)
                ++n;
                double delta_r = sr - mean_r;
                mean_r += delta_r / n;
                m2_r += delta_r * (sr - mean_r);

                double delta_g = sg - mean_g;
                mean_g += delta_g / n;
                m2_g += delta_g * (sg - mean_g);

                double delta_b = sb - mean_b;
                mean_b += delta_b / n;
                m2_b += delta_b * (sb - mean_b);

                if (need_aux_aovs) {
                    // Incremental mean for albedo (per-channel)
                    double ar = samp_albedo.x();
                    double ag = samp_albedo.y();
                    double ab = samp_albedo.z();
                    double delta_ar = ar - mean_ar;
                    mean_ar += delta_ar / n;
                    double delta_ag = ag - mean_ag;
                    mean_ag += delta_ag / n;
                    double delta_ab = ab - mean_ab;
                    mean_ab += delta_ab / n;

                    // Accumulate normals as simple vector sum (will normalize at end)
                    normal_sum_x += samp_normal.x();
                    normal_sum_y += samp_normal.y();
                    normal_sum_z += samp_normal.z();
                }

                // Adaptive convergence check: after min samples and on intervals
                if (adaptive && n >= min_samples && (n % check_interval) == 0) {
                    // compute sample variances (population estimator uses / (n-1) if n>1)
                    double var_r = (n > 1) ? (m2_r / (n - 1)) : 0.0;
                    double var_g = (n > 1) ? (m2_g / (n - 1)) : 0.0;
                    double var_b = (n > 1) ? (m2_b / (n - 1)) : 0.0;

                    double std_err_r = (n > 0) ? (std::sqrt(var_r) / std::sqrt((double)n)) : 0.0;
                    double std_err_g = (n > 0) ? (std::sqrt(var_g) / std::sqrt((double)n)) : 0.0;
                    double std_err_b = (n > 0) ? (std::sqrt(var_b) / std::sqrt((double)n)) : 0.0;

                    double rel_r = (std::abs(mean_r) > 1e-12) ? (std_err_r / std::abs(mean_r)) : std_err_r;
                    double rel_g = (std::abs(mean_g) > 1e-12) ? (std_err_g / std::abs(mean_g)) : std_err_g;
                    double rel_b = (std::abs(mean_b) > 1e-12) ? (std_err_b / std::abs(mean_b)) : std_err_b;

                    double rel_max = std::max(rel_r, std::max(rel_g, rel_b));

                    // If relative standard error is below threshold, or absolute std_err small, stop
                    if (rel_max < rel_thresh ||
                        (std_err_r < abs_thresh && std_err_g < abs_thresh && std_err_b < abs_thresh))
                    {
                        break;
                    }
                }
            }

            // finalize per-pixel linear value by using the Welford mean
            double scale = 1.0 / std::max(1, n);
            double r_lin = mean_r;
            double g_lin = mean_g;
            double b_lin = mean_b;

            // finalize albedo mean
            double a_lin_r = mean_ar;
            double a_lin_g = mean_ag;
            double a_lin_b = mean_ab;

            int idxf = 3 * (j * w + i);
            vec3 avgN(0, 0, 0);
            if (need_aux_aovs) {
                // finalize normal (average and renormalize)
                avgN = vec3((float)(normal_sum_x * scale), (float)(normal_sum_y * scale), (float)(normal_sum_z * scale));
                // Defer normalization: compute destination index and push raw vector into tile-local buffer
                tile_normals.push_back(avgN.x());
                tile_normals.push_back(avgN.y());
                tile_normals.push_back(avgN.z());
                tile_idxf.push_back(idxf);
            }

            // Sanitize components to avoid NaN/Inf and extremely large values
            auto sanitize = [](double v) -> double {
                if (!std::isfinite(v) || v <= 0.0) return 0.0;
                return v;
            };

            r_lin = sanitize(r_lin);
            g_lin = sanitize(g_lin);
            b_lin = sanitize(b_lin);

            // Exposure (linear multiplier) comes from renderer state
            double exposure = this->exposure;
            r_lin *= exposure; g_lin *= exposure; b_lin *= exposure;

            // Store into HDR float buffer (float32)
            hdr_buffer[idxf + 0] = (float)r_lin;
            hdr_buffer[idxf + 1] = (float)g_lin;
            hdr_buffer[idxf + 2] = (float)b_lin;
            if (need_aux_aovs) {
                // Store albedo AOV (mean albedo per pixel)
                albedo_buffer[idxf + 0] = (float)a_lin_r;
                albedo_buffer[idxf + 1] = (float)a_lin_g;
                albedo_buffer[idxf + 2] = (float)a_lin_b;

                // Store normal AOV (normalized average normal)
                normal_buffer[idxf + 0] = avgN.x();
                normal_buffer[idxf + 1] = avgN.y();
                normal_buffer[idxf + 2] = avgN.z();
            }
        }

        if (progress) {
            // Keep scanline count for partial image display, but don't update ETA
            // (ETA is now computed per-tile after the tile finishes)
            ++progress->completed_scanlines; // atomic
        }

        if (enable_progressive_updates) {
            // Mark this scanline as complete for safe partial display
            row_done[j] = 1;
        }

        // Outside critical section: throttled partial image callback
        if (enable_progressive_updates) {
            // Check cancel flag first - don't waste time on callbacks if cancelled
            if (cancel_flag && cancel_flag->load()) continue;
            
            auto now = clock::now();
            long long now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
            long long last_ms = s_last_progress_ms.load();
            if (now_ms - last_ms >= progress_min_interval_ms) {
                if (s_last_progress_ms.compare_exchange_strong(last_ms, now_ms)) {
                    // Apply progressive denoising if enabled (with separate, less frequent throttle)
                    const std::vector<float>* display_hdr = &hdr_buffer;
                    std::vector<float> display_hdr_copy;
#ifdef HAVE_OIDN
                    if (need_aux_aovs && this->progressive_denoise && this->use_denoiser && this->denoiser_strength > 0.0) {
                        long long last_denoise_ms = s_last_denoise_ms.load();
                        // Only denoise if enough time has passed AND we're not cancelled
                        if (now_ms - last_denoise_ms >= progress_denoise_interval_ms && 
                            (!cancel_flag || !cancel_flag->load())) {
                            if (s_last_denoise_ms.compare_exchange_strong(last_denoise_ms, now_ms)) {
                                // OIDN is not thread-safe - use a static mutex, but try_lock to avoid blocking
                                static std::mutex s_oidn_mutex;
                                std::unique_lock<std::mutex> oidn_lock(s_oidn_mutex, std::try_to_lock);
                                
                                if (oidn_lock.owns_lock()) {
                                    try {
                                        // Double-check cancel flag before expensive OIDN operation
                                        if (cancel_flag && cancel_flag->load()) continue;
                                        
                                        oidn::DeviceRef device = oidn::newDevice();
                                        if (device) {
                                            device.commit();
                                            
                                            const char* errorMessage = nullptr;
                                            if (device.getError(errorMessage) == oidn::Error::None) {
                                                oidn::FilterRef filter = device.newFilter("RT");
                                                if (filter) {
                                                    size_t bufBytes = (size_t)w * (size_t)h * 3 * sizeof(float);
                                                    oidn::BufferRef colorBuf = device.newBuffer(bufBytes);
                                                    oidn::BufferRef outBuf = device.newBuffer(bufBytes);
                                                    oidn::BufferRef albedoBuf = device.newBuffer(bufBytes);
                                                    oidn::BufferRef normalBuf = device.newBuffer(bufBytes);
                                                    
                                                    void* colorPtr = colorBuf.getData();
                                                    if (colorPtr) std::memcpy(colorPtr, hdr_buffer.data(), bufBytes);
                                                    void* albPtr = albedoBuf.getData();
                                                    if (albPtr) std::memcpy(albPtr, albedo_buffer.data(), bufBytes);
                                                    void* nrmPtr = normalBuf.getData();
                                                    if (nrmPtr) std::memcpy(nrmPtr, normal_buffer.data(), bufBytes);
                                                    
                                                    filter.setImage("color", colorBuf, oidn::Format::Float3, w, h);
                                                    filter.setImage("albedo", albedoBuf, oidn::Format::Float3, w, h);
                                                    filter.setImage("normal", normalBuf, oidn::Format::Float3, w, h);
                                                    filter.setImage("output", outBuf, oidn::Format::Float3, w, h);
                                                    filter.set("hdr", true);
                                                    filter.set("srgb", false);
                                                    filter.set("strength", (float)this->denoiser_strength);
                                                    filter.commit();
                                                    
                                                    // Final cancel check before expensive execute
                                                    if (!cancel_flag || !cancel_flag->load()) {
                                                        filter.execute();
                                                        
                                                        void* outPtr = outBuf.getData();
                                                        if (outPtr) {
                                                            // Copy denoised result into a shared cache so the UI can
                                                            // display a stable denoised image while rendering continues.
                                                            {
                                                                std::lock_guard<std::mutex> cache_lock(s_cached_denoised_mutex);
                                                                if (s_cached_denoised_hdr.size() != hdr_buffer.size()) {
                                                                    s_cached_denoised_hdr.resize(hdr_buffer.size());
                                                                }
                                                                std::memcpy(s_cached_denoised_hdr.data(), outPtr, bufBytes);
                                                            }
                                                            s_cached_denoised_ts.store(now_ms);
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                    } catch (const std::exception& e) {
                                        std::fprintf(stderr, "[OIDN ERROR] Progressive denoise exception: %s\n", e.what());
                                    } catch (...) {
                                        std::fprintf(stderr, "[OIDN ERROR] Progressive denoise unknown exception\n");
                                    }
                                }
                                // else: mutex was locked, skip denoising this frame (no blocking)
                            }
                        }
                    }
#endif
                    // If we have a recently cached denoised result, use it for a
                    // stable partial display to avoid flicker between noisy/denoised.
                    {
                        long long cached_ts = s_cached_denoised_ts.load();
                        if (need_aux_aovs && cached_ts != 0 && (now_ms - cached_ts) <= progress_denoise_hold_ms) {
                            std::lock_guard<std::mutex> cache_lock(s_cached_denoised_mutex);
                            if (s_cached_denoised_hdr.size() == hdr_buffer.size()) {
                                display_hdr_copy.resize(hdr_buffer.size());
                                std::memcpy(display_hdr_copy.data(), s_cached_denoised_hdr.data(), hdr_buffer.size() * sizeof(float));
                                display_hdr = &display_hdr_copy;
                            }
                        }
                    }

                    // Build partial 8-bit image from completed scanlines only
                    render_result partial;
                    partial.width = w;
                    partial.height = h;
                    partial.pixels.resize((size_t)w * (size_t)h * 3);

                    // Luminance-based Reinhard tone mapping (preserves color saturation)
                    auto tonemap_reinhard_lum = [](double r, double g, double b, double& out_r, double& out_g, double& out_b) {
                        // Calculate luminance
                        double lum = 0.2126 * r + 0.7152 * g + 0.0722 * b;
                        if (lum < 1e-6) {
                            out_r = out_g = out_b = 0.0;
                            return;
                        }
                        // Tone map luminance only
                        double lum_compressed = lum / (lum + 1.0);
                        // Scale RGB by the compression ratio to preserve saturation
                        double scale = lum_compressed / lum;
                        out_r = r * scale;
                        out_g = g * scale;
                        out_b = b * scale;
                    };

                    for (int yy = 0; yy < h; ++yy) {
                        bool done_line = (row_done[yy] != 0);
                        for (int xx = 0; xx < w; ++xx) {
                            int idx = 3 * (yy * w + xx);
                            if (done_line) {
                                int hidx = idx;
                                double r_lin = (double)(*display_hdr)[hidx + 0];
                                double g_lin = (double)(*display_hdr)[hidx + 1];
                                double b_lin = (double)(*display_hdr)[hidx + 2];
                                double r_t, g_t, b_t;
                                tonemap_reinhard_lum(r_lin, g_lin, b_lin, r_t, g_t, b_t);
                                double r_g = linear_to_gamma(r_t);
                                double g_g = linear_to_gamma(g_t);
                                double b_g = linear_to_gamma(b_t);
                                partial.pixels[idx + 0] = (std::uint8_t)(int(256 * intensity.clamp(r_g)));
                                partial.pixels[idx + 1] = (std::uint8_t)(int(256 * intensity.clamp(g_g)));
                                partial.pixels[idx + 2] = (std::uint8_t)(int(256 * intensity.clamp(b_g)));
                            } else {
                                partial.pixels[idx + 0] = 0;
                                partial.pixels[idx + 1] = 0;
                                partial.pixels[idx + 2] = 0;
                            }
                        }
                    }

                    try {
                        progress_callback(partial);
                    } catch (...) {
                        // swallow any callback exceptions to avoid breaking render
                    }
                }
            }
        }
    }

        // After finishing a tile, batch-normalize the collected normals (fast path)
        int normals_count = need_aux_aovs ? (int)tile_idxf.size() : 0;
        if (normals_count > 0) {
            // normalize in-place (ISPC optimized when available)
            ispc_normalize_batch(tile_normals.data(), normals_count);

            // Write normalized normals back into the global normal AOV buffer
            for (int k = 0; k < normals_count; ++k) {
                int idf = tile_idxf[k];
                normal_buffer[idf + 0] = tile_normals[3*k + 0];
                normal_buffer[idf + 1] = tile_normals[3*k + 1];
                normal_buffer[idf + 2] = tile_normals[3*k + 2];
            }
        }

        // Update ETA using per-tile exponential moving average
        if (progress) {
            // Measure this tile's duration
            auto now = clock::now();
            double tile_seconds = std::chrono::duration<double>(now - tile_start).count();
            
            int tdone = ++completed_tiles; // atomic increment
            progress->completed_tiles.store(tdone);

            // Update EMA and ETA only every few tiles to reduce noise
            const int eta_update_interval = 8; // update every N tiles
            if (tdone % eta_update_interval == 0 || tdone >= num_tiles) {
                #pragma omp critical
                {
                    // Keep the old EMA for compatibility but also record the tile time
                    const double EMA_ALPHA = 0.05; // smooth long-term average
                    double prev_avg = avg_tile_time.load();
                    double new_avg = (prev_avg <= 0.0) ? tile_seconds : (EMA_ALPHA * tile_seconds + (1.0 - EMA_ALPHA) * prev_avg);
                    if (!std::isfinite(new_avg) || new_avg <= 0.0) {
                        new_avg = std::max(0.001, tile_seconds);
                    }
                    avg_tile_time.store(new_avg);

                    // Push this tile time into the recent window and cap its size
                    recent_tile_times.push_back(tile_seconds);
                    if ((int)recent_tile_times.size() > RECENT_TILE_WINDOW) recent_tile_times.pop_front();

                    // Compute median of recent tile times to avoid bias from startup/outliers
                    double median_tile = new_avg;
                    if (!recent_tile_times.empty()) {
                        std::vector<double> tmp(recent_tile_times.begin(), recent_tile_times.end());
                        std::sort(tmp.begin(), tmp.end());
                        size_t m = tmp.size() / 2;
                        if (tmp.size() % 2 == 1) median_tile = tmp[m];
                        else median_tile = 0.5 * (tmp[m-1] + tmp[m]);
                        // sanitize
                        if (!std::isfinite(median_tile) || median_tile <= 0.0) median_tile = new_avg;
                    }

                    // Use the median (recent window) for ETA; this reduces overestimates caused
                    // by a few slow startup tiles or occasional heavy tiles.
                    int remaining = std::max(0, num_tiles - tdone);
                    int workers = 1;
#ifdef _OPENMP
                    workers = std::max(1, omp_get_max_threads());
#endif
                    double eta = median_tile * remaining / (double)workers;
                    if (eta < 0.0) eta = 0.0;

                    // Update elapsed time from render start
                    double elapsed = std::chrono::duration<double>(now - render_start).count();

                    progress->eta_seconds.store(eta);
                    progress->elapsed_seconds.store(elapsed);
                }
            }
        }
    }

    // Optionally run OpenImageDenoise on the linear HDR buffer
    std::vector<float> denoised_buffer;
    denoised_buffer = hdr_buffer; // default: copy

#ifdef HAVE_OIDN
    if (this->use_denoiser && need_aux_aovs) {
        try {
            const int pixels = w * h;
            oidn::DeviceRef device = oidn::newDevice();
            device.commit();
            oidn::FilterRef filter = device.newFilter("RT");

            // Create device-accessible buffers and copy host HDR data into them
            size_t bufBytes = (size_t)pixels * 3 * sizeof(float);
            oidn::BufferRef colorBuf = device.newBuffer(bufBytes);
            oidn::BufferRef outBuf = device.newBuffer(bufBytes);
            // Copy HDR floats into the device buffer
            void* colorPtr = colorBuf.getData();
            if (colorPtr) {
                std::memcpy(colorPtr, hdr_buffer.data(), bufBytes);
            }

            // Create and copy AOV buffers (albedo + normal) into device buffers
            oidn::BufferRef albedoBuf = device.newBuffer(bufBytes);
            oidn::BufferRef normalBuf = device.newBuffer(bufBytes);
            void* albPtr = albedoBuf.getData();
            if (albPtr) {
                std::memcpy(albPtr, albedo_buffer.data(), bufBytes);
            }
            void* nrmPtr = normalBuf.getData();
            if (nrmPtr) {
                std::memcpy(nrmPtr, normal_buffer.data(), bufBytes);
            }

            filter.setImage("color", colorBuf, oidn::Format::Float3, w, h);
            filter.setImage("albedo", albedoBuf, oidn::Format::Float3, w, h);
            filter.setImage("normal", normalBuf, oidn::Format::Float3, w, h);
            filter.setImage("output", outBuf, oidn::Format::Float3, w, h);
            filter.set("hdr", true);
            filter.set("srgb", false);
            // strength: 0.0 = off, typical 0.2..0.5
            filter.set("strength", (float)this->denoiser_strength);
            filter.commit();
            filter.execute();

            // Copy denoised data back to host-visible vector
            void* outPtr = outBuf.getData();
            if (outPtr) {
                std::memcpy(denoised_buffer.data(), outPtr, bufBytes);
            }
        } catch (const std::exception& e) {
            std::fprintf(stderr, "OIDN exception: %s\n", e.what());
        } catch (...) {
            std::fprintf(stderr, "Unknown exception while running OIDN\n");
        }
    }
#else
    (void)denoised_buffer; // silence unused warning when OIDN disabled
#endif

    // Final tonemap/gamma pass: convert denoised linear floats -> 8-bit
    // Luminance-based Reinhard tone mapping (preserves color saturation)
    auto tonemap_reinhard_lum = [](double r, double g, double b, double& out_r, double& out_g, double& out_b) {
        // Calculate luminance
        double lum = 0.2126 * r + 0.7152 * g + 0.0722 * b;
        if (lum < 1e-6) {
            out_r = out_g = out_b = 0.0;
            return;
        }
        // Tone map luminance only
        double lum_compressed = lum / (lum + 1.0);
        // Scale RGB by the compression ratio to preserve saturation
        double scale = lum_compressed / lum;
        out_r = r * scale;
        out_g = g * scale;
        out_b = b * scale;
    };

    for (int j = 0; j < h; ++j) {
        for (int i = 0; i < w; ++i) {
            int idx = 3 * (j * w + i);
            double r_lin = (double)denoised_buffer[idx + 0];
            double g_lin = (double)denoised_buffer[idx + 1];
            double b_lin = (double)denoised_buffer[idx + 2];

            double r_t, g_t, b_t;
            tonemap_reinhard_lum(r_lin, g_lin, b_lin, r_t, g_t, b_t);

            double r_g = linear_to_gamma(r_t);
            double g_g = linear_to_gamma(g_t);
            double b_g = linear_to_gamma(b_t);

            int rbyte = int(256 * intensity.clamp(r_g));
            int gbyte = int(256 * intensity.clamp(g_g));
            int bbyte = int(256 * intensity.clamp(b_g));

            out.pixels[idx + 0] = static_cast<unsigned char>(rbyte);
            out.pixels[idx + 1] = static_cast<unsigned char>(gbyte);
            out.pixels[idx + 2] = static_cast<unsigned char>(bbyte);
        }
    }

    // Free large temporary HDR buffers before returning to reduce peak resident memory
    {
        std::vector<float>().swap(hdr_buffer);
        std::vector<float>().swap(albedo_buffer);
        std::vector<float>().swap(normal_buffer);
        std::vector<float>().swap(denoised_buffer);
    }

    return out;
}
