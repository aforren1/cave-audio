/*
 * ambi_test.c — the 3rd-order ACN/SN3D spherical-harmonic encode (front half of the production
 * binaural monitor). Checks known SH values at the cardinal directions and basic sanity.
 */
#include "ambisonics.h"

#include <math.h>
#include <stdio.h>

static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", (m)); ++fails; } } while (0)
#define EQ(a, b)    (fabsf((a) - (b)) < 1e-5f)

int main(void) {
    float y[BWA_AMBI_CH];
    const float SQ3_2 = 0.8660254f, S58 = 0.7905694f, S38 = 0.6123724f;

    /* +x (front): l=1 picks out x; zonal/sectoral l2,l3 take known values */
    ambi_encode_sn3d((const float[]){ 1, 0, 0 }, y);
    CHECK(EQ(y[0], 1) && EQ(y[3], 1) && EQ(y[1], 0) && EQ(y[2], 0), "+x: W and l=1");
    CHECK(EQ(y[6], -0.5f) && EQ(y[8], SQ3_2), "+x: l=2");
    CHECK(EQ(y[13], -S38) && EQ(y[15], S58) && EQ(y[12], 0), "+x: l=3");

    /* +y (left) */
    ambi_encode_sn3d((const float[]){ 0, 1, 0 }, y);
    CHECK(EQ(y[0], 1) && EQ(y[1], 1) && EQ(y[3], 0) && EQ(y[2], 0), "+y: W and l=1");
    CHECK(EQ(y[8], -SQ3_2) && EQ(y[6], -0.5f), "+y: l=2");
    CHECK(EQ(y[9], -S58) && EQ(y[11], -S38), "+y: l=3");

    /* +z (up): only the zonal (m=0) harmonics are non-zero */
    ambi_encode_sn3d((const float[]){ 0, 0, 1 }, y);
    CHECK(EQ(y[0], 1) && EQ(y[2], 1) && EQ(y[1], 0) && EQ(y[3], 0), "+z: W and l=1");
    CHECK(EQ(y[6], 1) && EQ(y[12], 1) && EQ(y[8], 0), "+z: zonal l=2/l=3");

    /* W is always unity; all harmonics finite for an arbitrary direction */
    ambi_encode_sn3d((const float[]){ 0.5773503f, 0.5773503f, 0.5773503f }, y);
    int finite = 1;
    for (int i = 0; i < BWA_AMBI_CH; ++i) finite &= isfinite(y[i]);
    CHECK(finite && EQ(y[0], 1), "diagonal: finite and W=1");

    /* The product / sectoral harmonics (ACN 4,5,7,8,10,11,14) are EXACTLY ZERO at every cardinal
     * direction, so a sign flip in any of them — the m<0 SH-sign bug class that has bitten this repo
     * — is invisible to the cardinal checks above. Pin their signs+values at an ASYMMETRIC direction
     * (2,1,2)/3 (unit; x!=y so the m=+2 terms don't vanish either). */
    ambi_encode_sn3d((const float[]){ 0.6666667f, 0.3333333f, 0.6666667f }, y);
    CHECK(EQ(y[4],  0.3849002f), "asym: ACN4 (l2 m-2, x*y)");
    CHECK(EQ(y[5],  0.3849002f), "asym: ACN5 (l2 m-1, y*z)");
    CHECK(EQ(y[7],  0.7698004f), "asym: ACN7 (l2 m+1, x*z)");
    CHECK(EQ(y[8],  0.2886751f), "asym: ACN8 (l2 m+2, x^2-y^2)");
    CHECK(EQ(y[10], 0.5737753f), "asym: ACN10 (l3 m-2, x*y*z)");
    CHECK(EQ(y[11], 0.2494813f), "asym: ACN11 (l3 m-1)");
    CHECK(EQ(y[14], 0.4303315f), "asym: ACN14 (l3 m+2)");

    /* phonon interop: SN3D encode * ambi_phonon_scale must reproduce phonon's orthonormal SH
     * (third_party/steam-audio core/src/core/sh/spherical_harmonics.cc hardcoded constants). At
     * AmbiX front (x=1) = phonon Google +x: W=0.282095, ACN3(x)=0.488603, ACN6(l2,m0)=-0.315392,
     * ACN8(l2,m2)=0.546274 — confirms the SN3D->N3D/sqrt(4pi) match the decode needs. */
    ambi_encode_sn3d((const float[]){ 1, 0, 0 }, y);
    CHECK(EQ(y[0] * ambi_phonon_scale[0],  0.282095f), "phonon W = 0.282095");
    CHECK(EQ(y[3] * ambi_phonon_scale[3],  0.488603f), "phonon ACN3 (front) = 0.488603");
    CHECK(EQ(y[6] * ambi_phonon_scale[6], -0.315392f), "phonon ACN6 (l=2,m=0) = -0.315392");
    CHECK(EQ(y[8] * ambi_phonon_scale[8],  0.546274f), "phonon ACN8 (l=2,m=2) = 0.546274");

    if (fails) { printf("ambi_test: %d FAILURES\n", fails); return 1; }
    printf("ambi_test OK (3rd-order ACN/SN3D encode at cardinal directions)\n");
    return 0;
}
