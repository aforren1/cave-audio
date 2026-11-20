/* calib_job_block: the calibration ASIO shell's per-block logic (examples/calib_job.c), driven the
 * way bufferSwitch drives it. One timeline per scenario and block size: every output sample is
 * compared with an expectation built from the request times alone, and the "input" is a ramp of the
 * global sample index, so a capture offset by one sample cannot match. */
#include "calib_job.h"
#include <stdio.h>
#include <string.h>

#define NSPK   4
#define T_MAX  40000
#define NMAX   1024
#define CAPMAX 4000
#define NSEQ   6
#define JUNK   7.f                 /* prefilled into the block: an unwritten output sample shows */

static int   g_fail;
static float g_got[NSPK][T_MAX], g_exp[NSPK][T_MAX];
static float g_blk[(NSPK + 1) * NMAX];        /* one row past nspk: a canary, never written */
static float g_sig_a[3 * NMAX], g_sig_b[300];

#define CHECK(c, ...) do { if (!(c)) { ++g_fail; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

typedef struct {
    CalibJobState st;
    CalibJob      job[2];
    long          n, t;                          /* block size, samples run so far */
    int           fin[NSEQ], taken[NSEQ];        /* per sequence: finish reports, input samples kept */
    float         cap[NSEQ][CAPMAX];
} Sim;
static Sim S;

static float in_at(long t) { return (float)(t + 1); }      /* exact in a float below 2^24 */

static void sim_reset(long n) {
    memset(&S, 0, sizeof S);
    memset(g_got, 0, sizeof g_got);
    memset(g_exp, 0, sizeof g_exp);
    S.n = n;
}

/* what the control thread does: fill the slot r names, before r is ever passed */
static unsigned publish(unsigned seq, int ch, int ch2, int gap, const float* sig, int nsig, int ncap) {
    const unsigned slot = seq & 1u, r = (seq << 1) | slot;
    CalibJob* J = &S.job[slot];
    J->ch = ch; J->ch2 = ch2; J->gap2 = gap; J->play = sig; J->play_len = nsig; J->cap_len = ncap;
    return r;
}

static void step(unsigned r, int blocks) {
    const unsigned q = r >> 1;
    for (int b = 0; b < blocks; ++b) {
        int cap_at = -1, take = -1;
        for (long i = 0; i < (NSPK + 1) * S.n; ++i) g_blk[i] = JUNK;
        const int fin = calib_job_block(&S.st, r, S.job, NSPK, S.n, g_blk, &cap_at, &take);
        for (long i = NSPK * S.n; i < (NSPK + 1) * S.n; ++i)
            if (g_blk[i] != JUNK) { CHECK(0, "n=%ld t=%ld: wrote past output %d", S.n, S.t, NSPK - 1); break; }
        if (S.t + S.n > T_MAX) { CHECK(0, "timeline overflow"); return; }
        for (int c = 0; c < NSPK; ++c)
            memcpy(&g_got[c][S.t], g_blk + (size_t)c * (size_t)S.n, (size_t)S.n * sizeof(float));
        CHECK(take >= 0 && take <= S.n, "n=%ld t=%ld: take %d", S.n, S.t, take);
        if (r == 0) CHECK(take == 0 && !fin, "n=%ld t=%ld: idle block took %d, fin %d", S.n, S.t, take, fin);
        if (take > 0) {
            const int cl = S.job[r & 1u].cap_len;
            CHECK(cap_at == S.taken[q], "n=%ld seq %u: cap_at %d, expected %d", S.n, q, cap_at, S.taken[q]);
            CHECK(cap_at + take <= cl, "n=%ld seq %u: capture runs past cap_len", S.n, q);
            if (cap_at >= 0 && cap_at + take <= CAPMAX)
                for (int i = 0; i < take; ++i) S.cap[q][cap_at + i] = in_at(S.t + i);
            S.taken[q] += take;
        }
        if (fin) ++S.fin[q];
        S.t += S.n;
    }
}

/* run r until it reports finished; returns the blocks that took */
static int run_to_fin(unsigned r) {
    const unsigned q = r >> 1;
    int b = 0;
    while (!S.fin[q] && b < 200) { step(r, 1); ++b; }
    CHECK(S.fin[q] == 1, "n=%ld seq %u: never finished", S.n, q);
    return b;
}

/* sig on channel c from t0, cut at tend (an abort, or the block that finished the capture) */
static void expect_play(int c, long t0, long tend, const float* sig, int nsig) {
    for (int k = 0; k < nsig && t0 + k < tend; ++k) g_exp[c][t0 + k] = sig[k];
}

static void check_out(const char* what) {
    for (int c = 0; c < NSPK; ++c)
        for (long t = 0; t < S.t; ++t)
            if (g_got[c][t] != g_exp[c][t]) {
                CHECK(0, "%s n=%ld: output %d sample %ld is %g, expected %g", what, S.n, c, t,
                      (double)g_got[c][t], (double)g_exp[c][t]);
                break;
            }
}

static void check_cap(const char* what, unsigned r, long t0, int ncap) {
    const unsigned q = r >> 1;
    CHECK(S.fin[q] == 1, "%s n=%ld seq %u: finished %d times", what, S.n, q, S.fin[q]);
    CHECK(S.taken[q] == ncap, "%s n=%ld seq %u: took %d of %d", what, S.n, q, S.taken[q], ncap);
    for (int i = 0; i < ncap; ++i)
        if (S.cap[q][i] != in_at(t0 + i)) {
            CHECK(0, "%s n=%ld seq %u: capture sample %d is input %g, expected %g", what, S.n, q, i,
                  (double)S.cap[q][i], (double)in_at(t0 + i));
            break;
        }
}

/* (a) two captures back to back, the second in the very next block; then the driver keeps calling
 * with the finished request, which is what req holds between captures */
static void back_to_back(long n) {
    sim_reset(n);
    step(0, 2);
    const long t1 = S.t;
    const unsigned r1 = publish(1, 0, -1, 0, g_sig_a, 700, 1500);
    run_to_fin(r1);
    expect_play(0, t1, S.t, g_sig_a, 700);
    const long t2 = S.t;
    const unsigned r2 = publish(2, 2, -1, 0, g_sig_b, 300, 1100);
    run_to_fin(r2);
    expect_play(2, t2, S.t, g_sig_b, 300);
    step(r2, 3);                                   /* stale: no output, no input, no second finish */
    check_out("back to back");
    check_cap("back to back", r1, t1, 1500);
    check_cap("back to back", r2, t2, 1100);
}

/* (b) a pair: the second output plays the same signal `gap` samples later */
static void pair_gap(long n, int gap) {
    sim_reset(n);
    step(0, 1);
    const long t1 = S.t;
    const unsigned r = publish(1, 1, 3, gap, g_sig_a, 700, 2000);
    run_to_fin(r);
    expect_play(1, t1, S.t, g_sig_a, 700);
    expect_play(3, t1 + gap, S.t, g_sig_a, 700);
    step(r, 2);
    check_out("pair");
    check_cap("pair", r, t1, 2000);
}

/* (c) an abort partway through the signal, then a new request. zero_blocks = 0 is the watchdog's 0
 * that the callback never saw: the new request arrives straight over the old one. */
static void abort_then_new(long n, int zero_blocks) {
    sim_reset(n);
    const long t1 = S.t;
    const unsigned r1 = publish(1, 0, 1, 50, g_sig_a, 700, 3000);
    step(r1, 2);                                   /* 128..2000 samples of a 3000-sample capture */
    expect_play(0, t1, S.t, g_sig_a, 700);
    expect_play(1, t1 + 50, S.t, g_sig_a, 700);
    step(0, zero_blocks);
    const long t2 = S.t;
    const unsigned r2 = publish(2, 3, -1, 0, g_sig_b, 300, 900);
    run_to_fin(r2);
    expect_play(3, t2, S.t, g_sig_b, 300);
    step(r2, 2);
    check_out("abort");
    CHECK(S.fin[1] == 0, "abort n=%ld: the aborted request reported finished", n);
    CHECK(S.taken[1] == 2 * n, "abort n=%ld: aborted request took %d", n, S.taken[1]);
    check_cap("abort", r2, t2, 900);
}

/* (d) one output, a capture much longer than the signal: silence after it, input still recorded */
static void long_tail(long n) {
    sim_reset(n);
    step(0, 3);
    const long t1 = S.t;
    const unsigned r = publish(1, 3, -1, 0, g_sig_b, 300, 3777);
    const int blocks = run_to_fin(r);
    CHECK(blocks == (int)((3777 + n - 1) / n), "long tail n=%ld: finished after %d blocks", n, blocks);
    expect_play(3, t1, S.t, g_sig_b, 300);
    step(r, 2);
    check_out("long tail");
    check_cap("long tail", r, t1, 3777);
}

/* (e) a capture of exactly k blocks, the signal as long as the capture: done in block k, not k + 1 */
static void exact_blocks(long n) {
    const int k = 3, len = (int)(k * n);
    sim_reset(n);
    step(0, 1);
    const long t1 = S.t;
    const unsigned r = publish(1, 2, -1, 0, g_sig_a, len, len);
    const int blocks = run_to_fin(r);
    CHECK(blocks == k, "exact n=%ld: finished after %d blocks, expected %d", n, blocks, k);
    expect_play(2, t1, S.t, g_sig_a, len);
    step(r, 2);
    check_out("exact");
    check_cap("exact", r, t1, len);
}

int main(void) {
    static const long sizes[] = { 64, 256, 1000, 1024 };
    for (int k = 0; k < 3 * NMAX; ++k) g_sig_a[k] = 0.25f + 1e-3f * (float)k;      /* never 0, never JUNK */
    for (int k = 0; k < 300; ++k) g_sig_b[k] = -0.5f - 1e-3f * (float)k;
    for (int i = 0; i < 4; ++i) {
        back_to_back(sizes[i]);
        pair_gap(sizes[i], 1001);          /* no multiple of any block size here */
        pair_gap(sizes[i], 1300);          /* gap + nsig == ncap: the last sample played is the last captured */
        abort_then_new(sizes[i], 2);
        abort_then_new(sizes[i], 0);
        pair_gap(sizes[i], (int)sizes[i] - 1);   /* the second output starts on a block's last sample */
        long_tail(sizes[i]);
        exact_blocks(sizes[i]);
    }
    if (g_fail) { printf("calib_capture_block: %d FAILED\n", g_fail); return 1; }
    printf("calib_capture_block: all passed\n");
    return 0;
}
