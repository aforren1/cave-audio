/*
 * clicker_track.h - the tracked clicker: WHERE a capsule-survey clap happened, measured instead of typed.
 *
 * The capsule self-survey (zylia.h, zylia_survey) needs each clap's position relative to the array
 * center. Typed from a tape measure, that position is the weakest number in the survey: a centimeter
 * is about 0.2 deg at 2.5 m, and the survey's residual cannot tell a mis-typed position from a bad
 * capsule. So the clicker (a hand clicker or clapper) carries an OptiTrack rigid body, and its TIP,
 * the point where the click is made, is p + R(q) . tip (placement.h, place_center), tip in the body's
 * axes. A tip of 0 says Motive's pivot was moved to the tip.
 *
 * WHEN, AND WHY STILLNESS INSTEAD OF A CLOCK MATCH. A clap is instantaneous but the hand moves: a hand at
 * 1 m/s for 10 ms is 1 cm, the very error this exists to remove. Pairing the clap's onset with a
 * NatNet frame precisely would need three clocks agreed (the ASIO sample clock, the host's monotonic
 * clock, Motive's camera clock) to a few milliseconds, and none of that chain is verified. So the rule
 * is a STILLNESS requirement instead: the tip must sit within CLICKER_STILL_M of its mean over the
 * CLICKER_WINDOW_S that ends CLICKER_GUARD_S before the clap's onset, and that mean is the
 * clap position. A clap made while the clicker moves is refused, not guessed at. The clock pairing this
 * leaves is coarse and one-sided: poses are stamped with clicker_clock_s() at local ARRIVAL (NatNet
 * delivers them some milliseconds after exposure, which only moves the window earlier), and the onset is
 * STAMPED by the capture callback on the same clock (zylia_capture.h, "THE ONSET STAMP": the trigger's
 * sample within its ASIO block, from that block's switch time), late by at most the driver's dispatch
 * delay and any input latency the driver does not report, a few milliseconds. The guard covers that, so
 * the window lies wholly BEFORE the click, however late the UI noticed the snapshot. What is left: the
 * window's own spread (<= 4 mm by the rule) plus how far a hand that has just held still for 300 ms
 * drifts in the 50 ms between the window's end and the click, about a millimeter. A clock match without
 * the stillness rule would leave v * dt, a centimeter at 1 m/s and 10 ms.
 *
 * Without a stamp (ZP_ONSET_NONE) the Zylia tab falls back to the old ESTIMATE: the frame that saw the
 * snapshot minus the shell's post-roll (ZP_SNAP_N - ZP_SNAP_PRE samples, 75 ms at 48 kHz). That is late
 * by one ASIO block plus one UI frame on a good day, and by any UI hitch on a bad one: past about 20 ms
 * of hitch the window reaches the click and post-click motion gets a still clap refused. The simulated
 * session publishes its snapshots that late on purpose (ZY_SIM_HITCH_S in calib_view.cpp), so the tests
 * show the stamp holding where the estimate fails.
 *
 * BOTH BODIES ON ONE STREAM. The ZM-1's stand already has a NatNet session (mic_track.cpp, natnet_open,
 * on the Placement panel's poller). This opens a SECOND, thread-free one (NatNetRaw) on the same data
 * port, on its own thread, and picks the clicker out of every frame with natnet_parse_bodies. That works
 * because NatNet's supported transport is MULTICAST and both sockets set SO_REUSEADDR before binding and
 * join the group: the OS hands each multicast datagram to EVERY socket bound to that port and joined to
 * the group (Windows and Linux alike). It would NOT work for a unicast stream, where one socket gets each
 * datagram, but natnet.h already refuses to be a unicast client. The alternative, one session reading
 * both bodies, would mean changing mic_track; this leaves it untouched.
 *
 * A SIMULATED clicker stands in for Motive. A script walks it to K positions around an anchor (the true
 * array center), each held at its own orientation, and calls for a click once it has held still for
 * CLICKER_SIM_HOLD_S. Its poses go out as NatNet 4.1 FRAMEOFDATA payloads carrying a decoy body (the
 * stand, first in the frame) and the clicker, through the same natnet_parse_bodies and the same ingest
 * as the live path, and a name is resolved through a MODELDEF the script writes, through
 * natnet_parse_modeldef. Its truth (the tip at the click) comes from its own quaternion code and its own
 * tip offset (CLICKER_SIM_TIP), never from place_center or the offset the tool was given, so a wrong
 * or ignored tip offset lands off the truth.
 *
 * THE SIMULATION KEEPS ITS OWN CLOCK. It used to run on the wall clock: a thread wrote 240 Hz poses as
 * wall time passed, and the UI asked the script for a click at its own wall-clock reading. The script
 * started the next walk at that reading, the pose thread kept writing that walk, and the UI judged the
 * click (the arm, the onset) at LATER readings of the same clock. Under load the UI thread is preempted
 * between those reads for tens of milliseconds, so the arm saw its own click's next walk already in the
 * ring and refused a still click as "moving". Now the simulated session has a virtual clock
 * (clicker_now) that the UI steps once per frame (clicker_sim_advance). The step writes every pose up to
 * the new time, and only then can the script fire a click at that time, so the ring always holds exactly
 * the poses up to the click and none after it, however late the frame ran. Inside one frame the clock
 * does not move. A long frame is clipped to CLICKER_SIM_MAX_STEP_S, so a stalled UI slows the script
 * down instead of jumping it past the 0.2 s gaps it is built from. The live session is untouched:
 * clicker_now is the wall clock there, and its poses are stamped at arrival as before. A simulated clap
 * carries its onset stamp on this virtual clock, in the same ZpShared field the capture callback fills.
 *
 * Unverified on hardware: nothing here has met live Motive or a real clap.
 */
#ifndef BWA_CLICKER_TRACK_H
#define BWA_CLICKER_TRACK_H

extern "C" {
#include "tracking/natnet.h"
#include "calib/placement.h"
}
#include <atomic>
#include <mutex>
#include <thread>
#include <stddef.h>
#include <stdint.h>

#define CLICKER_HIST        2048     /* pose ring: 8.5 s at 240 Hz, 17 s at 120 Hz */
#define CLICKER_WINDOW_S    0.30     /* the stillness window */
#define CLICKER_GUARD_S     0.05     /* the window ends this long before the onset */
#define CLICKER_STILL_M     0.004f   /* every tip in the window within this of the mean */
#define CLICKER_MIN_SAMPLES 8        /* 36 at Motive's slowest 120 Hz: a window this short of it is a dropout */
#define CLICKER_WAIT_S      0.5      /* how long a clap waits for poses covering its window */
#define CLICKER_STALE_S     0.25     /* a pose older than this is not live (natnet.c's NN_STALE_MS) */

/* the simulated clicker */
enum { CLICKER_SIM_OFF = 0, CLICKER_SIM_SPREAD = 1, CLICKER_SIM_RING = 2 };
#define CLICKER_SIM_RATE_HZ   240.0
#define CLICKER_SIM_BODY_ID   12
#define CLICKER_SIM_STAND_ID  7       /* a decoy in the same frame, ahead of the clicker */
#define CLICKER_SIM_WALK_S    0.6     /* each leg's walk from the last spot */
#define CLICKER_SIM_HOLD_S    0.6     /* held still this long before the click */
#define CLICKER_SIM_MAX_STEP_S (1.0 / 30.0)   /* the most one clicker_sim_advance moves the virtual clock */
#define CLICKER_SIM_MAX_LEGS  16
/* the simulated clicker's TRUE tip offset, body axes (m): a tip 11 cm out on a stalk, a little off axis */
#define CLICKER_SIM_TIP_X     0.020f
#define CLICKER_SIM_TIP_Y    -0.030f
#define CLICKER_SIM_TIP_Z     0.110f

/* SIMULATED INTERFERERS: a transient from somewhere OTHER than the clicker (someone clapping across the
 * room, a door), fired while the clicker is held still at a spot. The script reports it through
 * clicker_sim_poll_click like a click, with ITS true position, and the tab synthesizes it through the same
 * snapshot path. That position comes from the script's own code: the leg's spot turned
 * CLICKER_SIM_IF_AZ_DEG about the vertical through the anchor, same range and height, so about 55 deg
 * from the clicker's direction at the spread legs' heights.
 *   BEFORE: timed like a real click, CLICKER_SIM_IF_AT_S after the clicker reached the spot (it has been
 *           still long enough to arm by then), and the clicker clicks CLICKER_SIM_IF_GAP_S later.
 *   AFTER:  the clicker clicks at the usual time, holds still CLICKER_SIM_IF_POST_S longer, and the
 *           interferer fires CLICKER_SIM_IF_GAP_S after the click, so it lands while the clicker is still.
 * GAP is 0.4 s because the real capture cannot see a second transient sooner: a snapshot is 85 ms and the
 * trigger then holds off for 300 ms (zylia_capture.cpp). */
#define CLICKER_SIM_MAX_IF     4
#define CLICKER_SIM_IF_AZ_DEG  60.0
#define CLICKER_SIM_IF_AT_S    0.8
#define CLICKER_SIM_IF_GAP_S   0.4
#define CLICKER_SIM_IF_POST_S  0.6
enum { CLICKER_SIM_IF_BEFORE = 1, CLICKER_SIM_IF_AFTER = 2 };
/* what clicker_sim_poll_click reports */
enum { CLICKER_SIM_EV_NONE = 0, CLICKER_SIM_EV_CLICK = 1, CLICKER_SIM_EV_INTERF = 2 };
struct ClickerSimIf { int leg; int when; };   /* leg = the spot's index (0-based), when = CLICKER_SIM_IF_* */

enum { CLICKER_OFF = 0, CLICKER_CONNECTING = 1, CLICKER_LIVE = 2, CLICKER_FAILED = 3 };
enum { CLICKER_TAKE_OK = 0, CLICKER_TAKE_PENDING = 1, CLICKER_TAKE_REFUSED = 2 };

struct ClickerCfg {
    char  body[64];           /* rigid-body streaming id or name ("clicker" resolves in simulate too) */
    char  server[64];         /* Motive host: needed for a name and for the bitstream version */
    char  multicast[64];      /* "" = 239.255.42.99 */
    float tip_m[3];           /* pivot -> tip, body axes (m); 0 = the pivot IS the tip */
    int   sim;                /* CLICKER_SIM_* */
    float sim_anchor[3];      /* simulate: the true array center the legs are placed around */
    int   sim_moving_leg;     /* simulate: one extra click halfway through this leg's walk (< 1 = none) */
    int   sim_nif;            /* simulate: interferers (CLICKER_SIM_MAX_IF at most) */
    ClickerSimIf sim_if[CLICKER_SIM_MAX_IF];
};

struct ClickerSample { double t; float tip[3]; };

struct ClickerTake {
    float  tip[3];            /* the window's mean tip: THE clap position (room, m) */
    float  spread_m;          /* the largest distance of a window tip from the mean */
    int    n;                 /* samples in the window */
    double t0, t1;            /* the window, clicker_clock_s */
    char   why[192];          /* ASCII, when refused */
};

struct ClickerTrack {
    ClickerCfg        cfg;
    std::thread       th;
    bool              th_live;
    std::atomic<int>  state;              /* CLICKER_OFF / CONNECTING / LIVE / FAILED */
    std::atomic<bool> stop;
    char              msg[256];           /* why it failed */
    int32_t           id;                 /* the streaming id in use */
    /* everything below is under mu */
    std::mutex        mu;
    ClickerSample     ring[CLICKER_HIST];
    int               head, n;
    double            last_frame_t, last_pose_t;   /* clicker_clock_s; 0 = never */
    /* the script (simulate). The virtual clock is written by the UI thread only (clicker_sim_advance);
     * atomic so clicker_now needs no lock and can be called with mu held. */
    std::atomic<double> vt;
    double            sim_next;           /* the next pose's time on the 240 Hz grid */
    int32_t           sim_fno;            /* the next FRAMEOFDATA's frame number */
    int               sim_nlegs, sim_leg, sim_moving_done;
    float             sim_legs[CLICKER_SIM_MAX_LEGS][3];
    double            sim_legq[CLICKER_SIM_MAX_LEGS][4];
    float             sim_from[3];
    double            sim_fromq[4];
    double            sim_leg_t0;
    int               sim_if_done[CLICKER_SIM_MAX_IF];
    int               sim_ifp_on;         /* an AFTER interferer waiting to fire */
    double            sim_ifp_t;
    float             sim_ifp_pos[3];
    int               sim_ifp_idx;
};

/* The host's monotonic clock, seconds: os_monotonic_ns (QPC on Windows), the clock the ZM-1 capture
 * stamps each onset with. */
double clicker_clock_s(void);
/* The session's clock, the one both halves read: the clap's detection time and every pose's stamp are on
 * it. Live: clicker_clock_s. Simulated: the virtual clock, which moves only in clicker_sim_advance. */
double clicker_now(const ClickerTrack* T);

/* Start the session on its own thread (NatNet's handshake and model-definition request block for up to
 * about two seconds, so a GUI never calls them). Returns at once; poll clicker_state. A previous session
 * must be stopped first. */
void clicker_start(ClickerTrack* T, const ClickerCfg* c);
/* Ask the thread to stop and join it (at most about 200 ms: the receive timeout). */
void clicker_stop(ClickerTrack* T);
int  clicker_state(const ClickerTrack* T);

/* The live readout: the newest tip, its age, and the spread over the last CLICKER_WINDOW_S (-1 when the
 * window is short). Returns 1, or 0 with no pose yet. */
int  clicker_latest(ClickerTrack* T, float tip[3], double* age_s, float* spread_m);

/* The clap position for a clap whose onset was at t_onset (clicker_clock_s): the mean tip over
 * [t_onset - GUARD - WINDOW, t_onset - GUARD], when every tip in it sits within CLICKER_STILL_M of the
 * mean. CLICKER_TAKE_PENDING = the newest pose is older than the window's end (ask again; give up after
 * CLICKER_WAIT_S), REFUSED = why says why (moving, a dropout, not tracking), OK = out->tip. */
int  clicker_take(ClickerTrack* T, double t_onset, ClickerTake* out);

/* ---- simulate ---- */
/* Step the virtual clock by dt (clipped to 0 .. CLICKER_SIM_MAX_STEP_S) and write every pose up to the new
 * time into the ring, through the NatNet payload and the same ingest as the live path. Once per frame,
 * before anything reads the clicker. A no-op unless a simulated session is live. */
void clicker_sim_advance(ClickerTrack* T, double dt);
/* The script asks for a transient at the session's clock (clicker_now, whose poses are all in the ring):
 * CLICKER_SIM_EV_CLICK when a click is due, consuming it, with its TRUE tip (the script's own code), or
 * CLICKER_SIM_EV_INTERF when an interferer is due, with ITS true position (not the clicker's). The UI
 * synthesizes the clap from that. CLICKER_SIM_EV_NONE = nothing due, or not a simulated session. At most
 * one per call. */
int  clicker_sim_poll_click(ClickerTrack* T, float true_pos[3]);
/* Clicks and interferers the script still has to ask for (0 = done, or not simulating). */
int  clicker_sim_left(ClickerTrack* T);
/* The simulated clicker's true tip offset (body axes), for a UI that offers to fill it in. */
void clicker_sim_tip(float out[3]);

#endif /* BWA_CLICKER_TRACK_H */
