# Bucket rendering

Renders are split into square buckets that render in parallel, like Cycles
before 3.0. A bucket appears noisy and refines in place until it reaches its
sample count; once its neighbours have finished too, it is denoised. While a
render runs, the viewport marks buckets being traced with amber corners and
buckets being denoised with blue corners.

## How Cycles did it

Cycles' tile renderer (Blender 2.93 and earlier) behaves as follows, and these
buckets copy it:

- Inside a tile the outer loop is over samples: each pass traces one sample
  for every pixel of the tile and reports progress, so a tile starts noisy and
  cleans up.
- A tile is denoised only when all eight neighbours have rendered, since the
  denoiser needs real pixels around the tile. Denoising has priority over
  starting new tiles.
- Tile orders include Hilbert Spiral, Center, and left/right/top/bottom sweeps.

## Settings

Render settings, **Buckets** section:

| Setting | Default | Meaning |
| --- | --- | --- |
| Bucket Order | Spiral | Spiral out from the centre, Hilbert curve, or rows from the top |
| CPU Bucket Size | 64 px | Each CPU thread renders one bucket at a time |
| GPU Mode | Buckets | Buckets finishes a few 256 px buckets at a time; Progressive refines the whole frame |
| Denoise Buckets While Rendering | On | Denoise finished buckets in the viewport |

These settings are saved with the project (scene format 7).

## Denoising buckets

OpenImageDenoise looks well beyond the pixel it filters, so a bucket denoised
on its own shows seams where its neighbours are still black. A bucket is
therefore denoised once every bucket in its padded window has finished. The
window is the bucket plus half a bucket (at most 32 px) of real neighbouring
pixels on each side, shifted inward at image edges so one OIDN filter serves
every bucket. Only the bucket itself is written to the viewport.

The saved image is still denoised as one frame after the last bucket, so it
matches a render with bucket denoising off. Bucket denoising runs on one
background thread; CPU renders give it a quarter of the cores.

## CPU

`renderer::render` orders buckets with `dusk_tiles::make_tiles` and hands them
to OpenMP in that order. Each thread traces its bucket in passes of one sample
per unconverged pixel, writing the running means after every pass. A
publisher thread refreshes the viewport every 100 ms. It re-encodes only the
buckets that have new passes, then pastes denoised buckets on top, so render
threads never tone-map or wait for the UI.

## Vulkan

In Buckets mode, a group of buckets (4, or `DUSK_GPU_TILE_GROUP`) takes every
sample before the next group starts. Per-pixel random sequences depend only on
the pixel and sample index, so Buckets and Progressive produce identical HDR
images. Adaptive sampling stops a group once all its pixels converge. About
seven times a second, a preview runs the display pass over just the group's
pixels and reads back only those, keeping the rest of the frame on the host.
When a group finishes, its accumulators are copied to the host for bucket
denoising.

## Performance

Buckets and Progressive take the same time on the GPU. To compare them on
your machine, run `DuskGpuTests out.png --bucket-benchmark`, a 1080p,
256-sample render with editor settings. Set `DUSK_GPU_TIMINGS=1` to see GPU busy
time and the host time spent submitting, previewing, checking convergence
and reading buckets back.

Previews and bucket readbacks read GPU results from host-cached memory.
Uncached (write-combined) memory made each 1080p preview cost about 700 ms
and each bucket readback about 360 ms, which stalled the GPU between
submissions.

To watch the sequence outside the editor, render a saved project and keep the
viewport frames:

```
DuskGpuTests out.png --render-project Scene.dusk image.png 512 1280 frames
```

Add `cpu` after the frames folder to use the CPU renderer.

## Implementation

- `include/core/render_tiles.h`, `src/core/render_tiles.cpp`: bucket orders,
  the shared display transform, and the bucket denoiser.
- `render_progress_state` in `include/core/renderer.h`: active buckets for the
  viewport overlay.
- `src/core/renderer.cpp` and `src/gpu/vulkan_rt.cpp`: the two bucket loops.

## Verification

`DuskGpuTests` checks that every order covers each pixel once, that a CPU
bucket refines all its pixels together, that a bucket waits for its neighbours, that bucket denoising matches full-frame denoising
(mean difference below half a display level), CPU bucket progress and frames,
that GPU Buckets and Progressive trace identical samples, that the GPU reports
the bucket it traces, that bucket denoising leaves the final frame unchanged,
and that adaptive sampling converges group by group.
