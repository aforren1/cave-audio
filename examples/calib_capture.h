/*
 * calib_capture.h — the speaker-sweep capture backends, shared by bwa_calibrate (CLI) and
 * bwa_calib_view's Capture tab (the calibration-station front-end). Two backends behind one shape:
 * ASIO full-duplex (one output per speaker + 1..CAL_MAX_INPUTS lockstep inputs, sample-aligned;
 * rig bring-up code, gated on BWA_HAVE_ASIO) and simulate (delay/attenuate the sweep per the
 * layout's speaker->mic distances + a deterministic sensitivity wobble, so the whole measure ->
 * solve -> writeback path runs without the rig). One input is the omni-mic survey; 19 is the ZM-1
 * over Dante Via (--zylia), whose capsules ride consecutive inputs on the SAME device as the
 * outputs. The speaker count is the LAYOUT's (Layout.count, 4..BWA_MAX_CHANNELS) — never assume 26.
 * The measurement/solve DSP these feed lives in measure.c / calib.c (unit-tested).
 */
#ifndef BWA_CALIB_CAPTURE_H
#define BWA_CALIB_CAPTURE_H

#ifdef __cplusplus
extern "C" {
#endif
#include "core/layout.h"
#include "calib/zylia.h"                 /* ZYLIA_MICS, the pressure proxy (the live reading) */
#include "calib/calib.h"                 /* CalibWindow: the expected-arrival window (sweep quality) */
#ifdef __cplusplus
}
#endif

/* sweep + capture geometry (one source of truth for CLI + tab) */
#define CAL_FS      48000.0
#define CAL_F1      20.0
#define CAL_F2      20000.0
#define CAL_NSWEEP  72000                /* 1.5 s exponential sweep */
#define CAL_NTAIL   24000                /* 0.5 s room-decay tail */
#define CAL_CAPLEN  (CAL_NSWEEP + CAL_NTAIL)
#define CAL_IRLEN   24000                /* 0.5 s room kernel retained per speaker */
#define CAL_BAND_LO 300.0                /* sensitivity band for the level measure */
#define CAL_BAND_HI 3000.0
#define CAL_MAX_INPUTS 19                /* input ceiling per capture — sized for the ZM-1's capsules */
/* The live aiming mode's shorter sweep (bwa_calibrate --live N --zylia, calib_view's Aim tab): 0.75 s
 * of capture a reading instead of 2 s. Same 20 Hz to 20 kHz span, so the IR and the tilt bands
 * mean what they mean in the full sweep; a third of the energy (4.8 dB less SNR). The tail has to
 * hold the ZM-1 chain's ~60 ms of latency plus enough room decay for the deconvolution: 0.25 s leaves
 * 190 ms. What is cut off late only thins the IR's late reverberant part at the top of the sweep;
 * the live mode reads nothing past the direct-sound gate. */
#define CAL_LIVE_NSWEEP 24000            /* 0.5 s */
#define CAL_LIVE_NTAIL  12000            /* 0.25 s */
#define CAL_LIVE_CAPLEN (CAL_LIVE_NSWEEP + CAL_LIVE_NTAIL)
/* the simulator's system latency: every simulated arrival is this plus the time of flight */
#define CAL_SIM_LATENCY_SAMPLES 512

/* simulate backend: synthesize what an ideal rig would capture for speaker `ch` (fractional
 * time-of-flight + 1/r + a deterministic +/-~1.4 dB sensitivity wobble, the layout's directivity
 * model when it has one, and the simulated room when calib_sim_set_room turned one on).
 * cap = CAL_CAPLEN floats.
 * `sos` is the speed of sound the time of flight is generated at: pass the SAME value the caller's
 * analyzer will divide back out, or the survey inflates every position by their ratio. Out-of-range
 * falls back to BWA_SOS_REF_MPS (sos.h). */
void calib_sim_capture(int ch, const Layout* L, const float mic[3], double sos, const float* sweep, float* cap);
/* The simulated speaker `ch`'s sensitivity (linear amplitude): the wobble every simulated capture
 * carries, so a test can hold a solved gain trim against the truth (20 log10(min / this)). */
double calib_sim_sensitivity(int ch);
/* calib_sim_capture with options; NULL = exactly calib_sim_capture. The live aiming mode uses all of
 * them: its shorter sweep and capture, a speaker whose TRUE position and aim differ from the layout's
 * (the installer is moving it), and the speaker's own on-axis response from the model's on_axis_db,
 * so the file's 0 deg tilt is checked against a speaker that has one. The on-axis response is off
 * everywhere else so the other simulated modes keep the flat speaker their pinned numbers came from.
 * `cap` is ncap floats. Not reentrant (static scratch, the room tail cache): one capture at a time. */
typedef struct {
    int          nsweep;     /* 0 = CAL_NSWEEP; the played sweep's length (<= CAL_NSWEEP) */
    int          ncap;       /* 0 = CAL_CAPLEN; samples captured (<= CAL_CAPLEN) */
    const float* true_pos;   /* NULL = the layout's position */
    const float* true_aim;   /* NULL = the layout's aim (rotated by calib_sim_set_aim_error); any length */
    int          on_axis;    /* 1 = apply the model's on-axis response (when it carries one) */
    float        screen_db;  /* > 0 = a screen in front of the speaker: this much loss well above
                              * CALIB_SIM_SCREEN_HZ, a smooth first-order-like shelf, on EVERY path out of
                              * the speaker (direct, images, tail). The same at every aim, so it is
                              * what the live mode's reference absorbs and the file cannot know. */
} CalibSimOpts;
#define CALIB_SIM_SCREEN_HZ 4000.0
void calib_sim_capture_ex(int ch, const Layout* L, const float mic[3], double sos, const CalibSimOpts* o, float* cap);
/* Rotate a unit axis `deg` degrees about the deterministic perpendicular calib_sim_set_aim_error
 * uses (the cross product with +y, or with +x for a near-vertical axis). */
void calib_sim_rotate(const float in[3], float deg, float out[3]);
/* Simulate only: rotate every speaker's TRUE aim `deg` away from the layout's before synthesizing
 * its capture, so --check-aim has a known error to recover (the self-check of the check). 0 = off. */
void calib_sim_set_aim_error(float deg);

/* Simulate only: the speakers really stand where `T` says (its `position` and `aim` per index), not where
 * the layout each capture is given says. bwa_calibrate --sim-truth: a simulated rig whose as-built differs
 * from the plan, so a session that measured the plan's positions back, or trimmed against them, lands off
 * the truth. The room's shoebox and the directivity model stay the given layout's. NULL = off. `T` must
 * outlive the captures. */
void calib_sim_set_truth(const Layout* T);

/* Simulate only: speaker `ch` plays `seconds` later than the others (bwa_calibrate --sim-speaker-latency),
 * on top of CAL_SIM_LATENCY_SAMPLES: a box whose Dante receive latency is set differently, so
 * --ref-speakers has a known outlier to flag. Every path out of that speaker carries it (direct, images,
 * tail). 0 clears it. Out-of-range channels and non-finite values are ignored. */
void   calib_sim_set_speaker_latency(int ch, double seconds);
/* ... and the extra latency speaker `ch` carries now (0 = none), for a synthetic path that does not go
 * through calib_sim_capture_ex (the --zylia position survey's exact wavefronts). */
double calib_sim_speaker_latency(int ch);

/* Simulate only: a ROOM around the array (bwa_calibrate --sim-room), so the direct-sound gate and
 * the reverberant dilution it exists for have something to act on. `absorption` is every wall's
 * energy absorption coefficient in (0, 1]; 0 = off, the anechoic capture (the default).
 *
 * The room is a shoebox enclosing every speaker with CALIB_SIM_ROOM_MARGIN_M to spare (the mic has to
 * sit inside it, which any sane mic position does). Its image sources, orders 1 and 2 (6 + 18),
 * each leave the speaker at their own DEPARTURE angle off its axis, so each carries the directivity
 * model's loss at that angle (a first reflection off the wall behind a speaker is its rear lobe, and
 * dull), plus 1/r and sqrt(1 - absorption) per bounce. Each is the same analytic delayed sweep the
 * direct sound is. Past the second order a deterministic noise tail stands in for the rest: Sabine
 * RT60 for the box, the diffuse-field level of the room equation (16 pi (1 - a) / (S a)) times the
 * (1 - a)^2 share orders 1 and 2 have not already delivered, spectrally shaped by the model's POWER
 * response (a directive speaker feeds the reverberant field less treble than its axis gets), and
 * starting two mean free paths after the direct sound. Deterministic, so every run is the same. */
#define CALIB_SIM_ROOM_MARGIN_M 0.5f
void calib_sim_set_room(float absorption);
/* One line describing the room the next capture uses for this layout (dimensions, RT60, the image
 * count), or "anechoic". Control thread. */
void calib_sim_room_describe(const Layout* L, char* buf, size_t cap);

/* Simulate only: an INTERFERER (bwa_calibrate --sim-interferer), something other than the speaker
 * under test that sounds during chosen captures, so the sweep-quality checks (calib.h) have something
 * to catch. Its truth is the simulator's: the captures it lands on are counted HERE, one per capture
 * (a ZM-1 capture's 19 rows are one), never by the code under test. It stands at the capture point
 * plus CALIB_SIM_INTF_OFFSET (room m, about 1.5 m off, not a speaker's position), so each capsule
 * hears it at its own time and 1/r.
 *   click  a 2 ms broadband transient (a door, a dropped tool)
 *   noise  a 250 ms noise burst (a cough, a voice)
 *   sweep  the capture's own sweep, played from that point (a second speaker on a mis-patched output):
 *          a strong arrival where this speaker's cannot be
 * `t_s` is when it sounds at its source, seconds into the capture; `level_db` its RMS over its own
 * duration at the capture point, dB re 1 (a unit-sensitivity speaker's sweep at 1 m peaks at 1). */
enum { CALIB_SIM_INTF_CLICK = 0, CALIB_SIM_INTF_NOISE = 1, CALIB_SIM_INTF_SWEEP = 2 };
typedef struct {
    float t_s;
    float level_db;
    int   kind;
} CalibSimInterferer;
#define CALIB_SIM_INTF_MAX 64
#define CALIB_SIM_INTF_OFFSET_X 1.1f
#define CALIB_SIM_INTF_OFFSET_Y (-0.6f)
#define CALIB_SIM_INTF_OFFSET_Z 0.9f
/* Arm it on the 1-based capture numbers in `captures` (up to CALIB_SIM_INTF_MAX), counting from the
 * next capture; n 0 or it NULL = off. Resets the count. Control thread. */
void calib_sim_set_interferer(const int* captures, int n, const CalibSimInterferer* it);
/* Captures synthesized since calib_sim_set_interferer, and how many of them carried the interferer. */
int  calib_sim_capture_count(void);
int  calib_sim_interferer_hits(void);
/* "3,9,20:0.9:12:noise" -> the capture list and the event (t 0.9 s, 12 dB, noise; t, level and kind
 * default to 0.9 s, 12 dB and click). Returns the capture count, 0 on a malformed spec (err says why). */
int  calib_sim_parse_interferer(const char* spec, int* captures, int cap, CalibSimInterferer* it, char* err, size_t errcap);

/* Simulate the ZM-1 (bwa_calibrate --zylia --simulate): calib_sim_capture once per capsule, each at
 * its OWN position (center + caps[j]), so every capsule gets its own time of flight, 1/r, directivity
 * bearing and simulated room. Free-field capsules: the rigid sphere's scattering (shadowing above
 * ~2 kHz, the longer diffraction path) is NOT modeled, so a simulated capsule is an omni at its
 * position. cap19 = [ZYLIA_MICS][CAL_CAPLEN] flat. */
void calib_sim_capture_zylia(int ch, const Layout* L, const float center[3], const float (*caps)[3], int ncaps,
                             double sos, const float* sweep, float* cap19);
/* ... with calib_sim_capture_ex's options; row j at cap19 + j * stride (the ASIO shell's stride is
 * CAL_CAPLEN whatever the capture length, so the live mode passes that). */
void calib_sim_capture_zylia_ex(int ch, const Layout* L, const float center[3], const float (*caps)[3], int ncaps,
                                double sos, const CalibSimOpts* o, float* cap19, int stride);

/* The simulated PHYSICAL ZM-1, for the modes that must not be simulated from the table they solve with
 * (bwa_calibrate --localize --zylia and --capsule-survey): the built-in table scaled to
 * CALIB_SIM_ZM1_RADIUS_M and turned inside its mount by CALIB_SIM_ZM1_MOUNT_YAW_DEG about the mount's
 * +y and then CALIB_SIM_ZM1_MOUNT_TILT_DEG about its +x. So the array the captures come from is never the
 * table a solve reads, the built-in or a survey: a survey has to MEASURE it to match it, and an
 * orientation-free reading has to be orientation-free to land on the truth. Channel order is the
 * built-in one (Dante Via carries the capsules in node order; bwa_zylia_probe checks it on the rig).
 *   calib_sim_zm1_body: the capsules about the array center in the stand's BODY axes.
 *   calib_sim_zm1_room: the same rotated by the stand's orientation q (xyzw, v_room = R(q) v_body), by
 *                       this file's own quaternion code; q NULL = the untracked simulation's fixed pose,
 *                       CALIB_SIM_ZM1_UNTRACKED_YAW_DEG about room +y. Room axes, about the center. */
#define CALIB_SIM_ZM1_RADIUS_M        0.0495f
#define CALIB_SIM_ZM1_MOUNT_YAW_DEG   40.0f
#define CALIB_SIM_ZM1_MOUNT_TILT_DEG  3.0f
#define CALIB_SIM_ZM1_UNTRACKED_YAW_DEG (-25.0f)
void calib_sim_zm1_body(float caps[ZYLIA_MICS][3]);
void calib_sim_zm1_room(const float q[4], float caps[ZYLIA_MICS][3]);

/* The engine's own per-speaker output stage (align.c: gain_db, delay_ms, the `eq` FIR, static
 * room_eq, and room_eq_grid at its flat start; the listener-tracked stages sit at identity) applied to
 * ONE channel: `in` (nin samples) goes in on output channel `ch` with every other channel silent, the
 * stage runs in blocks exactly as align_create(L->count, L, CAL_FS) + align_process build it, and
 * channel ch's output lands in `out` (nout samples; `in` is zero-padded past nin). A fresh aligner per
 * call, so no state carries between speakers. Control thread. Returns 1, 0 on allocation failure. */
int  calib_stage_signal(const Layout* L, int ch, const float* in, int nin, float* out, int nout);
/* calib_stage_signal in place on `nrows` rows of `len` samples each (rows[j * len]), for the
 * simulated verify pass, where the stage runs on the RAW capture instead of the sweep (the stage is
 * linear and time-invariant, so the order does not matter). When the stage's impulse response is a
 * single tap, a plain gain and whole-sample delay, each row is shifted and scaled by it directly,
 * which is exact. `tmp` is `len` floats of scratch. Returns 1, 0 on failure. */
int  calib_stage_rows(const Layout* L, int ch, float* rows, int nrows, int len, float* tmp);
/* Samples the stage can add after its input ends: the longest delay plus the FIR plus 50 ms of
 * biquad ring. --verify pads the played sweep by this much. */
int  calib_stage_pad(const Layout* L);

/* measure_response over `nrows` rows (rows[j * stride], ncap samples each) against `sweep`, on a few
 * worker threads: it is pure and reentrant, and each call holds up to ~10 MB of FFT scratch, hence
 * the cap of 8. ok[j] = its return. The ZM-1's 19 capsules are the case this exists for. */
void calib_measure_rows(const float* rows, int nrows, int stride, int ncap, const float* sweep, int nsweep,
                        const double band_hz[2], MeasureResult* res, int* ok);

/* calib_measure_rows for the ZM-1's 19 capsule rows, then every capsule's arrival refined by
 * cross-correlating the capsules' impulse responses (zylia_ir_tdoa) and written back into
 * res[j].delay_samples / delay_frac, so every consumer of those fields (the pressure proxy's center
 * arrival, zylia_doa, zylia_localize, the live position) reads the refined value. When the refine
 * cannot run (a dead capsule, no interior peak) the |IR|-peak arrivals stand. Returns 1 when the
 * refine ran, 0 when it fell back. [exp_lo, exp_hi) is every row's expected-arrival window
 * (measure_response_ex; the center's window widened by CALIB_WIN_ZM1_M), exp_hi <= exp_lo = none. */
int  calib_measure_zylia_rows(const float* rows, int stride, int ncap, const float* sweep, int nsweep,
                              const double band_hz[2], int exp_lo, int exp_hi,
                              MeasureResult res[ZYLIA_MICS], int ok[ZYLIA_MICS]);
/* The ZM-1 capture's quality from its 19 rows, onto the pooled `out` (after zylia_pressure_proxy,
 * which clears it): the window, `outside` when a majority of the capsules saw a stronger tap outside
 * it (an interferer reaches every capsule; one hot capsule is the dead-capsule check's business), and
 * the MEDIAN capsule's SNR. */
void calib_pool_quality(const MeasureResult rj[ZYLIA_MICS], MeasureResult* out);

/* The latency prior for the expected-arrival windows (calib.h, CalibWindow): simulate knows its own
 * latency exactly; known_latency_s >= 0 (--latency, --ref) is a measured loop, +/-
 * CALIB_WIN_LAT_KNOWN_S; else the ASIO driver's reported loop from the last open, a lower bound with
 * CALIB_WIN_LAT_DRIVER_S of slack above it. pos_m goes into w->pos_m as given (< 0: decided per
 * speaker, calib_window_pos_m). Returns 1 with *w filled and `desc` saying which, or 0 (no window:
 * nothing to time the arrival from) with `desc` saying so. */
int  calib_window_prior(int simulate, double known_latency_s, double pos_m, CalibWindow* w, char* desc, size_t cap);
/* The position margin for speaker s: CALIB_WIN_SURVEYED_M when a survey placed it (it carries a
 * plan_position), CALIB_WIN_PLAN_M when it still stands at its plan. */
double calib_window_pos_m(const Layout* L, int s);

/* One speaker's capture and measurement for the trim run and the verify pass: bwa_calibrate's trims and
 * --verify, and calib_view's Capture tab, all go through this, so the CLI and the GUI cannot measure
 * differently. The mic is the omni (one capture into `cap`) or the ZM-1 (zylia != 0: 19 capsule rows in
 * `cap19`, each deconvolved on its own, the arrivals refined by cross-correlation
 * (calib_measure_zylia_rows) and pooled by zylia_pressure_proxy; `cap` then receives the rows' MEAN,
 * the center pressure below about 2 kHz, for the IR consumers that read low frequencies). `through`
 * = play through the layout's output stage (the verify pass): on the rig the sweep itself is staged and
 * played (calib_asio_capture_signal); in simulate the RAW capture is staged instead, which is the same
 * thing because the stage is linear and time-invariant. Either way the measurement deconvolves against
 * the RAW sweep, so it carries the trims. Not reentrant (the simulator's and the refine's static
 * scratch). The ASIO shell must already be open with 1 or 19 inputs to match. */
typedef struct {
    const Layout* L;
    const float*  sim_at;              /* simulate: where the capture is synthesized (the mic, or a
                                        * tracked stand's TRUE center); unused on the rig */
    double        sos;                 /* the speed of sound the simulator generates at */
    int           simulate;
    int           zylia;
    const float*  sweep;               /* CAL_NSWEEP */
    float*        cap;                 /* CAL_CAPLEN */
    float*        cap19;               /* ZYLIA_MICS x CAL_CAPLEN, the ZM-1 only */
    float         caps[ZYLIA_MICS][3]; /* simulate: capsule positions about the center (zylia_capsules,
                                        * read AFTER the run has settled its capsule table) */
    float*        play;                /* CAL_CAPLEN: the verify pass on the rig, the staged sweep */
    int           nplay;               /* CAL_NSWEEP + calib_stage_pad(L) */
    float*        tmp;                 /* CAL_CAPLEN scratch */
    const double* band_hz;             /* the measurement's two band edges (measure_response); NULL = the
                                        * level band, CAL_BAND_LO..CAL_BAND_HI. --localize passes the
                                        * --check-aim tilt bands. */
    /* Sweep quality (calib.h): the expected-arrival window, the SNR floor, the re-sweep and the
     * agreement rule. have_win 0 = no window (the whole IR, as before); the SNR check and the
     * re-sweep still run. */
    const float*  mic;                 /* where the run BELIEVES the mic is (the windows are read from
                                        * it); NULL = sim_at. Never the simulator's truth. */
    int           have_win;
    CalibWindow   win;                 /* win.pos_m < 0: per speaker, calib_window_pos_m */
    int           agree;               /* 1 = a value counts only once two sweeps agree (trims, verify) */
} CalibPass;
#define CALIB_BG_SILENT_DBFS (-300.0f)
enum { CALIB_MEAS_OK = 1, CALIB_MEAS_FAILED = 0, CALIB_MEAS_TIMEOUT = -1, CALIB_MEAS_DEAD = -2,
       CALIB_MEAS_UNCLEAN = -3 };
typedef struct {
    float  capsule_spread_db;          /* the ZM-1: max over min capsule level, dB */
    int    dead;                       /* CALIB_MEAS_DEAD: the dead capsule's index, else -1 */
    double dead_level, median_level;   /* ... its level and the capsules' median, for the message */
    int    refined;                    /* the ZM-1: 1 = the cross-correlation refine ran */
    double arrival_s[ZYLIA_MICS];      /* the ZM-1: each capsule's arrival (s), refined when `refined`,
                                        * as the pool read them; the capsule survey's input */
    int    sweeps;                     /* sweeps this speaker took */
    int    rej_outside, rej_noisy, rej_disagree;   /* ... and why the extra ones were taken */
    float  snr_db;                     /* the accepted sweep's IR SNR (0 = unknown) */
    float  bg_dbfs;                    /* the accepted sweep's BACKGROUND: the raw capture's power before
                                        * this speaker's sound can arrive (up to MEASURE_NOISE_GUARD_S
                                        * before the window, or the arrival with none), dBFS, the median
                                        * over the ZM-1's capsules; CALIB_BG_SILENT_DBFS = silent (simulate) */
    char   log[1400];                  /* one ASCII line per re-sweep, "sweep N: <why>", or empty */
} CalibMeasInfo;
/* Captures and measures speaker s, re-sweeping (up to CALIB_SWEEP_MAX_TRIES) any sweep the window or
 * the SNR floor rejects (calib_sweep_check), and with P->agree until two clean sweeps agree
 * (calib_sweeps_agree): the later of the pair is the result, and `cap` / `cap19` hold its capture.
 * Returns CALIB_MEAS_OK with `out` filled; CALIB_MEAS_FAILED (a stage or deconvolution failure);
 * CALIB_MEAS_TIMEOUT (the rig's capture timed out); CALIB_MEAS_DEAD (a ZM-1 capsule is dead: its level
 * is non-finite or more than ZYLIA_PROXY_DEAD_DB under the median, and its arrival would throw the
 * center arrival off by milliseconds); CALIB_MEAS_UNCLEAN (every sweep was rejected, or none agreed:
 * info->log says why; nothing for this speaker may be used). `info` may be NULL. */
int calib_measure_speaker(const CalibPass* P, int s, int through, MeasureResult* out, CalibMeasInfo* info);

/* One live aiming reading (bwa_calibrate --live N --zylia, calib_view's Aim tab): sweep speaker `spk`
 * with the live sweep (`lsweep`, CAL_LIVE_NSWEEP samples), capture the 19 capsules into cap19
 * ([19][CAL_CAPLEN], the ASIO shell's rows), deconvolve each over the tilt bands (CALIB_AIM_*), and
 * pool them (zylia_pressure_proxy). simulate != 0 synthesizes the capture with `sim` (its nsweep and
 * ncap are forced to the live ones; the caller sets the truth); otherwise the ASIO shell must be open
 * with 19 inputs. Returns 0 on a capture timeout; 1 otherwise, with out->ok = 0 and out->dead set
 * when a capsule is dead (the reading is skipped, not the run). Control thread / one worker. */
typedef struct {
    int    ok;
    int    dead;                   /* -1, or the dead capsule's index */
    double arr[ZYLIA_MICS];        /* per-capsule arrivals (s), for zylia_live_position */
    MeasureResult pooled;          /* the pressure proxy over the tilt bands, with calib_pool_quality's
                                    * window, outside flag and SNR */
    int    have_tilt;
    float  tilt_db;                /* calib_direct_tilt_db of the pool */
    int    quality;                /* calib_sweep_check of the pool: CALIB_SWEEP_OK is a CLEAN reading,
                                    * the only kind the peak hold may hold (calib_peak_update) */
    char   why[200];               /* calib_sweep_why, when quality is not OK */
} CalibLiveReading;
/* `win` (NULL = no window): the expected-arrival window for the live speaker, from the ZM-1's center
 * (`believed`, where the run thinks the array is; NULL = center) to wherever the box may be: the
 * window is the union of the ones read from its as-built position and its plan, with win->pos_m of
 * margin around each, since the installer is moving it from one toward the other. */
int calib_live_read(int spk, const Layout* L, const float center[3], double sos, int simulate,
                    const CalibSimOpts* sim, const float* lsweep, float* cap19, CalibLiveReading* out,
                    const CalibWindow* win, const float* believed);

/* minimal mono IEEE-float WAV writer (retained per-speaker impulse responses) */
void calib_write_wav_f32(const char* path, const float* x, int n, int fs);

/* Registered-driver enumeration (ungated: without the ASIO SDK the count is 0 and list says so).
 * A fresh registry read each call; loads nothing, needs no session slot — safe while a capture
 * or the engine has a driver open. Feeds the CLI's --list-drivers and the station's pickers. */
int calib_asio_driver_names(char (*names)[32], int max);   /* fill up to max (<= 32); returns the count */
int calib_asio_list(void);                                  /* print them to stdout; 0 (2 = no-SDK build) */

#ifdef BWA_HAVE_ASIO
/* ASIO full-duplex: open `driver` (NULL = first with >= `nspk` outs + the mic input), start
 * streaming. `nspk` is the layout's speaker count (4..BWA_MAX_CHANNELS). calib_asio_capture(ch) plays
 * the sweep out channel `ch` and records CAL_CAPLEN mic samples into the `cap` given at open
 * (blocking, ~10 s watchdog; returns 0 on timeout). Single instance.
 * NOT verified on hardware here — rig bring-up code (mirrors asio_sink.cpp's host sequence). */
int  calib_asio_open(const char* driver, int mic_in, int nspk, const float* sweep, float* cap);
/* Same shell, `nin` consecutive inputs starting at `in_first` (1..CAL_MAX_INPUTS; open() is the
 * nin = 1 case). All inputs record in lockstep — one clock domain, which the ZM-1-over-Dante
 * route guarantees. `cap` is [nin][CAL_CAPLEN] flat; calib_asio_capture(ch) fills every row. */
int  calib_asio_open_multi(const char* driver, int in_first, int nin, int nspk,
                           const float* sweep, float* cap);
int  calib_asio_capture(int ch);
/* The same capture, playing `sig` (nsig samples, 1..CAL_CAPLEN) on channel `ch` instead of the
 * sweep given at open, for this capture only. --verify plays the sweep run through the output stage
 * (calib_stage_signal), which is longer than the sweep by the stage's delay and FIR. The recording
 * is still CAL_CAPLEN samples, so keep nsig well under it: the tail has to hold the latency and the
 * room decay. Same ~10 s watchdog. */
int  calib_asio_capture_signal(int ch, const float* sig, int nsig);
/* ... recording only `ncap` samples of every input (1..CAL_CAPLEN; rows keep the CAL_CAPLEN stride),
 * with 1 <= nsig <= ncap. The live aiming mode's shorter sweep: CAL_LIVE_NSWEEP into CAL_LIVE_CAPLEN.
 * Unverified on hardware like the rest of the shell. */
int  calib_asio_capture_len(int ch, const float* sig, int nsig, int ncap);
void calib_asio_close(void);
/* Driver-reported latencies from the LAST calib_asio_open (valid until the next open — they
 * survive close, so a solve that runs after teardown can still cross-check): output = render->DAC,
 * input = ADC->delivered, in frames at CAL_FS. Their SUM is the DIGITAL half of the sweep's round
 * trip — a hard lower bound for any measured/solved system latency, which adds DAC/ADC conversion
 * and analog on top. Logged at open; returns 1 when known, 0 when no device has been opened (or
 * the driver refused ASIOGetLatencies). */
int  calib_asio_latencies(long* in_frames, long* out_frames);
#endif

#endif /* BWA_CALIB_CAPTURE_H */
