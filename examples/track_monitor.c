/*
 * track_monitor.c — print the live head pose the engine is tracking (internal tracking).
 *
 * A production bring-up tool: confirms OptiTrack/NatNet data is flowing and the right rigid body
 * is selected, before you trust the array. Runs the binaural profile on the null sink (no audio
 * device needed — this is about tracking, not sound), connects the tracker, and polls
 * bwa_get_listener_pose. The tracker is configured by command line:
 *
 *   bwa_track_monitor [server] [rigid-body]
 *
 *   server      Motive host IP for the command handshake (omit for multicast-only)
 *   rigid-body  a rigid-body name (needs the server), or a numeric streaming ID;
 *               omit to follow the first rigid body in the frame
 */
#include "bw_audio.h"

#include <conio.h>
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

int main(int argc, char** argv) {
    bwa_desc cfg = { 0 };
    cfg.sink = BWA_SINK_NULL;                 /* tracking only; no audio device */
    cfg.profile = BWA_PROFILE_BINAURAL;
    cfg.sample_rate = 48000;
    cfg.block_size = 256;

    bwa_engine* e = bwa_create(&cfg);
    if (!e) { printf("bwa_create failed\n"); return 1; }
    if (bwa_start(e) != BWA_OK) { printf("bwa_start failed: %s\n", bwa_last_error(e)); bwa_destroy(e); return 1; }

    /* argv[2] is a streaming ID if fully numeric, else a rigid-body name */
    bwa_tracker_desc td = { 0 };
    if (argc > 1 && argv[1][0]) td.server = argv[1];
    const char* rb = (argc > 2) ? argv[2] : NULL;
    if (rb && rb[0]) {
        char* end; long v = strtol(rb, &end, 10);
        if (*end == 0) td.rigid_body_id = (int32_t)v; else td.rigid_body_name = rb;
    }
    if (bwa_tracker_connect(e, &td) != BWA_OK) {
        printf("tracker connect failed: %s\n", bwa_last_error(e));
        bwa_destroy(e);
        return 1;
    }

    printf("backend    : %s\n", bwa_get_audio_backend(e));
    printf("rigid body : %s\n", (rb && rb[0]) ? rb : "(first in frame)");
    printf("polling listener pose (press any key to quit)...\n");

    while (!_kbhit()) {
        float p[3], q[4];
        bwa_get_listener_pose(e, p, q);
        printf("\rpos [% .3f % .3f % .3f]  quat [% .3f % .3f % .3f % .3f]   ",
               p[0], p[1], p[2], q[0], q[1], q[2], q[3]);
        fflush(stdout);
        Sleep(33);                                 /* ~30 Hz readout */
    }

    printf("\n");
    bwa_tracker_disconnect(e);
    bwa_stop(e);
    bwa_destroy(e);
    return 0;
}
