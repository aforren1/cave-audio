/*
 * sink_tsbase.h — which clock a driver-supplied host stamp is on, and the block stamp a sink
 * builds from it.
 *
 * WHY THIS EXISTS. The ASIO SDK says ASIOTime.systemTime "on windows, must be derived from
 * timeGetTime()" (asio.h, AsioTimeInfo), in nanoseconds, and the SDK's own sample driver does
 * exactly that. timeGetTime is milliseconds since boot, ticking at the system timer (1 ms at best,
 * 15.6 ms at Windows' default), and it is NOT the QPC base the engine's host clock is on
 * (os_monotonic_ns, bwa_host_time_ns): on the development box the two read 27 ms apart. Some
 * drivers stamp from QPC instead. Nothing in the API says which, so the stamps have to.
 *
 * THE CLASSIFIER. Each block compares the stamp against both clocks read in the callback, keeps
 * the windowed MINIMUM of each difference (the dispatch delay's floor: a callback can be late, it
 * cannot be early), and names the base whose floor sits in [-0.5, +5] ms:
 *   SINK_TS_HOST:    QPC. The stamp IS the host clock, exact and free of dispatch jitter.
 *   SINK_TS_TGT:     timeGetTime.
 *   SINK_TS_UNKNOWN: neither, or too few blocks seen yet.
 *   SINK_TS_ABSENT:  the driver gave no stamp.
 * Two bases that happen to sit within 5 ms of each other classify as HOST; the error is then that
 * offset, under 5 ms. The window is two buckets of `per` blocks each, so drift between the clocks
 * cannot pile up in the minimum.
 *
 * Shared by the engine's ASIO sink (asio_sink.cpp) and the ZM-1 capture shell
 * (examples/zylia_capture.cpp): one classifier, so the two cannot disagree about a driver.
 *
 * AUDIO THREAD. Header-only, pure: no allocation, no locks, no clock reads of its own (the caller
 * reads the clocks and passes them in, which is also what makes it testable without a device). No
 * ASIO or Windows types, and C and C++ includable, like sink_convert.h.
 */
#ifndef BWA_SINK_TSBASE_H
#define BWA_SINK_TSBASE_H

#include <stdbool.h>
#include <stdint.h>

enum { SINK_TS_ABSENT = 0, SINK_TS_HOST = 1, SINK_TS_TGT = 2, SINK_TS_UNKNOWN = 3 };
#define SINK_TS_MIN_BLOCKS 16                 /* blocks seen before a base is named */
#define SINK_TS_LO_NS   (-500000LL)           /* the floor of (clock - stamp) for a match: -0.5 ms ... */
#define SINK_TS_HI_NS   (5000000LL)           /* ... to +5 ms */
#define SINK_TS_TICK_NS (16000000LL)          /* timeGetTime's coarsest tick, which can put it BEHIND the stamp */

/* the windowed minimum of (clock - stamp) for both candidate clocks: two buckets of `per` blocks each */
typedef struct {
    int64_t q_cur, q_prev, t_cur, t_prev;
    int     n, per, seen;
} SinkTsClass;

static inline void sink_ts_init(SinkTsClass* c, int blocks_per_bucket) {
    c->q_cur = c->q_prev = c->t_cur = c->t_prev = INT64_MAX;
    c->n = 0; c->seen = 0;
    c->per = blocks_per_bucket > 0 ? blocks_per_bucket : 1;
}
/* One block: host_ns = the host clock (os_monotonic_ns) at callback entry, tgt_ns = timeGetTime() * 1e6
 * read beside it, sys_ns = the driver's systemTime. Returns the base the stamps say (SINK_TS_*). */
static inline int sink_ts_note(SinkTsClass* c, int64_t host_ns, int64_t tgt_ns, int64_t sys_ns) {
    const int64_t dq = host_ns - sys_ns, dt = tgt_ns - sys_ns;
    if (dq < c->q_cur) c->q_cur = dq;
    if (dt < c->t_cur) c->t_cur = dt;
    if (++c->n >= c->per) {                 /* roll the window: drift between the clocks cannot pile up */
        c->q_prev = c->q_cur; c->t_prev = c->t_cur;
        c->q_cur = c->t_cur = INT64_MAX;
        c->n = 0;
    }
    if (c->seen < SINK_TS_MIN_BLOCKS) ++c->seen;
    if (c->seen < SINK_TS_MIN_BLOCKS) return SINK_TS_UNKNOWN;
    const int64_t mq = c->q_cur < c->q_prev ? c->q_cur : c->q_prev;
    const int64_t mt = c->t_cur < c->t_prev ? c->t_cur : c->t_prev;
    if (mq >= SINK_TS_LO_NS && mq <= SINK_TS_HI_NS) return SINK_TS_HOST;
    if (mt >= SINK_TS_LO_NS - SINK_TS_TICK_NS && mt <= SINK_TS_HI_NS) return SINK_TS_TGT;
    return SINK_TS_UNKNOWN;
}
/* The windowed floor of (host clock - stamp), ns (INT64_MAX = none yet). Under SINK_TS_HOST it is the
 * callback's dispatch delay; under any other base it is that base's offset from QPC plus the delay. */
static inline int64_t sink_ts_lag_ns(const SinkTsClass* c) { return c->q_cur < c->q_prev ? c->q_cur : c->q_prev; }

/* THE BLOCK STAMP (a sink's bwa_timestamp.system_time_ns), from the classifier and the host clock.
 *
 * The stamp means what the driver's own stamp means: the host time of the buffer switch, the moment
 * the block's sample position was current. Per block:
 *
 *   1. The driver's stamp, ONLY when the classifier says it is on QPC (SINK_TS_HOST). Exact.
 *   2. Otherwise the host clock read at callback entry, LESS the dispatch floor last measured while
 *      the floor sat in the HOST window (`lag_ns`; 0 when it never has). Entry is late by the
 *      dispatch delay, and that floor is the delay's measured lower bound, so both sources refer to
 *      the switch and a change between them moves the stamp only by that block's jitter above the
 *      floor. With no QPC driver stamp ever seen it is the plain entry read.
 *   3. Never backward, never equal: a stamp at or before the previous one becomes the previous one
 *      plus one nominal block (sink_quant's rule, docs/backends.md rule 3). Equal stamps would be the
 *      zero-slope pair the drift fit must not see, and a backward one makes bwa_get_clock run back.
 *
 * A timeGetTime stamp is NOT mapped onto QPC through the measured offset. The offset is a windowed
 * minimum over a stamp quantized to timeGetTime's tick (1 to 15.6 ms), so it is only known to that
 * tick, and every mapped stamp would carry the same quantization: a 1 ms sawtooth, against the
 * entry read's tens of microseconds of dispatch jitter. The tick can also change under a running
 * stream (any process may call timeBeginPeriod). The entry read is the better stamp. */
typedef struct {
    SinkTsClass cls;
    int64_t     lag_ns;       /* held dispatch floor for rule 2; 0 = never in the HOST window   */
    uint64_t    block_ns;     /* one nominal block, for rule 3                                   */
    uint64_t    last_ns;      /* the previous stamp returned                                     */
    bool        have_last;
    int         base;         /* the last block's classification (SINK_TS_*), for health         */
    bool        driver;       /* the last stamp was the driver's own (rule 1)                    */
} SinkTsSel;

/* `rate` and `block` give the nominal block period and size the classifier's window at about one
 * second per bucket (two buckets: about 2 s of minimum). Control thread, before the stream starts. */
static inline void sink_ts_sel_init(SinkTsSel* s, uint32_t rate, uint32_t block) {
    const uint32_t b = block ? block : 256u;
    sink_ts_init(&s->cls, (int)(rate / b) + 1);
    s->lag_ns    = 0;
    s->block_ns  = rate ? (uint64_t)b * 1000000000ull / (uint64_t)rate : 0;
    s->last_ns   = 0;
    s->have_last = false;
    s->base      = SINK_TS_ABSENT;
    s->driver    = false;
}

/* One block. host_ns: the host clock at callback entry. tgt_ns: timeGetTime() * 1e6 read beside it.
 * sys_valid / sys_ns: the driver's systemTime and its valid flag. Returns the block's stamp. */
static inline uint64_t sink_ts_select(SinkTsSel* s, uint64_t host_ns, uint64_t tgt_ns,
                                      bool sys_valid, uint64_t sys_ns) {
    int base = SINK_TS_ABSENT;
    if (sys_valid) {
        base = sink_ts_note(&s->cls, (int64_t)host_ns, (int64_t)tgt_ns, (int64_t)sys_ns);
        const int64_t mq = sink_ts_lag_ns(&s->cls);
        if (mq >= SINK_TS_LO_NS && mq <= SINK_TS_HI_NS) s->lag_ns = mq;   /* rule 2's floor */
    }
    uint64_t stamp;
    if (base == SINK_TS_HOST) stamp = sys_ns;
    else                      stamp = (uint64_t)((int64_t)host_ns - s->lag_ns);
    s->driver = base == SINK_TS_HOST;
    s->base   = base;

    if (s->have_last && stamp <= s->last_ns) {          /* rule 3 */
        stamp = s->last_ns + s->block_ns;
        s->driver = false;
    }
    s->last_ns   = stamp;
    s->have_last = true;
    return stamp;
}

#endif /* BWA_SINK_TSBASE_H */
