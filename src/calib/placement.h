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

typedef struct {
    float  tol_m;       /* <= 0: no tolerance, the gate accepts on stillness alone (--localize) */
    float  still_m;     /* spread limit over the window */
    double window_s;
    double hold_s;
} PlaceCfg;

/* The defaults above for a tolerance in meters (clamped to [0, PLACE_TOL_MAX_M]; non-finite = the
 * default 10 mm). tol_m <= 0 keeps a spread limit of PLACE_STILL_MAX_M. */
void place_cfg_default(PlaceCfg* c, float tol_m);

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

/* The bump check: did the center move more than half the tolerance between the center a run took
 * (`taken`) and now? Returns 1 = bumped, 0 = in place, -1 = cannot tell (a refused input or a
 * non-positive tolerance). moved_m (NULL ok) gets the distance when both inputs are usable. Half the
 * tolerance: once the mic moves, the run's speakers were measured from two points, and a move of d
 * shifts some arrivals against others by up to d / c. The tolerance is the whole placement budget,
 * so a move inside half of it cannot cost more than the placement itself was allowed to. */
int place_bump(const float taken[3], const float now[3], float tol_m, float* moved_m);

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

/* A short ASCII name for a state ("no pose", "moving", "off target", "settling", "OK"). */
const char* place_state_name(PlaceState s);

#ifdef __cplusplus
}
#endif

#endif /* BWA_PLACEMENT_H */
