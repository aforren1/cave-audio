/*
 * calib_capture.h — the speaker-sweep capture backends, shared by bw_calibrate (CLI) and
 * bw_calib_view's Capture tab (the calibration-station front-end). Two backends behind one shape:
 * ASIO full-duplex (26 outputs + one mic input, sample-aligned; rig bring-up code, gated on
 * BW_HAVE_ASIO) and simulate (delay/attenuate the sweep per the layout's speaker->mic distances +
 * a deterministic sensitivity wobble, so the whole measure -> solve -> writeback path runs without
 * the rig). The measurement/solve DSP these feed lives in measure.c / calib.c (unit-tested).
 */
#ifndef BW_CALIB_CAPTURE_H
#define BW_CALIB_CAPTURE_H

#ifdef __cplusplus
extern "C" {
#endif
#include "layout.h"
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

/* simulate backend: synthesize what an ideal rig would capture for speaker `ch` (fractional
 * time-of-flight + 1/r + a deterministic +/-~1.4 dB sensitivity wobble). cap = CAL_CAPLEN floats. */
void calib_sim_capture(int ch, const Layout* L, const float mic[3], const float* sweep, float* cap);

/* minimal mono IEEE-float WAV writer (retained per-speaker impulse responses) */
void calib_write_wav_f32(const char* path, const float* x, int n, int fs);

#ifdef BW_HAVE_ASIO
/* ASIO full-duplex: open `driver` (NULL = first with >= 26 outs + the mic input), start streaming.
 * calib_asio_capture(ch) plays the sweep out channel `ch` and records CAL_CAPLEN mic samples into
 * the `cap` given at open (blocking, ~10 s watchdog; returns 0 on timeout). Single instance.
 * NOT verified on hardware here — rig bring-up code (mirrors asio_sink.cpp's host sequence). */
int  calib_asio_open(const char* driver, int mic_in, const float* sweep, float* cap);
int  calib_asio_capture(int ch);
void calib_asio_close(void);
#endif

#endif /* BW_CALIB_CAPTURE_H */
