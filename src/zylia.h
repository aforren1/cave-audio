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
 * The geometry below is a PLACEHOLDER spread; replace zylia_geometry's table with the ZM-1 datasheet /
 * surveyed capsule directions before trusting on-hardware results. The math is geometry-agnostic —
 * only those 19 directions + the radius must match the real array. The DOA/localize solve is
 * unit-tested off-hardware (synthesize the 19 arrivals from a known position, recover it).
 */
#ifndef BW_ZYLIA_H
#define BW_ZYLIA_H

#include <stdint.h>

#define ZYLIA_MICS 19

/* Fill the 19 capsule UNIT directions (array-local frame) and the sphere radius (m). PLACEHOLDER
 * geometry — see the file header; swap in the datasheet coordinates for on-hardware use. */
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

/* Full single-position localization: Gauss-Newton refine the source position against the exact
 * spherical-wavefront model. center = the array centre in room coords (m); latency_s = the known system
 * latency (s); c = speed of sound (m/s). pos_out = source position (m); dist_out (optional) = range
 * from the centre. Returns 1 on success, 0 on a degenerate solve. The recovered DIRECTION is precise;
 * the DISTANCE inherits the latency's accuracy (see the header). */
int  zylia_localize(const double arrival_s[ZYLIA_MICS], const float center[3],
                    double latency_s, double c, float pos_out[3], float* dist_out);

#endif /* BW_ZYLIA_H */
