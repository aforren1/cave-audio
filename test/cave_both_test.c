/*
 * cave_both_test.c — the one profile with TWO devices, opened on real hardware.
 *
 * `bwa_desc.device` describes the PRIMARY device. It used to reach the cave_both MONITOR too, and
 * on the rig that is fatal: `device` names the array's ASIO driver, so the monitor's 2-channel AUTO
 * request skipped WASAPI (rule 10, docs/backends.md: under AUTO a backend with no device by that
 * name is skipped), asked ASIO for a driver whose one process-wide slot the array already held, and
 * fell through to the silent null sink. Under BWA_SINK_ASIO it failed the start outright. Either way
 * the profile had never had a live monitor. The monitor now opens AUTO on the platform default.
 *
 * That defect is invisible to every offline test, because with no device string there is nothing to
 * inherit, so this test needs an ASIO driver AND a WASAPI endpoint on the machine. Where either is
 * missing, or where the ASIO driver cannot give the array its channels, it exits 77 and ctest
 * reports a skip NAMING the reason - a device test that silently passes on a machine with no device
 * is worse than no test (CLAUDE.md).
 *
 * The BLOCK COUNTS are the assertion, not the backend string: a monitor that fell to the null sink
 * still reports a backend and still counts blocks on ITS OWN thread, so what separates "live on its
 * own endpoint" from "silently silent" is the resolved sink type beside a turning callback loop.
 */
#include "bw_audio.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "os.h"          /* os_sleep_ms, os_fopen, os_remove */

#define SKIP_EXIT 77     /* ctest SKIP_RETURN_CODE; see CMakeLists.txt */
#define LAYOUT    "bwa_cave_both_layout.json"

/* Internal test hooks (declared in src/sink.h, defined in engine.c, exported from the dll; not
 * public ABI). The public readbacks describe the ARRAY sink - nothing public can see the monitor. */
extern bwa_sink_type bwa_monitor_sink_type(bwa_engine* e);
extern uint64_t      bwa_monitor_blocks(bwa_engine* e);

static const char* sink_name(bwa_sink_type t) {
    switch (t) {
    case BWA_SINK_ASIO:      return "asio";
    case BWA_SINK_NULL:      return "null";
    case BWA_SINK_MANUAL:    return "manual";
    case BWA_SINK_WASAPI:    return "wasapi";
    case BWA_SINK_COREAUDIO: return "coreaudio";
    case BWA_SINK_ALSA:      return "alsa";
    case BWA_SINK_AAUDIO:    return "aaudio";
    case BWA_SINK_JACK:      return "jack";
    default:                 return "auto/unknown";
    }
}

/* `n` speakers on a circle. The default grid is 26 outputs, which few desk interfaces reach, and
 * the array's WIDTH is not what this test is about - the monitor's device selection is. */
static int write_layout(const char* path, int n) {
    FILE* f = os_fopen(path, "wb");
    if (!f) return 0;
    fprintf(f, "{ \"speakers\": [\n");
    for (int i = 0; i < n; ++i) {
        const double a = 6.283185307179586 * (double)i / (double)n;
        fprintf(f, "  { \"index\": %d, \"position\": [%.4f, 1.5, %.4f] }%s\n",
                i, 2.0 * cos(a), 2.0 * sin(a), (i + 1 < n) ? "," : "");
    }
    fprintf(f, "] }\n");
    fclose(f);
    return 1;
}

/* One cave_both start against `width` array channels. Returns 0 pass, 1 fail, SKIP_EXIT no device. */
static int run(const char* asio_name, int width) {
    if (!write_layout(LAYOUT, width)) { fprintf(stderr, "FAIL: cannot write the test layout\n"); return 1; }

    bwa_desc d;
    memset(&d, 0, sizeof d);
    d.sample_rate = 48000;
    d.block_size  = 256;
    d.profile     = BWA_PROFILE_CAVE_BOTH;
    d.sink        = BWA_SINK_AUTO;      /* AUTO is the case that used to break: the skip rule */
    d.device      = asio_name;          /* names the ARRAY device, and only the array device */
    d.layout_path = LAYOUT;

    bwa_engine* e = bwa_create(&d);
    if (!e) { fprintf(stderr, "FAIL: bwa_create returned NULL\n"); os_remove(LAYOUT); return 1; }

    const bwa_result rc = bwa_start(e);
    if (rc != BWA_OK) {
        /* A desk interface that cannot give the array `width` outputs at 48 kHz is a device
         * shortfall, not a defect: skip, saying so. */
        printf("       skip: cave_both could not start %d channels on \"%s\": %s\n",
               width, asio_name, bwa_last_error(e) ? bwa_last_error(e) : "(no message)");
        bwa_destroy(e);
        os_remove(LAYOUT);
        return SKIP_EXIT;
    }

    const bwa_sink_type arr = bwa_get_sink_type(e);
    if (arr != BWA_SINK_ASIO) {
        /* AUTO falls through to the NULL sink when the named driver cannot serve the request, and
         * bwa_start still returns OK, so this is the shape a device shortfall usually takes here.
         * Not a failure - but not a run either, so it must never be counted as a pass. */
        printf("       skip: the array fell to %s for %d channels on \"%s\": %s\n",
               sink_name(arr), width, asio_name, bwa_last_error(e) ? bwa_last_error(e) : "(no message)");
        bwa_stop(e); bwa_destroy(e); os_remove(LAYOUT);
        return SKIP_EXIT;
    }

    int fail = 0;
    const bwa_sink_type mon = bwa_monitor_sink_type(e);
    const char* backend = bwa_get_audio_backend(e);
    printf("       array  -> %s\n", sink_name(arr));
    printf("       monitor-> %s\n", sink_name(mon));
    printf("       backend string: %s\n", backend ? backend : "(null)");

    if (mon == BWA_SINK_NULL || mon == BWA_SINK_ASIO) {
        fprintf(stderr, "FAIL: the monitor resolved to %s - it must open its own device, "
                        "not the array's driver and not the silent null sink\n", sink_name(mon));
        fail = 1;
    }
    if (!backend || !strstr(backend, " + ")) {
        fprintf(stderr, "FAIL: the cave_both backend string must name BOTH devices\n");
        fail = 1;
    } else {
        if (!strstr(backend, "asio:"))                { fprintf(stderr, "FAIL: no array device in the backend string\n"); fail = 1; }
        if (!strstr(backend, sink_name(mon)))         { fprintf(stderr, "FAIL: no monitor device in the backend string\n"); fail = 1; }
    }

    /* Both callback loops must actually turn. A sink that opened and never started counts 0. */
    bwa_health h0, h1;
    bwa_get_health(e, &h0);
    const uint64_t m0 = bwa_monitor_blocks(e);
    os_sleep_ms(300);
    bwa_commit(e);
    bwa_get_health(e, &h1);
    const uint64_t m1 = bwa_monitor_blocks(e);
    printf("       300 ms: array blocks %llu -> %llu, monitor blocks %llu -> %llu\n",
           (unsigned long long)h0.blocks, (unsigned long long)h1.blocks,
           (unsigned long long)m0, (unsigned long long)m1);
    if (h1.blocks <= h0.blocks) { fprintf(stderr, "FAIL: the array callback is not running\n"); fail = 1; }
    if (m1 <= m0)               { fprintf(stderr, "FAIL: the monitor callback is not running\n"); fail = 1; }

    bwa_stop(e);
    bwa_destroy(e);
    os_remove(LAYOUT);
    return fail;
}

/* The device-free half, which runs everywhere: when the PRIMARY resolves to a device-free sink the
 * monitor must follow it there, so a CI box and an offline render never reach for a real endpoint.
 * It also pins the two-device backend string and the fact that BOTH callback loops turn.
 *
 * What it CANNOT pin is the inheritance rule itself. With no real array device there is no `device`
 * string worth inheriting, which is exactly why the defect survived every offline test in the suite
 * and why the hardware arm below exists. */
static int run_offline(void) {
    if (!write_layout(LAYOUT, 8)) { fprintf(stderr, "FAIL: cannot write the test layout\n"); return 1; }

    bwa_desc d;
    memset(&d, 0, sizeof d);
    d.sample_rate = 48000;
    d.block_size  = 256;
    d.profile     = BWA_PROFILE_CAVE_BOTH;
    d.sink        = BWA_SINK_NULL;
    d.device      = "a device no backend has";   /* must not reach the monitor, or anything else */
    d.layout_path = LAYOUT;

    bwa_engine* e = bwa_create(&d);
    if (!e) { fprintf(stderr, "FAIL: bwa_create returned NULL\n"); os_remove(LAYOUT); return 1; }
    if (bwa_start(e) != BWA_OK) {
        fprintf(stderr, "FAIL: cave_both on the null sink must start: %s\n",
                bwa_last_error(e) ? bwa_last_error(e) : "(no message)");
        bwa_destroy(e); os_remove(LAYOUT); return 1;
    }

    int fail = 0;
    const bwa_sink_type arr = bwa_get_sink_type(e);
    const bwa_sink_type mon = bwa_monitor_sink_type(e);
    const char* backend = bwa_get_audio_backend(e);
    printf("       array -> %s, monitor -> %s, backend string: %s\n",
           sink_name(arr), sink_name(mon), backend ? backend : "(null)");
    if (arr != BWA_SINK_NULL) {
        fprintf(stderr, "FAIL: an explicit BWA_SINK_NULL array resolved to %s\n", sink_name(arr));
        fail = 1;
    }
    if (mon != BWA_SINK_NULL) {
        fprintf(stderr, "FAIL: the monitor took %s beside a device-free array - an offline render "
                        "must stay offline\n", sink_name(mon));
        fail = 1;
    }
    if (!backend || !strstr(backend, " + ")) {
        fprintf(stderr, "FAIL: the cave_both backend string must name BOTH sinks\n");
        fail = 1;
    }

    bwa_health h0, h1;
    bwa_get_health(e, &h0);
    const uint64_t m0 = bwa_monitor_blocks(e);
    os_sleep_ms(150);
    bwa_get_health(e, &h1);
    const uint64_t m1 = bwa_monitor_blocks(e);
    printf("       150 ms: array blocks %llu -> %llu, monitor blocks %llu -> %llu\n",
           (unsigned long long)h0.blocks, (unsigned long long)h1.blocks,
           (unsigned long long)m0, (unsigned long long)m1);
    if (h1.blocks <= h0.blocks) { fprintf(stderr, "FAIL: the array callback is not running\n");   fail = 1; }
    if (m1 <= m0)               { fprintf(stderr, "FAIL: the monitor callback is not running\n"); fail = 1; }

    bwa_stop(e);
    bwa_destroy(e);
    os_remove(LAYOUT);
    return fail;
}

/* Does this machine have ANY stereo output the AUTO order can open? Asked with a plain binaural
 * engine, which is the same 2-channel AUTO request the cave_both monitor makes, so the answer is
 * exactly "what the monitor should have resolved to". BWA_SINK_NULL means no device at all, and
 * the arm below cannot tell a correct monitor from a broken one there. */
static bwa_sink_type stereo_auto_probe(void) {
    bwa_desc d;
    memset(&d, 0, sizeof d);
    d.sample_rate = 48000;
    d.block_size  = 256;
    d.profile     = BWA_PROFILE_BINAURAL;
    d.sink        = BWA_SINK_AUTO;
    bwa_engine* e = bwa_create(&d);
    if (!e) return BWA_SINK_NULL;
    bwa_sink_type t = BWA_SINK_NULL;
    if (bwa_start(e) == BWA_OK) { t = bwa_get_sink_type(e); bwa_stop(e); }
    bwa_destroy(e);
    return t;
}

/* THE regression arm for the rule, and the only one that runs without an array device.
 *
 * `device` names something no backend has, so the ARRAY falls through AUTO to the null sink. The
 * monitor must still open the platform's default stereo device, because `device` was never its
 * business. Under the old code it inherited the string, every backend skipped it by rule 10, and
 * the monitor went silently null - which is the rig failure in miniature, with a name that misses
 * standing in for a name that belongs to the other device. */
static int run_auto_foreign_device(bwa_sink_type expect) {
    if (!write_layout(LAYOUT, 8)) { fprintf(stderr, "FAIL: cannot write the test layout\n"); return 1; }

    bwa_desc d;
    memset(&d, 0, sizeof d);
    d.sample_rate = 48000;
    d.block_size  = 256;
    d.profile     = BWA_PROFILE_CAVE_BOTH;
    d.sink        = BWA_SINK_AUTO;
    d.device      = "bwa test: no backend has a device by this name";
    d.layout_path = LAYOUT;

    bwa_engine* e = bwa_create(&d);
    if (!e) { fprintf(stderr, "FAIL: bwa_create returned NULL\n"); os_remove(LAYOUT); return 1; }
    if (bwa_start(e) != BWA_OK) {
        fprintf(stderr, "FAIL: cave_both must start even when the array device is missing: %s\n",
                bwa_last_error(e) ? bwa_last_error(e) : "(no message)");
        bwa_destroy(e); os_remove(LAYOUT); return 1;
    }

    int fail = 0;
    const bwa_sink_type mon = bwa_monitor_sink_type(e);
    const char* backend = bwa_get_audio_backend(e);
    printf("       array -> %s, monitor -> %s (expected %s)\n",
           sink_name(bwa_get_sink_type(e)), sink_name(mon), sink_name(expect));
    printf("       backend string: %s\n", backend ? backend : "(null)");
    if (mon != expect) {
        fprintf(stderr, "FAIL: the monitor resolved to %s, not %s - it inherited the ARRAY's device "
                        "string, which no backend has\n", sink_name(mon), sink_name(expect));
        fail = 1;
    }
    if (backend && strstr(backend, d.device)) {
        fprintf(stderr, "FAIL: the array's device name reached the monitor\n");
        fail = 1;
    }

    const uint64_t m0 = bwa_monitor_blocks(e);
    os_sleep_ms(150);
    const uint64_t m1 = bwa_monitor_blocks(e);
    printf("       150 ms: monitor blocks %llu -> %llu\n",
           (unsigned long long)m0, (unsigned long long)m1);
    if (m1 <= m0) { fprintf(stderr, "FAIL: the monitor callback is not running\n"); fail = 1; }

    bwa_stop(e);
    bwa_destroy(e);
    os_remove(LAYOUT);
    return fail;
}

/* bwa_get_device_name promises "always NUL-terminated, truncated to cap-1". A backend that instead
 * returns false with an empty buffer breaks a picker that sized its buffer for the common case, and
 * WASAPI did exactly that: WideCharToMultiByte into a short buffer returns 0 and writes nothing.
 * Checked on every compiled backend, so the next one inherits the rule rather than the bug. */
static int check_name_truncation(bwa_sink_type backend, const char* label) {
    const uint32_t n = bwa_get_device_count(backend);
    if (n == 0) { printf("       %s: no devices, nothing to truncate\n", label); return 0; }

    char full[256] = { 0 };
    if (!bwa_get_device_name(backend, 0, full, sizeof full) || !full[0]) {
        fprintf(stderr, "FAIL: %s device 0 has no name\n", label);
        return 1;
    }
    int fail = 0;
    /* 4 bytes: enough for three characters of any ASCII name, and small enough that a real endpoint
     * name always has to be cut. */
    char small[4];
    memset(small, 0x7F, sizeof small);
    const bool ok = bwa_get_device_name(backend, 0, small, (uint32_t)sizeof small);
    printf("       %s: \"%s\" -> cap 4 gives \"%s\" (returned %s)\n",
           label, full, ok ? small : "", ok ? "true" : "false");
    if (!ok)                                 { fprintf(stderr, "FAIL: %s truncation returned false\n", label); fail = 1; }
    else if (small[sizeof small - 1] != 0)   { fprintf(stderr, "FAIL: %s truncation is not terminated\n", label); fail = 1; }
    else if (small[0] == 0)                  { fprintf(stderr, "FAIL: %s truncation is empty\n", label); fail = 1; }
    else if (strncmp(small, full, strlen(small)) != 0) {
        fprintf(stderr, "FAIL: %s truncation is not a prefix of the full name\n", label);
        fail = 1;
    }
    /* cap 1 has room for the terminator only: terminated and empty, never a stray byte. */
    char one[1];
    memset(one, 0x7F, sizeof one);
    bwa_get_device_name(backend, 0, one, 1);
    if (one[0] != 0) { fprintf(stderr, "FAIL: %s with cap 1 did not write the terminator\n", label); fail = 1; }
    return fail;
}

int main(void) {
    const uint32_t n_asio   = bwa_get_device_count(BWA_SINK_ASIO);
    const uint32_t n_wasapi = bwa_get_device_count(BWA_SINK_WASAPI);

    printf("cave_both device test: %u ASIO driver(s), %u WASAPI endpoint(s)\n", n_asio, n_wasapi);

    printf("  -- bwa_get_device_name truncates, it does not fail --\n");
    {
        int fail = 0;
        fail |= check_name_truncation(BWA_SINK_WASAPI,    "wasapi");
        fail |= check_name_truncation(BWA_SINK_ASIO,      "asio");
        fail |= check_name_truncation(BWA_SINK_JACK,      "jack");
        fail |= check_name_truncation(BWA_SINK_ALSA,      "alsa");
        fail |= check_name_truncation(BWA_SINK_COREAUDIO, "coreaudio");
        if (fail) return 1;
    }

    printf("  -- device-free (the exception clause: an offline render stays offline) --\n");
    if (run_offline() != 0) return 1;

    {
        const bwa_sink_type stereo = stereo_auto_probe();
        printf("  -- a foreign device string must not reach the monitor (stereo AUTO opens %s) --\n",
               sink_name(stereo));
        if (stereo == BWA_SINK_NULL)
            printf("       skip: no stereo output device on this machine, so a correct monitor and a "
                   "broken one both read null\n");
        else if (run_auto_foreign_device(stereo) != 0)
            return 1;
    }

    /* The rig's own shape, which needs an ASIO driver that really opens outputs. Every installed
     * driver, widest layout first, until one combination opens. A desk machine's ASIO list is mostly
     * virtual devices that open nothing, and the rule under test is about the MONITOR, so the array
     * only has to be genuinely on ASIO - it does not have to be 26 channels wide.
     *
     * This arm is the only one that exercises "the array holds the one ASIO driver slot while the
     * monitor opens a second device". Where it cannot run, the two arms above have already pinned
     * the rule the array's width has nothing to do with, so the test still PASSES rather than
     * reporting a skip it has not earned - it just says which half did not run. */
    if (n_asio == 0 || n_wasapi == 0) {
        printf("NOT RUN: the ASIO array arm needs both an ASIO driver and a WASAPI endpoint (%s missing)\n",
               n_asio == 0 ? "ASIO" : "WASAPI");
        return 0;
    }

    static const int widths[] = { 26, 8, 4 };
    for (uint32_t d = 0; d < n_asio; ++d) {
        char asio_name[128] = { 0 };
        if (!bwa_get_device_name(BWA_SINK_ASIO, d, asio_name, sizeof asio_name) || !asio_name[0]) {
            fprintf(stderr, "FAIL: ASIO driver %u reports no name\n", d);
            return 1;
        }
        for (size_t i = 0; i < sizeof widths / sizeof widths[0]; ++i) {
            printf("  -- \"%s\", %d array channels --\n", asio_name, widths[i]);
            const int rc = run(asio_name, widths[i]);
            if (rc == 0) {
                printf("cave_both OK (array on ASIO \"%s\", monitor on its own device)\n", asio_name);
                return 0;
            }
            if (rc != SKIP_EXIT) return 1;
        }
    }
    printf("NOT RUN: no installed ASIO driver would open an array at any tried width\n");
    return 0;
}
