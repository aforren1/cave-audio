/*
 * valid.h — instrumental validation of PHANTOM localization.
 *
 * The rest of the Zylia code answers "where are my speakers". This answers the harder question:
 * when the array RENDERS a source in some direction, where does it actually end up? Render a
 * phantom, measure it with the ZM-1 (zylia_intensity_doa), and report the angular miss against the
 * direction it was supposed to be. That is the measurement that grades a panner, a layout, or a
 * calibration, and no amount of listening to it produces a number.
 *
 * WHAT THE SIMULATED PATH DOES AND DOES NOT INCLUDE. valid_simulate builds the field a ZM-1 would
 * record in an ANECHOIC free field: every speaker's real solved gain, its layout trim and alignment
 * delay, 1/r spreading and the exact propagation delay to each of the 19 capsules, summed coherently.
 * So it contains the real phantom-source physics — interference between speakers included, which is
 * where phantom error actually comes from — but NO ROOM. Measured phantom error in a real room is
 * roughly a rendering term plus a room term; this isolates the RENDERING term, which is the one
 * placement and panner choice control, and the one that varies as the listener walks. The room term
 * needs hardware and is not modeled here. Do not read a simulated miss as a predicted in-room miss;
 * read it as the floor that the room then adds to. (Modeling the room here would repeat the
 * calibration trap of matching a measured RT60: see docs/calibration.md.)
 *
 * THE COMPARISON THIS EXISTS FOR. A fixed-sweet-spot renderer and a tracked one are not distinguished
 * by any measurement taken at the sweet spot — which is exactly where such measurements are usually
 * taken, and why they are quiet about the difference that matters here. So the solve position is
 * SEPARATE from the microphone position:
 *   - tracked = 1: the panner solves at the listener's ACTUAL position (what DBAP does per block).
 *   - tracked = 0: the panner solves at the layout's sweet spot while the listener stands elsewhere
 *     (what a fixed SPCAP/VBAP install does once, at load).
 * The source sits at a fixed WORLD position, so as the listener moves, the direction they should
 * perceive changes with them and the target follows honestly. Sweep listener positions across the
 * tracked walking envelope and the two renderers separate.
 *
 * Everything here is control-thread, offline analysis: it allocates, and it is not for the audio
 * thread. It drives the engine's OWN panner solve (bwa_panner_gains_batch), so it scores what will
 * actually ship rather than a reimplementation.
 */
#ifndef BWA_VALID_H
#define BWA_VALID_H

#include "layout.h"
#include "zylia.h"

#include <stdint.h>

/* One measured or simulated cell of the grid. */
typedef struct {
    int   panner;         /* bwa_panner; meaningless when reference = 1 */
    int   tracked;        /* 1 = solved at the mic, 0 = solved at the sweet spot */
    int   reference;      /* 0 = a grid phantom (tgt indexes the caller's target list)
                           * 1 = ONE speaker driven directly, no panning: a physical source
                           * 2 = the phantom MATCHED to it, rendered at that speaker's position.
                           * For 1 and 2, tgt is the SPEAKER index, so a pair is (lis, tgt). */
    int   lis;            /* index into the caller's listener list */
    int   tgt;            /* index into the caller's target list, or the speaker index if reference */
    float mic[3];         /* where the array was listened from (room m) */
    float target[3];      /* unit direction the source SHOULD have come from, seen from mic */
    float measured[3];    /* unit direction it actually came from */
    float miss_deg;       /* great-circle angle between them — the number */
    float diffuseness;    /* estimator confidence; high = measured in a smeared field */
    int   ok;             /* 0 = the estimator refused, miss_deg is meaningless */
} ValidCell;

/* ---- the measurement stimulus ----
 *
 * Localization accuracy is strongly content-dependent IN A ROOM: broadband localizes best, speech is
 * intermediate, sustained narrowband tones are worst, sometimes by tens of degrees. The mechanism for
 * the tone case is specific and worth knowing before reading any result — a steady tone sets up a
 * standing-wave field, and the active-intensity vector reports the net energy flux at ONE point,
 * which near a pressure node need not point back at the source. Broadband content averages that flux
 * over many independent bins and the off-source contributions cancel; a tone in a narrow band has no
 * such averaging.
 *
 * THERE ARE TWO MECHANISMS, and it is easy to credit only the first. The published study measured
 * its anechoic control with a PHYSICAL loudspeaker — one coherent source, nothing to interfere with —
 * and found every stimulus localized equally there, which makes the room look like the whole story.
 * It is not, for a PHANTOM:
 *  - **The room**: standing waves, as above. Needs hardware to see.
 *  - **The array itself**: a phantom is a coherent sum of many speakers, so its interference pattern
 *    is frequency-dependent even in free field. Broadband averages over that; one tone cannot. This
 *    shows up ANECHOICALLY, and the harness measures it (the `valid` test reports the anechoic
 *    content spread for a physical source versus a phantom: ~0.1 deg against tens of degrees).
 *
 * So --simulate DOES show content dependence for phantoms, and that is a result rather than a fault.
 * The negative control is the REFERENCE arm: a single driven speaker must localize the same whatever
 * the content, because there is nothing for it to interfere with. If that ever stops holding, the
 * analysis chain is at fault and every content finding built on it is an artifact.
 *
 * One more property worth knowing: a tone's error is *precisely wrong* — sub-degree repeatable and
 * tens of degrees biased. Repeat measurements agree beautifully with each other and with nothing else.
 *
 * The analysis band follows the stimulus: broadband gets 400-1200 Hz, a tone gets +-1/6 octave around
 * itself. A tone whose band lies entirely above the array's first-order ceiling (a 6 kHz tone) is
 * refused outright rather than measured badly.
 *
 * Module-level, control-thread only, mirroring zylia_set_capsules: these tools are single-threaded
 * and the stimulus is a property of a whole run, not of one cell. Returns 1 if the stimulus is
 * measurable, 0 if it is out of the array's reach (state is then unchanged). */
typedef enum {
    VALID_STIM_BROADBAND = 0,   /* a 24-tone sum across 420-1150 Hz: the default, best case */
    VALID_STIM_TONE      = 1    /* one sustained tone at `hz`: the pathological case */
} ValidStimKind;

int         valid_set_stimulus(ValidStimKind kind, double hz);
void        valid_get_stimulus_band(double* f_lo, double* f_hi);
const char* valid_stimulus_name(void);

/* Synthesize the 19 capsule signals a ZM-1 at `mic` records while the array renders a source at
 * world position `src_world`, with `panner` solved at `solve_pos`. buf = [ZYLIA_MICS][n] flat, row
 * stride n; it is overwritten and peak-normalized (level carries no information here — the estimator
 * reads direction, and normalizing keeps the integrity checks from crying clipping).
 * The stimulus is a fixed broadband tone sum inside the estimator's own band: a tone delays by a
 * phase shift, so every one of the 26 x 19 propagation paths is applied EXACTLY, with no fractional
 * -delay interpolation error anywhere in the model. Returns 1 on success, 0 on bad arguments. */
int valid_simulate(const Layout* L, int panner, const float solve_pos[3], const float mic[3],
                   const float src_world[3], double fs, double c, float* buf, uint32_t n);

/* Simulate one cell and score it: render, measure, fill `out`. `n` samples of capture (>= one
 * analysis frame; 8192 is plenty). Returns 1 if the cell was scored (check out->ok for whether the
 * estimator actually resolved it). */
int valid_cell(const Layout* L, int panner, int tracked, const float mic[3], const float src_world[3],
               double fs, double c, uint32_t n, ValidCell* out);

/* The nspk speaker feeds the array emits to render `src_world` with `panner` solved at `solve_pos`:
 * the panner gains, the layout's per-speaker level trim and alignment delay, driving the harness's
 * standard stimulus. feeds = [nspk][n] flat, row stride n; L->count rows are written.
 *
 * This is what goes OUT the device on the hardware path — the same render valid_simulate propagates
 * analytically offline, so the two paths measure the same thing. Note it applies the layout's trims
 * and delays ITSELF, exactly as align.c would: the hardware path drives the device directly rather
 * than through a running engine, which keeps the measurement deterministic and scoped to the panner
 * + layout + room rather than to engine runtime state (fades, limiter, ramps).
 *
 * The stimulus is STEADY-STATE, and that is a deliberate and useful property: unlike a swept
 * measurement, nothing here depends on knowing the round-trip latency. Play and capture concurrently,
 * analyze any window that is comfortably inside the steady state, and device latency simply does not
 * enter. Returns 1 on success, 0 on bad arguments. */
int valid_speaker_feeds(const Layout* L, int panner, const float solve_pos[3],
                        const float src_world[3], double fs, float* feeds, uint32_t n);

/* Score an ALREADY-CAPTURED 19-channel buffer (cap19 = [ZYLIA_MICS][n] flat, row stride n) against
 * the direction the source should have come from. This is the seam the hardware path enters
 * through: build feeds -> play/record -> valid_score. valid_cell is exactly this with the simulated
 * field substituted for the capture, so both paths share every line of the scoring and statistics
 * above them.
 * `exclude` is zylia_check_capsules' flags (or NULL): pass the session's mask so a faulty capsule is
 * dropped from the estimate rather than silently poisoning every cell of that placement — that check
 * is the one thing no amount of downstream agreement can substitute for.
 * Returns 1 if the cell was scored (check out->ok). */
int valid_score(const Layout* L, int panner, int tracked, const float mic[3], const float src_world[3],
                const float* cap19, uint32_t n, double fs, double c,
                const unsigned char* exclude, ValidCell* out);

/* ---- the physical reference arm: drive ONE speaker, measure where it lands ----
 *
 * Every miss above is an ABSOLUTE number, so it folds three things together: what the estimator
 * costs, what the room costs, and what the renderer costs. Separating them needs a physical source
 * measured through the same chain in the same room, which is why the published protocol moved a real
 * loudspeaker to each target direction across dozens of sessions.
 *
 * We do not have to move anything. THE ARRAY'S OWN SPEAKERS ARE PHYSICAL SOURCES at known positions.
 * Drive speaker i alone, with no panning, and the estimator's answer is a real-source measurement in
 * this room. Two things fall out:
 *
 *  - **A matched-cell contrast.** Render a phantom AT SPEAKER i'S OWN POSITION and you have the same
 *    direction, the same room and the same microphone placement, measured both ways. Subtracting is
 *    then meaningful in the way an absolute miss is not: "+11 deg over a real source here", rather
 *    than "16 deg", which silently includes the room and the instrument.
 *  - **An end-to-end instrument check.** If the estimator cannot find speaker 7 where the surveyed
 *    layout says speaker 7 is, nothing downstream is trustworthy — and you learn it in seconds
 *    rather than after a full session.
 *
 * The target is taken from the LAYOUT, so a reference miss also prices in survey error. That is
 * correct: a layout that misplaces a speaker misaims every phantom too.
 *
 * `valid_reference_feeds` is the hardware side (unit drive on one channel, still through its trim and
 * alignment delay, exactly as align.c would); `valid_reference_cell` is the offline equivalent. */
int valid_reference_feeds(const Layout* L, int spk, double fs, float* feeds, uint32_t n);

int valid_reference_cell(const Layout* L, int spk, const float mic[3],
                         double fs, double c, uint32_t n, ValidCell* out);

/* The GEOMETRIC PROXIES the layout optimizer climbs, evaluated on the same cell the harness
 * measures acoustically — so the two can be compared directly, which is the entire point.
 *
 * `re_err_deg` is the Gerzon energy-vector DIRECTION error and `spread_deg` the Frank perceived
 * source width 186.4(1-|rE|)+10.7 — exactly what layout_tool's Score and Optimize compute. They are
 * gain-domain quantities: a weighted sum of speaker directions, blind to what the speakers' outputs
 * actually do to each other at a point in the room. valid_score, by contrast, sums real acoustic
 * pressure at 19 capsules and reads the intensity vector, so it sees the inter-speaker interference
 * that IS the phantom. If the cheap proxy and the acoustic measurement disagree, the optimizer is
 * climbing the wrong hill, and that is worth knowing before trusting a layout it produced.
 * Returns 1 on success, 0 on bad arguments or a degenerate solve. */
int valid_re_proxy(const Layout* L, int panner, const float solve_pos[3], const float mic[3],
                   const float src_world[3], float* re_err_deg, float* spread_deg);

/* Sweep panners x listeners x targets. Sources are placed at `radius` meters from the layout's sweet
 * spot along each target direction, so every listener is judged against the same physical sources —
 * which is what makes the off-center cells comparable to the centered ones. cells_out must hold
 * npan * nlis * ntgt entries. Returns the number of cells written. */
int valid_run(const Layout* L, const int* panners, int npan, int tracked,
              const float (*listeners)[3], int nlis,
              const float (*targets)[3], int ntgt,
              float radius, double fs, double c, uint32_t n, ValidCell* cells_out);

/* Unit target directions on an azimuth x elevation grid (the paper's 24 x 3 shape). Azimuths are
 * naz even steps from 0; elevations are given in degrees. Returns the count written (naz * nel),
 * or 0 if it would exceed cap. */
int valid_target_grid(int naz, const float* elev_deg, int nel, float (*out)[3], int cap);

/* ---- statistics: medians and percentile bootstrap, the paper's reporting shape ----
 *
 * Medians rather than means throughout: localization error is heavy-tailed (a handful of directions
 * fail badly and would drag a mean around), and a median with a bootstrap interval says what a
 * typical cell does without pretending the tail is Gaussian. Seeds are explicit so a reported
 * interval is reproducible. */
double valid_median(const double* v, int n);

/* Percentile bootstrap CI of the median. nresamp ~ 2000. Returns 1 on success. */
int valid_bootstrap_ci(const double* v, int n, int nresamp, unsigned int seed,
                       double* lo, double* hi);

/* MATCHED-CELL contrast: a[] and b[] must be the same cells measured two ways (same direction, same
 * listener, different panner or different tracking), so the difference is PAIRED and the comparison
 * is not confounded by which cells happened to be measurable in each condition. Reports the median
 * of (b - a) and its bootstrap CI; an interval excluding zero is the claim worth making.
 * Cells where either side failed must be dropped by the caller first. Returns 1 on success. */
int valid_contrast(const double* a, const double* b, int n, int nresamp, unsigned int seed,
                   double* med_diff, double* lo, double* hi);

#endif /* BWA_VALID_H */
