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
 *
 * The device-free arm also pins the array sim's ROOM (arraysim.h: distance gain, propagation delay
 * and the speakers' directivity) where only an engine can: cave_both's monitor reads the array's own
 * device buffer, so the room stage must leave the array bit-identical to a plain cave render;
 * cave_sim must hear the directivity model; and BWA_PROFILE_BINAURAL, which has no physical
 * speakers, must hear neither the model nor ANY of the align stage (trims, correction FIR, tracked
 * room EQ): that profile skips it outright.
 */
#include "bw_audio.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "os/os.h"          /* os_sleep_ms, os_fopen, os_remove */

#if defined(_WIN32)
#include <windows.h>        /* the COM-balance check: CoInitializeEx on the calling thread */
#include <objbase.h>
#undef small               /* rpcndr.h defines it as char, and this file has a buffer by that name */
#if defined(_MSC_VER)
#pragma comment(lib, "ole32.lib")
#endif
#endif

#define SKIP_EXIT 77     /* ctest SKIP_RETURN_CODE; see CMakeLists.txt */
#define LAYOUT    "bwa_cave_both_layout.json"

/* Internal test hooks (declared in src/sink/sink.h, defined in engine.c, exported from the dll; not
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

/* ---- simulated speaker directivity, device-free (MANUAL sink: this thread pumps every block) ----
 * An 8-speaker ring whose speakers all aim straight UP, so a listener near ear height is 90 deg or
 * more off every axis, and a two-band model with a strong treble loss. with_model = 0 writes the same ring
 * with neither the model nor the aims. */
#define DIR_LAYOUT "bwa_cave_both_dir_layout.json"
static int write_dir_layout(const char* path, int with_model) {
    FILE* f = os_fopen(path, "wb");
    if (!f) return 0;
    fprintf(f, "{ \"speakers\": [\n");
    for (int i = 0; i < 8; ++i) {
        const double a = 6.283185307179586 * (double)i / 8.0;
        fprintf(f, "  { \"index\": %d, \"position\": [%.4f, 1.5, %.4f]%s }%s\n",
                i, 2.0 * cos(a), 2.0 * sin(a), with_model ? ", \"aim\": [0, 1, 0]" : "", (i + 1 < 8) ? "," : "");
    }
    fprintf(f, "]%s\n", with_model ? "," : " }");
    if (with_model)
        fprintf(f, "\"directivity\": { \"bands_hz\": [250, 4000], \"angles_deg\": [0, 30, 60, 90, 180],\n"
                   "  \"split_hz\": 1000, \"loss_db\": [[0, -1, -2, -3, -6], [0, -4, -10, -16, -25]] } }\n");
    fclose(f);
    return 1;
}

/* Render `nb` blocks of one profile on the MANUAL sink: a push source fed deterministic noise near
 * speaker 0 with the FDN reverb send on (so the BUS carries signal in every profile, binaural
 * included: its point voices bypass the bus, its diffuse tail does not), and a listener off the
 * ring's center and below it (at ear height the bearing off an UP aim is 90 deg from anywhere, ref
 * included, and the array's comp would sit at identity). Copies every output block into out (nb * block * channels floats); returns the
 * channel count, 0 on failure. */
static uint32_t dir_render(bwa_profile prof, const char* layout, int nb, float* out, uint32_t cap_floats) {
    bwa_desc d;
    memset(&d, 0, sizeof d);
    d.sample_rate = 48000;
    d.block_size  = 256;
    d.profile     = prof;
    d.sink        = BWA_SINK_MANUAL;
    d.layout_path = layout;
    bwa_engine* e = bwa_create(&d);
    if (!e) return 0;
    bwa_fdn_desc fd;
    memset(&fd, 0, sizeof fd);
    fd.enabled = 1;
    bwa_fdn_config(e, &fd);
    bwa_source s = bwa_source_create_push(e);
    uint32_t ch = 0, used = 0;
    if (s) {
        bwa_source_set_pos(e, s, 1.6f, 1.5f, 0.3f);
        bwa_source_set_reverb(e, s, true);
        bwa_set_listener_pose(e, 0.5f, 1.1f, -0.4f, 0.f, 0.f, 0.f, 1.f);   /* below the ring: off the ref bearing too */
        bwa_commit(e);
    }
    if (s && bwa_start(e) == BWA_OK) {
        uint32_t seed = 12345u;
        float buf[1024];
        for (int b = 0; b < nb; ++b) {
            while (bwa_source_push_space(e, s) >= 1024) {
                for (int i = 0; i < 1024; ++i) {
                    seed = seed * 1664525u + 1013904223u;
                    buf[i] = 0.2f * ((float)(seed >> 8) / 8388608.f - 1.f);
                }
                if (bwa_source_push(e, s, buf, 1024) == 0) break;
            }
            uint32_t c = 0, nf = 0;
            const float* p = bwa_render_block(e, &c, &nf);
            if (!p || used + c * nf > cap_floats) { ch = 0; break; }
            memcpy(out + used, p, sizeof(float) * c * nf);
            used += c * nf;
            ch = c;
        }
        bwa_stop(e);
    }
    bwa_destroy(e);
    return ch;
}

static double dir_energy_db(const float* x, uint32_t n) {
    double e = 0.0;
    for (uint32_t i = 0; i < n; ++i) e += (double)x[i] * x[i];
    return 10.0 * log10(e + 1e-30);
}

static int run_directivity_offline(void) {
    enum { NB = 120, CAPF = NB * 256 * 8 };
    static float a[CAPF], b[CAPF];
    int fail = 0;
    if (!write_dir_layout(DIR_LAYOUT, 1) || !write_dir_layout(LAYOUT, 0)) {
        fprintf(stderr, "FAIL: cannot write the directivity test layouts\n"); return 1;
    }
    /* 1. cave_both's ARRAY is untouched by its monitor's directivity stage: the monitor decodes from
     * the array's own device buffer, and the stage must work on a copy. Same layout, same scene:
     * cave and cave_both write the same array, bit for bit. With the stage run in place on dev26,
     * this went red. */
    uint32_t ca = dir_render(BWA_PROFILE_CAVE, DIR_LAYOUT, NB, a, CAPF);
    uint32_t cb = dir_render(BWA_PROFILE_CAVE_BOTH, DIR_LAYOUT, NB, b, CAPF);
    printf("       cave vs cave_both array (model in the layout): %u / %u channels, %s, level %.1f dB\n",
           ca, cb, (ca && ca == cb && !memcmp(a, b, sizeof(float) * NB * 256 * ca)) ? "bit-identical" : "DIFFERENT",
           ca ? dir_energy_db(a, NB * 256 * ca) : 0.0);
    if (!ca || ca != cb || memcmp(a, b, sizeof(float) * NB * 256 * ca) != 0) {
        fprintf(stderr, "FAIL: cave_both's array output differs from cave's: the monitor's directivity stage "
                        "reached the array's device buffer\n");
        fail = 1;
    }
    if (ca && dir_energy_db(a, NB * 256 * ca) < -60.0) {
        fprintf(stderr, "FAIL: the array rendered silence, so the comparison above proves nothing\n");
        fail = 1;
    }
    /* 2. cave_sim hears the model: every speaker points up, the listener is ~90 deg off every axis,
     * so the audition must come out well down on the model-free one (-3 dB bass, -16 dB treble in
     * the model). */
    uint32_t sa = dir_render(BWA_PROFILE_CAVE_SIM, DIR_LAYOUT, NB, a, CAPF);
    uint32_t sb = dir_render(BWA_PROFILE_CAVE_SIM, LAYOUT, NB, b, CAPF);
    const uint32_t tail = 40 * 256 * 2, off = (NB - 40) * 256 * 2;   /* the last 40 blocks: glides landed */
    if (sa != 2 || sb != 2) { fprintf(stderr, "FAIL: cave_sim did not render stereo\n"); fail = 1; }
    else {
        const double dm = dir_energy_db(a + off, tail) - dir_energy_db(b + off, tail);
        printf("       cave_sim with the model vs without: %+.2f dB\n", dm);
        if (!(dm < -3.0)) {
            fprintf(stderr, "FAIL: cave_sim's audition ignored the speakers' off-axis loss (%+.2f dB)\n", dm);
            fail = 1;
        }
    }
    /* 3. binaural has no physical speakers: a directivity model in the layout changes NOTHING, bit
     * for bit. The FDN tail is on its bus, so the array's directivity comp would color it if the
     * profile ran the align stage, and a room stage would if one were built here. Each of those
     * two, broken on purpose, turned this red. */
    uint32_t ba = dir_render(BWA_PROFILE_BINAURAL, DIR_LAYOUT, NB, a, CAPF);
    uint32_t bb = dir_render(BWA_PROFILE_BINAURAL, LAYOUT, NB, b, CAPF);
    const int bsame = (ba == 2 && bb == 2 && !memcmp(a, b, sizeof(float) * NB * 256 * 2));
    printf("       binaural with the model vs without: %s, level %.1f dB\n", bsame ? "bit-identical" : "DIFFERENT",
           ba ? dir_energy_db(a, NB * 256 * 2) : 0.0);
    if (!bsame) {
        fprintf(stderr, "FAIL: a directivity model changed the binaural render, which has no physical speakers\n");
        fail = 1;
    }
    os_remove(DIR_LAYOUT);
    os_remove(LAYOUT);
    return fail;
}

/* ---- BWA_PROFILE_BINAURAL bypasses the whole align stage ----
 * The same 8-speaker ring, once plain and once carrying every per-speaker correction the align stage
 * applies: gain_db and delay_ms trims, a correction FIR (`eq`) and a tracked room EQ grid
 * (`room_eq_grid`, two positions around the listener). All of it describes PHYSICAL speakers, so the
 * binaural render (whose bus holds only the synthesized-diffuse tail, decoded through virtual
 * directions) must come out bit-identical with and without it, while cave and cave_sim, which do
 * have physical speakers, must not. With the direct_on gate around rt_render's align stage removed,
 * the binaural check went red. */
#define TRIM_LAYOUT "bwa_cave_both_trim_layout.json"
static int write_trim_layout(const char* path, int with_trims) {
    FILE* f = os_fopen(path, "wb");
    if (!f) return 0;
    fprintf(f, "{ \"speakers\": [\n");
    for (int i = 0; i < 8; ++i) {
        const double a = 6.283185307179586 * (double)i / 8.0;
        fprintf(f, "  { \"index\": %d, \"position\": [%.4f, 1.5, %.4f]", i, 2.0 * cos(a), 2.0 * sin(a));
        if (with_trims)
            fprintf(f, ", \"gain_db\": %.2f, \"delay_ms\": %.3f, \"eq\": [0.9, 0.2, -0.1, 0.05, 0.02]",
                    -1.5 - 0.5 * i, 0.3 * i);
        fprintf(f, " }%s\n", (i + 1 < 8) ? "," : "");
    }
    fprintf(f, "]");
    if (with_trims) {
        fprintf(f, ",\n\"room_eq_grid\": [\n");
        const double gp[2][3] = { { 0.2, 1.1, -0.4 }, { 0.8, 1.1, -0.4 } };
        for (int g = 0; g < 2; ++g) {
            fprintf(f, "  { \"position\": [%.2f, %.2f, %.2f], \"speakers\": [", gp[g][0], gp[g][1], gp[g][2]);
            for (int i = 0; i < 8; ++i)
                fprintf(f, "[ {\"fc\": %.1f, \"gain_db\": %.1f, \"q\": 4.0} ]%s", 60.0 + 10.0 * i, -3.0 - 4.0 * g,
                        (i + 1 < 8) ? ", " : "");
            fprintf(f, "] }%s\n", g ? "" : ",");
        }
        fprintf(f, "]");
    }
    fprintf(f, " }\n");
    fclose(f);
    return 1;
}

static int run_binaural_align_bypass(void) {
    enum { NB = 120, CAPF = NB * 256 * 8 };
    static float a[CAPF], b[CAPF];
    int fail = 0;
    if (!write_trim_layout(TRIM_LAYOUT, 1) || !write_trim_layout(LAYOUT, 0)) {
        fprintf(stderr, "FAIL: cannot write the align-bypass test layouts\n"); return 1;
    }
    const bwa_profile profs[3] = { BWA_PROFILE_BINAURAL, BWA_PROFILE_CAVE, BWA_PROFILE_CAVE_SIM };
    const char* names[3] = { "binaural", "cave", "cave_sim" };
    for (int p = 0; p < 3; ++p) {
        const uint32_t ca = dir_render(profs[p], TRIM_LAYOUT, NB, a, CAPF);
        const uint32_t cb = dir_render(profs[p], LAYOUT, NB, b, CAPF);
        const int same = ca && ca == cb && !memcmp(a, b, sizeof(float) * NB * 256 * ca);
        printf("       %s with trims + eq + room_eq_grid vs without: %s, level %.1f dB\n", names[p],
               same ? "bit-identical" : "DIFFERENT", ca ? dir_energy_db(a, NB * 256 * ca) : 0.0);
        if (!ca || ca != cb) { fprintf(stderr, "FAIL: %s did not render\n", names[p]); fail = 1; continue; }
        if (dir_energy_db(a, NB * 256 * ca) < -60.0) {
            fprintf(stderr, "FAIL: %s rendered silence, so the comparison proves nothing\n", names[p]); fail = 1;
        }
        if (p == 0 && !same) {
            fprintf(stderr, "FAIL: the binaural render changed with per-speaker trims, eq or room_eq_grid in the "
                            "layout: the align stage reached a profile with no physical speakers\n");
            fail = 1;
        }
        if (p > 0 && same) {
            fprintf(stderr, "FAIL: %s ignored the layout's trims, so the binaural check above could not fail\n", names[p]);
            fail = 1;
        }
    }
    os_remove(TRIM_LAYOUT);
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

/* bwa_device_present, the whole rule through the public call. The fixed cases need no device and
 * hold on every machine; the per-device ones run over whatever this box lists. NOBODY names a
 * device no machine has. */
#define NOBODY "bwa_test_no_such_device"

/* Does this build carry `s`? Asked the only way the public ABI can say it: a named backend this
 * build lacks fails bwa_start with "this build has no ..." (sink.c, try_backend). Independent of
 * bwa_device_present, which is the thing under test. */
static bool backend_in_build(bwa_sink_type s) {
    bwa_desc d = { 0 };
    d.profile = BWA_PROFILE_BINAURAL; d.sample_rate = 48000; d.block_size = 256; d.sink = s;
    d.device = NOBODY;
    bwa_engine* e = bwa_create(&d);
    if (!e) return false;
    bool in_build = true;
    if (bwa_start(e) != BWA_OK) {
        const char* why = bwa_last_error(e);
        in_build = !(why && strstr(why, "this build has no"));
    } else bwa_stop(e);
    bwa_destroy(e);
    return in_build;
}

#if defined(_WIN32)
/* The ASIO SDK's driver list initializes COM apartment-threaded, ignores the failure on a thread
 * that is already multithreaded, and uninitializes anyway. Unfixed, every ASIO enumeration drops
 * one COM reference from an MTA caller (a WASAPI host, a multithreaded tool). So: make this
 * thread MTA (one reference), enumerate, and ask whether the apartment is still there. A second
 * initialize answers S_FALSE while it is, and S_OK once the calls have torn it down. */
static int asio_com_balance_on_this_thread(void) {
    if (bwa_get_device_count(BWA_SINK_ASIO) == 0) {
        printf("       no ASIO driver listed: the COM balance is NOT exercised (the SDK list only "
               "touches COM when it is non-empty)\n");
        return 0;
    }
    const HRESULT first = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (first != S_OK) {
        printf("       skip: this thread already has a COM apartment (0x%08lx)\n", (unsigned long)first);
        if (SUCCEEDED(first)) CoUninitialize();
        return 0;
    }
    char buf[64];
    for (int i = 0; i < 4; ++i) {
        bwa_get_device_count(BWA_SINK_ASIO);
        bwa_get_device_name(BWA_SINK_ASIO, 0, buf, sizeof buf);
        bwa_get_asio_driver_count();
        bwa_device_present(BWA_PROFILE_CAVE, BWA_SINK_ASIO, NOBODY);
        bwa_device_present(BWA_PROFILE_CAVE, BWA_SINK_AUTO, NOBODY);
    }
    const HRESULT again = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (SUCCEEDED(again)) CoUninitialize();     /* the probe's reference */
    if (again == S_FALSE) CoUninitialize();     /* ours, still held */
    if (again != S_FALSE) {
        fprintf(stderr, "FAIL: 20 ASIO enumerations on an MTA thread tore its COM apartment down "
                        "(re-initialize returned 0x%08lx, want S_FALSE)\n", (unsigned long)again);
        return 1;
    }
    printf("       20 ASIO enumerations on an MTA thread: the apartment is still initialized\n");
    return 0;
}
/* On a thread of its own: the main thread already carries whatever apartment the earlier
 * sections left it, and the check needs to own the one reference it counts. */
static DWORD WINAPI asio_com_thread(LPVOID out) { *(int*)out = asio_com_balance_on_this_thread(); return 0; }
static int check_asio_com_balance(void) {
    int fail = 1;
    HANDLE t = CreateThread(NULL, 0, asio_com_thread, &fail, 0, NULL);
    if (!t) { fprintf(stderr, "FAIL: could not start the COM check thread\n"); return 1; }
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    return fail;
}
#endif

static int check_device_present(void) {
    int fail = 0;
#define WANT(cond, ...) do { if (!(cond)) { fprintf(stderr, "FAIL: device_present: " __VA_ARGS__); \
                                            fprintf(stderr, "\n"); fail = 1; } } while (0)
    static const bwa_profile profiles[] = { BWA_PROFILE_CAVE, BWA_PROFILE_BINAURAL,
                                            BWA_PROFILE_CAVE_SIM, BWA_PROFILE_CAVE_BOTH };
    static const bwa_sink_type real[] = { BWA_SINK_ASIO, BWA_SINK_WASAPI, BWA_SINK_COREAUDIO,
                                          BWA_SINK_ALSA, BWA_SINK_AAUDIO, BWA_SINK_JACK,
                                          BWA_SINK_WORKLET };
    for (size_t p = 0; p < sizeof profiles / sizeof *profiles; ++p) {
        WANT(bwa_device_present(profiles[p], BWA_SINK_NULL, NULL),     "NULL needs no device");
        WANT(bwa_device_present(profiles[p], BWA_SINK_NULL, NOBODY),   "NULL ignores the name");
        WANT(bwa_device_present(profiles[p], BWA_SINK_MANUAL, ""),     "MANUAL needs no device");
        WANT(!bwa_device_present(profiles[p], BWA_SINK_AUTO, NOBODY),  "AUTO found a device nobody has (profile %d)", (int)profiles[p]);
    }
    for (size_t b = 0; b < sizeof real / sizeof *real; ++b) {
        const bwa_sink_type s = real[b];
        /* JACK, ALSA, AAudio and the browser sink open strings their list never held, so a named
         * device there is present wherever the build CARRIES the backend: the answer must never
         * send a rig that names "hw:1,0" to headphones. In a build without the backend the same
         * name is missing (that machine is a desk, and bwa_start would fail on it). The other
         * three match the list. */
        const bool unlisted = s == BWA_SINK_JACK || s == BWA_SINK_ALSA || s == BWA_SINK_AAUDIO ||
                              s == BWA_SINK_WORKLET;
        const bool in_build = backend_in_build(s);
        const bool want_named = unlisted && in_build;
        WANT(bwa_device_present(BWA_PROFILE_CAVE, s, NOBODY) == want_named, "sink %d + an unlisted name (in build: %d)", (int)s, (int)in_build);
        WANT(bwa_device_present(BWA_PROFILE_BINAURAL, s, NOBODY) == want_named, "sink %d + an unlisted name, stereo", (int)s);
        if (unlisted) WANT(bwa_device_present(BWA_PROFILE_CAVE, s, "hw:1,0") == in_build, "sink %d and hw:1,0 (in build: %d)", (int)s, (int)in_build);
#if defined(_WIN32)
        if (unlisted) WANT(!in_build, "a Windows build claims sink %d", (int)s);   /* pins the probe itself */
#endif

        const uint32_t n = bwa_get_device_count(s);
        /* Every "its own name is present" check below sits inside the device loop, so with no
         * device a matcher that always answered false would pass. Say so where it happens. */
        if (n == 0 && s == BWA_SINK_ASIO)
            printf("       no ASIO driver listed: the positive path (a listed name is present) is NOT exercised\n");
        if (n == 0 && s == BWA_SINK_WASAPI)
            printf("       no WASAPI endpoint listed: the positive path (a listed name is present) is NOT exercised\n");
        WANT(bwa_device_present(BWA_PROFILE_CAVE, s, NULL) == (n > 0), "sink %d with no device (NULL)", (int)s);
        WANT(bwa_device_present(BWA_PROFILE_CAVE, s, "")   == (n > 0), "sink %d with no device (\"\")", (int)s);
        for (uint32_t i = 0; i < n; ++i) {
            char name[256] = { 0 }, id[256] = { 0 }, longer[300];
            if (!bwa_get_device_name(s, i, name, sizeof name) || !name[0]) continue;
            bwa_get_device_id(s, i, id, sizeof id);
            WANT(bwa_device_present(BWA_PROFILE_CAVE, s, name), "sink %d misses its own '%s'", (int)s, name);
            if (id[0]) WANT(bwa_device_present(BWA_PROFILE_CAVE, s, id), "sink %d misses its own id '%s'", (int)s, id);
            /* Exact: a listed name with more text after it is another device. */
            snprintf(longer, sizeof longer, "%s_bwa_test_suffix", name);
            if (!unlisted) WANT(!bwa_device_present(BWA_PROFILE_CAVE, s, longer), "sink %d matched a prefix", (int)s);
#if defined(_WIN32)
            /* The Windows AUTO order: WASAPI then ASIO for stereo, ASIO alone for the array. So an
             * ASIO driver is present for both widths, and a WASAPI endpoint for stereo only
             * (unless an ASIO driver carries the same string). */
            if (s == BWA_SINK_ASIO) {
                WANT(bwa_device_present(BWA_PROFILE_CAVE, BWA_SINK_AUTO, name),      "AUTO array misses ASIO '%s'", name);
                WANT(bwa_device_present(BWA_PROFILE_CAVE_BOTH, BWA_SINK_AUTO, name), "AUTO cave_both misses ASIO '%s'", name);
                WANT(bwa_device_present(BWA_PROFILE_CAVE_SIM, BWA_SINK_AUTO, name),  "AUTO stereo misses ASIO '%s'", name);
            }
            if (s == BWA_SINK_WASAPI) {
                WANT(bwa_device_present(BWA_PROFILE_BINAURAL, BWA_SINK_AUTO, name),  "AUTO stereo misses WASAPI '%s'", name);
                if (!bwa_device_present(BWA_PROFILE_CAVE, BWA_SINK_ASIO, name))
                    WANT(!bwa_device_present(BWA_PROFILE_CAVE, BWA_SINK_AUTO, name), "AUTO hands the array to WASAPI '%s'", name);
            }
#endif
        }
    }
#if defined(_WIN32)
    {
        const bool asio = bwa_get_device_count(BWA_SINK_ASIO) > 0, wasapi = bwa_get_device_count(BWA_SINK_WASAPI) > 0;
        WANT(bwa_device_present(BWA_PROFILE_CAVE, BWA_SINK_AUTO, NULL) == asio,                 "AUTO array, no device");
        /* Stereo AUTO with no name is WASAPI's default endpoint alone: an installed ASIO driver
         * is not an output. (The open still tries ASIO after WASAPI; the answer is stricter on
         * purpose. test_audio_sink pins the rule itself with WASAPI's answer stubbed.) Here it
         * is the real lookup against a real WASAPI open of the default endpoint: the same lookup,
         * then the open. An open that succeeds means the endpoint is there, so that direction is
         * hard. Present without an open is possible (another application holds the endpoint
         * exclusively), so it is printed. */
        {
            const bool here = bwa_device_present(BWA_PROFILE_BINAURAL, BWA_SINK_AUTO, NULL);
            bwa_desc d = { 0 };
            d.profile = BWA_PROFILE_BINAURAL; d.sample_rate = 48000; d.block_size = 256; d.sink = BWA_SINK_WASAPI;
            bwa_engine* e = bwa_create(&d);
            const bool opened = e && bwa_start(e) == BWA_OK;
            if (e) { if (opened) bwa_stop(e); bwa_destroy(e); }
            printf("       stereo AUTO, no device: %s; a WASAPI open of the default endpoint %s\n",
                   here ? "present" : "missing", opened ? "succeeded" : "failed");
            WANT(!opened || here, "the default endpoint opened and stereo AUTO with no device says missing");
            if (here && !opened)
                printf("       note: present and not opened (the endpoint is held by another application, or "
                       "this build has no WASAPI)\n");
            WANT(!here || wasapi || !backend_in_build(BWA_SINK_WASAPI), "stereo AUTO reads present with no WASAPI endpoint listed");
            if (!wasapi && !asio)
                printf("       no WASAPI endpoint and no ASIO driver: the stereo rule is NOT exercised here "
                       "(counting ASIO would answer missing too)\n");
        }
    }
#endif
    /* The answer and the open agree. AUTO with a name nobody has is "not present", and bwa_start
     * then runs the offline sink. A named backend with that name is "not present", and bwa_start
     * fails (or the build has no such backend, which fails too). */
    {
        bwa_desc d = { 0 };
        d.profile = BWA_PROFILE_BINAURAL; d.sample_rate = 48000; d.block_size = 256;
        d.sink = BWA_SINK_AUTO; d.device = NOBODY;
        bwa_engine* e = bwa_create(&d);
        if (!e || bwa_start(e) != BWA_OK) { WANT(0, "AUTO with an unlisted device did not start on the offline sink"); }
        else WANT(bwa_get_sink_type(e) == BWA_SINK_NULL, "AUTO opened sink %d for a device reported missing", (int)bwa_get_sink_type(e));
        if (e) { bwa_stop(e); bwa_destroy(e); }
        d.sink = BWA_SINK_WASAPI;
        e = bwa_create(&d);
        WANT(e && bwa_start(e) != BWA_OK, "a named backend started on a device reported missing");
        if (e) { bwa_stop(e); bwa_destroy(e); }
    }
#undef WANT
    if (!fail) printf("       rule holds over this machine's lists\n");
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

    printf("  -- bwa_device_present --\n");
    if (check_device_present() != 0) return 1;
#if defined(_WIN32)
    printf("  -- ASIO enumeration leaves the caller's COM apartment alone --\n");
    if (check_asio_com_balance() != 0) return 1;
#endif

    printf("  -- device-free (the exception clause: an offline render stays offline) --\n");
    if (run_offline() != 0) return 1;

    printf("  -- device-free: the array sim's room --\n");
    if (run_directivity_offline() != 0) return 1;

    printf("  -- device-free: BWA_PROFILE_BINAURAL bypasses the align stage --\n");
    if (run_binaural_align_bypass() != 0) return 1;

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
