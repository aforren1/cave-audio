/*
 * calib.h — turn per-speaker acoustic measurements into layout trims, and write them back into
 * cave_layout.json. The measurement DSP is in measure.h; the full-duplex ASIO capture that feeds it
 * is the calibration tool (examples/calibrate.c). This part is pure + file I/O only (no audio thread,
 * no ASIO), so it is unit-tested (test/calib_test.c).
 */
#ifndef BW_CALIB_H
#define BW_CALIB_H

#include "measure.h"
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

/* Read the layout JSON at `in_path`, set each speaker's `gain_db` + `delay_ms` (preserving index,
 * position, the dbap block, everything else), and write to `out_path` (may equal in_path). The file's
 * speaker count must equal `n`. Returns 1 on success, 0 with a message in `err`. */
int calib_write_layout(const char* in_path, const char* out_path,
                       const float* gain_db, const float* delay_ms, int n, char* err, size_t errcap);

#ifdef __cplusplus
}
#endif

#endif /* BW_CALIB_H */
