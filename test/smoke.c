/*
 * smoke.c — lifecycle / link sanity against the public ABI (the DLL). Runs the full
 * control-side sequence for ALL THREE profiles (cave / binaural / both), so the
 * profile-specific device wiring — the binaural monitor and the 'both' dual-sink
 * double-buffer handoff — is exercised end to end. With BWAUDIO_SINK=null (set by ctest)
 * every device is the offline sink, so it needs no hardware. Asserts nothing about audio.
 */
#include "bwaudio.h"

#include <stdio.h>

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

int main(void) {
    if (run_profile(BW_PROFILE_CAVE,     "cave"))     return 1;
    if (run_profile(BW_PROFILE_BINAURAL, "binaural")) return 1;
    if (run_profile(BW_PROFILE_BOTH,     "both"))     return 1;
    printf("smoke OK (cave, binaural, both lifecycles)\n");
    return 0;
}
