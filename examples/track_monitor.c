/*
 * track_monitor.c — print the live head pose the engine is tracking (track_internal = A path).
 *
 * A production bring-up tool: confirms OptiTrack/NatNet data is flowing and the right rigid body
 * is selected, before you trust the array. Runs the binaural profile on the null sink (no audio
 * device needed — this is about tracking, not sound) and polls bw_get_listener_pose.
 *
 * Configure the tracker via env (see docs/api.md), e.g.:
 *   set BWAUDIO_NATNET_SERVER=192.168.1.10
 *   set BWAUDIO_NATNET_RIGIDBODY=Head      (a rigid-body name, or a numeric streaming ID)
 *   bw_track_monitor
 */
#include "bwaudio.h"

#include <conio.h>
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

int main(void) {
    _putenv("BWAUDIO_SINK=null");                 /* tracking only; no audio device */

    BwConfig cfg = { 0 };
    cfg.profile = BW_PROFILE_BINAURAL;
    cfg.sample_rate = 48000;
    cfg.block_size = 256;
    cfg.track_internal = true;

    BwEngine* e = bw_create(&cfg);
    if (!e) { printf("bw_create failed\n"); return 1; }
    if (bw_start(e) != 0) { printf("bw_start failed: %s\n", bw_last_error(e)); bw_destroy(e); return 1; }

    const char* rb   = getenv("BWAUDIO_NATNET_RIGIDBODY");
    const char* warn = bw_last_error(e);          /* non-fatal tracker issues (name not found, no server) */
    printf("backend    : %s\n", bw_audio_backend(e));
    printf("rigid body : %s\n", (rb && rb[0]) ? rb : "(first in frame)");
    if (warn && warn[0]) printf("tracker    : %s\n", warn);
    printf("polling listener pose (press any key to quit)...\n");

    while (!_kbhit()) {
        float p[3], q[4];
        bw_get_listener_pose(e, p, q);
        printf("\rpos [% .3f % .3f % .3f]  quat [% .3f % .3f % .3f % .3f]   ",
               p[0], p[1], p[2], q[0], q[1], q[2], q[3]);
        fflush(stdout);
        Sleep(33);                                 /* ~30 Hz readout */
    }

    printf("\n");
    bw_stop(e);
    bw_destroy(e);
    return 0;
}
