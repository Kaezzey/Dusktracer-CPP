# Sunlight, glass and subsurface transport

Sunlight uses a uniform finite disc in both CPU and Vulkan rendering. New suns
have a 0.266-degree angular radius. Existing lights saved with zero radius use
that same radius; positive radii are clamped to at least 0.01 degrees. Light
strength remains integrated incident radiance, and the radiance seen by a
refracted ray is strength divided by the disc solid angle. Dielectric Fresnel,
refraction and radiance-mode IOR factors are shared with emissive-light paths.

The editor no longer builds additive photon maps, and the integrator no longer
adds photon or single-sphere MNEE estimates. Legacy settings are still readable
but inactive. Small light discs can produce noisy caustics with unidirectional
path tracing. Use sufficient path depth for the receiver, glass entry and exit,
and fixed sampling when judging convergence; early adaptive termination and
denoising can conceal rare paths. Point lights remain delta lights and their
specular-chain caustics require a dedicated estimator, which is not supplied by
this change.

SSS uses normalized Burley spatial diffusion in the CPU and GPU path tracers. Strength
is the fraction of the diffuse substrate replaced by scattering, in [0,1],
rather than extra light added to a complete diffuse lobe. Radius is a
world-space diffusion distance. Scatter tint is bounded reflectance and is
applied once; IOR controls the normalized dielectric entry-direction term.

A subsurface event samples an entry point by surface projection using a mixture
of three axes. The spatial PDF includes the area Jacobian and the intersection
selection probability for every axis. Entry points must belong to the same
object instance and material as the outgoing point. Probes preserve shutter
time and recover outward geometric normals. Lighting, shadow visibility and
indirect continuation are evaluated at the sampled entry point; area-light and
sun estimates use complementary MIS with those continuation rays.

All legacy nonzero SSS model IDs now select Burley diffusion. The editor exposes
None and Burley diffusion instead of claiming distinct Single, Skin or Foliage
solvers. Stored strengths are preserved, and the UI now edits their actual 0..1
values instead of multiplying the slider by 0.1. Legacy scale and sample fields
remain in project files; radius and pixel samples control distance and
convergence. GPU requests execute the spatial sampler and entry lighting with
hardware ray queries; active SSS no longer requires CPU fallback.

Diffusion is an approximation for optically thick scattering material, not a
volumetric random walk. Very thin sheets and disconnected shells within one
mesh remain limitations of this Euclidean-distance profile. Separate object
instances never exchange subsurface light even when they share a material.

## Verification

Build and run the regression suite:

```powershell
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The transport regressions compare sunlight against an equal-radiance distant
emissive sphere through glass, verify legacy zero-radius sunlight on CPU and
GPU, and ensure saved caustic gain settings cannot add light. SSS checks cover
independent profile integrals and radial moments, directional normalization,
white-furnace energy, tint, shared-material object isolation, moving instances,
backlighting, external shadows, indirect lighting and emissive-light MIS.

Comparison images can be regenerated with:

```powershell
.\build\Release\DuskPbrTests.exe .\build\material-transport-chart.png --materials
```

This saves glass-sun.png, glass-emissive.png, subsurface-off.png and
subsurface-on.png under build. The glass pair uses a 12-degree disc at 2048 spp
for practical visual inspection; production sunlight defaults to 0.266 degrees
and converges more slowly. The SSS pair changes only scattering strength.
Denoising and adaptive sampling are disabled in these comparisons.

GPU checks compare furnace energy, backlighting, external shadows, tint and
IOR, transformed instances, mesh slots, legacy IDs and emissive-light MIS with
the CPU. On/off GPU images are saved under `build/gpu-fixtures`. Raw HDR samples
remain accessible independently of optional firefly reconstruction and OIDN.

## References

- [Pixar: An Approximate Reflectance Profile for Efficient Subsurface Scattering](https://graphics.pixar.com/library/ApproxBSSRDF/SIGGRAPH_Tech_Talk.pdf)
- [PBRT: Sampling Subsurface Reflection Functions](https://www.pbr-book.org/3ed-2018/Light_Transport_II_Volume_Rendering/Sampling_Subsurface_Reflection_Functions)
