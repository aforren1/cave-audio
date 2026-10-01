/*
 * placement.h - putting the tracked ZM-1 where a measurement needs it, and knowing where it was.
 *
 * On the rig the ZM-1 stands on a rigid stand carrying OptiTrack markers, and Motive's frame IS the
 * room frame. The pose gives the stand's body origin and orientation; a body-frame capsule survey
 * (zylia.h, ZyliaMount) gives the probed offset from that origin to the array's acoustic center.
 * This file is the pure math between the two and a decision:
 *
 *   center = p + R(q) . offset                                     (place_center)
 *   delta  = center - target, per axis and total                   (place_gate_update)
 *   still  = every center of the last ~0.5 s within a small spread (the stillness window)
 *   gate   = within tolerance AND still, and has STAYED so for a hold time
 *   bump   = the center after a capture against the one the run took  (place_bump)
 *   turn   = the stand's orientation against the one the run took     (place_turn_deg, place_bump_pose)
 *
 * The orientation half exists for the DIRECTION modes only (the --zylia position survey, the live-aim
 * position readout, bwa_validate): they turn capsule arrival differences into room directions, so a
 * stand turned about the array center, which moves no center at all, turns every direction they read.
 * The center-only modes (trims, verify, the tilt meter, --localize, --room-eq-grid) keep center-only
 * checks: their pressure proxy does not rotate, and a turn about any other point moves the center.
 *
 * No clock, no tracker, no I/O: the caller passes the time and the pose, so every rule here is
 * unit-tested with synthetic poses (test/placement_test.c). The tools that poll a live tracker
 * (bwa_calibrate --track, bwa_calib_view's Placement panel) sit on top, in examples/mic_track.cpp.
 *
 * Every input is guarded the sane.h way: a non-finite OR absurd coordinate, quaternion, offset or
 * time is refused, never propagated. A tracker frame with a garbage body is a dropped frame, and a
 * dropped frame must read as "no pose", not as a center 1e30 m away that the stillness spread then
 * squares into Inf.
 */
#ifndef BWA_PLACEMENT_H
#define BWA_PLACEMENT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A tracker coordinate past this (meters) is a corrupt frame, not a room. Far inside float range
 * for the squares the spread and the delta take, and far past any CAVE. */
#define PLACE_MAX_COORD_M   1000.0f
/* A mount offset past this is a corrupt survey (the arm is about 10 cm); the survey loader uses the
 * same bound. */
#define PLACE_MAX_OFFSET_M  10.0f

/* The defaults, and why (docs/calibration.md, "Placing the ZM-1 with the tracker"):
 * WINDOW: half a second of centers. Long enough to span a hand letting go of the stand, short enough
 *   that the readout answers within a breath of the stand stopping.
 * STILL: the window's spread limit is a quarter of the tolerance, clamped to [0.5, 2] mm. Motive's
 *   rigid-body jitter is a few tenths of a millimeter, so 0.5 mm is the floor it can pass; 2 mm is
 *   well under the bump limit (half the tolerance, 5 mm at the default 10), so a stand that reads
 *   still cannot be halfway to a bump.
 * HOLD: one second of "within tolerance and still" before the gate opens, on top of the window.
 *   A stand reads still the moment the hand pauses; it has settled only when it stays put after
 *   the hand is gone. 1.5 s of stillness in all costs nothing against a 2 s sweep. */
#define PLACE_WINDOW_S       0.5
#define PLACE_HOLD_S         1.0
#define PLACE_STILL_MIN_M    0.0005f
#define PLACE_STILL_MAX_M    0.002f
#define PLACE_TOL_DEFAULT_M  0.010f
#define PLACE_TOL_MAX_M      1.0f
/* ring capacity; samples closer together than window / (PLACE_HIST / 2) are not stored, so any
 * polling rate fills the window without overflowing it */
#define PLACE_HIST           128

/* The orientation limits, and why (docs/calibration.md, "The bump check"):
 * BUDGET: a turn of theta about the array center turns every direction a direction mode reads by
 *   theta, so a result at range r moves theta * r. The center's bump limit, half the tolerance, is what
 *   a placement may lose to a move; the same budget as a turn is atan(tol / 2 / r) (place_turn_limit_deg):
 *     bwa_validate, 20 mm at the 1.4 m source radius: 0.41 deg (the position limit is the same 0.4 deg
 *       of direction, a fifth of the estimator's 2 deg anechoic floor);
 *     the --zylia survey, 10 mm against the farthest speaker (2 to 3 m on the CAVE): 0.10 to 0.14 deg;
 *     the live position readout, 10 mm against its one speaker: about the same.
 * JITTER: Motive's orientation of a small rigid body jitters by a few hundredths of a degree RMS, with
 *   peaks of a tenth or two (0.1 mm of marker noise across a 60 mm ring of 4 markers is 0.05 deg RMS,
 *   0.2 deg at 4 sigma). Every limit here compares ONE pose with a window mean, so it has to clear that
 *   single-frame peak: PLACE_TURN_MIN_DEG is 0.3 deg, 1.5x the 0.2 deg peak, and every limit floors
 *   there. The --zylia survey and the live readout therefore run AT the floor: 0.3 deg is 13 mm at
 *   2.5 m, about the whole 10 mm tolerance rather than half of it. That is what one Motive frame can
 *   resolve, not what the survey would like. Unverified against live Motive: if a still stand trips it
 *   on the rig, the floor is the number to revisit.
 * STILL: the gate's orientation term is the window's spread (the largest angle of a sample from the
 *   window mean) under max(PLACE_TURN_MIN_DEG, half the turn limit): 0.3 deg at every default, for the
 *   same single-frame reason. A stand that is still in position but turning about its center reads
 *   MOVING ("turning").
 * CAP: PLACE_TURN_MAX_DEG, so an absurd tolerance or range cannot switch the check off. */
#define PLACE_TURN_MIN_DEG   0.3f
#define PLACE_TURN_MAX_DEG   5.0f

/* center = p + R(q) . offset. q is xyzw (NatNet's order) and is normalized here; a non-finite q or
 * one whose norm is outside [0.5, 2] is refused (a tracker quaternion is unit to float precision, so
 * anything else is a corrupt frame, NOT a request for identity). p is bounded by PLACE_MAX_COORD_M,
 * offset by PLACE_MAX_OFFSET_M (NULL = zero offset). Returns 1, or 0 with out untouched. */
int place_center(const float p[3], const float q[4], const float offset[3], float out[3]);

/* The stand's mount orientation for a report, from a unit quaternion (0 on a refused q):
 * yaw  = the bearing of the body's +z axis, clockwise from above, 0 = room-ahead (+z), 90 =
 *        room-right (-x): the aim sheet's convention;
 * tilt = the angle between the body's +y axis and room up (0 = a level stand). */
int place_mount_angles(const float q[4], float* yaw_deg, float* tilt_deg);

/* The angle of the rotation between two orientations, degrees in [0, 180]: the angle of a^-1 . b, so
 * how far the stand turned from a to b about ANY axis. Sign-safe: q and -q are one rotation. Each q is
 * refused the way place_center refuses it (non-finite, or a norm outside [0.5, 2]). Returns 1, or 0
 * with deg untouched. */
int place_turn_deg(const float a[4], const float b[4], float* deg);

/* A direction mode's turn limit for a result at range_m: atan(tol / 2 / range) in degrees, floored at
 * PLACE_TURN_MIN_DEG and capped at PLACE_TURN_MAX_DEG (see above). A non-finite or non-positive input
 * gives the floor, the strictest limit. */
float place_turn_limit_deg(float tol_m, float range_m);

typedef struct {
    float  tol_m;       /* <= 0: no tolerance, the gate accepts on stillness alone (--localize) */
    float  still_m;     /* spread limit over the window */
    double window_s;
    double hold_s;
    float  still_deg;   /* orientation spread limit over the window; 0 = center only (no orientation term) */
} PlaceCfg;

/* The defaults above for a tolerance in meters (clamped to [0, PLACE_TOL_MAX_M]; non-finite = the
 * default 10 mm). tol_m <= 0 keeps a spread limit of PLACE_STILL_MAX_M. Center only: still_deg = 0. */
void place_cfg_default(PlaceCfg* c, float tol_m);
/* Add the orientation term for a direction mode: still_deg = max(PLACE_TURN_MIN_DEG, turn_limit / 2),
 * capped at PLACE_TURN_MAX_DEG (a non-finite limit gives the floor). */
void place_cfg_direction(PlaceCfg* c, float turn_limit_deg);

typedef enum {
    PLACE_NO_POSE = 0,  /* no usable pose this update (and the history is dropped) */
    PLACE_MOVING,       /* the window is not full yet, or its spread is over the limit */
    PLACE_OFF_TARGET,   /* still, but the window mean is outside the tolerance */
    PLACE_SETTLING,     /* still and within tolerance, but not yet for the hold time */
    PLACE_OK            /* the gate is open: take `mean` as the mic position */
} PlaceState;

typedef struct {
    PlaceCfg   cfg;
    /* the window: a ring of stored samples */
    double     t[PLACE_HIST];
    float      c[PLACE_HIST][3];
    float      q[PLACE_HIST][4];  /* unit, when hq */
    unsigned char hq[PLACE_HIST]; /* the sample carried a usable orientation */
    int        head, n;
    double     last_t;            /* the last update's time (any update, stored or not) */
    int        have_last;
    int        ok_run;            /* within tolerance and still since ok_since */
    double     ok_since;
    /* the last update's outputs */
    PlaceState state;
    float      center[3];         /* the latest center */
    float      mean[3];           /* the window mean: what the gate accepts */
    float      spread_m;          /* the largest distance of a window sample from the mean */
    float      delta[3];          /* latest center - target (the live readout: what to move) */
    float      dist_m;            /* |delta| */
    float      mean_dist_m;       /* |mean - target|: what the tolerance is tested on */
    double     span_s;            /* time the window's samples cover */
    double     held_s;            /* time within tolerance and still (0 unless SETTLING or OK) */
    /* the orientation, when every window sample carried one (have_q) */
    int        have_q;
    float      qmean[4];          /* the window's mean orientation (unit): what a direction run takes */
    float      turn_spread_deg;   /* the largest angle of a window sample from qmean */
    int        turning;           /* MOVING only because of the orientation term */
} PlaceGate;

/* Start (or restart) a gate. cfg NULL = place_cfg_default(10 mm). */
void place_gate_init(PlaceGate* g, const PlaceCfg* cfg);
/* Drop the history and the hold, keep the config (a new target, a remount). */
void place_gate_reset(PlaceGate* g);

/* One update at time t (seconds, any monotonic origin). center NULL, or a refused center or target,
 * or a non-finite t, = no pose: the history and the hold are dropped and the state is NO_POSE,
 * because an occluded stand may come back somewhere else. A time that runs BACKWARD also drops the
 * history (a clock step is not stillness). Still = the window covers at least 90% of window_s with
 * at least 3 samples, and every sample sits within still_m of their mean. Tolerance is tested on the
 * MEAN, inclusive (dist <= tol); the readout's delta is the LATEST center, which answers the hand
 * without the window's lag. Returns the new state. */
PlaceState place_gate_update(PlaceGate* g, double t, const float center[3], const float target[3]);
/* The same with the pose's orientation q (xyzw, NULL = none). A usable q is stored with the sample, and
 * when every window sample has one the gate reports qmean and turn_spread_deg. With cfg.still_deg > 0
 * (a direction mode) a NULL or refused q is no pose, and still also needs turn_spread_deg <= still_deg;
 * a stand that fails only that reads MOVING with `turning` set. With still_deg == 0 the orientation
 * never changes the state. */
PlaceState place_gate_update_q(PlaceGate* g, double t, const float center[3], const float q[4], const float target[3]);
/* Why the gate is not open, for a readout: "turning" when the orientation alone holds it, else
 * place_state_name(g->state). */
const char* place_gate_reason(const PlaceGate* g);

/* The bump check: did the center move more than half the tolerance between the center a run took
 * (`taken`) and now? Returns 1 = bumped, 0 = in place, -1 = cannot tell (a refused input or a
 * non-positive tolerance). moved_m (NULL ok) gets the distance when both inputs are usable. Half the
 * tolerance: once the mic moves, the run's speakers were measured from two points, and a move of d
 * shifts some arrivals against others by up to d / c. The tolerance is the whole placement budget,
 * so a move inside half of it cannot cost more than the placement itself was allowed to. */
int place_bump(const float taken[3], const float now[3], float tol_m, float* moved_m);

/* The orientation-aware bump check for a direction mode: place_bump on the centers, plus the turn
 * between q_taken and q_now against turn_limit_deg (place_turn_limit_deg). Returns a mask,
 * PLACE_BUMP_MOVED | PLACE_BUMP_TURNED, 0 = in place, or -1 = cannot tell. turn_limit_deg <= 0 is a
 * center-only check: the q arguments are ignored and the verdict is place_bump's. A NaN limit is -1,
 * never "center only", and a limit past PLACE_TURN_MAX_DEG is capped there. A definite move
 * stands even when a q is refused; otherwise a refused q (with a limit) is -1. moved_m and turned_deg
 * (NULL ok) get each quantity that could be computed. */
#define PLACE_BUMP_MOVED  1
#define PLACE_BUMP_TURNED 2
int place_bump_pose(const float c_taken[3], const float q_taken[4], const float c_now[3], const float q_now[4],
                    float tol_m, float turn_limit_deg, float* moved_m, float* turned_deg);

/* ---- the mount offset from a ring of markers (--mount-offset ring) ----
 * The ZM-1's markers sit in a ring around the housing's equator. Motive's default pivot is the
 * marker CENTROID, which lies on the ring's axis only when the markers are evenly spaced, and uneven
 * spacing is what gives the body its yaw: 0/90/180/225 degrees on a 60 mm ring puts the centroid
 * 11 mm off the axis. So fit the ring instead: a plane through the markers (the smallest principal
 * axis of their scatter), then a circle in that plane (the algebraic least-squares fit, exact for
 * markers on a circle however they are spaced). The circle's center, in the body frame the markers
 * are given in, IS the mount offset.
 *
 * ASSUMPTION: the ring sits at the height of the array center, the capsule sphere's equator. The fit
 * cannot see a ring that sits higher or lower on the housing, and that vertical error passes
 * straight into the center.
 *
 * Refused (ok = 0, why set): fewer than 3 markers, a non-finite or absurd marker, markers on one line
 * (under 1 mm RMS across it), more than PLACE_RING_MAX_RMS_M RMS off the plane, or a circle residual
 * over the same. Returns ok. */
#define PLACE_RING_MAX_RMS_M   0.003f
#define PLACE_RING_MIN_WIDTH_M 0.001f
#define PLACE_RING_MAX_N       64
typedef struct {
    int         ok;
    const char* why;               /* ASCII refusal reason, "" when ok */
    int         n;
    float       center[3];         /* the ring's center, marker frame: the mount offset */
    float       normal[3];         /* unit, sign-fixed to the frame's +y side */
    float       centroid[3];       /* the markers' mean */
    float       radius_m;
    float       plane_rms_m;       /* RMS distance of the markers from the fitted plane */
    float       circle_rms_m;      /* RMS of |in-plane distance from the center| - radius */
    float       centroid_to_center_m;
} PlaceRing;
int place_ring_fit(const float (*markers)[3], int n, PlaceRing* out);

/* ---- the move, in installer words ----
 * `err_m` is where a thing IS minus where it SHOULD be (room meters): the tracked stand's center minus
 * its target, or a speaker's measured position minus its plan. The instruction is the OPPOSITE move,
 * projected on the room basis (BWA_ROOM_RIGHT / UP / AHEAD, so room-right is -x) and written as
 *     "move 12 mm toward room-left, 4 mm down, 30 mm toward the front wall"
 * in that axis order: right/left, up/down, then front wall (room-ahead, +z) / back wall. An axis whose
 * move is under `dead_m` says nothing, so the line stays short; with every axis under it the line reads
 * "no move: every axis within N mm". The Placement panel and live aiming both print this one function,
 * so the stand and the box cannot be described in two conventions. ASCII. Returns the number of axes
 * named, or -1 (writes "n/a") on a non-finite input. `buf` is always NUL-terminated (cap >= 1). */
#define PLACE_MOVE_DEAD_STAND_M 0.001f   /* the ZM-1 stand: a tenth of the default 10 mm tolerance */
#define PLACE_MOVE_DEAD_BOX_M   0.005f   /* a speaker box: about what a bracket can be set to, and half of the
                                          * 1 cm the live position readout is trusted to (docs/calibration.md) */
int place_move_words(const float err_m[3], float dead_m, char* buf, size_t cap);

/* A short ASCII name for a state ("no pose", "moving", "off target", "settling", "OK"). */
const char* place_state_name(PlaceState s);

#ifdef __cplusplus
}
#endif

#endif /* BWA_PLACEMENT_H */
