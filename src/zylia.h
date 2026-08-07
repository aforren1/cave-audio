/*
 * zylia.h — single-position speaker localization with a Zylia ZM-1 (19-mic spherical array).
 *
 * The omni-mic survey (calib_trilaterate) recovers speaker positions by moving ONE mic to >=5 spots
 * and trilaterating. The ZM-1 has 19 capsules on a rigid sphere, so ONE placement already sees each
 * sweep arrive at 19 slightly-different times — the arrival-time DIFFERENCES across the sphere give the
 * direction-of-arrival (DOA) directly, and (with the known system latency) the distance. So a speaker's
 * position falls out of a SINGLE Zylia placement: direction x distance + the array center.
 *
 * Accuracy characteristics (be honest about them):
 *   - DIRECTION is latency-independent (uses arrival DIFFERENCES) and reaches ~1 deg given sub-sample
 *     arrival times (measure.c's sub-sample IR peak delivers that). This is the headline: a quick
 *     "where is every speaker, from one spot" that the multi-position omni survey can't do in one shot.
 *   - DISTANCE is only as good as the supplied system latency. The array is ~5 cm, so at meters the
 *     wavefront curvature across it is sub-millimeter of differential delay — far too weak to
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
 * dir_out = unit vector from the array center TOWARD the source. Latency-independent. Returns 1 on
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
 * dir_out  = unit vector from the array center TOWARD the source, ROOM axes (the frame zylia_geometry
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

/* ---- how much does the array COMB? the timbral cost of spreading a source over many speakers ----
 *
 * A phantom is N loudspeakers radiating COHERENT copies of ONE signal. At any point in the room those
 * copies arrive at N different times, so what a microphone sees is the source through a comb filter:
 * peaks where the copies add, notches where they cancel. That is the price amplitude panning pays for
 * a phantom, it is invisible to every direction estimator above (a comb barely moves the intensity
 * vector), and it is exactly what SPCAP's `focus` knob trades. High focus puts a source on few
 * speakers and combs little; low focus spreads it over many and combs hard. So this is the number that
 * turns "focus sounds tighter" into a measurement.
 *
 * THE DISCRIMINATING AXIS IS FREQUENCY, NOT THE CAPSULE AXIS, and getting that backwards is the one
 * design mistake worth naming here. The ZM-1's shell is 49 mm, so the widest capsule pair sits 98 mm
 * apart, while a 400-1200 Hz analysis band spans wavelengths from 0.86 m down to 0.29 m: every pair
 * lies within 0.11 to 0.34 of a wavelength, so all 19 capsules see very nearly the SAME comb. They
 * average its noise down; they do NOT sample it independently. A statistic taken ACROSS capsules would
 * therefore be measuring nothing. The statistic is the ROUGHNESS ALONG FREQUENCY of one capsule's
 * spectrum, and the 19 capsules only average that.
 *
 * Method: Welch power spectrum per capsule (ZYLIA_COMB_NFFT-point Hann frames, half-overlapped), then
 * band energies over the analysis band in overlapping sub-bands one eighth of the band wide, in dB,
 * with a straight line in (log f, dB) fitted out. What is left is the ripple. `depth_db` is its
 * INTERDECILE range (90th minus 10th percentile), averaged over the included capsules — a peak-to-notch
 * figure that one catastrophic null cannot run away with. 0 dB is a flat response, which is what a
 * single coherent arrival gives.
 *
 * READ IT AS AN EXCESS, NEVER AS AN ABSOLUTE. Three other things put ripple in a spectrum: the room,
 * the stimulus's own line structure, and the analysis itself (a sub-band holds a whole number of
 * stimulus tones, and that count jitters band to band). All three are present when ONE speaker is
 * driven alone, which is exactly what valid.c's physical reference arm already does for the angular
 * miss. Measure the reference the same way and subtract; the difference is what panning cost. An
 * absolute comb depth says as little on its own as an absolute angular miss, and for the same reason.
 *
 * LIMITS, all of them structural:
 *  - Ripple SLOWER than the analysis band is indistinguishable from a spectral tilt and the detrend
 *    removes it. Copies within about 1 ms of each other put their first notch above the band and read
 *    as flat. That is the time-aligned case, and it is the right answer at the point the alignment was
 *    computed for and the wrong one everywhere else.
 *  - Ripple FASTER than a sub-band is averaged away. On a 400-1200 Hz band the sub-band is 100 Hz, so
 *    notch spacings under about 200 Hz (copies more than about 5 ms apart) are understated.
 *  - A band too narrow to hold enough sub-bands is REFUSED, which is why a single-tone stimulus gets no
 *    comb number. That is correct rather than a gap: one frequency cannot show a frequency-dependent
 *    effect, and a comb evaluated at one point is just a gain.
 *  - There is no first-order ceiling here. Nothing inverts b_n(ka) or projects onto spherical
 *    harmonics, so ZYLIA_FOA_FMAX does not apply and the band is yours. Match it to the DOA band
 *    anyway when the two numbers are to be read side by side.
 *  - n must be >= ZYLIA_COMB_NFFT. The frame is four times the DOA's because here the frequency
 *    resolution IS the measurement, where the DOA only integrates a vector over bins.
 *
 * x / n / fs / exclude match zylia_intensity_doa. There is no `c`: nothing here depends on the speed
 * of sound. `exclude` is zylia_check_capsules' flags again, and a flagged capsule is skipped outright.
 * depth_db_out (optional) = the number above, in dB.
 * quality_out (optional) = 0..1, the counterpart of zylia_intensity_doa's diffuseness and read the
 *   other way round: 1 means the capsules AGREE about the depth, which the geometry above says they
 *   must. It falls with the spread across capsules, which is the only honest use of that axis. Below
 *   about 0.5 they are not seeing one comb: suspect a capsule fault (run zylia_check_capsules first),
 *   a band far above the shell's coherence, or a capture that drifted.
 * Returns 1 on success, 0 on bad arguments, too few samples, a band too narrow, too few healthy
 * capsules, or a silent capture. */
#define ZYLIA_COMB_NFFT 8192     /* analysis frame: 5.9 Hz bins at 48 kHz, and the minimum n */

int  zylia_comb_depth(const float* const x[ZYLIA_MICS], uint32_t n, double fs,
                      double f_lo, double f_hi, const unsigned char* exclude,
                      float* depth_db_out, float* quality_out);

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
 * where m_i is the capsule's position relative to the array center. t0_k is unknown and unknowable —
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
 * extra passes. Benefit: the geometry comes back to well under a millimeter.
 *
 * Give it K >= 6 well-SPREAD source positions. Spread is the trap: claps in a horizontal ring around
 * the array are coplanar, the normal matrix goes singular in the vertical, and the capsules' heights
 * are unrecoverable — the solve would return a flattened array rather than admit it. Clap high and low
 * too. `spread_out` measures exactly this (1 = isotropic, 0 = coplanar); below 0.05 it refuses.
 *
 * The array center enters only as the origin the source positions are measured from, so a tape measure
 * suffices: at 2.5 m a 5 cm center error tilts a direction by ~1 deg, which lands at the TDOA noise
 * floor. (The recovered array is centered on its own capsule centroid — that IS the array center, and
 * it is what a subsequent zylia_doa / zylia_localize is relative to.)
 *
 * src_m    = [nobs][3] source positions RELATIVE TO THE ARRAY CENTER (m, room axes). Must be >= 0.2 m.
 * arrival_s= [nobs][19] per-capsule arrival times, as zylia_tdoa returns them (RELATIVE is fine).
 * caps_out = [19][3] recovered capsule positions (m, array-centered, room axes, ASIO-channel-indexed).
 * resid_us / radius_out / spread_out = optional diagnostics: RMS fit residual in MICROSECONDS (the
 * "should I trust this?" number — exact-model, per-observation constant fitted out), the recovered mean
 * |m_i| (should land near 49 mm), and the spread above. nobs is clamped to ZYLIA_SURVEY_MAX. Returns 1
 * on success, 0 if nobs < 4, a source is inside the array, the directions are degenerate, or the solve
 * is singular. */
int  zylia_survey(const float src_m[][3], const double (*arrival_s)[ZYLIA_MICS], int nobs, double c,
                  float caps_out[ZYLIA_MICS][3], float* resid_us, float* radius_out, float* spread_out);

/* Install a surveyed capsule table (positions in m, array-centered, ASIO-channel-indexed). zylia_doa,
 * zylia_localize and zylia_geometry all follow it from here on. NULL reverts to the built-in table.
 * Control-thread only (these tools are single-threaded); not for the audio thread. */
void zylia_set_capsules(const float caps_m[ZYLIA_MICS][3]);

/* The capsule POSITIONS the solves actually use (m, array-centered): the installed survey if there is
 * one, else radius x the built-in dodecahedral directions. */
void zylia_capsules(float caps_m[ZYLIA_MICS][3]);

/* Persist / restore a survey (JSON). A survey is a property of one physical ZM-1 and its mounting, so
 * it belongs beside the layout, not in it. Save writes the capsules plus the diagnostics that say
 * whether to believe them; load installs the result via zylia_set_capsules. Both return 1 on success,
 * 0 on an I/O or parse failure (err, if given, gets a one-line reason). */
/* ---- tracked mount: survey once, then follow the stand ----
 *
 * A survey pins the array's orientation FOR THAT MOUNTING. Move the microphone, which a validation
 * session does six or seven times, and the channel order survives but the orientation does not: a
 * remount is a fresh unknown yaw, plus pitch and roll if the stand is not level. Fitting that
 * rotation back out of the measurements afterwards is possible but statistical, and it conflates
 * mount error with the thing you were trying to measure.
 *
 * If the mic is bolted to something the motion capture already tracks, you can MEASURE it instead:
 *
 *   survey once      caps_body = R_mount(survey)^T . caps_room     (zylia_capsules_rotate, transpose)
 *   every placement  caps_room = R_mount(now)     . caps_body      (rotate back, then set_capsules)
 *                    center    = mount_pos + R_mount(now) . offset
 *
 * The capsule table comes back array-centered, so directions need no translation; only the array
 * CENTER does, and that is the `offset` below. Note the rotation falls out of the survey plus one
 * pose sample and needs no probing. The offset does need probing, because zylia_survey takes source
 * positions relative to a center it therefore cannot solve for.
 *
 * MECHANICAL REQUIREMENT, and it is not optional: the coupling has to be rigid and stay rigid. No
 * shock mount, and do not loosen the collar after surveying. You are propagating an ORIENTATION
 * through the mount now, so a quarter turn on the thread is 90 degrees of azimuth error and nothing
 * downstream will notice. Mark the collar.
 *
 * This is also the EASY use of the tracker: the mic does not move during a capture, so one static
 * pose per placement is enough. No prediction, no velocity, no clock-domain question, and a wrong
 * reading is visible rather than subtle. */
typedef struct {
    int   body_frame;      /* 1 = `capsules` are in the MOUNT's body frame, not room axes */
    int   have_offset;
    float offset_m[3];     /* mount body origin -> array acoustic center, expressed in BODY axes */
} ZyliaMount;

/* Rotation matrix (ROW-major 3x3, v_room = R . v_body) from a unit quaternion in xyzw order, which
 * is the layout NatNet delivers. A non-unit quaternion is normalized first. */
void zylia_quat_to_matrix(const float q[4], float R[9]);

/* Rotate a capsule table by a row-major 3x3. transpose = 0 applies R (body -> room), transpose = 1
 * applies R^T (room -> body). `in` and `out` may be the same array. */
void zylia_capsules_rotate(const float caps_in[ZYLIA_MICS][3], const float R[9], int transpose,
                           float caps_out[ZYLIA_MICS][3]);

/* `mount` (NULL ok) carries the tracked-mount metadata above. Saving with a NULL mount writes the
 * historical room-axes form; loading a file without those fields reports body_frame = 0, so old
 * surveys keep working unchanged.
 * When body_frame comes back 1 the INSTALLED table is in the mount's axes, and you must rotate it by
 * the live pose and re-install before any solve — otherwise every direction is wrong by whatever the
 * mount happened to be turned to at survey time. Loading a body-frame file with a NULL mount_out is
 * refused outright, because that caller has no way to find out it needs to. */
int  zylia_survey_save(const char* path, const float caps_m[ZYLIA_MICS][3],
                       float resid_us, float radius_m, float spread, int nobs,
                       const ZyliaMount* mount, char* err, int errcap);
int  zylia_survey_load(const char* path, ZyliaMount* mount_out, char* err, int errcap);

/* Full single-position localization: Gauss-Newton refine the source position against the exact
 * spherical-wavefront model. center = the array center in room coords (m); latency_s = the known system
 * latency (s); c = speed of sound (m/s). pos_out = source position (m); dist_out (optional) = range
 * from the center. Returns 1 on success, 0 on a degenerate solve. The recovered DIRECTION is precise;
 * the DISTANCE inherits the latency's accuracy (see the header). */
int  zylia_localize(const double arrival_s[ZYLIA_MICS], const float center[3],
                    double latency_s, double c, float pos_out[3], float* dist_out);

#endif /* BWA_ZYLIA_H */
