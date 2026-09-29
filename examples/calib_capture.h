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

/* simulate backend: synthesize what an ideal rig would capture for speaker `ch` (fractional
 * time-of-flight + 1/r + a deterministic +/-~1.4 dB sensitivity wobble, the layout's directivity
 * model when it has one, and the simulated room when calib_sim_set_room turned one on).
 * cap = CAL_CAPLEN floats.
 * `sos` is the speed of sound the time of flight is generated at: pass the SAME value the caller's
 * analyzer will divide back out, or the survey inflates every position by their ratio. Out-of-range
 * falls back to BWA_SOS_REF_MPS (sos.h). */
void calib_sim_capture(int ch, const Layout* L, const float mic[3], double sos, const float* sweep, float* cap);
/* Simulate only: rotate every speaker's TRUE aim `deg` away from the layout's before synthesizing
 * its capture, so --check-aim has a known error to recover (the self-check of the check). 0 = off. */
void calib_sim_set_aim_error(float deg);

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
