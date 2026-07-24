/*
 * steam_decode_test.c — the production HRTF monitor decode runs and spatializes (with-SDK only).
 *
 * The 26->stereo Steam Audio HRTF decode (steam_decode.c) was the one phonon path with no test: it was
 * built + linked but exercised by no assertion. This closes that gap, mirroring monitor_test's checks
 * (which cover the simple-pan fallback in binaural.c) for the phonon path: build the monitor, drive one
 * hard-side speaker, and assert the decode (a) runs and produces finite, audible stereo and (b)
 * preserves laterality — right speaker -> right ear, left -> left, and a 180-degree head turn flips it.
 * It does NOT judge HRTF *quality* (that stays the by-ear check); it proves the decode actually works.
 */
#include "steam_decode.h"
#include "layout.h"
#include "rt.h"            /* the live-composition probe drives the real DBAP bus + pose plumbing */
#include "sink.h"          /* BWA_CHANNELS */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N 1024u            /* device block == phonon frameSize */

static float* bus;         /* BWA_CHANNELS * N */
static float* out;         /* 2 * N (L at [0,N), R at [N,2N)) */
static int    fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", (msg)); ++fails; } } while (0)

static double e_left(void)  { double e = 0; for (uint32_t i = 0; i < N; ++i) e += fabs(out[i]);     return e; }
static double e_right(void) { double e = 0; for (uint32_t i = 0; i < N; ++i) e += fabs(out[N + i]); return e; }

static void decode_channel(SteamMonitor* m, int ch, const float q[4]) {
    const float p[3] = { 0, 1.5f, 0 };   /* the default grid's ear point (floor origin) */
    memset(bus, 0, sizeof(float) * (size_t)BWA_CHANNELS * N);
    /* Drive a TONE in the hearing band, never DC: the default HRTF's per-ear DC gains are laterally
     * OPPOSITE its audible ILD, so a DC-driven laterality assertion passes exactly when the field is
     * mirrored (that trap shipped a mirrored decode once — see steam_decode.c CONVENTION 2b). */
    for (uint32_t i = 0; i < N; ++i)
        bus[(size_t)ch * N + i] = sinf(6.2831853f * 660.0f * (float)i / 48000.0f);
    /* phonon crossfades an orientation CHANGE across one block (AmbisonicsRotateEffect keeps the
     * previous rotation and interpolates), so the first block after a new q is a smear of old and
     * new. Render twice and measure the settled block. (The old head convention masked this: its
     * identity equalled phonon's initial internal rotation, so block 1 happened to be pure.) */
    steam_monitor_process(m, bus, NULL, NULL, 0, p, q, out, N);
    steam_monitor_process(m, bus, NULL, NULL, 0, p, q, out, N);
}

int main(void) {
    Layout L = layout_default();
    bus = (float*)calloc((size_t)BWA_CHANNELS * N, sizeof(float));
    out = (float*)calloc((size_t)2 * N, sizeof(float));
    if (!bus || !out) { printf("FAIL: alloc\n"); return 1; }

    SteamMonitor* m = steam_monitor_create(&L, 48000, N, NULL, 8);   /* built-in HRTF + a small
                                                                      * per-voice fleet (mode 2) */
    CHECK(m != NULL, "steam_monitor_create (built-in HRTF)");
    if (!m) { printf("steam_decode_test: %d FAILURES\n", fails); return 1; }
    CHECK(steam_monitor_pervoice(m), "per-voice fleet created");

    int right = -1, left = -1;     /* pure-lateral speakers (y,z ~ 0) so the HRTF L/R isn't diluted by elevation */
    for (int k = 0; k < (int)BWA_CHANNELS; ++k) {     /* identity faces +z, so the listener's right is -x */
        float x = L.speakers[k].pos[0], y = L.speakers[k].pos[1], z = L.speakers[k].pos[2];
        if (fabsf(y - 1.5f) > 0.01f || fabsf(z) > 0.01f) continue;   /* lateral = at ear height */
        if (x < -1.0f) right = k;
        if (x >  1.0f) left  = k;
    }
    CHECK(right >= 0 && left >= 0, "default layout has pure-lateral right (-x) and left (+x) speakers");

    const float ident[4]  = { 0, 0, 0, 1 };          /* head facing forward (+z) */
    const float yaw180[4] = { 0, 1, 0, 0 };          /* head turned 180 about +y */

    /* 1. the decode runs + is finite + audible */
    decode_channel(m, right, ident);
    CHECK(isfinite(e_left()) && isfinite(e_right()) && (e_left() + e_right()) > 1e-6,
          "HRTF decode runs: finite + audible stereo");

    /* 2. laterality: right speaker -> right ear, left -> left */
    double rL = e_left(), rR = e_right();
    printf("right speaker: L=%.4g R=%.4g\n", rL, rR);
    CHECK(rR > rL * 1.1, "right speaker favors the right ear");
    decode_channel(m, left, ident);
    printf("left speaker:  L=%.4g R=%.4g\n", e_left(), e_right());
    CHECK(e_left() > e_right() * 1.1, "left speaker favors the left ear");

    /* 3. a 180-degree head turn flips the right speaker to the left ear */
    decode_channel(m, right, yaw180);
    printf("right + 180:   L=%.4g R=%.4g\n", e_left(), e_right());
    CHECK(e_left() > e_right() * 1.1, "head turned 180: the right speaker now favors the left ear");

    /* 4. the LIVE composition (render_binaural minus the device): a source rendered through the real
     * rt core — DBAP gains, commit snapshot, pose plumbing — into the 26-ch bus, then this decode.
     * Checks 1-3 drive single bus channels directly, so the rt×monitor seam (bus channel order vs
     * layout positions, committed pose vs decode orientation) was unpinned — exactly where a live
     * mirror could hide while every direct-drive check stays green. Source at room +x = the identity
     * listener's LEFT (the room frame is RH, +y up, +z ahead: right ear at -x, bw_audio.h). */
    {
        RtCore* c = rt_create(4, 4, 48000, BWA_CHANNELS);
        CHECK(c != NULL, "live: rt_create");
        if (c) {
            rt_set_layout(c, &L);
            char perr[256] = {0};
            uint32_t h = rt_source_create_stream(c, perr, sizeof perr);
            CHECK(h != 0, "live: push source");
            float* blk = (float*)malloc(sizeof(float) * 4 * N);
            for (uint32_t i = 0; i < 4 * N; ++i)   /* a tone, not DC (see decode_channel) */
                blk[i] = 0.5f * sinf(6.2831853f * 660.0f * (float)i / 48000.0f);
            rt_source_push(c, h, blk, 4 * N);
            rt_source_set_pos(c, h, 1.5f, 1.5f, 0.f);        /* the playground's spawn side: room +x */
            const float pl[3] = { 0.f, 1.5f, 0.f };          /* listener at the ear point */
            const float qi[4] = { 0, 0, 0, 1 };
            rt_set_listener(c, pl, qi);
            rt_commit(c);
            bwa_timestamp ts = { 0, 0 };
            float pg[3], qg[4];                              /* the decode gets its pose the way
                                                              * render_binaural does: rt_get_listener */
            rt_render(c, bus, N, &ts);                       /* block 1: gains ramp in */
            rt_render(c, bus, N, &ts);                       /* settled */
            rt_get_listener(c, pg, qg);
            steam_monitor_process(m, bus, NULL, NULL, 0, pg, qg, out, N);   /* twice: phonon smears an orientation */
            steam_monitor_process(m, bus, NULL, NULL, 0, pg, qg, out, N);   /* change across its first block */
            printf("live +x, ident: L=%.4g R=%.4g\n", e_left(), e_right());
            CHECK(e_left() > e_right() * 1.1, "live: +x source (listener's LEFT) favors the left ear");
            const float q180[4] = { 0, 1, 0, 0 };
            rt_set_listener(c, pl, q180);                    /* array gains ignore orientation; the decode doesn't */
            rt_commit(c);
            rt_render(c, bus, N, &ts);
            rt_get_listener(c, pg, qg);
            steam_monitor_process(m, bus, NULL, NULL, 0, pg, qg, out, N);
            steam_monitor_process(m, bus, NULL, NULL, 0, pg, qg, out, N);
            printf("live +x, yaw180: L=%.4g R=%.4g\n", e_left(), e_right());
            CHECK(e_right() > e_left() * 1.1, "live: head turned 180 - the +x source now favors the right ear");
            free(blk);
            rt_destroy(c);
        }
    }

    /* 5. the live DIRECT composition (BWA_PROFILE_BINAURAL minus the device): the same scene through
     * rt's direct mode — the per-voice SH encode (ambi_encode_phonon at the true direction) summed
     * into the decode alongside the (silent) bus. Pins the rt-direct × phonon seam: encode basis,
     * ACN order, and the decode-side orientation, live. */
    {
        RtCore* c = rt_create(4, 4, 48000, BWA_CHANNELS);
        CHECK(c != NULL, "direct: rt_create");
        if (c) {
            rt_set_direct_ambi(c, 1);
            rt_set_layout(c, &L);
            char perr[256] = {0};
            uint32_t h = rt_source_create_stream(c, perr, sizeof perr);
            CHECK(h != 0, "direct: push source");
            float* blk = (float*)malloc(sizeof(float) * 4 * N);
            for (uint32_t i = 0; i < 4 * N; ++i)             /* a tone, not DC (see decode_channel) */
                blk[i] = 0.5f * sinf(6.2831853f * 660.0f * (float)i / 48000.0f);
            rt_source_push(c, h, blk, 4 * N);
            rt_source_set_pos(c, h, 1.5f, 1.5f, 0.f);        /* room +x = the identity listener's LEFT */
            const float pl[3] = { 0.f, 1.5f, 0.f };
            const float qi[4] = { 0, 0, 0, 1 };
            rt_set_listener(c, pl, qi);
            rt_commit(c);
            bwa_timestamp ts = { 0, 0 };
            float pg[3], qg[4];
            rt_render(c, bus, N, &ts);
            rt_render(c, bus, N, &ts);
            const float* direct = rt_direct_ambi(c);
            CHECK(direct != NULL, "direct: rt_direct_ambi returns the SH field");
            {   /* the direct field carries the voice; the speaker bus does NOT (it kept the diffuse layer) */
                double de = 0, be = 0;
                for (uint32_t i = 0; i < 16 * N && direct; ++i) de += fabs(direct[i]);
                for (uint32_t i = 0; i < (uint32_t)BWA_CHANNELS * N; ++i) be += fabs(bus[i]);
                CHECK(de > 1e-3, "direct: the SH field is audible");
                CHECK(be < 1e-9, "direct: the point voice stays OFF the speaker bus");
            }
            rt_get_listener(c, pg, qg);
            steam_monitor_process(m, bus, direct, NULL, 0, pg, qg, out, N);
            steam_monitor_process(m, bus, direct, NULL, 0, pg, qg, out, N);
            printf("direct +x, ident: L=%.4g R=%.4g\n", e_left(), e_right());
            CHECK(e_left() > e_right() * 1.1, "direct: +x source (listener's LEFT) favors the left ear");
            const float q180[4] = { 0, 1, 0, 0 };
            rt_set_listener(c, pl, q180);
            rt_commit(c);
            rt_render(c, bus, N, &ts);
            direct = rt_direct_ambi(c);
            rt_get_listener(c, pg, qg);
            steam_monitor_process(m, bus, direct, NULL, 0, pg, qg, out, N);
            steam_monitor_process(m, bus, direct, NULL, 0, pg, qg, out, N);
            printf("direct +x, yaw180: L=%.4g R=%.4g\n", e_left(), e_right());
            CHECK(e_right() > e_left() * 1.1, "direct: head turned 180 - the +x source now favors the right ear");
            free(blk);
            rt_destroy(c);
        }
    }

    /* 6. the live MODE-2 composition (per-voice IPLBinauralEffect): the same scene through rt's
     * per-voice point taps — a spread-0 voice leaves the SH field ~empty, rides its own mono slot,
     * and the per-voice HRTF convolution carries the laterality (and flips with the head). Pins the
     * whole mode-2 chain: the point/field power split, the slot render, the dv_view publish, the
     * room->head-local direction handoff, and the effect fleet. */
    {
        RtCore* c = rt_create(4, 4, 48000, BWA_CHANNELS);
        CHECK(c != NULL, "mode2: rt_create");
        if (c) {
            rt_set_direct_ambi(c, 2);
            rt_set_layout(c, &L);
            char perr[256] = {0};
            uint32_t h = rt_source_create_stream(c, perr, sizeof perr);
            CHECK(h != 0, "mode2: push source");
            float* blk = (float*)malloc(sizeof(float) * 6 * N);
            for (uint32_t i = 0; i < 6 * N; ++i)
                blk[i] = 0.5f * sinf(6.2831853f * 660.0f * (float)i / 48000.0f);
            rt_source_push(c, h, blk, 6 * N);
            rt_source_set_pos(c, h, 1.5f, 1.5f, 0.f);        /* room +x = the identity listener's LEFT */
            const float pl[3] = { 0.f, 1.5f, 0.f };
            const float qi[4] = { 0, 0, 0, 1 };
            rt_set_listener(c, pl, qi);
            rt_commit(c);
            bwa_timestamp ts = { 0, 0 };
            float pg[3], qg[4];
            rt_render(c, bus, N, &ts);
            rt_render(c, bus, N, &ts);
            const float* direct = rt_direct_ambi(c);
            const RtDirectVoice* dvs = NULL;
            uint32_t ndv = rt_direct_voices(c, &dvs);
            CHECK(ndv > 0 && dvs != NULL, "mode2: rt_direct_voices exposes the point taps");
            {   /* spread 0: the voice rides its slot; the SH field stays ~empty (sqrt(0) share) */
                int found = 0; double fe = 0, se = 0;
                for (uint32_t i = 0; i < ndv; ++i)
                    if (dvs[i].active && dvs[i].mono) {
                        ++found;
                        for (uint32_t s = 0; s < N; ++s) se += fabs(dvs[i].mono[s]);
                        CHECK(dvs[i].dir[0] > 0.9f, "mode2: the published dir points at room +x");
                    }
                for (uint32_t i = 0; direct && i < 16 * N; ++i) fe += fabs(direct[i]);
                CHECK(found == 1, "mode2: exactly one active point tap");
                CHECK(se > 1e-3, "mode2: the point slot carries the voice");
                CHECK(fe < 1e-6 * (1.0 + se), "mode2: the SH field stays empty at spread 0");
            }
            rt_get_listener(c, pg, qg);
            steam_monitor_process(m, bus, direct, dvs, ndv, pg, qg, out, N);
            steam_monitor_process(m, bus, direct, dvs, ndv, pg, qg, out, N);
            printf("mode2 +x, ident: L=%.4g R=%.4g\n", e_left(), e_right());
            CHECK(e_left() > e_right() * 1.1, "mode2: +x source (listener's LEFT) favors the left ear");
            const float q180[4] = { 0, 1, 0, 0 };
            rt_set_listener(c, pl, q180);
            rt_commit(c);
            rt_render(c, bus, N, &ts);
            direct = rt_direct_ambi(c);
            ndv = rt_direct_voices(c, &dvs);
            rt_get_listener(c, pg, qg);
            steam_monitor_process(m, bus, direct, dvs, ndv, pg, qg, out, N);
            steam_monitor_process(m, bus, direct, dvs, ndv, pg, qg, out, N);
            printf("mode2 +x, yaw180: L=%.4g R=%.4g\n", e_left(), e_right());
            CHECK(e_right() > e_left() * 1.1, "mode2: head turned 180 - the +x source now favors the right ear");
            free(blk);
            rt_destroy(c);
        }
    }

    steam_monitor_destroy(m);
    free(bus); free(out);
    if (fails) { printf("steam_decode_test: %d FAILURES\n", fails); return 1; }
    printf("steam_decode_test OK (HRTF decode runs + preserves laterality: virtual-speaker, direct field, per-voice)\n");
    return 0;
}
