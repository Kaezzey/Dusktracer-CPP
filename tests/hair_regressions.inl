// Hair fibre BSDF (Chiang et al. 2016) and the Hair material. Included by
// pbr_tests.cpp, which provides require(), near(), sequence() and surface().
#include "core/materials/hair.h"

static vec3 hair_direction(double sin_theta, double phi) {
    double cos_theta = std::sqrt(std::max(0.0, 1 - sin_theta * sin_theta));
    return vec3(sin_theta, cos_theta * std::cos(phi), cos_theta * std::sin(phi));
}

static void check_hair_bsdf() {
    // Without absorption a fibre scatters every photon: R + TT + TRT + the
    // residual sum to one, and each longitudinal and azimuthal lobe is
    // normalised. Independent uniform-solid-angle quadrature, not the sampler.
    const int nz = 300, nphi = 600;
    const double cell = 4 * pi / (nz * nphi);
    double worst_energy = 0, worst_density = 0;
    for (double beta_m : {.25, .6}) for (double beta_n : {.3, .8}) for (double h : {-.5, .3}) for (double sin_o : {0., .6}) {
        hair::parameters p;
        p.h = h; p.beta_m = beta_m; p.beta_n = beta_n; p.sigma_a = colour(0, 0, 0);
        hair::bsdf fibre(p);
        vec3 wo = hair_direction(sin_o, .7);
        double energy = 0, density = 0;
        for (int z = 0; z < nz; ++z) for (int k = 0; k < nphi; ++k) {
            vec3 wi = hair_direction(-1 + 2 * (z + .5) / nz, 2 * pi * (k + .5) / nphi);
            energy += fibre.f(wo, wi).x() * cell;
            density += fibre.pdf(wo, wi) * cell;
        }
        worst_energy = std::max(worst_energy, std::abs(energy - 1));
        worst_density = std::max(worst_density, std::abs(density - 1));
    }
    std::cout << "Hair white furnace: worst energy error " << worst_energy << ", density error " << worst_density << '\n';
    require(worst_energy < .03, "a fibre without absorption conserves energy");
    require(worst_density < .03, "the fibre sampling density integrates to one");

    // The sampler's weights and densities agree with evaluation, and its
    // estimator integrates the same lobes.
    hair::parameters p;
    p.h = .2; p.beta_m = .35; p.beta_n = .4; p.sigma_a = hair::absorption_from_colour(colour(.8, .55, .3), .4);
    hair::bsdf fibre(p);
    const vec3 wo = hair_direction(.3, 1.1);
    colour sampled(0, 0, 0), integrated(0, 0, 0);
    const int count = 65536;
    for (int i = 0; i < count; ++i) {
        const double u[4] = {sequence(i, 2), sequence(i, 3), sequence(i, 5), sequence(i, 7)};
        vec3 wi; colour weight; double density;
        if (!fibre.sample(wo, u, wi, weight, density)) continue;
        sampled += weight / count;
        if (i < 64) {
            near(density, fibre.pdf(wo, wi), 1e-9 * std::max(1.0, density), "hair sample density matches evaluation");
            colour f = fibre.f(wo, wi);
            near(weight.y() * density, f.y(), 1e-9 * std::max(1.0, f.y()), "hair sample weight is f over its density");
        }
    }
    for (int z = 0; z < nz; ++z) for (int k = 0; k < nphi; ++k)
        integrated += fibre.f(wo, hair_direction(-1 + 2 * (z + .5) / nz, 2 * pi * (k + .5) / nphi)) * cell;
    for (int c = 0; c < 3; ++c) near(sampled[c], integrated[c], .02, "hair importance sampling matches quadrature");
    require(integrated.x() > integrated.y() && integrated.y() > integrated.z() && integrated.x() < 1,
            "fibre colour comes from absorption, reddest channel scattering most");

    // Strand colouring (Unreal): a ray through the fibre centre transmits the
    // square root of Base Color; volume colouring absorbs far less per strand.
    const colour strand_sigma = hair::absorption_from_fibre_colour(colour(.81, .36, .09));
    near(std::exp(-2 * strand_sigma.x()), .9, 1e-12, "a strand transmits the square root of its colour");
    near(std::exp(-2 * strand_sigma.z()), .3, 1e-12, "a dark strand transmits the square root of its colour");
    require(hair::absorption_from_colour(colour(.81, .36, .09), .3).y() < strand_sigma.y() * .2,
            "volume colouring absorbs much less per strand than strand colouring");

    // Specular and Backlit scale exactly the R and TT lobes.
    p.r_weight = 0; p.tt_weight = 0; p.alpha = 0;
    hair::bsdf dark(p);
    hair::parameters only = p;
    only.r_weight = 1;
    vec3 mirror(-wo.x(), wo.y(), wo.z());
    require(hair::bsdf(only).f(wo, mirror).x() > dark.f(wo, mirror).x(), "Specular drives the primary highlight");

    // Material: the strand follows the card's V axis by default, and light
    // sampling, BSDF sampling and their densities share one fibre per hit.
    hair_material strands;
    strands.base_color = colour(.7, .5, .3);
    auto hit = surface();
    hit.p = point3(.123, .456, .789);
    auto frame = strands.prepare(hit);
    near(frame.x.y(), 1, 1e-12, "default Tangent (0,1,0) runs along the bitangent");
    strands.tangent = vec3(1, 0, 0);
    near(strands.prepare(hit).x.x(), 1, 1e-12, "Tangent (1,0,0) runs along the card's U axis");
    hittable_list empty;
    const vec3 view = unit_vector(vec3(.3, -.4, .8));
    ray incoming(hit.p + view, -view, 0);
    for (int i = 0; i < 32; ++i) {
        const double u[4] = {sequence(i, 2), sequence(i, 3), sequence(i, 5), sequence(i, 7)};
        bsdf_sample sample;
        if (!strands.sample_fibre(incoming, hit, u, sample)) continue;
        vec3 direction = sample.scattered.direction();
        double density = strands.bsdf_pdf(hit, view, direction);
        near(sample.pdf, density, 1e-9 * std::max(1.0, density), "hair material sampler and evaluator agree");
        colour direct = strands.shade_direct(hit, view, direction, colour(1, 1, 1), empty);
        near(sample.weight.x() * density, direct.x(), 1e-9 * std::max(1.0, direct.x()),
             "hair material weight is direct response over density");
    }
    // Backlit dims only direct light: light sampling and directly reached
    // emitters see it, while the continuation carries full transmission.
    strands.backlit = .3;
    for (int i = 0; i < 64; ++i) {
        const double u[4] = {sequence(i, 2), sequence(i, 3), sequence(i, 5), sequence(i, 7)};
        bsdf_sample sample;
        if (!strands.sample_fibre(incoming, hit, u, sample)) continue;
        vec3 direction = sample.scattered.direction();
        colour direct = strands.shade_direct(hit, view, direction, colour(1, 1, 1), empty);
        colour full = strands.prepare(hit).bsdf.f(strands.prepare(hit).local(view), strands.prepare(hit).local(direction));
        for (int c = 0; c < 3; ++c) {
            near(sample.weight[c] * sample.pdf, full[c], 1e-9 * std::max(1.0, full[c]),
                 "hair continuations carry full transmission");
            near(sample.weight[c] * sample.pdf * sample.direct_scale[c], direct[c], 1e-9 * std::max(1.0, direct[c]),
                 "directly reached lights see Backlit, matching light sampling");
        }
        require(direct.x() <= full.x() + 1e-12, "Backlit below one only dims direct light");
    }
    strands.backlit = 1;
    require(strands.fibre_offset(hit.p) == strands.fibre_offset(hit.p) &&
                std::abs(strands.fibre_offset(hit.p)) <= 1 &&
                strands.fibre_offset(hit.p) != strands.fibre_offset(hit.p + vec3(1e-9, 0, 0)),
            "each hit picks a stable, independent offset across the fibre");
    // Clip cards are two-sided cutouts.
    strands.alpha_mode = 1;
    strands.alpha_cutoff = .5;
    strands.alpha_tex = std::make_shared<solid_colour>(colour(.3, .3, .3));
    hit.front_face = false;
    near(strands.opacity_at(hit), 0, 0, "hair cards cut out below the opacity threshold from either side");
}
