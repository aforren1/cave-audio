/*
 * smoke.c — lifecycle / link sanity against the public ABI (the DLL). Runs the full
 * control-side sequence for ALL THREE profiles (cave / binaural / both), so the
 * profile-specific device wiring — the binaural monitor and the 'both' dual-sink
 * double-buffer handoff — is exercised end to end. Every desc forces the offline null
 * sink (.sink = BWA_SINK_NULL), so it needs no hardware. Asserts nothing about audio.
 */
#include "bw_audio.h"

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

int main(void) {
    if (run_profile(BWA_PROFILE_CAVE,     "cave"))     return 1;
    if (run_profile(BWA_PROFILE_BINAURAL, "binaural")) return 1;
    if (run_profile(BWA_PROFILE_BOTH,     "both"))     return 1;
    if (run_room_eq_guard())                          return 1;
    printf("smoke OK (cave, binaural, both lifecycles; room_eq start guard)\n");
    return 0;
}
