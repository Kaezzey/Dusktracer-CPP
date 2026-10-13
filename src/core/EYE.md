# Eye shading model

**Eye** is a material class for eyeballs. It follows Unreal's Eye shading
model: a wet cornea over an iris that sits below it. Game eye meshes are a
single eyeball surface carrying one texture (iris and sclera painted together),
so the depth and refraction come from the shader, as in Unreal's
`EyeRefraction` material function, not from separate cornea geometry.

| Part | What the shader does |
| --- | --- |
| Cornea | A GGX specular layer. Reflectance F0 = 0.08 × Specular (Unreal's convention), Roughness from the Roughness pin |
| Iris | Inside the iris disc, the base colour is looked up where the view ray, refracted by the cornea, meets the iris below the surface. The iris shifts as the view turns and looks set into the eye |
| Iris lighting | Unreal's EyeBxDF: the iris is lit through its own flat plane rather than the curved surface, plus a caustic lobe. Light from one side brightens the far side of the iris, as the cornea focuses it there |
| Sclera | Plain diffuse under the cornea |

Blender has no eye shader. Its eyes are built from geometry: a glass cornea
mesh over an iris mesh. That also works in Dusk with a Dielectric cornea, but
it needs a second mesh that game assets do not have.

## Setting up an eye

1. Select the eye material and set **Class** to **Eye**. IOR becomes 1.336
   (the aqueous humour) and Roughness is capped at 0.1.
2. Connect the eye texture to **Base Color** in the Material Graph.
3. Set **Iris Centre (UV)** and **Iris Radius (UV)** to the coloured disc of
   the texture. Unreal's default radius is 0.15; a texture painted as a whole
   eyeball, like most game eyes, is usually around 0.25. To check the fit,
   raise **Iris Depth** to 1.5 for a moment: the region that shifts as you
   orbit is the iris.
4. Tune **Iris Depth** (in iris radii; a real eye is about 0.5) and
   **Limbus Width** (the softness of the iris edge).

## Inputs

| Input | Graph pin | Fallback |
| --- | --- | --- |
| Base Color | 0 | Base Color |
| Specular | 1 (PBR's Metallic) | Specular, 0.5. 0.31 matches a real cornea |
| Roughness | 2 | Roughness |
| Normal | 3 | Cornea normal map; none |
| Opacity | 4 | Base colour alpha, under the material's Alpha Mode |
| Iris Mask | 5 | A disc: Iris Centre, Iris Radius, softened by Limbus Width |
| Iris Distance | 6 | Depth below the cornea in iris radii: Iris Depth × (1 − r²), a parabolic anterior chamber |

A connected Iris Mask or Iris Distance pin replaces the fallback, so a
painted mask or a depth map (Unreal's MidPlaneDisplacement) can be used. Pins
evaluate at the surface UV; only Base Color is read through the cornea.

## Model details

All three backends share `materials/eye.h` (`pathtrace.comp: irisAt`,
`raster_shaders.h: shadeSurface`):

- **Refraction.** The view direction is refracted into the surface in tangent
  space with Snell's law (1/IOR) and followed down to the iris. Depth is in UV
  units (Iris Distance × Iris Radius), so the parallax does not depend on the
  model's scale.
- **Iris plane.** The eyeball surface tilts away from the optical axis by
  about asin(r/2) across the iris, because the iris radius is about half the
  eyeball's. The iris plane normal undoes that tilt. Unreal reads the same
  normal from a texture.
- **Caustic.** Unreal's term, with its caustic normal bent toward −N by
  Iris Mask × Iris Distance for concavity:
  `IrisNoL × (0.8 + 0.2 (P + 1) pow(CausticNoL, P))`, `P = lerp(12, 1, IrisNoL)`.
  Integrated over the hemisphere it reflects up to 4% more than it receives,
  so the tracers scale it by 0.96 to stay within unit albedo.
- **Sampling.** The eye is the PBR material with this diffuse term: GGX
  visible-normal sampling for the cornea and cosine sampling for the iris and
  sclera, with MIS against light sampling. Metallic and subsurface scattering
  are not used.

Unreal's eye also relies on extra meshes: an occlusion shell that darkens the
eye's corners and a tear-line mesh. Cloud's `eye_shadow` mesh is the
occlusion shell; give it a PBR material with its texture's alpha on Opacity
and **Alpha Mode** set to **Blend**.

## Verification

`DuskPbrTests` checks the iris disc and limbus, the refraction direction
(away from the viewer, bent toward the normal, deeper irises shifting more),
that the iris lights up on the side away from the light, a white furnace (at
most 0.92 for a white eye) with sampling and quadrature in agreement, and that
the base colour is read through the cornea. `DuskGpuTests --eye` compares the
Vulkan eye with the CPU at the iris centre, through the cornea from the side,
and on the sclera, plus the Iris Mask pin. The viewport test refracts the iris
by its depth and honours Iris Mask, and saves `eye-raster.png`.
