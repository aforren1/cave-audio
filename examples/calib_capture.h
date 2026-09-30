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
#include "calib/zylia.h"                 /* ZYLIA_MICS, the pressure proxy (the live reading) */
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
/* The live aiming mode's shorter sweep (bwa_calibrate --live N --zylia, calib_view's Aim tab): 0.75 s
 * of capture a reading instead of 2 s. Same 20 Hz to 20 kHz span, so the IR and the tilt bands
 * mean what they mean in the full sweep; a third of the energy (4.8 dB less SNR). The tail has to
 * hold the ZM-1 chain's ~60 ms of latency plus enough room decay for the deconvolution: 0.25 s leaves
 * 190 ms. What is cut off late only thins the IR's late reverberant part at the top of the sweep;
 * the live mode reads nothing past the direct-sound gate. */
#define CAL_LIVE_NSWEEP 24000            /* 0.5 s */
#define CAL_LIVE_NTAIL  12000            /* 0.25 s */
#define CAL_LIVE_CAPLEN (CAL_LIVE_NSWEEP + CAL_LIVE_NTAIL)
/* the simulator's system latency: every simulated arrival is this plus the time of flight */
#define CAL_SIM_LATENCY_SAMPLES 512

/* simulate backend: synthesize what an ideal rig would capture for speaker `ch` (fractional
 * time-of-flight + 1/r + a deterministic +/-~1.4 dB sensitivity wobble, the layout's directivity
 * model when it has one, and the simulated room when calib_sim_set_room turned one on).
 * cap = CAL_CAPLEN floats.
 * `sos` is the speed of sound the time of flight is generated at: pass the SAME value the caller's
 * analyzer will divide back out, or the survey inflates every position by their ratio. Out-of-range
 * falls back to BWA_SOS_REF_MPS (sos.h). */
void calib_sim_capture(int ch, const Layout* L, const float mic[3], double sos, const float* sweep, float* cap);
/* calib_sim_capture with options; NULL = exactly calib_sim_capture. The live aiming mode uses all of
 * them: its shorter sweep and capture, a speaker whose TRUE position and aim differ from the layout's
 * (the installer is moving it), and the speaker's own on-axis response from the model's on_axis_db,
 * so the file's 0 deg tilt is checked against a speaker that has one. The on-axis response is off
 * everywhere else so the other simulated modes keep the flat speaker their pinned numbers came from.
 * `cap` is ncap floats. Not reentrant (static scratch, the room tail cache): one capture at a time. */
typedef struct {
    int          nsweep;     /* 0 = CAL_NSWEEP; the played sweep's length (<= CAL_NSWEEP) */
    int          ncap;       /* 0 = CAL_CAPLEN; samples captured (<= CAL_CAPLEN) */
    const float* true_pos;   /* NULL = the layout's position */
    const float* true_aim;   /* NULL = the layout's aim (rotated by calib_sim_set_aim_error); any length */
    int          on_axis;    /* 1 = apply the model's on-axis response (when it carries one) */
    float        screen_db;  /* > 0 = a screen in front of the speaker: this much loss well above
                              * CALIB_SIM_SCREEN_HZ, a smooth first-order-like shelf, on EVERY path out of
                              * the speaker (direct, images, tail). The same at every aim, so it is
                              * what the live mode's reference absorbs and the file cannot know. */
} CalibSimOpts;
#define CALIB_SIM_SCREEN_HZ 4000.0
void calib_sim_capture_ex(int ch, const Layout* L, const float mic[3], double sos, const CalibSimOpts* o, float* cap);
/* Rotate a unit axis `deg` degrees about the deterministic perpendicular calib_sim_set_aim_error
 * uses (the cross product with +y, or with +x for a near-vertical axis). */
void calib_sim_rotate(const float in[3], float deg, float out[3]);
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

/* Simulate the ZM-1 (bwa_calibrate --zylia --simulate): calib_sim_capture once per capsule, each at
 * its OWN position (center + caps[j]), so every capsule gets its own time of flight, 1/r, directivity
 * bearing and simulated room. Free-field capsules: the rigid sphere's scattering (shadowing above
 * ~2 kHz, the longer diffraction path) is NOT modeled, so a simulated capsule is an omni at its
 * position. cap19 = [ZYLIA_MICS][CAL_CAPLEN] flat. */
void calib_sim_capture_zylia(int ch, const Layout* L, const float center[3], const float (*caps)[3], int ncaps,
                             double sos, const float* sweep, float* cap19);
/* ... with calib_sim_capture_ex's options; row j at cap19 + j * stride (the ASIO shell's stride is
 * CAL_CAPLEN whatever the capture length, so the live mode passes that). */
void calib_sim_capture_zylia_ex(int ch, const Layout* L, const float center[3], const float (*caps)[3], int ncaps,
                                double sos, const CalibSimOpts* o, float* cap19, int stride);

/* The engine's own per-speaker output stage (align.c: gain_db, delay_ms, the `eq` FIR, static
 * room_eq, and room_eq_grid at its flat start; the listener-tracked stages sit at identity) applied to
 * ONE channel: `in` (nin samples) goes in on output channel `ch` with every other channel silent, the
 * stage runs in blocks exactly as align_create(L->count, L, CAL_FS) + align_process build it, and
 * channel ch's output lands in `out` (nout samples; `in` is zero-padded past nin). A fresh aligner per
 * call, so no state carries between speakers. Control thread. Returns 1, 0 on allocation failure. */
int  calib_stage_signal(const Layout* L, int ch, const float* in, int nin, float* out, int nout);
/* calib_stage_signal in place on `nrows` rows of `len` samples each (rows[j * len]), for the
 * simulated verify pass, where the stage runs on the RAW capture instead of the sweep (the stage is
 * linear and time-invariant, so the order does not matter). When the stage's impulse response is a
 * single tap, a plain gain and whole-sample delay, each row is shifted and scaled by it directly,
 * which is exact. `tmp` is `len` floats of scratch. Returns 1, 0 on failure. */
int  calib_stage_rows(const Layout* L, int ch, float* rows, int nrows, int len, float* tmp);
/* Samples the stage can add after its input ends: the longest delay plus the FIR plus 50 ms of
 * biquad ring. --verify pads the played sweep by this much. */
int  calib_stage_pad(const Layout* L);

/* measure_response over `nrows` rows (rows[j * stride], ncap samples each) against `sweep`, on a few
 * worker threads: it is pure and reentrant, and each call holds up to ~10 MB of FFT scratch, hence
 * the cap of 8. ok[j] = its return. The ZM-1's 19 capsules are the case this exists for. */
void calib_measure_rows(const float* rows, int nrows, int stride, int ncap, const float* sweep, int nsweep,
                        const double band_hz[2], MeasureResult* res, int* ok);

/* calib_measure_rows for the ZM-1's 19 capsule rows, then every capsule's arrival refined by
 * cross-correlating the capsules' impulse responses (zylia_ir_tdoa) and written back into
 * res[j].delay_samples / delay_frac, so every consumer of those fields (the pressure proxy's center
 * arrival, zylia_doa, zylia_localize, the live position) reads the refined value. When the refine
 * cannot run (a dead capsule, no interior peak) the |IR|-peak arrivals stand. Returns 1 when the
 * refine ran, 0 when it fell back. */
int  calib_measure_zylia_rows(const float* rows, int stride, int ncap, const float* sweep, int nsweep,
                              const double band_hz[2], MeasureResult res[ZYLIA_MICS], int ok[ZYLIA_MICS]);

/* One live aiming reading (bwa_calibrate --live N --zylia, calib_view's Aim tab): sweep speaker `spk`
 * with the live sweep (`lsweep`, CAL_LIVE_NSWEEP samples), capture the 19 capsules into cap19
 * ([19][CAL_CAPLEN], the ASIO shell's rows), deconvolve each over the tilt bands (CALIB_AIM_*), and
 * pool them (zylia_pressure_proxy). simulate != 0 synthesizes the capture with `sim` (its nsweep and
 * ncap are forced to the live ones; the caller sets the truth); otherwise the ASIO shell must be open
 * with 19 inputs. Returns 0 on a capture timeout; 1 otherwise, with out->ok = 0 and out->dead set
 * when a capsule is dead (the reading is skipped, not the run). Control thread / one worker. */
typedef struct {
    int    ok;
    int    dead;                   /* -1, or the dead capsule's index */
    double arr[ZYLIA_MICS];        /* per-capsule arrivals (s), for zylia_live_position */
    MeasureResult pooled;          /* the pressure proxy over the tilt bands */
    int    have_tilt;
    float  tilt_db;                /* calib_direct_tilt_db of the pool */
} CalibLiveReading;
int calib_live_read(int spk, const Layout* L, const float center[3], double sos, int simulate,
                    const CalibSimOpts* sim, const float* lsweep, float* cap19, CalibLiveReading* out);

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
/* The same capture, playing `sig` (nsig samples, 1..CAL_CAPLEN) on channel `ch` instead of the
 * sweep given at open, for this capture only. --verify plays the sweep run through the output stage
 * (calib_stage_signal), which is longer than the sweep by the stage's delay and FIR. The recording
 * is still CAL_CAPLEN samples, so keep nsig well under it: the tail has to hold the latency and the
 * room decay. Same ~10 s watchdog. */
int  calib_asio_capture_signal(int ch, const float* sig, int nsig);
/* ... recording only `ncap` samples of every input (1..CAL_CAPLEN; rows keep the CAL_CAPLEN stride),
 * with 1 <= nsig <= ncap. The live aiming mode's shorter sweep: CAL_LIVE_NSWEEP into CAL_LIVE_CAPLEN.
 * Unverified on hardware like the rest of the shell. */
int  calib_asio_capture_len(int ch, const float* sig, int nsig, int ncap);
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
