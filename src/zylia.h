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
 * wrong one, and no off-hardware test can catch either. Pin them on the rig (bw_zylia_probe / a clap from
 * a known direction), or measure the geometry outright with the capsule self-survey. docs/calibration.md.
 *
 * The DOA/localize solve is unit-tested off-hardware (synthesize the 19 arrivals from a known position,
 * recover it) and the table's dodecahedral structure is pinned separately — see zylia_test.c.
 */
#ifndef BW_ZYLIA_H
#define BW_ZYLIA_H

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

#endif /* BW_ZYLIA_H */
