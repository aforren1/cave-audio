/*
 * idle_test.c — the two background threads that used to sleep-poll must cost NOTHING when idle.
 *
 * The asset loader polled its job ring every 2 ms and the file-streaming refill ran every 3 ms,
 * whether or not there was anything to do, so an engine sitting at a menu with no assets loading and
 * no stream open still woke both threads continuously. Invisible on a desk, battery on a headset.
 * How MANY wakes that was depends on the platform timer: a Windows desk box at the default 15.6 ms
 * tick measured 34 + 34 per 500 ms, and the same code with the resolution raised (which any open
 * device sink used to do) reaches ~250 + ~170. Both now block on an os_event (src/os.h); the stream
 * thread keeps a bounded timeout only while a ring is actually draining, because its consumer is the
 * AUDIO thread and the audio thread may not signal.
 *
 * What makes this test worth having is that the assertion is a COUNT, not a timing: on the event
 * both counts are 0 and the bound is 5, so there is no arithmetic in which a poll passes. Both
 * sleeps were put back on purpose and the two assertions went red before this was believed green
 * (CLAUDE.md, "a self-checking test that CANNOT FAIL").
 *
 * Links bwa_core ONLY, deliberately. The wakeup counters are file-scope in assets.c and stream.c;
 * a target that linked both bwa_core and the dll would compile a SECOND copy of them into itself
 * and read the copy the engine never touches.
 */
#include "assets.h"
#include "os.h"
#include "rt.h"
#include "stream.h"

#include <stdio.h>
#include <string.h>

#define RATE  48000u
#define VOICES 16u
#define SOUNDS 16u

static int fails = 0;
#define CHECK(c, msg) do { if (c) printf("  ok   %s\n", (msg)); \
                           else { printf("  FAIL %s\n", (msg)); ++fails; } } while (0)

/* A tiny mono wav, written by hand so the fixture does not depend on any decoder. */
static int write_wav(const char* path, uint32_t frames) {
    FILE* f = os_fopen(path, "wb");
    if (!f) return 0;
    const uint32_t data = frames * 2u, riff = 36u + data, rate = RATE, byte_rate = RATE * 2u;
    const uint32_t fmt_size = 16u;
    const uint16_t fmt = 1, ch = 1, align = 2, bits = 16;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); fwrite(&fmt_size, 4, 1, f);
    fwrite(&fmt, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&rate, 4, 1, f);
    fwrite(&byte_rate, 4, 1, f); fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
    for (uint32_t i = 0; i < frames; ++i) { const int16_t v = (int16_t)((i % 64u) * 128u); fwrite(&v, 2, 1, f); }
    fclose(f);
    return 1;
}

int main(void) {
    const char* WAV = "bwa_idle_test.wav";
    char err[256] = { 0 };

    if (!write_wav(WAV, 8000)) { printf("idle_test: cannot write the fixture wav\n"); return 1; }

    /* rt_create starts the streaming thread; assets_create does NOT start the loader thread (it is
     * lazy), so the async acquire below is what brings it into existence. Measuring an idle thread
     * that was never started would prove nothing. */
    RtCore* rt = rt_create(VOICES, SOUNDS, RATE, 26);
    CHECK(rt != NULL, "rt_create");
    if (!rt) { os_remove(WAV); return 1; }
    AssetCache* ac = assets_create(rt, RATE, SOUNDS);
    CHECK(ac != NULL, "assets_create");
    if (!ac) { rt_destroy(rt); os_remove(WAV); return 1; }

    /* ---- (c) latency: the worker must pick a job up at once, not at the next poll ----
     *
     * WHAT THE BOUND PROTECTS, and what it deliberately does not. The assertion with teeth is
     * `ready`: the control thread signals AFTER the ring push, so a lost or mis-ordered wake means
     * the loader never runs again and this never completes. No timing is involved in that, and the
     * 2 s ceiling in the poll below is what turns "never" into a failure.
     *
     * The elapsed MILLISECONDS are reported, not asserted past a gross ceiling, and that is a
     * deliberate retreat rather than an oversight (CLAUDE.md: say so rather than imply coverage).
     * Two attempts failed first. An absolute few-millisecond bound passed alone and tripped once
     * under the full ctest run, because every number in it is a scheduler measurement on a loaded
     * box. Comparing against a 2 ms sleep-poll timed in the same process is no better, because the
     * two sides do not measure the same thing: an acquire includes opening and decoding a file, and
     * on a filesystem where that costs milliseconds (a WSL /mnt/c share, say) the comparison reads
     * the disk rather than the wake. It failed under ctest for exactly that reason. Both numbers
     * are printed instead, which is what a human comparing a before and after actually wants.
     *
     * The FIRST async acquire can never show the difference, and that trap is worth naming: the
     * loader thread is started lazily by that very call, so its first pass finds the job already in
     * the ring and no wait of any kind happens. Only a SECOND acquire, against a parked loader, is
     * measuring the wake at all. */
    {
        /* warm-up: start the loader and let it park on an empty ring */
        const uint32_t warm = assets_acquire_async(ac, WAV, 0, err, sizeof err);
        CHECK(warm != 0, warm ? "async acquire hands back a handle" : err);
        for (int i = 0; i < 3000 && !assets_is_ready(ac, warm, NULL, 0); ++i) os_sleep_ms(1);
        assets_release(ac, warm, NULL, 0);
        os_sleep_ms(20);

        /* what one 2 ms sleep-poll costs on this machine, right now, under whatever load there is */
        double poll_ms = 1e9;
        for (int i = 0; i < 5; ++i) {
            const uint64_t p0 = os_monotonic_ns();
            os_sleep_ms(2);
            const double d = (double)(os_monotonic_ns() - p0) / 1.0e6;
            if (d < poll_ms) poll_ms = d;
        }

        double best_ms = 1e9;
        int ready = 0;
        for (int rep = 0; rep < 5; ++rep) {
            char path[64];
            snprintf(path, sizeof path, "bwa_idle_test_%d.wav", rep);   /* a fresh cache key each time */
            if (!write_wav(path, 8000)) { CHECK(0, "write the per-rep fixture"); break; }
            const uint64_t t0 = os_monotonic_ns();
            const uint32_t snd = assets_acquire_async(ac, path, 0, err, sizeof err);
            ready = 0;
            /* No sleep in this poll: the wall clock is what is being measured, and a 1 ms sleep
             * would quantize the answer to the very cadence this change removed. */
            for (int i = 0; i < 20000000 && !ready; ++i) {
                ready = assets_is_ready(ac, snd, NULL, 0) ? 1 : 0;
                if (!ready && os_monotonic_ns() - t0 > 2000000000ull) break;   /* 2 s hard ceiling */
            }
            const double ms = (double)(os_monotonic_ns() - t0) / 1.0e6;
            if (ready && ms < best_ms) best_ms = ms;
            assets_release(ac, snd, NULL, 0);
            os_remove(path);
            if (!ready) break;
        }
        printf("       async acquire of an 8000-frame wav, best of 5: %.2f ms "
               "(a 2 ms sleep-poll costs %.2f ms here)\n", best_ms, poll_ms);
        CHECK(ready, "the async load completes (a lost wake would never finish)");
        /* Gross on purpose: 500 ms is a hundred times the observed cost and still eight times
         * tighter than the ceiling the assets test uses, so it catches a wake deferred behind
         * something pathological without ever reading the scheduler as a defect. */
        CHECK(best_ms < 500.0, "the async load lands inside half a second");
    }

    /* ---- (b) idle wakeups: nothing loading, nothing streaming, for 500 ms ---- */
    {
        for (int i = 0; i < 50; ++i) { assets_pump(ac); os_sleep_ms(1); }   /* let both threads settle */

        const uint64_t l0 = bwa_assets_loader_wakeups();
        const uint64_t s0 = bwa_stream_thread_wakeups();
        const uint64_t t0 = os_monotonic_ns();
        os_sleep_ms(500);
        const double held_ms = (double)(os_monotonic_ns() - t0) / 1.0e6;
        const uint64_t lw = bwa_assets_loader_wakeups() - l0;
        const uint64_t sw = bwa_stream_thread_wakeups() - s0;

        printf("       idle %.0f ms: loader woke %llu times, stream thread woke %llu\n",
               held_ms, (unsigned long long)lw, (unsigned long long)sw);
        /* Both read 0 on the event. Under 5 leaves room for one wake that was already in flight
         * when the baseline was taken, and no room at all for a poll: putting the sleeps back
         * measured 34 and 34 here, and more on a box whose timer resolution is raised. */
        CHECK(lw < 5, "the asset loader is idle-free (34 wakes per 500 ms on the old 2 ms poll)");
        CHECK(sw < 5, "the streaming thread is idle-free (34 wakes per 500 ms on the old 3 ms poll)");
    }

    /* ---- an OPEN stream still gets served: the timeout is bounded while a ring can drain ---- */
    {
        StreamSet* set = stream_set_create(RATE);
        CHECK(set != NULL, "stream_set_create");
        if (set) {
            err[0] = 0;
            Stream* s = stream_open(set, WAV, err, sizeof err);
            CHECK(s != NULL, s ? "stream_open" : err);
            if (s) {
                const uint64_t s0 = bwa_stream_thread_wakeups();
                stream_start(s, 1);                       /* looping: the ring never runs out of file */
                int pre = 0;
                for (int i = 0; i < 1000 && !pre; ++i) { pre = stream_prebuffered(s); os_sleep_ms(1); }
                CHECK(pre, "an opened stream prebuffers");
                /* Pull like an audio thread would for 200 ms, so the ring keeps draining and the
                 * refill thread has to keep waking. */
                float blk[256];
                uint64_t pos = 0;
                const uint64_t t0 = os_monotonic_ns();
                while (os_monotonic_ns() - t0 < 200000000ull) {
                    pos += stream_pull(s, pos, blk, 256);
                    os_sleep_ms(1);
                }
                const uint64_t sw = bwa_stream_thread_wakeups() - s0;
                printf("       200 ms of pulling: stream thread woke %llu times, %llu samples pulled\n",
                       (unsigned long long)sw, (unsigned long long)pos);
                CHECK(sw > 5, "an active stream still wakes the refill thread");
                CHECK(pos > 0, "the consumer got samples");
                stream_close(set, s);
            }
            stream_set_destroy(set);
        }
    }

    /* ---- a streamed voice through the REAL rt core, rendered at real time ----
     *
     * The counter that must not regress. The refill thread no longer wakes on a flat 3 ms poll, so
     * "does a playing stream still get fed" has to be measured rather than argued: rt counts every
     * block a streamed voice found its ring empty without the asset having ended
     * (bwa_health.stream_starves). Rendering as fast as possible would starve any cadence, so the
     * loop paces on the monotonic clock exactly as a device would. */
    {
        err[0] = 0;
        const uint32_t snd = rt_load_sound_streaming(rt, WAV, err, sizeof err);
        CHECK(snd != 0, snd ? "rt_load_sound_streaming" : err);
        const uint32_t src = rt_source_create(rt);
        CHECK(src != 0, "rt_source_create");
        if (snd && src) {
            const uint32_t BLK = 256;
            static float bus[26 * 256];
            rt_source_set_pos(rt, src, 0.f, 1.5f, -1.f);
            rt_source_play(rt, src, snd, /*loop*/ true);
            rt_commit(rt);

            const uint64_t s0 = rt_stream_starves(rt);
            const uint64_t t0 = os_monotonic_ns();
            const uint64_t per_ns = (uint64_t)BLK * 1000000000ull / RATE;
            uint64_t blocks = 0;
            for (; blocks < 94; ++blocks) {           /* 94 * 256 frames = ~500 ms at 48 kHz */
                os_sleep_until_ns(t0 + (blocks + 1) * per_ns);
                bwa_timestamp ts;
                ts.sample_pos     = blocks * BLK;
                ts.system_time_ns = os_monotonic_ns();
                rt_render(rt, bus, BLK, &ts);
            }
            const uint64_t starves = rt_stream_starves(rt) - s0;
            printf("       %llu blocks of a streamed voice at real time: %llu stream starves\n",
                   (unsigned long long)blocks, (unsigned long long)starves);
            /* The prebuffer is 8192 samples and the ring 65536, so a fed stream starves only on the
             * first blocks before the decoder gets ahead. Anything past a handful means the refill
             * thread is not being woken often enough. */
            CHECK(starves < 8, "a streamed voice rendered at real time barely starves");
            rt_source_stop(rt, src);
            rt_commit(rt);
        }
    }

    assets_destroy(ac);
    rt_destroy(rt);
    os_remove(WAV);
    if (fails) { printf("idle_test: %d FAILURES\n", fails); return 1; }
    printf("idle_test OK (loader + streaming threads block when idle; an active stream is still served)\n");
    return 0;
}
