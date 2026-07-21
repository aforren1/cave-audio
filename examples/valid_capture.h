/*
 * valid_capture.h — the phantom-capture backend for bwa_validate. Full duplex on ONE device: the
 * array's speaker feeds go OUT while the ZM-1's 19 capsules come IN, in a single clock domain.
 *
 * That single-device requirement is the whole reason this is practical. Two separate interfaces
 * would leave the render and the capture free-running against each other; here Dante Via puts the
 * ZM-1 on the same network the Digiface already presents, so one ASIO driver exposes the speaker outputs and
 * the 19 capsule inputs together. Same story as docs/calibration.md's sweep path.
 *
 * WHAT MAKES THIS EASY, AND IT IS WORTH SAYING PLAINLY: the harness stimulus is STEADY-STATE, not a
 * sweep. A swept measurement has to know its round-trip latency to the sample, which is most of the
 * difficulty in calib_capture. Here we play and record concurrently and analyse a window taken well
 * inside the steady state, so device latency, driver buffering and the Digiface's own delay never enter the
 * result at all. VAL_SKIP is simply "long enough that everything has arrived".
 *
 * The DSP this feeds (valid_speaker_feeds -> valid_score, and zylia_intensity_doa under it) lives in
 * valid.c / zylia.c and is unit-tested off-hardware, including a check that these exact feeds and the
 * offline analytic path land on the same direction. THIS file is the rig-bound part and is NOT
 * verified on hardware — it mirrors calib_capture.cpp's host sequence, which carries the same
 * caveat. Compiled only when the ASIO SDK is present (BWA_HAVE_ASIO).
 */
#ifndef BWA_VALID_CAPTURE_H
#define BWA_VALID_CAPTURE_H

#ifdef __cplusplus
extern "C" {
#endif
#include "layout.h"
#include "zylia.h"
#ifdef __cplusplus
}
#endif

#include <stdint.h>

#define VAL_FS       48000.0
#define VAL_SKIP     9600u                    /* 200 ms discarded at the head: covers device latency,
                                               * driver buffering and the room's initial build-up */
#define VAL_ANALYZE  8192u                    /* the window handed to the estimator (4 frames) */
#define VAL_CAPLEN   (VAL_SKIP + VAL_ANALYZE)

#ifdef __cplusplus
extern "C" {
#endif

#ifdef BWA_HAVE_ASIO
/* Open `driver` (NULL = the first device exposing >= nspk outputs and 19 inputs from `mic_in`) and
 * start streaming. `nspk` is the LAYOUT's speaker count (4..BWA_CHANNELS) — never assume 26.
 * Returns 0 on success, 1 on failure (message on stderr). Single instance. */
int  valid_asio_open(const char* driver, int mic_in, int nspk);

/* Play `feeds` ([nspk][VAL_CAPLEN], as valid_speaker_feeds writes them) out the array while
 * recording the 19 capsules, then hand back the LAST VAL_ANALYZE samples of each capsule in
 * `cap19` ([ZYLIA_MICS][VAL_ANALYZE] flat) — the head is dropped per VAL_SKIP above.
 * Blocking, with a watchdog. Returns 1 on success, 0 on timeout. */
int  valid_asio_capture(const float* feeds, float* cap19);

void valid_asio_close(void);
#endif /* BWA_HAVE_ASIO */

#ifdef __cplusplus
}
#endif

#endif /* BWA_VALID_CAPTURE_H */
