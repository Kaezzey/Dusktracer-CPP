# Vulkan hardware ray tracing

Choose **CPU** or a compatible GPU in the device selector beside **Render**.
CPU remains the default. Devices are discovered asynchronously and identified
by Vulkan device UUID, rather than enumeration order. A failed GPU render
reports its reason in the viewport and retries on the CPU. Cancelling a render
does not start a fallback render.

## Build and runtime requirements

The optional backend uses Vulkan 1.2 with `VK_KHR_acceleration_structure`,
`VK_KHR_deferred_host_operations`, `VK_KHR_ray_query`, buffer device addresses,
and a compute queue. It traces rays against hardware acceleration structures
from a compute shader. It does not use a software BVH traversal shader.

```powershell
cmake -S . -B build -DDUSK_ENABLE_VULKAN_RT=ON -DDUSK_BUILD_GRAPH_TESTS=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The Vulkan SDK and `glslc` are needed at build time. Shader compilation produces
embedded SPIR-V, so the executable has no runtime dependency on shader files or
the SDK. A missing SDK produces a CPU-only build; explicitly disable discovery
with `-DDUSK_ENABLE_VULKAN_RT=OFF`. The Windows system Vulkan loader is loaded
dynamically. Missing drivers therefore do not prevent the editor from starting.

## Ownership and data flow

1. The editor copies scene metadata, camera, renderer settings, and device choice
   into the render worker. Subsequent edits apply to the next render.
2. `gpu_scene.cpp` and the shared `gpu_material.cpp` lower only used materials and reachable graph instructions
   into owned upload records. It imports mesh geometry with the CPU importer’s
   normalization and axis conventions, and exports the CPU transform matrix.
3. `vulkan_rt.cpp` owns one device, queue, and command pool for that render.
   `upload_scene`, `build_world`, and `create_pipeline` prepare its resources.
   BLAS geometry is shared between instances of each mesh asset; the TLAS keeps
   each instance’s transform and material-slot mapping.
4. The compute shader accumulates scene-linear HDR, albedo, normals, and variance
   in a storage buffer. Work is submitted in bounded 256-by-256 tiles, up to four samples per dispatch.
   Cancellation is checked between submissions and preparation steps. A four-byte
   convergence readback ends adaptive renders when every pixel has converged.
5. Readback occurs at most every 750 ms during tracing. The existing
   `render_result` handoff carries progressive/final RGB pixels; OpenGL uploads
   remain on the main thread. Final output supports OIDN, exposure, luminance
   Reinhard mapping, and sRGB encoding.

Fences complete before buffers are read or released. Explicit barriers cover
upload-to-build, BLAS-to-TLAS, build-to-trace, accumulation, and readback.
Upload ranges copy directly into mapped staging memory, without an intermediate
scene-sized byte vector. Scratch, staging, and CPU upload records are released
after their last use.
All remaining resources are released by the noncopyable context on success,
cancellation, or exceptions. Device loss is reported through the same fallback
path; an actual device-loss event is not simulated by the tests.

A successful GPU render never builds a CPU BVH. Fallback constructs one from
the same scene snapshot on the render worker. There is no cross-render GPU cache
for geometry or materials: rebuilding each render avoids stale scene resources.
The driver pipeline binary is cached separately under `.dusk-cache/vulkan`, with
shader fingerprint, driver/device identifiers, Vulkan cache UUID, length, and
checksum validation. A stale or corrupt cache rebuilds automatically.

## Supported rendering

- Analytic spheres/ellipsoids, cube faces, and imported triangle meshes, including mesh
  instancing, material-slot overrides, UVs, smooth normals, and tangent frames.
- Lambert, legacy fuzzy metal, ideal dielectric, emissive, isotropic surface,
  and the existing single-scattering GGX PBR material.
- Every current graph operation, scalar broadcast, output channels, UV tiling,
  automatic/explicit texture color spaces, normal mapping, and opacity.
- HDR emission and textures; ordinary textures occupy one RGBA8 word per texel.
  HDR texels retain four float channels. Texture sampling is nearest/clamped,
  with repeat on connected UV graph inputs, matching the CPU implementation.
- Environment background, finite/delta sun, point lights, emissive surface
  sampling, MIS, glass Fresnel/refraction weighting, and partial-opacity shadows.
- Camera transforms, field of view, depth of field, sample/depth settings,
  progressive results, exposure, and adaptive variance checks.

GPU graphs use liveness-based register reuse with 32 simultaneous values. Long
chains can use all 256 source nodes; wider graphs that exceed the live-value
limit fall back explicitly to CPU. GPU arithmetic is float32; the CPU generally
uses double precision. Pixel-identical Monte Carlo noise is not expected.

## Deliberate limits and differences

Active subsurface scattering and experimental photon/MNEE caustics require CPU
rendering. They are not silently omitted. The PBR model retains the CPU model’s
single-scattering and normal-mapping approximations; this backend adds no new
Principled layers or multiple-scattering compensation.

The GPU also samples emissive mesh triangles directly. The current CPU backend
reaches those through BSDF paths. These estimates have different variance but
the same intended radiance. Point-light selection is uniform on the GPU and
importance-weighted on the CPU; both include their selection probabilities.

Progressive GPU previews are undenoised. Final OIDN runs on its CPU device to
avoid another GPU context competing for memory. The strength control blends
the filtered result with the raw image (full filtering by default). The viewport
reports the final denoising phase and any error; errors preserve the raw image. Maximum GPU depth is 128 and dimensions
are limited to 8192 per axis, subject to the device’s actual storage/memory limits.
Scene import and driver pipeline/AS creation cannot be interrupted midway through
an individual API call. GPU submissions are drained before cancellation returns.
The interactive sphere preview uses the CPU material evaluator with reduced
preview textures; it is a studio-light material preview, not a scene path trace.

## Validation

`DuskGpuTests` executes actual hardware queries. A machine without a compatible
GPU skips hardware checks with CTest code 77; a Vulkan-disabled build tests the
CPU-only fallback. Tests cover graph arithmetic/channel/color-space semantics,
long graph register reuse, UVs, alpha, transformed material slots, normal maps,
glass entry/exit, area-light sample-count invariance, point lights, cancellation,
adaptive sampling, device-selection failure, and a production material chart.

Enable Khronos validation during hardware checks:

```powershell
$env:DUSK_VK_VALIDATION = '1'
$env:VK_LAYER_VALIDATE_SYNC = '1'
& .\build\Release\DuskGpuTests.exe .\build\gpu-validation.png
```

This requires the SDK validation layer. Validation errors fail the hardware test.
The development RTX 4070 passed the expanded suite with no reported validation
errors. Rotated UVs, alpha cutouts, and emissive mesh-slot fixtures matched CPU
display pixels exactly; the mapped-normal mesh fixture averaged approximately
0.08/255 absolute difference. The stochastic PBR fixture averaged approximately
1/255. These are fixture results, not a guarantee for all scenes or GPUs.

The comparison uncovered and fixed a CPU triangle-construction bug that discarded
imported bitangent handedness. CPU and GPU now prepare vertex frames with the
same handedness-preserving helper. A separate CPU regression covers mirrored UVs.

References: [Khronos ray-tracing guide](https://docs.vulkan.org/guide/latest/extensions/ray_tracing.html),
[acceleration-structure specification](https://docs.vulkan.org/spec/latest/chapters/accelstructures.html),
and [synchronization specification](https://docs.vulkan.org/spec/latest/chapters/synchronization.html).

For a short scheduling and denoising regression/benchmark, run `DuskGpuTests
build/gpu-validation.png --scheduling`. `DUSK_GPU_BATCH_SAMPLES=1` is a diagnostic
override for comparing single-sample submissions with the default batch of four;
values are clamped to 1–4. Batching preserves RNG sequences and exact HDR pixels.

Final filtering follows the [OIDN API](https://www.openimagedenoise.org/documentation.html):
HDR color with albedo/normal guides, checked device errors, and cancellable execution.
Strength is an application-side blend; it is not passed as a filter parameter.

`DUSK_GPU_TIMINGS=1` reports instance/device setup, upload, acceleration-structure
builds, pipeline creation, tracing, and output resolve separately. On this RTX
4070, the small scheduling fixture spent about 1.45 s creating an uncached pipeline
and about 28–31 ms loading the cached pipeline. Total times were approximately
1.64 s cold and 0.23 s warm. These timings include setup and are not general scene
throughput claims.
