/*
 * ism_test.c — the image-source geometry (ism.c): six first-order mirrors of a shoebox, their
 * positions, and the per-band reflection coefficients. Pure geometry, no audio thread.
 */
#include "ism.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); ++fails; } } while (0)

int main(void) {
    /* a 10 x 4 x 8 m room, floor-based: x in [-5,5], y in [0,4], z in [-4,4] */
    IsmRoom r;
    memset(&r, 0, sizeof r);
    r.w = 10.f; r.h = 4.f; r.d = 8.f; r.valid = 1;
    for (int f = 0; f < ISM_FACES; ++f) {                 /* 20% low / 30% mid / 60% high absorption */
        r.absorb[f][0] = 0.2f; r.absorb[f][1] = 0.3f; r.absorb[f][2] = 0.6f;
    }

    /* a source off-centre: 1 m from the +x wall, 3 m from -z, mid height */
    const float src[3] = { 4.f, 1.5f, -1.f };
    IsmImage img[ISM_IMAGES];
    int n = ism_images(&r, src, img);
    CHECK(n == ISM_IMAGES, "six first-order images");

    /* face order is -x,+x,-y,+y,-z,+z: each image mirrors ONE coordinate across its plane and
     * leaves the other two alone (that is the whole geometry). */
    CHECK(fabsf(img[0].pos[0] - (-14.f)) < 1e-4f, "-x image: 2*(-5) - 4 = -14");
    CHECK(fabsf(img[1].pos[0] - (  6.f)) < 1e-4f, "+x image: 2*( 5) - 4 = 6 (1 m past the near wall)");
    CHECK(fabsf(img[2].pos[1] - (-1.5f)) < 1e-4f, "-y (floor) image mirrors below the floor");
    CHECK(fabsf(img[3].pos[1] - (  6.5f)) < 1e-4f, "+y (ceiling) image: 2*4 - 1.5 = 6.5");
    CHECK(fabsf(img[4].pos[2] - ( -7.f)) < 1e-4f, "-z image: 2*(-4) - (-1) = -7");
    CHECK(fabsf(img[5].pos[2] - (  9.f)) < 1e-4f, "+z image: 2*( 4) - (-1) = 9");
    int untouched = 1;
    for (int f = 0; f < ISM_IMAGES; ++f) {
        int axis = f >> 1;
        for (int k = 0; k < 3; ++k)
            if (k != axis && fabsf(img[f].pos[k] - src[k]) > 1e-6f) untouched = 0;
    }
    CHECK(untouched, "a mirror moves only the coordinate normal to its face");

    /* the nearest wall makes the EARLIEST, LOUDEST reflection — the whole point of rendering these:
     * image 1 (+x, 1 m away) is nearer the source than image 0 (-x, 9 m away) */
    float d1 = fabsf(img[1].pos[0] - src[0]), d0 = fabsf(img[0].pos[0] - src[0]);
    CHECK(d1 < d0, "the near wall's image is the closer one (its reflection arrives first)");

    /* reflection coefficient is PRESSURE: sqrt(1 - energy absorption) */
    CHECK(fabsf(img[0].refl[0] - sqrtf(0.8f)) < 1e-5f, "low band: sqrt(1 - 0.2)");
    CHECK(fabsf(img[0].refl[1] - sqrtf(0.7f)) < 1e-5f, "mid band: sqrt(1 - 0.3)");
    CHECK(fabsf(img[0].refl[2] - sqrtf(0.4f)) < 1e-5f, "high band: sqrt(1 - 0.6)");
    CHECK(img[0].refl[2] < img[0].refl[1] && img[0].refl[1] < img[0].refl[0],
          "walls absorb treble hardest (the reflection is duller than the direct sound)");

    /* a perfectly reflective face returns the signal intact; a fully absorptive one kills it */
    r.absorb[1][0] = r.absorb[1][1] = r.absorb[1][2] = 0.f;
    r.absorb[0][0] = r.absorb[0][1] = r.absorb[0][2] = 1.f;
    ism_images(&r, src, img);
    CHECK(fabsf(img[1].refl[1] - 1.f) < 1e-6f, "a = 0 -> full reflection");
    CHECK(img[0].refl[1] < 1e-6f,              "a = 1 -> no reflection");

    /* a source OUTSIDE the room has no valid mirror geometry: no images (the caller renders it dry) */
    const float out[3] = { 7.f, 1.5f, 0.f };
    CHECK(ism_images(&r, out, img) == 0, "a source outside the room produces no images");
    const float below[3] = { 0.f, -0.1f, 0.f };
    CHECK(ism_images(&r, below, img) == 0, "a source below the floor produces no images");

    /* an unset/degenerate room produces nothing (the engine renders dry until bw_scene_set_box) */
    IsmRoom bad; memset(&bad, 0, sizeof bad);
    CHECK(ism_images(&bad, src, img) == 0, "an unset room produces no images");
    bad.valid = 1; bad.w = 0.f; bad.h = 3.f; bad.d = 3.f;
    CHECK(ism_images(&bad, src, img) == 0, "a zero-width room produces no images");

    if (fails) { printf("ism_test: %d FAILURES\n", fails); return 1; }
    printf("ism_test OK (six mirrors, per-axis geometry, pressure coefficients, outside/degenerate guards)\n");
    return 0;
}
