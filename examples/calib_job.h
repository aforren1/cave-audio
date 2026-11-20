/* calib_job — the calibration ASIO shell's per-block logic, with no SDK type in it.
 *
 * calib_capture.cpp's bufferSwitch is this function plus the sample-format conversion, so the part
 * that decides WHAT plays and WHAT is recorded can run under ctest (calib_capture_block) on every
 * platform. No allocation, no locks, no I/O: it runs on the audio thread. */
#ifndef BWA_CALIB_JOB_H
#define BWA_CALIB_JOB_H

#ifdef __cplusplus
extern "C" {
#endif

/* One capture's play parameters. The control thread fills the slot the previous capture did not use
 * and only then publishes it, so the callback never reads a job that is being written. */
typedef struct CalibJob {
    int          ch, ch2, gap2;      /* the output, and a second one playing the same signal gap2
                                      * samples later, -1 = none */
    const float* play;
    int          play_len, cap_len;  /* samples played, and recorded per input */
} CalibJob;

/* Callback-only state. Zero = idle. */
typedef struct CalibJobState {
    unsigned cur;                    /* the request last seen */
    int      running, play_pos, cap_pos;
} CalibJobState;

/* One block of n frames for request r ((sequence << 1) | slot; 0 = none, which also aborts).
 * Fills out ([nspk][n] planar, every sample written) and says which input samples to keep: `*take`
 * frames from the start of this block, to capture offset `*cap_at` (take 0 = none).
 * Returns 1 in the one block that completes r's capture, else 0. */
int calib_job_block(CalibJobState* st, unsigned r, const CalibJob job[2], int nspk, long n,
                    float* out, int* cap_at, int* take);

#ifdef __cplusplus
}
#endif
#endif
