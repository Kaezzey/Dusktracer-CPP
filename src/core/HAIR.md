# Hair shading model

**Hair** is a material class for hair cards and strands. It follows Unreal's
Hair shading model inputs and renders the fibre scattering of Chiang, Bitterli,
Tappan and Burley (2016), the model behind Cycles' Principled Hair BSDF. A
fibre scatters light in three lobes:

| Lobe | Look |
| --- | --- |
| R | White primary highlight, shifted along the strand by the cuticle tilt |
| TT | Light passing through the strand: the soft glow against a back light |
| TRT | Coloured secondary highlight, shifted the other way |

Light bouncing between many strands is traced by the path tracer, which is
what makes light-coloured hair look soft and bright.

## Setting up hair cards

1. Select the hair material and set **Class** to **Hair**.
2. In the Material Graph, connect the card's colour texture to **Base Color**
   and its alpha (the sample's **A** or **Mask** output) to **Opacity**. Keep
   **Alpha Mode** on **Blend** so the strands' soft edges stay soft; **Clip**
   gives Unreal's Masked look, cut at the Clip Threshold.
3. Check the strand direction. **Strand Tangent** `(0,1,0)` follows the card's
   V texture axis, as in Unreal. Use `(1,0,0)` for cards laid out along U.
   Negate the vector to swap root and tip, which moves the highlights.
4. Add a back or rim light and raise **Max bounce depth** to 8–16. Light hair
   needs several bounces between cards to look soft.

A flow (tangent) map can drive the **Tangent** pin. The pin takes a
tangent-space vector, so decode a [0, 1] texture with Multiply by 2 and
Subtract 1 first.

## Inputs

Graph pins override the scalar defaults in Details.

| Pin | Default | Meaning | Unreal |
| --- | --- | --- | --- |
| Base Color | Base Color | Hair colour; converted to absorption (see Base Color Is) | Base Color |
| Specular | 0.5 | R lobe strength; 0.5 is physically based | Specular |
| Roughness | Roughness | Longitudinal roughness: highlight spread along the strand | Roughness |
| Tangent | Strand Tangent | Strand direction in tangent space | Tangent |
| Opacity | texture alpha | Card coverage, two-sided, under the Alpha Mode (Blend or Clip) | Opacity Mask |
| Radial Roughness | 0.3 | Roughness around the strand: low glints, high looks soft | (Scatter) |
| Backlit | 1 | Direct light through strands: the glow of a light behind the hair | Backlit |

**IOR** (about 1.55 for human hair) and **Cuticle Tilt** (about 2 degrees)
are material parameters. Pins keep their index across classes: Hair's
Specular and Tangent occupy PBR's Metallic and Normal pins. If a PBR normal map
was connected, the Hair inspector offers **Disconnect Tangent**, because a
normal map in that pin points the strands out of the card.

**Base Color Is** chooses how colour becomes absorption:

- **Strand** (default): Base Color is one strand's colour, as in Unreal. A ray
  through a strand's centre transmits the square root of the colour. Use it
  for hair cards, where one card stands in for a whole lock.
- **Volume**: Base Color is the colour of a thick lock of many strands, using
  the fit from Chiang et al. as Cycles does. Use it for real strand geometry;
  on cards it looks pale, because a single card has too few strands to
  build up the colour.

## How cards become strands

The fibre model needs to know where across a strand a ray lands. A card does
not record this, so each hit picks the offset from its position. Light
sampling, BSDF sampling and their MIS densities at that hit share it, and
pixel jitter averages it over the fibre's width. This far-field response is
what turns a flat card into many soft, shiny strands instead of one shaded
sheet.

Specular scales the R lobe. Backlit scales the TT lobe for direct light only,
as Unreal's does: light sampling, and emitters a bounce reaches directly, see
it. Light crossing hair between bounces keeps full transmission. Applied to
every bounce, Backlit below 1 would absorb the light that reaches the inside
of a hairstyle through its outer strands, and turn the crown dark grey. At
the defaults, the model conserves energy for a non-absorbing fibre.

## Backends

- CPU: `include/core/materials/hair_bsdf.h` (BSDF, sampling) and
  `include/core/materials/hair.h` (material, inputs, card handling).
- Vulkan: `hairF`, `hairPDF` and `sampleHair` in `src/gpu/shaders/pathtrace.comp`
  mirror the CPU code in float32. Each hit draws its fibre offset at random.
- Raster viewport: Karis's real-time approximation of the same lobes from
  Unreal, plus a wrapped Kajiya-Kay term that stands in for the inter-strand
  bounces the viewport does not trace. It previews strand direction, tilt and
  Backlit but is not an exact match for the path tracer.

## Verification

- `DuskPbrTests`: white furnace within 0.2% across roughnesses, offsets and
  views; sampled weights and densities match evaluation; importance sampling
  matches quadrature; Tangent, cutouts and per-hit offsets.
- `DuskGpuTests --hair`: Vulkan against the CPU for front, oblique and grazing
  views with sun and sky, and for the R, TT, tilt and volume-colouring terms in
  isolation (within about 1.5%). It also writes `gpu-fixtures/hair-sphere.png`,
  a denoised hair ball for visual checks.
- `DuskGraphTests`: all seven Material Output pins compile and serialize.
- `DuskGraphIntegrationTests --raster`: viewport highlights follow the
  tangent, Backlit transmits through cards, and Opacity cuts them out.
