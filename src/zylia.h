/*
 * zylia.h — single-position speaker localization with a Zylia ZM-1 (19-mic spherical array).
 *
 * The omni-mic survey (calib_trilaterate) recovers speaker positions by moving ONE mic to >=5 spots
 * and trilaterating. The ZM-1 has 19 capsules on a rigid sphere, so ONE placement already sees each
 * sweep arrive at 19 slightly-different times — the arrival-time DIFFERENCES across the sphere give the
 * direction-of-arrival (DOA) directly, and (with the known system latency) the distance. So a speaker's
 * position falls out of a SINGLE Zylia placement: direction x distance + the array centre.
 *
 * Accuracy characteristics (be honest about them):
 *   - DIRECTION is latency-independent (uses arrival DIFFERENCES) and reaches ~1 deg given sub-sample
 *     arrival times (measure.c's sub-sample IR peak delivers that). This is the headline: a quick
 *     "where is every speaker, from one spot" that the multi-position omni survey can't do in one shot.
 *   - DISTANCE is only as good as the supplied system latency. The array is ~5 cm, so at metres the
 *     wavefront curvature across it is sub-millimetre of differential delay — far too weak to
 *     self-calibrate the latency. Feed a loopback-measured latency (or fuse with calib_trilaterate).
 *
 * zylia_geometry is the real array: a vertex-up dodecahedron minus its nadir vertex (see zylia.c). What
 * it does NOT fix is the CHANNEL ORDER (node i vs ASIO input i) or the AZIMUTH REFERENCE (which capsule
 * faces the device front) — a permutation or a yaw offset still produces a confident direction, just the
 * wrong one, and no off-hardware test can catch either. Pin them on the rig (bwa_zylia_probe / a clap from
 * a known direction), or measure the geometry outright with the capsule self-survey. docs/calibration.md.
 *
 * The DOA/localize solve is unit-tested off-hardware (synthesize the 19 arrivals from a known position,
 * recover it) and the table's dodecahedral structure is pinned separately — see zylia_test.c.
 */
#ifndef BWA_ZYLIA_H
#define BWA_ZYLIA_H

#include <stdint.h>

#define ZYLIA_MICS 19
#define ZYLIA_SURVEY_MAX 64    /* max clap observations zylia_survey will take (far more than it needs) */

/* Fill the 19 capsule UNIT directions (array-local frame: +X right, +Y up, -Z front) and the sphere
 * radius (m). Vertex-up dodecahedron minus the nadir — see zylia.c for the construction and for the
 * two things it can't pin (channel order, azimuth reference). */
void zylia_geometry(float dirs[ZYLIA_MICS][3], float* radius_m);

/* Direction-of-arrival from 19 per-mic arrival times (seconds; the sub-sample IR peak per channel).
 * dir_out = unit vector from the array centre TOWARD the source. Latency-independent. Returns 1 on
 * success, 0 if the linear solve is singular. Far-field fit — a small near-field direction bias at
 * close range that zylia_localize's refined position removes. */
int  zylia_doa(const double arrival_s[ZYLIA_MICS], float dir_out[3]);

/* Arrival times from a LIVE 19-channel transient snapshot (a clap / hand-click), for feeding
 * zylia_doa without a sweep: finds the onset on the strongest channel, windows every channel
 * around it (tight, so early room reflections stay out), cross-correlates each against that
 * reference over lags within +-max_lag samples, and refines the peak to sub-sample by parabolic
 * interpolation. The array is ~10 cm, so the true inter-capsule lag is <= ~14 samples at 48 kHz —
 * pass max_lag ~32 for margin. x = 19 planar channel pointers, n samples each (sample-locked,
 * which the ZM-1's shared ADC guarantees). arrival_s comes back RELATIVE (reference channel = 0);
 * a common offset is meaningless — zylia_doa uses differences only. Returns 1 on success, 0 if
 * there is no usable transient (peak < 6x the channel RMS) or the window doesn't fit. */
int  zylia_tdoa(const float* const x[ZYLIA_MICS], uint32_t n, double fs, uint32_t max_lag,
                double arrival_s[ZYLIA_MICS]);

/* ---- validation-grade DOA: active intensity in the spherical-harmonic domain ----
 *
 * Everything above answers "where are my SPEAKERS", from a transient, by arrival times. This answers
 * a different question: "where does this SOUND appear to come from" — for arbitrary continuous
 * content, over whatever the array is actually rendering. That is the measurement a rendered PHANTOM
 * needs (a phantom has no arrival time of its own; it is the summed output of many speakers), and it
 * is what a walking-listener panner comparison runs on. Two estimators, two jobs, one capture rig.
 *
 * Method (Jarrett/Habets/Naylor pseudointensity, the standard): STFT each capsule, least-squares
 * project the 19 pressures onto first-order real SH (ACN/SN3D, via ambi_encode_sn3d so the basis is
 * the engine's own xval-pinned one), divide out the RIGID-SPHERE mode strengths b_n(ka) — the ZM-1 is
 * a scattering sphere, not an open array, and ignoring that biases the direction — then accumulate the
 * active intensity I = sum_f Re{conj(W) . (X,Y,Z)} over the band. For a plane wave in SN3D the
 * first-order components ARE the direction cosines, so I points straight AT the source.
 *
 * BAND. This is the estimator's real limit and it is not negotiable: a 49 mm sphere hits kr ~ 1 at
 * about 1.1 kHz, and above that the first-order inversion stops being trustworthy. Broadband content
 * gets 400-1200 Hz; a tone gets +-1/6 octave around itself. f_hi is CLAMPED to ZYLIA_FOA_FMAX, so a
 * band that lies entirely above it (a 6 kHz tone) collapses and the call REFUSES — which is the point.
 * A confident wrong direction is worse than no direction. Anything needing HF is a different estimator.
 *
 * x        = 19 planar channel pointers, n samples each, sample-locked (the ZM-1's shared ADC gives
 *            this). n must be >= one 2048-sample frame; frames hop by half and are Hann-windowed.
 * fs, c    = sample rate (Hz) and speed of sound (m/s); c sets ka, so it must be the real one.
 * f_lo/f_hi= analysis band (Hz). See the clamp above.
 * dir_out  = unit vector from the array centre TOWARD the source, ROOM axes (the frame zylia_geometry
 *            and zylia_doa use), so it drops straight into the same angular-miss maths.
 * diffuseness_out (optional) = 0 = a clean plane wave, 1 = fully diffuse. This is the "should I
 *            believe this?" number, the counterpart of zylia_survey's resid_us: a measurement made in
 *            the reverberant tail rather than the direct sound shows up here as a high value.
 * Returns 1 on success, 0 on bad arguments, a collapsed band, too few samples, or a singular solve. */
#define ZYLIA_FOA_FMAX 1200.0    /* first-order validity ceiling (kr ~ 1 on a 49 mm sphere) */

int  zylia_intensity_doa(const float* const x[ZYLIA_MICS], uint32_t n, double fs, double c,
                         double f_lo, double f_hi, const unsigned char* exclude,
                         float dir_out[3], float* diffuseness_out);

/* ---- per-session signal integrity: check the CAPSULES before believing any direction ----
 *
 * The failure this exists for is not a quiet one, it is a LOUD one. A capsule that dies goes to zero
 * and is obvious; a capsule that goes hot — self-noise, a failing preamp — keeps the array's total
 * power looking perfectly healthy while corrupting the spherical-harmonic projection, because every
 * SH channel is a weighted sum over ALL capsules. One bad channel therefore poisons the direction and
 * nothing about the level says so. Worse, it poisons every estimator equally, so agreement between
 * two independent DOA methods does NOT clear it. This has to be caught on the raw signals or not at
 * all. (The AES validation paper hit exactly this: one capsule emitting ~45 dB of broadband
 * self-noise across five consecutive sessions; flagging and excluding it took one measurement cell
 * from 60.5 deg of error back to 5.5.)
 *
 * Checks, all against the array's own ROBUST MEDIAN so a fault cannot define the baseline it is
 * judged by: per-capsule RMS far below (dead) or far above (hot) the median, hard clipping, and
 * coherence against the per-sample median signal over a small lag search (a capsule at the right
 * LEVEL carrying the wrong SIGNAL). Thresholds are deliberately loose — this is a fault detector,
 * not a calibration; it should fire on a broken capsule and never on a merely off-axis one.
 *
 * flags_out[i] = 0 for a healthy capsule, else the OR of the ZYLIA_CAP_* bits below. The array is
 * directly usable as the `exclude` argument to zylia_intensity_doa / zylia_srp_doa, which is the
 * intended flow: check, report what you dropped, then estimate on what is left. Returns the number
 * of FAULTY capsules (0 = all healthy), or -1 on bad arguments.
 *
 * Report every exclusion. A direction computed on 17 capsules is fine; a direction computed on 17
 * capsules that you believed came from 19 is not. */
#define ZYLIA_CAP_DEAD        0x01   /* RMS far below the array median */
#define ZYLIA_CAP_HOT         0x02   /* RMS far above it — the self-noise fault */
#define ZYLIA_CAP_CLIPPED     0x04   /* pinned at full scale */
#define ZYLIA_CAP_INCOHERENT  0x08   /* right level, wrong signal */

int  zylia_check_capsules(const float* const x[ZYLIA_MICS], uint32_t n,
                          unsigned char flags_out[ZYLIA_MICS]);

/* ---- independent cross-check: SH-domain steered-response power, PHAT-whitened ----
 *
 * A second DOA estimator that shares as little as possible with zylia_intensity_doa. Intensity reads
 * a PHASE relationship between two SH degrees at each bin; this steers a beam over a direction grid
 * and takes the most powerful one. Different failure modes, so when they agree the answer is probably
 * real, and when they diverge something is wrong with the CAPTURE — which is what you actually want
 * to know before writing a number into a results table.
 *
 * Read the limits honestly. It is not a free upgrade over the intensity estimator:
 *  - It goes HIGHER. 19 capsules support order 3 (16 channels), good to kr ~ 3 (~3.3 kHz), so this is
 *    the only path here that sees above the first-order ceiling at all. f_hi clamps to ZYLIA_SH3_FMAX.
 *  - It is COARSER. The answer is the best direction on a finite grid, so its resolution is the grid
 *    (~2 deg), against the intensity vector's continuous solve. Use it to CHECK a number, not to be one.
 *  - Order 3 on 19 capsules is a tight fit and the ZM-1 is no spherical design, so the projection is
 *    ridge-regularized. Excluding capsules tightens it further: the order drops automatically to what
 *    the remaining capsules support (3 -> 2 -> 1) rather than returning a confidently over-fitted answer.
 *  - PHAT whitening normalizes every bin to unit norm, so loud bins cannot dominate. That is what makes
 *    it reverberation-robust, and also what makes it ignore your stimulus's spectrum entirely.
 *
 * Arguments match zylia_intensity_doa. Returns 1 on success, 0 on bad arguments, a collapsed band,
 * too few samples, or too few healthy capsules to resolve even first order. */
#define ZYLIA_SH3_FMAX 3300.0    /* third-order validity ceiling (kr ~ 3) */

int  zylia_srp_doa(const float* const x[ZYLIA_MICS], uint32_t n, double fs, double c,
                   double f_lo, double f_hi, const unsigned char* exclude, float dir_out[3]);

/* ---- capsule self-survey: MEASURE the array instead of trusting the table ----
 *
 * The built-in table says where the 19 capsules are, but not which ASIO channel each one feeds, nor how
 * the array is turned in the room. Both survive every off-hardware test (a permutation and a rotation
 * preserve the geometry's every structural property), and both produce a confident, wrong direction.
 * The survey dissolves them by measuring the capsule positions directly — INDEXED BY THE CHANNEL that
 * fed them, expressed in ROOM axes. So the result *is* the channel order and *is* the orientation;
 * there is nothing left to pin by hand.
 *
 * Sound from a KNOWN direction d_k reaches capsule i at   tau[k][i] = t0_k - (1/c) * m_i . d_k,
 * where m_i is the capsule's position relative to the array centre. t0_k is unknown and unknowable —
 * system latency, the moment the clap happened, whichever channel zylia_tdoa chose as its reference —
 * and it does not matter: it is one constant per observation, so subtracting each observation's MEAN
 * over the 19 capsules kills it exactly (that mean IS t0_k, once the gauge sum_i m_i = 0 is imposed,
 * and the least-squares solution satisfies that gauge automatically). What survives is linear in m_i
 * and SEPARABLE: each capsule gets its own 3-unknown solve over the K observations, all sharing one
 * 3x3 normal matrix. No sweep, no sample-sync, no second audio device — K claps from known directions
 * through the capture shell that already exists.
 *
 * The plane wave above is not quite the truth — a clap 2.5 m out is a SPHERE, and its curvature across
 * a 49 mm array is a systematic ~1.4 us, which is 2-3 mm of capsule error if ignored. So we take the
 * source's POSITION, not merely its direction (you know where you clapped — that is how you knew the
 * direction), and iterate the exact-minus-plane-wave correction on top of the linear seed. Cost: a few
 * extra passes. Benefit: the geometry comes back to well under a millimetre.
 *
 * Give it K >= 6 well-SPREAD source positions. Spread is the trap: claps in a horizontal ring around
 * the array are coplanar, the normal matrix goes singular in the vertical, and the capsules' heights
 * are unrecoverable — the solve would return a flattened array rather than admit it. Clap high and low
 * too. `spread_out` measures exactly this (1 = isotropic, 0 = coplanar); below 0.05 it refuses.
 *
 * The array centre enters only as the origin the source positions are measured from, so a tape measure
 * suffices: at 2.5 m a 5 cm centre error tilts a direction by ~1 deg, which lands at the TDOA noise
 * floor. (The recovered array is centred on its own capsule centroid — that IS the array centre, and
 * it is what a subsequent zylia_doa / zylia_localize is relative to.)
 *
 * src_m    = [nobs][3] source positions RELATIVE TO THE ARRAY CENTRE (m, room axes). Must be >= 0.2 m.
 * arrival_s= [nobs][19] per-capsule arrival times, as zylia_tdoa returns them (RELATIVE is fine).
 * caps_out = [19][3] recovered capsule positions (m, array-centred, room axes, ASIO-channel-indexed).
 * resid_us / radius_out / spread_out = optional diagnostics: RMS fit residual in MICROSECONDS (the
 * "should I trust this?" number — exact-model, per-observation constant fitted out), the recovered mean
 * |m_i| (should land near 49 mm), and the spread above. nobs is clamped to ZYLIA_SURVEY_MAX. Returns 1
 * on success, 0 if nobs < 4, a source is inside the array, the directions are degenerate, or the solve
 * is singular. */
int  zylia_survey(const float src_m[][3], const double (*arrival_s)[ZYLIA_MICS], int nobs, double c,
                  float caps_out[ZYLIA_MICS][3], float* resid_us, float* radius_out, float* spread_out);

/* Install a surveyed capsule table (positions in m, array-centred, ASIO-channel-indexed). zylia_doa,
 * zylia_localize and zylia_geometry all follow it from here on. NULL reverts to the built-in table.
 * Control-thread only (these tools are single-threaded); not for the audio thread. */
void zylia_set_capsules(const float caps_m[ZYLIA_MICS][3]);

/* The capsule POSITIONS the solves actually use (m, array-centred): the installed survey if there is
 * one, else radius x the built-in dodecahedral directions. */
void zylia_capsules(float caps_m[ZYLIA_MICS][3]);

/* Persist / restore a survey (JSON). A survey is a property of one physical ZM-1 and its mounting, so
 * it belongs beside the layout, not in it. Save writes the capsules plus the diagnostics that say
 * whether to believe them; load installs the result via zylia_set_capsules. Both return 1 on success,
 * 0 on an I/O or parse failure (err, if given, gets a one-line reason). */
int  zylia_survey_save(const char* path, const float caps_m[ZYLIA_MICS][3],
                       float resid_us, float radius_m, float spread, int nobs, char* err, int errcap);
int  zylia_survey_load(const char* path, char* err, int errcap);

/* Full single-position localization: Gauss-Newton refine the source position against the exact
 * spherical-wavefront model. center = the array centre in room coords (m); latency_s = the known system
 * latency (s); c = speed of sound (m/s). pos_out = source position (m); dist_out (optional) = range
 * from the centre. Returns 1 on success, 0 on a degenerate solve. The recovered DIRECTION is precise;
 * the DISTANCE inherits the latency's accuracy (see the header). */
int  zylia_localize(const double arrival_s[ZYLIA_MICS], const float center[3],
                    double latency_s, double c, float pos_out[3], float* dist_out);

#endif /* BWA_ZYLIA_H */
