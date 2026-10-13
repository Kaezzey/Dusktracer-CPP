# Sunlight, glass and subsurface transport

Sunlight uses a uniform finite disc in both CPU and Vulkan rendering. New suns
have a 0.266-degree angular radius. Existing lights saved with zero radius use
that same radius; positive radii are clamped to at least 0.01 degrees. Light
strength remains integrated incident radiance, and the radiance seen by a
refracted ray is strength divided by the disc solid angle. Dielectric Fresnel,
refraction and radiance-mode IOR factors are shared with emissive-light paths.

The editor no longer builds additive photon maps. Legacy caustic gain settings
are readable but inactive. CPU and GPU now directly sample refracted sun paths
through homogeneous, uniformly scaled glass spheres. The spherical Snell map
is split at its folds, enumerating every transmitted branch. Its solid-angle
Jacobian supplies the focusing factor; exact Fresnel transmission and both
interface tints supply throughput. Complementary MIS weights on ordinary
two-interface paths prevent double counting. Connections obey depth limits and
check occlusion before, within and after the sphere. Offsetting specular rays
along their direction preserves the refracted line through the lens.
The expensive sphere solve uses up to two sun-disc proposals per shading event;
its MIS uses that actual count, independently of higher shadow sample counts.

This sampler supports the primary two-transmission sun caustic, including
multiple glass spheres as separate candidates. Internal reflections, chains
through several objects, nonuniformly scaled ellipsoids and glass meshes retain
ordinary path sampling. Point-light caustics also retain ordinary transport.
Small-source paths in these cases still need enough samples. MIS and direct
light sampling must be enabled to use the sphere proposal; disabling them keeps
the ordinary estimator. No caustic gain or radiance clamp is applied.

SSS uses normalized Burley spatial diffusion in the CPU and GPU path tracers. Strength
is the fraction of the diffuse substrate replaced by scattering, in [0,1],
rather than extra light added to a complete diffuse lobe. Radius is a
world-space diffusion distance with independent RGB multipliers. Scatter tint is bounded reflectance and is
applied once; IOR controls the normalized dielectric entry-direction term.

A subsurface event samples an entry point by surface projection using a mixture
of three axes and three spectral distance proposals. The spatial PDF includes
all channels, the area Jacobian and the intersection
selection probability for every axis. Entry points must belong to the same
object instance and material as the outgoing point. Probes preserve shutter
time and recover outward geometric normals. Lighting, shadow visibility and
indirect continuation are evaluated at the sampled entry point; area-light and
sun estimates use complementary MIS with those continuation rays.

All legacy nonzero SSS model IDs now select Burley diffusion. The editor exposes
None, the two Unreal models described below and Burley diffusion (legacy)
instead of claiming distinct Single, Skin or Foliage solvers. Stored strengths are preserved, and the UI now edits their actual 0..1
values instead of multiplying the slider by 0.1. Legacy scale and sample fields
remain in project files; radius and pixel samples control distance and
convergence. GPU requests execute the spatial sampler and entry lighting with
hardware ray queries; active SSS no longer requires CPU fallback.

Diffusion is an approximation for optically thick scattering material, not a
volumetric random walk. Very thin sheets and disconnected shells within one
mesh remain limitations of this Euclidean-distance profile. Separate object
instances never exchange subsurface light even when they share a material.

The **Skin** profile uses relative RGB distances `(1, 0.372, 0.186)`, so red
travels farther than green or blue and warms cast-shadow boundaries and the
light terminator. These ratios derive from Jensen et al.'s Skin1 reduced
scattering and absorption data via `1/sqrt(3*sigma_a*(sigma_a+sigma_s'))`.
The preset sets IOR 1.3, strength 1 and radius 0.0037 scene units (3.7 mm when
one unit is one metre), preserving the authored surface colour and roughness.
Adjust Radius for imported model scale. This remains an RGB diffusion
approximation, not a layered anatomical skin model or an added shadow-edge glow.
**Neutral** preserves existing equal-channel diffusion; **Custom** exposes RGB
multipliers. CPU and GPU use the same profile and channel-mixture PDF.

## Unreal Subsurface and Subsurface Profile

The material's SSS **Shading Model** selects None, Subsurface (Unreal),
Subsurface Profile (Unreal) or Burley Diffusion (legacy). Scenes saved before
format version 4 load as legacy Burley diffusion and render unchanged.
`lower_subsurface` in `scene.cpp` converts the editor parameters into one
description. The CPU tracer, the Vulkan tracer and the raster viewport all
consume that description, so the backends cannot drift apart.

The implementation follows the published formulas and Unreal's documented
behaviour. It does not copy Unreal Engine source code.

### Subsurface (Unreal)

This model adds Unreal's transmission term to the ordinary PBR lobes:

```text
InScatter    = saturate(dot(L, -V))^12 * lerp(3, 0.1, Opacity)
BackScatter  = saturate(dot(N, H) * Opacity + 1 - Opacity) / (2 * pi)
Transmission = SubsurfaceColor * lerp(BackScatter, 1, InScatter)
```

The term already includes its geometry, so a light of radiance `L` contributes
`L * Transmission`, for lights on either side of the surface. Opacity 0 wraps
light all the way around a thin object; opacity 1 follows the surface normal.
With **Multiply by Base Color** (on by default), Subsurface Color is multiplied
by the textured base colour, as when an Unreal material wires its colour map
into Subsurface Color.

Light behind the geometric surface crosses the object's own body. Its
thickness is measured to the object's surface nearest the light, and the light
is attenuated by `exp(-thickness / Transmission Distance)`. Other objects block
light as usual. Unreal derives this shadowing from the Opacity input; Dusk
exposes the extinction length directly, in scene units. Choose it relative to
the object: a distance much shorter than the object's radius cuts the wrap light
off within a few degrees past the terminator. The default is 0.5.

As in Unreal, the transmission term applies to direct lights only: the sun,
point lights and emissive area lights, through light sampling and without MIS.
Indirect light treats the subsurface colour as extra Lambert diffuse, the way
Unreal treats it for ambient and global illumination. The tracers sample that
ambient lobe separately and ignore lights that it reaches directly, so no light
is counted twice. Because a path tracer bounces light repeatedly, the ambient
colour is limited so that diffuse and ambient together reflect at most all
incident light; the viewport's ambient fill uses the same limit.

### Subsurface Profile (Unreal)

This model uses the existing spatial Burley diffusion with Unreal's Burley
profile parameters. Each channel's shape distance is `d = MFP / s(A)`, where:

- `MFP` is **Mean Free Path Color** times **Mean Free Path Distance**, in
  profile units of **World Unit Scale** centimetres (Unreal's convention),
  converted with **Scene Units per Metre**.
- `A` is **Surface Albedo**.
- `s(A) = 1.85 - A + 7 * |A - 0.8|^3` (Christensen and Burley, 2015).

The base colour times **Tint** colours the scattered light. **Subsurface Mask**
(Unreal's Opacity input) is the fraction of diffuse light that scatters.
**Entry IOR** sets the entry Fresnel term. Specular uses two GGX lobes, with
roughness multiplied by **Specular Roughness 0** and **Specular Roughness 1**,
weighted `1 - mix` and `mix`. Masking uses the mixed roughness.

The defaults are Unreal's default skin profile as listed in the UE 4.27
`SubsurfaceProfileStruct` API reference: Surface Albedo (0.91058, 0.338275,
0.2718), Mean Free Path Color (1.0, 0.088964, 0.072095), Mean Free Path
Distance 2.6748 with World Unit Scale 0.1 cm (2.67 mm), Roughness 0.75 and 1.3,
Lobe Mix 0.85 and IOR 1.55. Scene Units per Metre defaults to 1.

With these colours, green and blue scatter about 26 and 38 times less far
than red, so beyond the terminator only red light survives. Raising the mean
free path widens that red band but leaves the white-to-red transition abrupt;
raising the green and blue Mean Free Path Color softens it.

Dusk does not implement Unreal's Scattering Distribution, Extinction Scale,
Normal Scale, Boundary Color Bleed or Transmission Tint, which tune Unreal's
screen-space approximation rather than a spatial integral. Separable
(pre-Burley) profiles are not supported. Subsurface colour and the scattering
mask are material scalars; the material graph has no outputs for them yet.

### Raster preview parity

The raster viewport evaluates both models with the tracers' functions. When a
scene contains subsurface materials, the shadow pass also writes RG32F
light-space surface maps. Each texel stores depth toward the sun (or distance
from the first shadowed point light), and a 16-bit object and material identity
with the cosine to the light packed into its fraction.

- **Thickness** reads the same surface nearest the light that the tracers find.
- **Diffusion** evaluates the tracers' integral of the Burley profile times
  entry lighting over the lit surface of the same object and material. Its 36
  taps importance-sample the tracers' channel mixture in the light's plane; the
  projection Jacobian cancels the entry cosine, and the receiver's tangent plane
  supplies tap depth within shadow-map precision. This carries light across
  cast-shadow edges, around terminators and through thin backlit geometry.
  Tap radii and densities depend only on the material, so the CPU computes them
  once per material. Taps within 1.5 shadow texels of the receiver use its
  tangent plane, lit with its filtered shadow visibility, so the shadow map's
  texel staircase never shows along terminators. When the whole kernel fits in
  one shadow texel, the integral reduces to local entry lighting, which the
  viewport uses directly.
- The material sphere preview evaluates the same integrals analytically for its
  unit sphere.

Shading decisions use the interpolated normal, which matches the tracers'
smooth normal on spheres; the faceted triangle normal only positions shadow-map
lookups. The sun's normal offset grows at grazing angles so lit surfaces clear
their own silhouette, and surfaces behind a receiver, such as a floor beyond the
silhouette, never count as blockers for transmitted light.

Limits: only the sun and the first point light have surface maps. Other point
lights use local entry lighting and no thickness. Taps rotate per pixel, which
leaves fine noise. Ambient fill and indirect light are not diffusion-integrated.
Raster spheres are tessellated, so silhouettes and terminators differ slightly
from the tracers' analytic spheres. When a diffusion distance approaches the
object's own size, most of its 12 taps per channel fall outside the object and
the integral becomes coarse; on a sphere whose red distance exceeded its radius
the viewport measured 6% less red than the tracer. Identities repeat every 255 objects, so two
such objects touching each other could exchange diffused light in the viewport.

Diffusion costs time in proportion to screen coverage. On the development
RTX 4070, a Subsurface Profile sphere filling a 1280x720 viewport took about
7.2 ms per frame at the parity fixture's scale and 5.5 ms at real skin scale,
compared with 1.2 ms without subsurface scattering. Fewer taps were faster but
fell below 90% accuracy on thin geometry. To measure your GPU, set
`DUSK_SSS_TIMING=1` and run the raster checks.

`raster_materials_and_shadows` renders the viewport's linear direct-lighting
view and CPU path tracing through the same pixel centres at depth one. It
requires 90% accuracy (one minus the relative L1 difference) over each object's
interior. The development RTX 4070 measured:

| Fixture | Accuracy | Energy ratio |
| --- | --- | --- |
| Subsurface, sun behind | 98.8% | 1.012 |
| Subsurface, opacity 0, sun to the side | 99.8% | 1.000 |
| Subsurface, point light behind | 99.4% | 1.006 |
| Subsurface Profile, sun | 97.3% | 0.999 |
| Subsurface Profile, real skin scale, sun | 97.9% | 0.997 |
| Subsurface Profile, point light | 96.9% | 0.994 |
| Subsurface Profile, thin sphere, sun behind | 90.8% | 1.022 |
| Burley Diffusion (legacy), sun | 97.9% | 1.000 |
| Subsurface Profile, cast-shadow edge | 96.3% | 1.000 |

These are fixture results, not a guarantee for every scene. The thin fixture is
dim, so Monte Carlo and tap noise dominate its residual. Side-by-side images
(viewport left, path traced right) are saved as `parity-*.png` under
`build/raster-validation`.

For a visual check with real textures, set `DUSK_SSS_SHOWCASE` to the folder
holding the `skin_0001_*_1k` textures (for example `build/Release/models`) and
run `DuskGraphIntegrationTests <folder> --raster`. It renders a normal-mapped
skin sphere on a floor beside a wall with both models, in the viewport and on
the GPU tracer, front-lit and side-lit with a red wall, and reports
direct-lighting parity for each. The traced reference box-filters every pixel,
as the viewport's mipmapped textures do. In the dim side-lit views, noise in
near-black pixels at the terminator limits per-pixel accuracy to about 80-90%,
while total energy agrees within 3%.

## Normal maps at silhouettes

Near a silhouette, a normal map can tilt a shading normal away from the viewer.
Such a normal receives no light and used to render black in the tracers, which
showed as dark cracks along the rim of normal-mapped spheres. The CPU tracer,
the Vulkan tracer and the viewport now bend those normals back toward the viewer
until they are no more grazing than the geometry (at least `min(0.9 N_g.V, 0.1)`),
as production renderers do. Normals that already face the viewer are unchanged.

Normal and bump maps also keep lighting perturbed normals right up to the
terminator of the surface beneath them, where light then stops at once, leaving
a hard edge. All three backends multiply the surface lobes by the
shadow-terminator term of Chiang, Li and Burley (2019),
`G = min(1, (Ng.L) / ((Ns.L)(Ns.Ng)))` smoothed as `-G^3 + G^2 + G`, which fades
that light as the surface turns away. `Ng` here is the interpolated vertex
normal before normal mapping, not the flat triangle, so the fade follows the
smooth surface. With an unperturbed normal the term is exactly one.

## Coarse smooth-shaded meshes

A low-poly mesh with smooth vertex normals, such as a game character's face,
used to show its triangles in the tracers while the viewport looked smooth.
Three things followed the flat triangles instead of the smooth surface:

- Light was only accepted where the flat triangle faced it, so near the
  terminator whole triangles switched off together: a sawtooth shadow edge.
- Shadow rays started on the flat facet, which lies inside the smooth surface,
  so neighbouring facets shadowed it.
- Subsurface entry points were lit through the flat triangle normal. With a
  small diffusion radius every triangle averaged to one flat shade, a blocky
  staircase across cheeks, chin and neck.

Now light is gated by the interpolated normal (sampled directions still have to
leave the flat triangle). Shadow rays start on the smooth surface the vertex
normals describe, from Hanika's "Hacking the Shadow Terminator" (2021): `p` is
lifted onto each vertex's tangent plane where it lies below it, and the lifted
points are blended with the barycentric weights (`smooth_surface_offset` in
`triangle.h`, `Hit.so` on the GPU). Where the smooth normal faces a light that
the flat triangle faces away from, the shadow ray starts above the smooth
surface and ignores its own triangle, as Cycles does. Subsurface entries are
lit through the smooth normal, matching the viewport's diffusion. Flat-shaded
meshes are unchanged: their offset is zero and both normals agree.

Hard edges must be authored in the mesh as split normals. A mesh that looks
faceted in the viewport too has flat or split normals in the file itself.

## Surfaces just behind transparent ones

Where a ray passes through transparent alpha (a Blend or Clip surface), both
tracers continue the same ray from just past the hit, a few single-precision
steps further (`camera::past`, `past()` in the shader). Shadow rays step past
transparent occluders the same way. They used to restart 0.001 units further
on, which skipped any surface within a millimetre: a character's eyeball sits
about that far behind its occlusion shell, so the eyes rendered whatever lay
behind the eyeball.

To render a saved project through the viewport and the GPU tracer for
comparison, set `DUSK_SSS_PROJECT` to its `Scene.dusk` and run
`DuskGraphIntegrationTests <folder> --raster`; it writes `project-raster.png`
and `project-traced.png`.

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
backlighting, external shadows, indirect lighting and emissive-light MIS. A
coarse smooth sphere must be lit wherever its smooth surface faces the sun, on
the CPU and on the GPU (with and without subsurface scattering).

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
- [Jensen et al.: A Practical Model for Subsurface Light Transport](https://graphics.stanford.edu/papers/bssrdf/)
- [Zeltner et al.: Specular Manifold Sampling](https://rgl.epfl.ch/publications/Zeltner2020Specular)
- [Christensen and Burley: Approximate Reflectance Profiles for Efficient Subsurface Scattering](https://graphics.pixar.com/library/ApproxBSSRDF/paper.pdf)
- [Chiang, Li and Burley: Taming the Shadow Terminator](https://www.yiningkarlli.com/projects/shadowterminator.html)
- [Hanika: Hacking the Shadow Terminator (Ray Tracing Gems II)](https://jo.dreggn.org/home/2021_terminator.pdf)
- [Unreal Engine: SubsurfaceProfileStruct (4.27 Python API)](https://dev.epicgames.com/documentation/en-us/unreal-engine/python-api/class/SubsurfaceProfileStruct?application_version=4.27)
- [Unreal Engine: Subsurface Profile Shading Model](https://dev.epicgames.com/documentation/en-us/unreal-engine/subsurface-profile-shading-model-in-unreal-engine)
