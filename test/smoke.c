/*
 * smoke.c — M0 build/link sanity check. Drives the full control-side lifecycle
 * against the stub engine (docs/api.md "Canonical call sequence"). It proves the
 * DLL loads and every exported symbol resolves; it asserts nothing about audio.
 */
#include "bwaudio.h"

#include <stdio.h>

int main(void) {
    BwConfig cfg = {
        .profile     = BW_PROFILE_BINAURAL,
        .layout_path = NULL,
        .hrtf_path   = NULL,
        .sample_rate = 48000,
        .block_size  = 256,
        .track_internal = false,
    };

    BwEngine* e = bw_create(&cfg);
    if (!e) { fprintf(stderr, "FAIL: bw_create returned NULL\n"); return 1; }

    if (bw_start(e) != 0) {
        const char* err = bw_last_error(e);
        fprintf(stderr, "FAIL: bw_start: %s\n", err ? err : "(no message)");
        bw_destroy(e);
        return 1;
    }

    BwSound  snd = bw_load_sound(e, "footsteps.wav"); /* may be 0 if the file is absent — fine here */
    BwSource s   = bw_source_create(e);
    if (s == 0) {
        fprintf(stderr, "FAIL: bw_source_create returned 0\n");
        bw_destroy(e);
        return 1;
    }

    /* one simulated frame */
    bw_source_play(e, s, snd, /*loop*/ true);
    bw_source_set_gain(e, s, 0.8f);
    bw_source_set_pos(e, s, 1.0f, 0.0f, -0.5f);
    bw_set_listener_pose(e, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 1.f);
    bw_commit(e);

    bw_play_oneshot(e, snd, 0.f, 1.f, 0.f, 1.0f);

    /* teardown */
    bw_source_stop(e, s);
    bw_source_destroy(e, s);
    bw_unload_sound(e, snd);
    bw_stop(e);
    bw_destroy(e);

    printf("smoke OK (sound=%u source=%u)\n", snd, s);
    return 0;
}
