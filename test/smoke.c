/*
 * smoke.c — lifecycle / link sanity against the public ABI (the DLL). Runs the full
 * control-side sequence for ALL THREE profiles (cave / binaural / both), so the
 * profile-specific device wiring — the binaural monitor and the 'both' dual-sink
 * double-buffer handoff — is exercised end to end. With BWAUDIO_SINK=null (set by ctest)
 * every device is the offline sink, so it needs no hardware. Asserts nothing about audio.
 */
#include "bwaudio.h"

#include <stdio.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>           /* Sleep */

static int run_profile(BwProfile profile, const char* name) {
    BwConfig cfg = {
        .profile     = profile,
        .layout_path = NULL,
        .hrtf_path   = NULL,
        .sample_rate = 48000,
        .block_size  = 256,
        .track_internal = false,
    };
    BwEngine* e = bw_create(&cfg);
    if (!e) { fprintf(stderr, "FAIL[%s]: bw_create returned NULL\n", name); return 1; }
    if (bw_start(e) != 0) {
        const char* err = bw_last_error(e);
        fprintf(stderr, "FAIL[%s]: bw_start: %s\n", name, err ? err : "(no message)");
        bw_destroy(e);
        return 1;
    }

    BwSound  snd = bw_load_sound(e, "footsteps.wav"); /* may be 0 if absent — fine here */
    BwSource s   = bw_source_create(e);
    if (s == 0) { fprintf(stderr, "FAIL[%s]: bw_source_create returned 0\n", name); bw_destroy(e); return 1; }

    bw_source_play(e, s, snd, /*loop*/ true);
    bw_source_set_gain(e, s, 0.8f);
    bw_source_set_pos(e, s, 1.0f, 0.0f, -0.5f);
    bw_set_listener_pose(e, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 1.f);
    bw_commit(e);
    bw_play_oneshot(e, snd, 0.f, 1.f, 0.f, 1.0f);

    Sleep(30);                          /* let the audio thread(s) run several blocks */

    bw_source_stop(e, s);
    bw_source_destroy(e, s);
    bw_unload_sound(e, snd);
    bw_stop(e);
    bw_destroy(e);
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
    const char* LAY = "bw_smoke_room_eq.json";
    if (!write_room_eq_layout(LAY)) { fprintf(stderr, "FAIL[guard]: cannot write layout\n"); return 1; }
    BwConfig cfg = {
        .profile     = BW_PROFILE_CAVE,
        .layout_path = LAY,
        .hrtf_path   = NULL,
        .sample_rate = 48000,
        .block_size  = 256,
        .track_internal = false,
    };
    int rc = 1;
    BwEngine* e = bw_create(&cfg);
    if (!e) { fprintf(stderr, "FAIL[guard]: bw_create returned NULL\n"); goto done; }
    if (bw_start(e) == 0) { fprintf(stderr, "FAIL[guard]: bw_start accepted room_eq + DBAP (moving listener)\n"); goto done; }
    {
        const char* err = bw_last_error(e);
        if (!err || !strstr(err, "room_eq")) { fprintf(stderr, "FAIL[guard]: error does not name room_eq: %s\n", err ? err : "(none)"); goto done; }
    }
    bw_set_panner(e, BW_PAN_SPCAP);                   /* fixed-listener session: the same layout is fine */
    if (bw_start(e) != 0) {
        fprintf(stderr, "FAIL[guard]: bw_start rejected room_eq + SPCAP: %s\n", bw_last_error(e));
        goto done;
    }
    bw_stop(e);
    rc = 0;
done:
    if (e) bw_destroy(e);
    remove(LAY);
    return rc;
}

int main(void) {
    if (run_profile(BW_PROFILE_CAVE,     "cave"))     return 1;
    if (run_profile(BW_PROFILE_BINAURAL, "binaural")) return 1;
    if (run_profile(BW_PROFILE_BOTH,     "both"))     return 1;
    if (run_room_eq_guard())                          return 1;
    printf("smoke OK (cave, binaural, both lifecycles; room_eq start guard)\n");
    return 0;
}
