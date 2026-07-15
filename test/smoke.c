/*
 * smoke.c — lifecycle / link sanity against the public ABI (the DLL). Runs the full
 * control-side sequence for ALL THREE profiles (cave / binaural / both), so the
 * profile-specific device wiring — the binaural monitor and the 'both' dual-sink
 * double-buffer handoff — is exercised end to end. Every desc forces the offline null
 * sink (.sink = BWA_SINK_NULL), so it needs no hardware. Asserts nothing about audio.
 */
#include "bw_audio.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>           /* Sleep */

static int run_profile(bwa_profile profile, const char* name) {
    bwa_desc cfg = {
        .profile     = profile,
        .layout_path = NULL,
        .hrtf_path   = NULL,
        .sample_rate = 48000,
        .block_size  = 256,
        .sink        = BWA_SINK_NULL,          /* hermetic: no real device */
    };
    bwa_engine* e = bwa_create(&cfg);
    if (!e) { fprintf(stderr, "FAIL[%s]: bwa_create returned NULL\n", name); return 1; }
    if (bwa_start(e) != 0) {
        const char* err = bwa_last_error(e);
        fprintf(stderr, "FAIL[%s]: bwa_start: %s\n", name, err ? err : "(no message)");
        bwa_destroy(e);
        return 1;
    }

    bwa_sound  snd = bwa_load_sound(e, "footsteps.wav"); /* may be 0 if absent — fine here */
    bwa_source s   = bwa_source_create(e);
    if (s == 0) { fprintf(stderr, "FAIL[%s]: bwa_source_create returned 0\n", name); bwa_destroy(e); return 1; }

    bwa_source_play(e, s, snd, /*loop*/ true);
    bwa_source_set_gain(e, s, 0.8f);
    bwa_source_set_pos(e, s, 1.0f, 0.0f, -0.5f);
    bwa_set_listener_pose(e, 0.f, 1.5f, 0.f, 0.f, 0.f, 0.f, 1.f);   /* the default grid's ear point */
    bwa_commit(e);
    bwa_play_oneshot(e, snd, 0.f, 1.f, 0.f, 1.0f);

    /* the bwa_bed_* facade forwards to the same per-voice machinery — exercise every export on a
     * live engine (no AmbiX asset here, so no bed_play; the rest is enqueue-only and must be safe
     * on an idle bed handle) */
    bwa_bed bed = bwa_bed_create(e);
    if (bed == 0) { fprintf(stderr, "FAIL[%s]: bwa_bed_create returned 0\n", name); bwa_destroy(e); return 1; }
    bwa_bed_set_gain(e, bed, 0.5f);
    bwa_bed_set_rotation(e, bed, 0.5f);
    bwa_bed_set_priority(e, bed, 255);
    bwa_bed_set_group(e, bed, 1);
    bwa_bed_fade_to(e, bed, 0.2f, 0.1f);
    bwa_bed_set_paused(e, bed, true);
    bwa_bed_set_paused(e, bed, false);
    bwa_bed_seek(e, bed, 0);
    if (bwa_bed_is_playing(e, bed)) { fprintf(stderr, "FAIL[%s]: idle bed reads as playing\n", name); bwa_destroy(e); return 1; }
    bwa_bed_fade_out(e, bed, 0.05f);
    bwa_bed_stop(e, bed);
    bwa_bed_destroy(e, bed);

    Sleep(30);                          /* let the audio thread(s) run several blocks */

    bwa_source_stop(e, s);
    bwa_source_destroy(e, s);
    bwa_unload_sound(e, snd);
    bwa_stop(e);
    bwa_destroy(e);
    return 0;
}

/* a valid grid layout whose speaker 0 carries a room_eq section (static-listener room correction) */
static int write_room_eq_layout(const char* path) {
    FILE* f = fopen(path, "wb");
    if (!f) return 0;
    const float ax[3] = { -1.5f, 0.f, 1.5f };
    int k = 0;
    fprintf(f, "{ \"speakers\": [\n");
    for (int yi = 0; yi < 3; ++yi) for (int xi = 0; xi < 3; ++xi) for (int zi = 0; zi < 3; ++zi) {
        if (ax[xi] == 0 && ax[yi] == 0 && ax[zi] == 0) continue;
        fprintf(f, "  {\"index\":%d,\"position\":[%g,%g,%g]%s}%s\n", k, ax[xi], ax[yi], ax[zi],
                k == 0 ? ",\"room_eq\":[{\"fc\":62.5,\"gain_db\":-6.0,\"q\":4.0}]" : "",
                (k == 25) ? "" : ",");
        ++k;
    }
    fprintf(f, "] }\n");
    fclose(f);
    return k == 26;
}

/* the room_eq start guard: a room_eq'd layout must refuse a moving-listener session (DBAP is the
 * default panner) and start cleanly once the session is a fixed-listener one (SPCAP/VBAP). */
static int run_room_eq_guard(void) {
    const char* LAY = "bwa_smoke_room_eq.json";
    if (!write_room_eq_layout(LAY)) { fprintf(stderr, "FAIL[guard]: cannot write layout\n"); return 1; }
    bwa_desc cfg = {
        .profile     = BWA_PROFILE_CAVE,
        .layout_path = LAY,
        .hrtf_path   = NULL,
        .sample_rate = 48000,
        .block_size  = 256,
        .sink        = BWA_SINK_NULL,          /* hermetic: no real device */
    };
    int rc = 1;
    bwa_engine* e = bwa_create(&cfg);
    if (!e) { fprintf(stderr, "FAIL[guard]: bwa_create returned NULL\n"); goto done; }
    if (bwa_start(e) == 0) { fprintf(stderr, "FAIL[guard]: bwa_start accepted room_eq + DBAP (moving listener)\n"); goto done; }
    {
        const char* err = bwa_last_error(e);
        if (!err || !strstr(err, "room_eq")) { fprintf(stderr, "FAIL[guard]: error does not name room_eq: %s\n", err ? err : "(none)"); goto done; }
    }
    bwa_set_panner(e, BWA_PAN_SPCAP);                   /* fixed-listener session: the same layout is fine */
    if (bwa_start(e) != 0) {
        fprintf(stderr, "FAIL[guard]: bwa_start rejected room_eq + SPCAP: %s\n", bwa_last_error(e));
        goto done;
    }
    bwa_stop(e);
    rc = 0;
done:
    if (e) bwa_destroy(e);
    remove(LAY);
    return rc;
}

/* the push-API kind guards, at the ABI: pushing to a live NON-push source must accept nothing AND
 * report (a bare 0 reads exactly like ring backpressure), a real push must work, and a pending play
 * must read as playing before the first rendered block (the poll-then-destroy window). */
static int run_push_guard(void) {
    bwa_desc cfg = {
        .profile     = BWA_PROFILE_CAVE,
        .sample_rate = 48000,
        .block_size  = 256,
        .sink        = BWA_SINK_NULL,          /* hermetic: no real device */
    };
    int rc = 1;
    float blk[64] = {0};
    bwa_engine* e = bwa_create(&cfg);
    if (!e) { fprintf(stderr, "FAIL[push]: bwa_create returned NULL\n"); return 1; }
    if (bwa_start(e) != 0) { fprintf(stderr, "FAIL[push]: bwa_start: %s\n", bwa_last_error(e)); goto done; }
    {
        bwa_source plain = bwa_source_create(e);
        bwa_source push  = bwa_source_create_stream(e);
        if (!plain || !push) { fprintf(stderr, "FAIL[push]: source create\n"); goto done; }
        if (!bwa_source_is_playing(e, push)) { fprintf(stderr, "FAIL[push]: pending play must read as playing\n"); goto done; }
        if (bwa_source_push(e, plain, blk, 64) != 0 || !bwa_last_error(e)) {
            fprintf(stderr, "FAIL[push]: push on a non-push source must accept nothing AND set bwa_last_error\n"); goto done;
        }
        if (bwa_source_push_space(e, plain) != 0) { fprintf(stderr, "FAIL[push]: push_space on a non-push source\n"); goto done; }
        if (bwa_source_push(e, push, blk, 64) != 64) { fprintf(stderr, "FAIL[push]: push source refused frames\n"); goto done; }
        bwa_source_destroy(e, push);
        bwa_source_destroy(e, plain);
    }
    bwa_stop(e);
    rc = 0;
done:
    bwa_destroy(e);
    return rc;
}

/* binaural laterality through the REAL dll, end to end: bwa_create's own construction (layout,
 * monitor at the sink block size), render_binaural on the sink thread, whichever monitor the build
 * carries (steam HRTF or simple-pan — same convention either way). The null sink's test tap
 * observes the device-bound stereo the sink otherwise discards. The room frame is RH, +y up,
 * +z ahead: the identity listener's RIGHT ear is at -x (bw_audio.h BWA_ROOM_RIGHT), so a source
 * at (-1.5, 1.5, 0) must reach device channel 1 (R) — and channel 0 (L) after a 180-degree turn. */
__declspec(dllimport) extern void (*bwa_null_sink_tap)(const float* bus, unsigned channels, unsigned block_size);
static double tap_sum[2];
static void stereo_tap(const float* bus, unsigned channels, unsigned n) {
    if (channels != 2) return;
    for (unsigned i = 0; i < n; ++i) { tap_sum[0] += fabs(bus[i]); tap_sum[1] += fabs(bus[n + i]); }
}
/* the PUBLIC capture path (bwa_set_output_capture) — fires per block in render_binaural with the same
 * final device-bound stereo, planar. Same accumulation as the tap; asserted to carry the laterality. */
static double cap_sum[2];
static void capture_probe(void* user, const float* planar, uint32_t channels, uint32_t n) {
    (void)user;
    if (channels != 2) return;
    for (uint32_t i = 0; i < n; ++i) { cap_sum[0] += fabs(planar[i]); cap_sum[1] += fabs(planar[n + i]); }
}
static int run_binaural_laterality(void) {
    bwa_desc cfg = {
        .profile     = BWA_PROFILE_BINAURAL,
        .sample_rate = 48000,
        .block_size  = 256,
        .sink        = BWA_SINK_NULL,
    };
    int rc = 1;
    bwa_engine* e = bwa_create(&cfg);
    if (!e) { fprintf(stderr, "FAIL[lat]: bwa_create returned NULL\n"); return 1; }
    {
        bwa_source s = bwa_source_create_stream(e);
        if (!s) { fprintf(stderr, "FAIL[lat]: create_stream: %s\n", bwa_last_error(e)); goto done; }
        /* a TONE, never DC: the HRTF's per-ear DC gains are laterally opposite its audible ILD, so a
         * DC-driven laterality check passes exactly when the image is MIRRORED (steam_decode.c 2b) */
        float blk[4096];
        for (int k = 0; k < 16; ++k) {
            for (int i = 0; i < 4096; ++i)
                blk[i] = 0.25f * sinf(6.2831853f * 660.0f * (float)(k * 4096 + i) / 48000.0f);
            bwa_source_push(e, s, blk, 4096);                            /* ~1.4 s of signal */
        }
        bwa_source_set_pos(e, s, -1.5f, 1.5f, 0.0f);        /* the identity listener's RIGHT (-x) */
        bwa_set_listener_pose(e, 0.f, 1.5f, 0.f, 0.f, 0.f, 0.f, 1.f);
        bwa_commit(e);
        bwa_null_sink_tap = stereo_tap;
        bwa_set_output_capture(e, capture_probe, NULL);     /* the public capture path, alongside the test tap */
        tap_sum[0] = tap_sum[1] = 0.0;
        cap_sum[0] = cap_sum[1] = 0.0;
        if (bwa_start(e) != 0) { fprintf(stderr, "FAIL[lat]: bwa_start: %s\n", bwa_last_error(e)); goto done; }
        char backend[96];
        snprintf(backend, sizeof backend, "%s", bwa_get_audio_backend(e));   /* read while the sink is open */
        Sleep(400);
        bwa_stop(e);                                        /* sink thread joined: tap_sum is safe to read */
        printf("smoke[lat] %s: -x ident L=%.3g R=%.3g\n", backend, tap_sum[0], tap_sum[1]);
        if (!(tap_sum[1] > tap_sum[0] * 1.1)) {
            fprintf(stderr, "FAIL[lat]: a -x source must reach the RIGHT device channel at identity\n"); goto done;
        }
        printf("smoke[cap] bwa_set_output_capture: L=%.3g R=%.3g\n", cap_sum[0], cap_sum[1]);
        if (!(cap_sum[0] > 0.0) || !(cap_sum[1] > cap_sum[0] * 1.1)) {   /* fired + carried the final stereo */
            fprintf(stderr, "FAIL[cap]: bwa_set_output_capture must deliver the final binaural output (right-biased for a -x source)\n"); goto done;
        }
        bwa_set_listener_pose(e, 0.f, 1.5f, 0.f, 0.f, 1.f, 0.f, 0.f);   /* yaw 180: -x is now the LEFT */
        bwa_commit(e);
        tap_sum[0] = tap_sum[1] = 0.0;
        if (bwa_start(e) != 0) { fprintf(stderr, "FAIL[lat]: restart: %s\n", bwa_last_error(e)); goto done; }
        Sleep(400);
        bwa_stop(e);
        printf("smoke[lat] yaw180: L=%.3g R=%.3g\n", tap_sum[0], tap_sum[1]);
        if (!(tap_sum[0] > tap_sum[1] * 1.1)) {
            fprintf(stderr, "FAIL[lat]: after a 180-degree turn the -x source must reach the LEFT channel\n"); goto done;
        }
    }
    rc = 0;
done:
    bwa_null_sink_tap = NULL;
    bwa_destroy(e);
    return rc;
}

int main(void) {
    if (run_profile(BWA_PROFILE_CAVE,     "cave"))     return 1;
    if (run_profile(BWA_PROFILE_BINAURAL, "binaural")) return 1;
    if (run_profile(BWA_PROFILE_BOTH,     "both"))     return 1;
    if (run_room_eq_guard())                          return 1;
    if (run_push_guard())                             return 1;
    if (run_binaural_laterality())                    return 1;
    printf("smoke OK (cave, binaural, both lifecycles; room_eq start guard; push kind guards; binaural laterality)\n");
    return 0;
}
