/*
 * portable.h — the handful of OS calls the console examples need, on either platform.
 *
 * The examples are CLIENT code of the public ABI (include/bw_audio.h): they show what a game or an
 * experiment writes, so they deliberately do not reach into src/. That leaves them one gap a real
 * client also has — "wait a frame" and "what time is it" — and this header is that gap and nothing
 * else. src/os.h is the ENGINE's shim; this is the demo's, and the two never mix.
 */
#ifndef BWA_EXAMPLES_PORTABLE_H
#define BWA_EXAMPLES_PORTABLE_H

#include <stdint.h>

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <conio.h>

static inline void     bwa_sleep_ms(unsigned ms) { Sleep((DWORD)ms); }
static inline int      bwa_key_pressed(void)     { return _kbhit(); }
static inline uint64_t bwa_ticks_ms(void)        { return (uint64_t)GetTickCount64(); }
static inline double   bwa_now_us(void) {
    static double hz = 0.0;
    if (hz == 0.0) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); hz = (double)f.QuadPart; }
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1.0e6 / hz;
}

#else

#include <errno.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

static inline void bwa_sleep_ms(unsigned ms) {
    struct timespec ts;
    ts.tv_sec  = (time_t)(ms / 1000u);
    ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) { }   /* finish the remainder after a signal */
}
static inline uint64_t bwa_ticks_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}
static inline double bwa_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1.0e6 + (double)ts.tv_nsec / 1.0e3;
}
/* The terminal stays in its normal LINE-buffered mode, so this answers true once the user has
 * pressed Enter, not on the first keystroke the way _kbhit does. Good enough for a poll loop that
 * only wants a way out; a tool that needs raw keys would set termios itself. */
static inline int bwa_key_pressed(void) {
    fd_set fds;
    struct timeval tv = { 0, 0 };
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    return select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0;
}

#endif

#endif /* BWA_EXAMPLES_PORTABLE_H */
