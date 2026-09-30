/*
 * mic_track.h - the tracked ZM-1, shared by the C++ tools (bwa_calibrate --track, bwa_validate
 * --track, bwa_calib_view's Placement panel).
 *
 * On the rig the ZM-1 stands on a rigid stand carrying OptiTrack markers, and Motive's frame IS the
 * room frame. This opens NatNet for the stand's rigid body, loads what the mount needs, polls the
 * live pose, and hands back the array's acoustic center (placement.h: center = p + R(q) . offset)
 * and, with a body-frame survey, the capsule table re-aimed for the stand's current orientation.
 *
 * WHAT NEEDS A SURVEY. Only a mode that turns capsule arrival DIFFERENCES into a room direction
 * needs the array's orientation: the --zylia position survey, the live-aim position readout, and
 * bwa_validate. Everything that reads only the center does not: the pressure proxy's power mean and
 * zylia_center_arrival are the same at any orientation (the DOA and the capsule centroid are both in
 * the array's own frame, so their dot product does not rotate). So the offset comes from one of:
 *   - a BODY-FRAME survey (zylia_survey_save with a mount): the probed offset plus the re-aimable
 *     table. Required by the direction modes (need_body_frame);
 *   - --mount-offset x,y,z (body axes, meters) with no survey, or with a room-axes survey, which is
 *     then installed for its channel order and geometry only and never re-aimed;
 *   - --mount-offset ring: the center of the circle through the body's markers, from Motive's model
 *     definition (placement.h, place_ring_fit). For markers in a ring around the housing's equator,
 *     where Motive's default pivot (the marker centroid) sits off the ring's axis whenever the markers
 *     are unevenly spaced. It assumes the ring sits at the height of the array center;
 *   - neither: zero, which says the rigid body's pivot IS the array center (move it there in Motive).
 *
 * A SIMULATED source (--track-sim) stands in for Motive: MIC_SIM_FIXED is one constant pose (the
 * bwa_validate self-check), MIC_SIM_SCRIPT walks the stand in from about 8 cm off the target over 2 s,
 * settles about 5 mm off it, and optionally bumps it 15 mm after a set number of captures. Its truth
 * is computed by its own quaternion code (mic_track_sim_truth), not by placement.c, so a tool that
 * reports the truth beside what it measured is checking the measurement against something else.
 *
 * Unverified on hardware: everything that touches live Motive. The NatNet parser and lifecycle are
 * tested off-wire; this file's live path is not.
 */
#ifndef BWA_MIC_TRACK_H
#define BWA_MIC_TRACK_H

extern "C" {
#include "calib/zylia.h"
#include "calib/placement.h"
#include "tracking/natnet.h"
}
#include <stddef.h>
#include <atomic>

enum { MIC_SIM_OFF = 0, MIC_SIM_FIXED = 1, MIC_SIM_SCRIPT = 2 };
enum { MIC_OFFSET_ZERO = 0, MIC_OFFSET_SURVEY = 1, MIC_OFFSET_FLAG = 2, MIC_OFFSET_RING = 3 };

#define MIC_SIM_BUMP_M 0.015f          /* the scripted bump's size: past half of any sane tolerance */
/* After a retarget the scripted stand stays where it was for this long before it walks in, the way an
 * operator takes a moment to carry it. Longer than the gate's 0.5 s window plus 1 s hold, so a gate
 * that accepts stillness anywhere opens at the PREVIOUS placement, which is the failure it exists to
 * expose. */
#define MIC_SIM_LINGER_S 2.5

struct MicTrackCfg {
    const char* body;              /* rigid-body streaming id or name (ignored by a simulated source) */
    const char* server;            /* Motive host; REQUIRED to track by name */
    const char* multicast;         /* NULL = 239.255.42.99 */
    const char* survey_path;       /* NULL = no survey */
    int         need_body_frame;   /* a direction mode: refuse anything but a body-frame survey */
    int         have_offset;       /* --mount-offset x,y,z given */
    float       offset_m[3];       /* ... in the rigid body's axes, meters */
    int         offset_ring;       /* --mount-offset ring: fit the marker ring from Motive's model definition */
    int         sim;               /* MIC_SIM_* */
    int         sim_builtin_body;  /* sim with no survey: install the built-in table AS the body frame
                                    * (bwa_validate's self-check, which is about wiring, not the loader) */
    int         sim_bump_after;    /* MIC_SIM_SCRIPT: bump after this many captures, 0 = never */
    int         virtual_clock;     /* sim: time advances only through mic_track_advance/_note_capture */
};

struct MicPose { float p[3], q[4], center[3]; };

struct MicTrack {
    NatNet*    nn;
    int        sim, sim_bump_after, virtual_clock;
    int        have_survey, body_frame, offset_src;
    ZyliaMount mount;
    float      offset[3];                   /* the offset in use, body axes */
    PlaceRing  ring;                        /* the ring fit, when offset_src == MIC_OFFSET_RING */
    float      sim_true_off[3];             /* the simulated stand's TRUE offset (the ring's own center) */
    float      caps_body[ZYLIA_MICS][3];    /* valid when body_frame */
    char       body[128];
    double     vt;                          /* the virtual clock */
    long long  t0_ns;                       /* the wall clock's origin */
    std::atomic<int> ncap;                  /* captures noted (a run's worker writes, the poller reads) */
    float      sim_target[3];
    double     sim_t0;
    float      sim_prev[3];                 /* where the stand stood before the last retarget */
    int        sim_have_prev;
    int        sim_ntarget;                 /* retargets so far: the first has no previous spot */
};

/* Open: load the survey (installed through zylia_survey_load), settle the offset, and open NatNet
 * (or the simulated source). Blocks up to about a second on a live server's handshake, so a GUI calls
 * it from a worker. Returns 0, or an exit code (2 = a usage problem, 1 = the tracker would not open)
 * with a one-line reason in err. Call mic_track_close on success only. */
int  mic_track_open(MicTrack* T, const MicTrackCfg* c, char* err, size_t errcap);
void mic_track_close(MicTrack* T);

/* Where the offset came from and what the table is, one line each (ASCII, no trailing newline). */
void mic_track_describe(const MicTrack* T, char* buf, size_t cap);

/* The source's clock (seconds): wall time since open, or the virtual clock. */
double mic_track_now(MicTrack* T);
void   mic_track_advance(MicTrack* T, double s);             /* virtual clock only; no-op otherwise */
/* A capture finished: count it (the scripted bump is keyed on the count, which makes it ordering
 * rather than timing) and advance the virtual clock by its length. Any thread. */
void   mic_track_note_capture(MicTrack* T, double capture_s);
/* MIC_SIM_SCRIPT: restart the approach toward a new target from now. No-op otherwise. */
void   mic_track_sim_target(MicTrack* T, const float target[3]);
/* The simulated source's TRUE center at its current time (independent quaternion code). 0 = not a
 * simulated source. */
int    mic_track_sim_truth(MicTrack* T, float center[3]);

/* One LIVE pose and its center. A live tracker must report NN_STATUS_LIVE (see the comment in
 * mic_track.cpp); a refused pose or offset reads as no pose. Returns 1 with out filled, else 0. */
int    mic_track_read(MicTrack* T, MicPose* out);

/* With a body-frame survey: rotate the body table by q and install it (zylia_set_capsules), the
 * array as it is turned right now. Returns 1; 0 = no body-frame table, nothing installed. */
int    mic_track_aim_capsules(const MicTrack* T, const float q[4]);

/* "move 12 mm toward the front, 5 mm right, 3 mm down" for delta = center - target, in room words
 * (+z is the front, room-right is -x, +y is up); axes under 0.5 mm are left out, "on target" if all. */
void   mic_track_move_words(const float delta[3], char* buf, size_t cap);

/* ---- console helpers for bwa_calibrate ---- */

struct MicPlaced {
    float center[3];               /* the window mean the gate accepted: THE mic position */
    float q[4];                    /* the pose's orientation at acceptance */
    float dist_m;                  /* |center - target| */
    float yaw_deg, tilt_deg;       /* the mount (place_mount_angles) */
    int   forced;                  /* a key took the reading before the gate opened */
};

/* Wait for the gate on a live single-line readout (console \r updates): the measured center, the
 * target, dx/dy/dz and the total in mm, and HOLD or OK. keys != 0 (the rig): a key takes the current
 * reading anyway, with a warning; with no pose at all it aborts. Returns 0 placed (out filled, the
 * capsule table re-aimed when it can be), 1 timed out or aborted (the reason printed). */
int mic_track_place_console(MicTrack* T, const float target[3], const PlaceCfg* cfg, double timeout_s,
                            int keys, const char* what, MicPlaced* out);

/* The bump check after one capture: note it, read the center, place_bump against `taken`. A live
 * tracker gets up to half a second for a fresh pose. Returns 1 bumped, 0 in place, -1 no pose. */
int mic_track_bump_console(MicTrack* T, const float taken[3], float tol_m, double capture_s, float* moved_m,
                           float now_center[3]);

#endif /* BWA_MIC_TRACK_H */
