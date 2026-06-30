/*
 * zylia_test.c — single-position ZM-1 localization recovers a known speaker position (off-hardware).
 *
 * Synthesize the 19 per-capsule arrival times for a speaker at a known position (exact spherical
 * wavefront), then check: (1) zylia_doa recovers the direction within a degree, (2) zylia_localize
 * recovers the full position to sub-mm on exact data, (3) the DOA is latency-independent.
 */
#include "zylia.h"

#include <math.h>
#include <stdio.h>

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); ++fails; } } while (0)

/* exact arrival time at each capsule for a source at `src`, array centred at `center`. */
static void synth(const float center[3], const double src[3], double latency, double c, double arr[ZYLIA_MICS]) {
    float dirs[ZYLIA_MICS][3]; float R; zylia_geometry(dirs, &R);
    for (int i = 0; i < ZYLIA_MICS; ++i) {
        double mx = center[0] + R*dirs[i][0], my = center[1] + R*dirs[i][1], mz = center[2] + R*dirs[i][2];
        double dx = mx - src[0], dy = my - src[1], dz = mz - src[2];
        arr[i] = sqrt(dx*dx + dy*dy + dz*dz) / c + latency;
    }
}

int main(void) {
    const double C = 343.0, LAT = 0.0047;       /* arbitrary nonzero system latency */
    const float center[3] = { 0.1f, 1.2f, -0.3f };   /* array placed off-origin in the room */

    struct { double pos[3]; const char* name; } cases[] = {
        {{  2.0,  1.2, -0.3 }, "right  (+X)"},
        {{  0.1,  1.2, -3.0 }, "front  (-Z)"},
        {{ -2.5,  2.0,  1.0 }, "back-left-up"},
        {{  0.1,  4.0, -0.3 }, "overhead (+Y)"},
        {{  1.0, -0.5, -2.0 }, "low diagonal"},
    };

    for (int t = 0; t < 5; ++t) {
        double arr[ZYLIA_MICS];
        synth(center, cases[t].pos, LAT, C, arr);

        double tx = cases[t].pos[0]-center[0], ty = cases[t].pos[1]-center[1], tz = cases[t].pos[2]-center[2];
        double td = sqrt(tx*tx + ty*ty + tz*tz); tx/=td; ty/=td; tz/=td;

        float doa[3];
        int ok = zylia_doa(arr, doa);
        double dot = doa[0]*tx + doa[1]*ty + doa[2]*tz;
        double deg = acos(fmax(-1.0, fmin(1.0, dot))) * 180.0 / 3.14159265358979;

        float pos[3], dist;
        int ok2 = zylia_localize(arr, center, LAT, C, pos, &dist);
        double perr = sqrt((pos[0]-cases[t].pos[0])*(pos[0]-cases[t].pos[0]) +
                           (pos[1]-cases[t].pos[1])*(pos[1]-cases[t].pos[1]) +
                           (pos[2]-cases[t].pos[2])*(pos[2]-cases[t].pos[2]));
        printf("[%-12s] dist=%.2f m  doa_err=%.3f deg  pos_err=%.4f mm\n",
               cases[t].name, dist, deg, perr * 1000.0);
        CHECK(ok  && deg  < 1.0,  "DOA within 1 deg from a single placement");
        CHECK(ok2 && perr < 1e-3, "localize within 1 mm (Gauss-Newton, exact data)");
    }

    /* DOA is latency-independent: add a constant to every arrival -> same direction. */
    {
        double a0[ZYLIA_MICS], a1[ZYLIA_MICS];
        double src[3] = { -1.5, 0.8, -2.2 };
        synth(center, src, 0.0,   C, a0);
        synth(center, src, 0.013, C, a1);   /* +13 ms of latency */
        float d0[3], d1[3];
        zylia_doa(a0, d0); zylia_doa(a1, d1);
        double dot = d0[0]*d1[0] + d0[1]*d1[1] + d0[2]*d1[2];
        CHECK(dot > 0.99999, "DOA unchanged by a constant latency shift");
    }

    printf("%s\n", fails ? "FAIL: zylia localization" : "PASS: zylia single-position localization");
    return fails ? 1 : 0;
}
