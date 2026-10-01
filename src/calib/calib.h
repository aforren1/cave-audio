/*
 * calib.h — turn per-speaker acoustic measurements into layout trims, and write them back into
 * cave_layout.json. The measurement DSP is in measure.h; the full-duplex ASIO capture that feeds it
 * is the calibration tool (examples/calibrate.c). This part is pure + file I/O only (no audio thread,
 * no ASIO), so it is unit-tested (test/calib_test.c).
 */
#ifndef BWA_CALIB_H
#define BWA_CALIB_H

#include "calib/measure.h"
#include "core/layout.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Solve per-speaker trims from the measurements + geometry:
 *   delay_ms[i] aligns every speaker's arrival to the FARTHEST (the latest measured delay -> 0 trim,
 *               nearer speakers delayed to match). The common system latency cancels in the difference.
 *   gain_db[i]  equalizes SENSITIVITY: the measured level is back-projected to a reference by the
 *               speaker->mic distance (level*dist, factoring out 1/r), then trimmed cut-only (<= 0 dB,
 *               relative to the least-sensitive speaker) so nothing is boosted into clipping.
 * `pos[i]` and `mic` are room-space meters; a near-silent speaker (level ~ 0, e.g. unplugged) gets
 * 0 dB and is excluded from the reference. fs is the sample rate. */
void calib_solve(const MeasureResult* m, const float (*pos)[3], const float mic[3], int n, double fs,
                 float* gain_db, float* delay_ms);

/* calib_solve with a per-speaker sensitivity correction: `corr[i]` (linear amplitude, > 0) multiplies
 * speaker i's measured level before the equalization; NULL = calib_solve. The calibration tool derives
 * it from the layout's directivity model (calib_directivity_corr: loss(listening-point bearing) /
 * loss(mic bearing), acting on the capture's direct share only), so a trim measured off a speaker's
 * axis is re-aimed to what the listening point hears of that speaker,
 * the way the 1/r term is already divided out. With the mic AT the listening point every factor is 1
 * and the two solves agree exactly. A non-finite or non-positive factor reads as 1. */
void calib_solve_corr(const MeasureResult* m, const float (*pos)[3], const float mic[3], int n, double fs,
                      const float* corr, float* gain_db, float* delay_ms);

/* The `corr` for calib_solve_corr from the layout's directivity model. Per speaker, the model's
 * free-field ratio r = D(ref) / D(mic): the amplitude loss toward the listening point over the loss
 * toward the mic, each averaged uniformly over [f_lo, f_hi] the way measure.c's level is (pass 2*f1
 * and f2/2 of the sweep). But `level` is the WHOLE response, and in a live room part of it is
 * reverberant energy that does not follow the speaker's axis, so applying r to all of it would
 * over-correct. With f = m[i].direct_frac (the gated share of the level band's energy):
 *
 *     corr = sqrt(f * r^2 + (1 - f)),   computed as sqrt(1 + f * (r^2 - 1))
 *
 * which is r in an anechoic capture (f = 1), 1 in a purely diffuse one (f = 0), and in between
 * otherwise; the second spelling makes a mic AT the listening point (r == 1 exactly, same bearing)
 * give exactly 1 whatever f is. `m` may be NULL: every f reads as 1 (the free-field factor). A
 * non-finite f also reads as 1. Fills corr[count] and returns 1 when the layout carries a model;
 * returns 0 and leaves corr alone when it does not. The ONE implementation of this factor, so the
 * CLI and the GUI cannot write different trims. */
int calib_directivity_corr(const Layout* L, const float mic[3], double f_lo, double f_hi,
                           const MeasureResult* m, float* corr);

/* ---- aim check (bwa_calibrate --check-aim; docs/calibration.md) --------------------------------
 * The directivity model multiplies each speaker's AIM, and the aim is the one input nothing else
 * verifies: a mount pointed at the floor, or a layout `aim` typed wrong, is 2 to 4 dB of treble at
 * the listening point on a waveguide monitor. The --localize captures already hold what is needed:
 * every speaker swept from K known mic positions, each yielding a mid and high band level of the
 * DIRECT sound (measure.c's band_direct[1]/[2]: the IR gated before the first reflection, because
 * the reverberant part does not follow the axis, and left in, it dilutes the bearing dependence and
 * the fit reads errors too small). Their ratio, the TILT, changes with the bearing off the axis the
 * way the model says, and the speaker's own response and the mic cancel out of it as a per-speaker
 * constant. So: predict the tilt at each position for a candidate aim, remove the mean, and the
 * residual RMS scores the aim. A grid search around the layout's aim finds the best one.
 *
 * The bands are CALIB_AIM_MID_HZ..CALIB_AIM_HIGH_HZ (mid) and CALIB_AIM_HIGH_HZ..f2 (high), not the
 * level bands' 300 Hz / 3 kHz: a gate of a few ms smears the spectrum over 1/gate (250 Hz at 4 ms,
 * 500 Hz behind a 2 ms floor bounce), and the gate length changes with the mic position, so a mid
 * band starting at 300 Hz would carry a position-dependent window error into exactly the quantity
 * the fit compares across positions. From 1 kHz the band sits at least two resolution cells above
 * that. The cost is contrast: on the Genelec 4410A model the 45 deg tilt is 2.8 dB instead of 3.8.
 * The measurement (the band_hz passed to measure_response) and the model (calib_aim_tilt_db's
 * band_hz) MUST use the same bands; the CLI passes these constants to both.
 *
 * What it can resolve: an aim error of the order of the bearing SPREAD the positions give it, so
 * with positions spanning 20 to 30 deg of bearing, roughly 10 deg. What it cannot: the balloon
 * shape (far too little coverage, and gated HF in a live room is noisy at the dB level), which is
 * why it fits ONE direction and refuses below a minimum spread rather than reporting a confident
 * wrong answer. Diagnostic only; nothing writes the fitted aim back. */
#define CALIB_AIM_MID_HZ  1000.0
#define CALIB_AIM_HIGH_HZ 3000.0
typedef struct {
    int   ok;              /* 1 = fitted; 0 = refused (no model, < 3 positions, or < 8 deg of bearing
                            * spread); the layout-aim fields below are still filled when a model exists */
    int   npos;
    float spread_deg;      /* max bearing difference between any two positions: the fit's leverage */
    float rms_layout_db;   /* tilt residual RMS (mean removed) with the layout's aim */
    float rms_fit_db;      /* ... with the fitted aim */
    float aim_fit[3];      /* unit vector; = the layout aim when refused */
    float aim_err_deg;     /* angle between the layout aim and the fit (0 when refused) */
} CalibAimResult;

/* The model's predicted TILT (dB) at `angle_deg` off axis: the high band's mean amplitude loss over
 * [band_hz[1], f2] against the mid band's over [band_hz[0], band_hz[1]], each averaged the way
 * measure.c's band means are. 0 without a model. */
float calib_aim_tilt_db(const Directivity* d, float angle_deg, const double band_hz[2], double f2);

/* Fit speaker `s`'s aim from measured tilts: `tilt_db[k]` = 20 log10(band_direct[2]/band_direct[1])
 * of its capture at `mic[k]`, K positions, `band_hz`/`f2` as passed to measure_response. Uses L->speakers[s].pos
 * (pass a layout carrying the SOLVED positions) and .aim as the starting point. Pure. Returns
 * out->ok. */
int calib_check_aim(const Layout* L, int s, const float (*mic)[3], const float* tilt_db, int K,
                    const double band_hz[2], double f2, CalibAimResult* out);

/* ---- live aiming (bwa_calibrate --live N --zylia, calib_view's Aim tab; docs/calibration.md) ----
 * One mic position sees one bearing per speaker, so the tilt above cannot fit an axis. What it CAN
 * give is the MAGNITUDE of the angle between the speaker's axis and the direction to the mic, and
 * never which way the box points: every axis on a cone around the mic direction reads the same.
 *
 * The tilt is the direct-sound high-to-mid ratio, 20 log10(band_direct[2] / band_direct[1]) over the
 * CALIB_AIM_* bands. Two readings of it:
 *   relative  the installer turns the box until the tilt PEAKS (calib_peak_*): no calibration, and
 *             the loss of whatever sits on the fixed speaker-to-mic path (a screen, the mic's own
 *             response) is the same at every aim, so it cannot move the peak;
 *   absolute  measured tilt minus the tilt the SAME path shows at 0 deg, inverted through the
 *             model's tilt-versus-angle curve. The 0 deg tilt comes from the vendor file's
 *             on-axis response (calib_on_axis_tilt_db) or, better, from a speaker known to point at
 *             the mic (a stored reference), which also absorbs the mic, the gate and the screen.
 *
 * The curve is SHALLOW near 0 (on the Genelec 4410A: 0.06 dB at 10 deg, 0.25 at 15, 0.9 at 25), so
 * the inversion reports a bracket for a tilt uncertainty tol_db and says "on axis" when the bracket
 * reaches 0 deg. CALIB_LIVE_TILT_TOL_DB is the default: the ZM-1 pressure proxy's residual direction
 * dependence (0.55 dB max-min at 4 to 8 kHz, docs/calibration.md) is about +/-0.3 dB of tilt when the
 * reference speaker and the measured one sit in different directions from the array. */
#define CALIB_LIVE_TILT_TOL_DB 0.3f
/* The LIVE aiming tilt bands: 3-10 kHz (mid) against 10 kHz up (high). Not CALIB_AIM_*_HZ: those
 * were chosen for --check-aim, whose band edge had to stay clear of a position-dependent gate, and
 * on the 4410A their tilt falls only 0.06 dB at 10 deg and 0.25 dB at 15, under the 0.3 dB the
 * reading is good to, so a peak meter on them cannot find the peak. This pair falls 0.51 dB at 10
 * deg and 1.00 dB at 15 (the waveguide's treble narrows far faster than the 3 kHz region), and a
 * few-ms gate resolves both bands. The measurement side (measure_response's band_hz) and the model
 * side (calib_aim_curve, calib_on_axis_tilt_db) MUST use the same pair. */
#define CALIB_LIVE_MID_HZ  3000.0
#define CALIB_LIVE_HIGH_HZ 10000.0
#define CALIB_AIM_CURVE_N      721        /* the curve: 0 .. 180 deg in 0.25 deg steps */

/* The model's tilt-versus-angle curve, curve[i] = calib_aim_tilt_db at i/4 deg (0 at 0 deg by the
 * model's own convention). All zeros without a model. Pure; tens of ms, so build it once per model. */
void calib_aim_curve(const Directivity* d, const double band_hz[2], double f2, float curve[CALIB_AIM_CURVE_N]);

typedef struct {
    int   ok;          /* 1 = inverted; 0 = a curve that never falls (no model) or a non-finite reading */
    float rel_db;      /* the reading: measured tilt minus the 0 deg tilt */
    float angle_deg;   /* the estimate: the smallest angle whose curve value reaches rel_db */
    float lo_deg;      /* the bracket for rel_db +/- tol_db: lo from rel_db + tol_db, */
    float hi_deg;      /* ... hi from rel_db - tol_db */
    int   on_axis;     /* 1 = lo_deg is 0: report "on axis (under hi_deg)", never a number */
    int   beyond;      /* 1 = the reading lies past the curve's range: at least max_deg */
    float max_deg;     /* where the curve's running minimum bottoms out (the range it can invert) */
} CalibAimAngle;

/* Invert a reading through the curve. The curve is not monotonic everywhere (the 4410A reads
 * +0.01 dB at 5 deg and has rear lobes), so the inversion walks its RUNNING MINIMUM from 0 deg,
 * which is monotonic, up to the angle where that minimum stops falling; a reading past it
 * (max_deg) is reported as beyond. A reading above 0 dB (brighter than on axis) is 0 deg. tol_db is
 * clamped to >= 0. Pure. Returns out->ok. */
int calib_aim_invert(const float curve[CALIB_AIM_CURVE_N], float rel_db, float tol_db, CalibAimAngle* out);

/* The measured direct-sound tilt of one capture (or of zylia_pressure_proxy's pool): 1 and *out,
 * or 0 when either band is non-positive or non-finite. */
int calib_direct_tilt_db(const MeasureResult* m, float* out);

/* The tilt the model's on-axis response gives over the same bands: the file's 0 deg reference.
 * 1 and *out, or 0 when the model carries no on_axis_db. The curve above is relative to on axis,
 * so the tilt of (on-axis response x loss) is taken as the sum of the two tilts; on the 4410A the
 * on-axis response is flat enough (within 1.6 dB from 1 to 16 kHz) that the cross term is small. */
int calib_on_axis_tilt_db(const Directivity* d, const double band_hz[2], double f2, float* out);

/* Peak hold for the relative meter. One contaminated reading must not set the peak the installer then
 * chases, so a reading only counts when `clean` (it passed the expected-arrival window and the SNR
 * check, calib_sweep_check) and the peak is only raised once TWO CONSECUTIVE clean readings agree
 * within tol_db, to the LOWER of the pair: the held peak is a level two readings in a row reached. A
 * rejected reading breaks the run, so the pair around it is not consecutive. Non-finite readings
 * are ignored. update returns how far the reading sits BELOW the peak, in dB (>= 0; 0 at or above
 * it, and 0 before any peak). Pure. */
typedef struct {
    int   n;           /* finite readings taken since the last reset, clean or not */
    float last_db;
    float peak_db;
    int   peak_index;  /* n at the reading that confirmed the peak (1-based), 0 before any */
    int   prev_clean;  /* the previous finite reading was clean: the next one can pair with it */
    float prev_db;
    int   nrejected;   /* readings not held because they were not clean */
} CalibPeakHold;
void  calib_peak_reset(CalibPeakHold* p);
float calib_peak_update(CalibPeakHold* p, float value_db, int clean, float tol_db);

/* ---- sweep quality: the expected-arrival window, the IR's SNR, the re-sweep and the agreement
 * rule (docs/calibration.md, "Sweep quality") ----
 * Every sweep is the tool's own: it knows when the sweep plays, and roughly when each speaker's sound
 * must arrive, the system latency plus the distance over c. So the arrival is searched only there,
 * a capture whose strongest tap lies elsewhere is flagged (something other than this speaker was
 * louder), and the IR's peak is held against its noise floor (measure.h). The window:
 *
 *   lo = (lat - lat_early + max(0, d - pos_m) / c) * fs
 *   hi = (lat + lat_late  + (d + pos_m) / c + CALIB_WIN_SPK_S) * fs + 1
 *
 * d the distance from the mic to where the layout says the speaker stands. The margins:
 *   pos_m   CALIB_WIN_SURVEYED_M for a speaker a survey placed (it carries a plan_position: the survey
 *           recorded the plan when it wrote the position), CALIB_WIN_PLAN_M for one at its plan, and
 *           always CALIB_WIN_PLAN_M in the modes whose job is to find or move a position (--localize,
 *           the --zylia survey, --check, live aiming). bwa_calibrate --window-m overrides it.
 *   latency simulate: the simulator's own, exact. --latency (or --ref): +/- CALIB_WIN_LAT_KNOWN_S.
 *           Else the ASIO driver's reported loop, a hard LOWER bound: lat_early 0 and lat_late
 *           CALIB_WIN_LAT_DRIVER_S for what the driver does not report (converters, analog, the ZM-1's
 *           Dante Via leg, about 60 ms on the rig). No driver number: no window, the whole IR.
 * A window only restricts where the peak is searched, so on a capture whose strongest tap is inside
 * it the arrival is the same sample as before, to the bit: it cannot bias a clean measurement. It
 * changes the answer only on a capture it also flags, and a flagged capture is re-swept, never used.
 * Every threshold here is PROVISIONAL: nothing has been measured on the rig yet. */
typedef struct {
    double lat_s;           /* the system latency prior, seconds */
    double lat_early_s;     /* how much EARLIER the true latency can be */
    double lat_late_s;      /* how much LATER */
    double pos_m;           /* how far the speaker can stand from the position the window is read from */
} CalibWindow;
#define CALIB_WIN_SURVEYED_M   0.10     /* a surveyed position: survey error plus a 1% error in c at 4 m */
#define CALIB_WIN_PLAN_M       0.50     /* a planned position: a box hung half a meter off is still found */
#define CALIB_WIN_LAT_KNOWN_S  0.001    /* --latency / --ref: a measured loop, 34 cm of slack */
#define CALIB_WIN_LAT_DRIVER_S 0.150    /* the unreported part over the driver's loop (the ZM-1 chain ~60 ms) */
#define CALIB_WIN_SPK_S        0.002    /* the box's own delay: its crossover's group delay puts the peak
                                         * after the onset; latency the box adds is in lat already */
#define CALIB_WIN_ZM1_M        0.05     /* a ZM-1 capsule sits up to the array's radius from its center */
/* [*lo, *hi) on the capture's sample axis, from the speaker at spk and the mic at mic. Returns 1, or
 * 0 on non-finite or negative input (no window). Pure. */
int calib_arrival_window(const CalibWindow* w, const float spk[3], const float mic[3], double sos, double fs,
                         int* lo, int* hi);
/* The IR's peak against its noise floor, below which a capture is re-swept. Simulated captures read
 * 70 to 300 dB with and without --sim-room (the room's late tail is the floor there); a real rig
 * will read less. PROVISIONAL until rig data exists. */
#define CALIB_SWEEP_MIN_SNR_DB    40.0f
/* Two sweeps of one speaker AGREE when their arrivals differ by at most a sample and their levels by
 * at most 0.2 dB (the trims and --verify count a value only then). PROVISIONAL. */
#define CALIB_SWEEP_AGREE_SAMPLES 1.0f
#define CALIB_SWEEP_AGREE_DB      0.2f
#define CALIB_SWEEP_MAX_TRIES     5       /* sweeps per speaker before the run gives up on it */
enum { CALIB_SWEEP_OK = 0, CALIB_SWEEP_OUTSIDE = 1, CALIB_SWEEP_NOISY = 2 };
/* CALIB_SWEEP_OUTSIDE when m->outside, else CALIB_SWEEP_NOISY when the SNR is known and under
 * min_snr_db, else CALIB_SWEEP_OK. An unknown SNR (no noise region) passes. Pure. */
int  calib_sweep_check(const MeasureResult* m, float min_snr_db);
/* One ASCII line saying why (the arrival, the window, the outside tap and its level, or the SNR). */
void calib_sweep_why(const MeasureResult* m, int code, double fs, char* buf, size_t cap);
/* 1 when two sweeps of one speaker agree (the rule above). *d_samples and *d_db (either may be NULL)
 * receive the differences. Pure. */
int  calib_sweeps_agree(const MeasureResult* a, const MeasureResult* b, float* d_samples, float* d_db);

/* ---- the solved latency against the driver's own loop (--localize, the --zylia survey) ----
 * The solve recovers the WHOLE loop; the ASIO driver reports only its digital half. The residual,
 * solved minus reported, is what the driver cannot see, so it is never negative: below
 * -CALIB_LAT_FLOOR_S is IMPOSSIBLE (wrong device, a sample-rate mismatch, clocking) for every mic. Its
 * upper bound depends on the mic:
 *   omni (an analog input): converters plus analog, a few ms. Past CALIB_LAT_OMNI_S an extra buffer
 *     is in the loop (the Dante latency setting).
 *   ZM-1 (Dante Via, the rig's only measurement mic): the Via leg and the converters, which the driver
 *     does not report either, about 60 ms on the rig. The bound is CALIB_WIN_LAT_DRIVER_S, the same
 *     allowance the arrival window grants past the driver's loop: a residual beyond it means the window
 *     could not have held the arrival, so the latency is not what every sweep assumed.
 * PROVISIONAL, like the window: nothing has been measured on the rig yet. */
#define CALIB_LAT_FLOOR_S 0.0005   /* rounding slack below the driver's loop: 24 samples at 48 kHz */
#define CALIB_LAT_OMNI_S  0.020    /* an omni's residual past this: an unexpected buffer */
enum { CALIB_LAT_OK = 0, CALIB_LAT_WARN = 1, CALIB_LAT_IMPOSSIBLE = 2 };
/* zylia: the mic is the ZM-1 on Dante Via. solved_s, driver_s: the solved and the driver's loop, in
 * seconds. *resid_s (may be NULL) receives solved - driver. Non-finite input reads WARN. Pure. */
int  calib_latency_check(int zylia, double solved_s, double driver_s, double* resid_s);
double calib_latency_bound_s(int zylia);           /* the upper bound above, per mic */

/* ---- the second pass (bwa_calibrate --verify; docs/calibration.md) ------------------------------
 * The trim run sweeps the RAW outputs and writes trims it never plays. --verify sweeps every speaker
 * THROUGH the layout's output stage (align.c, exactly as the engine builds it) from the same mic
 * placement, deconvolves against the raw sweep so each measurement carries the trims, and this
 * scores what is left over. `m` is those through-the-trims measurements. Per speaker k:
 *
 *   arrival_us[k] = t_k - (d_k(mic) - d_k(align_pt)) / c,  median removed, in microseconds
 *   level_db[k]   = 20 log10(level_k * d_k(mic) * corr_k),  median removed
 *
 * t_k is the measured arrival (delay_samples + delay_frac, over fs). The trims' job is to make every
 * arrival equal at the point they were solved from (calib_solve aligns the arrivals it measured), so
 * with the mic at that point the expected arrival is one constant: the system latency plus the
 * farthest speaker's flight time plus nothing per speaker. The median stands in for it, which also
 * makes the check independent of the latency. `align_pt` is that point: pass the trim run's mic
 * (bwa_calibrate does, since it verifies from the same placement), and the geometric term is zero;
 * from another placement it predicts how the aligned arrivals spread apart. The level is normalized
 * exactly the way calib_solve_corr normalizes the trim run's (distance divided out, the directivity
 * re-aim factor `corr` applied, NULL = none), so after correct gain trims every speaker reads 0 dB.
 *
 * Flags: ARRIVAL beyond +/-CALIB_VERIFY_ARRIVAL_US, LEVEL beyond +/-CALIB_VERIFY_LEVEL_DB, DEAD for a
 * non-positive or non-finite level (excluded from both medians; its residuals read 0). Why these:
 * 100 us is 3.4 cm of path and about 5 samples, several times what a correct pass leaves (the trims
 * are whole samples, so up to about 1 sample, 21 us, plus the peak interpolation) and a tenth of the
 * ~1 ms range over which an inter-speaker delay moves a phantom; a 0.5 ms error reads 5x over it. 1 dB is
 * about the level just-noticeable difference for broadband noise, and three times what a correct pass
 * leaves in simulation. Returns the number of flagged speakers (0 = pass), or -1 on bad input.
 * `sum` (NULL ok) gets the peak-to-peak spreads over the live speakers. Pure. */
#define CALIB_VERIFY_ARRIVAL_US 100.0f
#define CALIB_VERIFY_LEVEL_DB     1.0f
#define CALIB_VERIFY_FLAG_ARRIVAL 0x01
#define CALIB_VERIFY_FLAG_LEVEL   0x02
#define CALIB_VERIFY_FLAG_DEAD    0x04
typedef struct {
    int   nlive;
    int   nflag;              /* speakers with any flag */
    float arrival_spread_us;  /* max - min over the live speakers */
    float level_spread_db;
} CalibVerifySummary;
int calib_verify_residuals(const MeasureResult* m, const float (*pos)[3], const float mic[3],
                           const float align_pt[3], int n, double fs, double c, const float* corr,
                           float* arrival_us, float* level_db, int* flags, CalibVerifySummary* sum);

/* ---- the aiming sheet (bwa_calibrate --aim-sheet; docs/calibration.md) --------------------------
 * Angles an installer can set with a protractor and an inclinometer, in the ROOM frame (frame.h,
 * bw_audio.h: +y up, +z room-ahead, room-right = -x):
 *   bearing_deg    horizontal direction, clockwise seen from above: 0 = room-ahead (+z), 90 =
 *                  room-right (-x), 180 = behind (-z), 270 = room-left (+x). In [0, 360). A vertical
 *                  vector has no bearing and reads 0.
 *   down_tilt_deg  elevation below the horizontal: +90 = straight down, 0 = level, negative = up.
 * `v` need not be unit length. */
void calib_aim_angles(const float v[3], float* bearing_deg, float* down_tilt_deg);

/* One speaker's row of the sheet. The target is Layout.ref, which the loader sets to
 * `listening_point_m` when the file declares one and to the array centroid otherwise, and which is
 * also where a speaker with no `aim` points: so the sheet uses exactly the point the engine uses. */
typedef struct {
    float pos[3];
    float target[3];          /* L->ref */
    float dist_m;             /* |target - pos| */
    float aim[3];             /* the aim to set: unit vector from pos toward target */
    float bearing_deg, down_tilt_deg;
    float layout_aim[3];      /* L->speakers[s].aim */
    float layout_bearing_deg, layout_down_tilt_deg;
    float off_deg;            /* angle between layout_aim and aim */
    int   have_loss;          /* 1 = the layout carries a directivity model */
    float loss_2k_db, loss_16k_db;   /* the model's loss at off_deg (<= 0 dB) */
} CalibAimRow;
int calib_aim_row(const Layout* L, int s, CalibAimRow* out);   /* 1, or 0 on bad input */

/* What the loaded Layout cannot tell apart: whether the file DECLARED listening_point_m, and which
 * speakers carry an explicit `aim` (the rest were pointed at ref by the loader). Reads the JSON at
 * `path`; `aim_explicit` is n long (NULL ok). Returns 1, or 0 when the file cannot be read or its
 * speaker count is not n. */
int calib_layout_declared(const char* path, int n, int* has_listening_point, unsigned char* aim_explicit);

/* Write the sheet as CSV: one plain header row, then one row per speaker (no comment lines, so a
 * spreadsheet opens it as is). `aim_explicit` (NULL = all default) fills the aim-source column. A row
 * whose layout aim is more than `flag_deg` off the aim toward the target gets flag OFF_AIM. `nflag`
 * (NULL ok) receives the count. Returns 1, or 0 with a message in `err`. */
int calib_write_aim_sheet(const char* csv_path, const Layout* L, const unsigned char* aim_explicit,
                          float flag_deg, int* nflag, char* err, size_t errcap);
#define CALIB_AIM_SHEET_FLAG_DEG 20.0f

/* Read the layout JSON at `in_path`, set each speaker's `gain_db` + `delay_ms` (preserving index,
 * position, the dbap block, everything else), and write to `out_path` (may equal in_path). The file's
 * speaker count must equal `n`. Returns 1 on success, 0 with a message in `err`. */
int calib_write_layout(const char* in_path, const char* out_path,
                       const float* gain_db, const float* delay_ms, int n, char* err, size_t errcap);

/* The layout file's recorded speed of sound (reference.speed_of_sound_mps), the room-temperature c
 * every acoustic RANGE in a survey is scaled by. Reads 1 + the value when the file carries a
 * plausible one (see sos.h), 0 otherwise — the caller then falls back to BWA_SOS_REF_MPS. Keeping it
 * in the file means an install sets its temperature once instead of remembering a flag per run, and
 * a collaborator rig at a different temperature carries its own. calib_write_sos records it back
 * (creating the `reference` block if the file predates the field); like every calib_write_*, it
 * re-parses and re-serializes so unknown fields survive. */
int calib_read_sos(const char* path, double* out_mps);
int calib_write_sos(const char* in_path, const char* out_path, double mps, char* err, size_t errcap);

/* Acoustic self-localization: solve one speaker's 3D position from its measured RANGE (= c * delay,
 * in meters, latency INCLUDED) to K known mic positions. The unknown constant system latency is
 * recovered jointly (as a range, c*tau) by linear least squares — so no separate loopback is needed.
 * Needs K >= 5 non-coplanar mic positions (more = more robust). `mic[k]` and `pos_out` are room meters.
 * Returns 1 + pos_out (+ latency_out = recovered c*tau if non-NULL); 0 if underdetermined/singular.
 * This sees speakers OPTICAL trackers can't (the sweep passes through acoustically-transparent screens). */
int calib_trilaterate(const double* range, const float (*mic)[3], int K, float* pos_out, double* latency_out);

/* calib_trilaterate with the latency KNOWN: the point x with |anchor_k - x| = range[k] - latency, by
 * Gauss-Newton from the start in pos_io (written back). Three unknowns instead of four, which matters
 * where the anchors all sit at about one distance from x (a dome around its listening point): there
 * the latency and the radial coordinate trade off and calib_trilaterate's linear solve can put the
 * latency tens of mm out while x barely moves. Take the latency from a run whose geometry DOES pin it
 * (--localize: the mic rows move, the speakers do not). Needs K >= 4. Returns 1, or 0 on a degenerate
 * or non-converging solve (pos_io untouched). */
int calib_locate_known_latency(const double* range, const float (*anchor)[3], int K, double latency, float pos_io[3]);

/* The least-squares refine of calib_trilaterate's answer, all four unknowns: alternate the
 * known-latency solve for the point with the latency as the mean range residual, until both settle.
 * calib_trilaterate subtracts equations to make the problem linear, and on a dome that step loses the
 * latency almost entirely (2 m out on 3.5 mm of position error in calib_test), even where the geometry
 * itself pins it to about calib_latency_dilution x the range noise. Start from calib_trilaterate's
 * point; the latency start is ignored. Returns 1, or 0 with both untouched. */
int calib_trilaterate_refine(const double* range, const float (*anchor)[3], int K, float pos_io[3], double* latency_io);

/* How badly the anchors' geometry seen from x amplifies range noise into the latency calib_trilaterate
 * solves: sqrt(K * [(J^T J)^-1]_33) for the 4-unknown Jacobian rows (-u_k, 1). 1 when the anchors
 * surround x evenly; it grows as they bunch to one side, and past about 3 a solved latency is not worth
 * quoting. Returns -1 when the geometry is degenerate. */
double calib_latency_dilution(const float (*anchor)[3], int K, const float x[3]);

/* Write recovered speaker positions back into the layout JSON (sets each speaker's "position" [x,y,z],
 * preserving everything else). `pos[i]` are room meters; the file's speaker count must equal `n`.
 * A measuring writer: a record with no plan yet first keeps its old position (and aim) as
 * plan_position / plan_aim (layout_json_keep_plan); `plan_recorded` (NULL ok) gets how many did. */
int calib_write_positions(const char* in_path, const char* out_path, const float (*pos)[3], int n,
                          int* plan_recorded, char* err, size_t errcap);

/* Per-speaker correction FIR with the calibration gate policy: `ir` starts at the direct arrival;
 * `first_refl` is the samples to the first reflection (0/unknown -> a default ~4 ms window). Gates so
 * the filter corrects the SPEAKER (direct sound), not the room, then inverts (see measure_correction).
 * Writes ntaps into `taps`. Returns 1 / 0. */
int calib_eq(const float* ir, int nir, int first_refl, double fs, int ntaps, float* taps);

/* Write per-speaker correction FIRs into the layout JSON as each speaker's "eq" array (replacing any
 * prior), preserving everything else. `taps` is n * max_taps row-major; `lens[i]` taps for speaker i
 * (0 = remove its eq). The file's speaker count must equal `n`. Returns 1 / 0. */
int calib_write_eq(const char* in_path, const char* out_path, const float* taps, const uint16_t* lens,
                   int n, int max_taps, char* err, size_t errcap);

/* STATIC-LISTENER room correction (docs/calibration.md): only valid when the listener sits at the
 * measurement point (the fixed-observer SPCAP/VBAP deployments). Produces BOTH halves from one IR:
 *   taps — a correction FIR from a frequency-dependent window (direct-gated at HF == the speaker EQ
 *          above; growing to include the room toward LF), covering 200 Hz up, boost-capped at +3 dB;
 *   cuts — LF modal peaking CUTS (30..200 Hz), where modes are minimum-phase and correctable.
 * The 200 Hz split means nothing is corrected twice. Returns the cut count (0 = flat), -1 on failure. */
int calib_room_eq(const float* ir, int nir, int first_refl, double fs, int ntaps, float* taps,
                  MeasureEqSection* cuts, int max_cuts);

/* Write per-speaker LF modal cuts into the layout JSON as each speaker's "room_eq" array of
 * { fc, gain_db, q } objects (replacing any prior; count 0 = remove). `cuts` is n * max_sections
 * row-major with `counts[i]` used per speaker. Returns 1 / 0. */
int calib_write_room_eq(const char* in_path, const char* out_path,
                        const MeasureEqSection* cuts, const int* counts, int n,
                        int max_sections, char* err, size_t errcap);

/* TRACKED room EQ (docs/calibration.md): merge ONE speaker's modal cuts measured at `npos` mic
 * positions into the congruent ladder the layout's room_eq_grid needs. Room modes don't move with
 * the mic — only their measured depth does — so per-position fcs within `tol_rel` (e.g. 0.08) are
 * the SAME mode: one output section per cluster, fc/q = the member medians, and per position that
 * position's measured depth (0 dB where it didn't see the mode). Keeps the deepest `max_out`
 * clusters. `cuts` is npos * max_in row-major with counts[p] used; writes fc/q (max_out) +
 * gain_db (npos * max_out row-major, <= 0). Returns the ladder size (0 = flat everywhere). */
int calib_room_grid_merge(const MeasureEqSection* cuts, const int* counts, int npos, int max_in,
                          double tol_rel, int max_out, float* fc, float* q, float* gain_db);

/* Merge THIS mic position's per-speaker modal cuts into the layout JSON's "room_eq_grid": existing
 * grid entries are read back (their sections re-treated as that position's cuts), an entry within
 * 5 cm of `mic` is replaced (else appended, up to BWA_RQ_GRID_MAX), every speaker's ladder is
 * re-merged across all positions (calib_room_grid_merge), and the congruent grid is rewritten —
 * so one bwa_calibrate run per mic placement accumulates the grid. Removes any static per-speaker
 * "room_eq" (the schemes are mutually exclusive). `cuts` is n * max_sections row-major with
 * counts[i] used per speaker. Returns 1 / 0. */
int calib_write_room_eq_grid(const char* in_path, const char* out_path, const float mic[3],
                             const MeasureEqSection* cuts, const int* counts, int n,
                             int max_sections, char* err, size_t errcap);
/* The same for `nmic` positions at once (the --room-eq-grid rows run): every mic is merged in order
 * as if by one call each, but the file is read once and written once, so a grid that would overflow
 * BWA_RQ_GRID_MAX part-way, or a NaN in any row, fails before anything is written. `cuts` is
 * nmic * n * max_sections row-major (mic, then speaker), `counts` nmic * n. */
int calib_write_room_eq_grid_n(const char* in_path, const char* out_path, int nmic, const float (*mics)[3],
                               const MeasureEqSection* cuts, const int* counts, int n,
                               int max_sections, char* err, size_t errcap);
/* The radius within which a grid position replaces an existing entry instead of adding one. */
#define CALIB_RQ_GRID_REPLACE_M 0.05f

/* Drift check: given measured ranges (c*delay, meters, latency included) from ONE mic at `mic` to n
 * speakers at their STORED positions `pos`, report each speaker's RADIAL deviation (meters) from where
 * it should be. The unknown common latency is removed as the MEDIAN residual (robust to a few moved
 * speakers), so a speaker bumped toward/away from the mic shows up as a non-zero deviation — one
 * fast single-position pass flags anything nudged. (Purely tangential moves don't change the range;
 * a full re-survey is calib_trilaterate.) */
void calib_check_drift(const double* range, const float (*pos)[3], const float mic[3], int n, float* deviation_m);

#ifdef __cplusplus
}
#endif

#endif /* BWA_CALIB_H */
