/*
 * smoke.c — lifecycle / link sanity against the public ABI (the DLL). Runs the full
 * control-side sequence for ALL THREE profiles (cave / binaural / both), so the
 * profile-specific device wiring — the headphone decodes and the 'cave_both' dual-sink
 * double-buffer handoff — is exercised end to end. Every desc forces the offline null
 * sink (.sink = BWA_SINK_NULL), so it needs no hardware. Asserts nothing about audio.
 */
#include "bw_audio.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>           /* Sleep */

static int run_profile(bwa_profile profile, const char* name) {
    bwa_desc cfg = {
        .profile     = profile,
        .layout_path = NULL,
        .hrtf_path   = NULL,
        .sample_rate = 48000,
        .block_size  = 256,
        .sink        = BWA_SINK_NULL,          /* hermetic: no real device */
    };
    bwa_engine* e = bwa_create(&cfg);
    if (!e) { fprintf(stderr, "FAIL[%s]: bwa_create returned NULL\n", name); return 1; }
    if (bwa_start(e) != 0) {
        const char* err = bwa_last_error(e);
        fprintf(stderr, "FAIL[%s]: bwa_start: %s\n", name, err ? err : "(no message)");
        bwa_destroy(e);
        return 1;
    }

    /* version + resolved-config readbacks: the DLL's version must match the header we compiled
     * against; the desc above was explicit, so the getters must echo it; and on the null sink the
     * resolved sink type is NULL (machine-readable side of bwa_get_audio_backend). */
    if (bwa_get_version() != BWA_VERSION) {
        fprintf(stderr, "FAIL[%s]: bwa_get_version %x != header %x\n", name, bwa_get_version(), BWA_VERSION);
        bwa_destroy(e); return 1;
    }
    if (bwa_get_sample_rate(e) != 48000 || bwa_get_block_size(e) != 256) {
        fprintf(stderr, "FAIL[%s]: resolved sample_rate/block_size readback\n", name); bwa_destroy(e); return 1;
    }
    if (bwa_get_sink_type(e) != BWA_SINK_NULL) {
        fprintf(stderr, "FAIL[%s]: bwa_get_sink_type is not NULL on the null sink\n", name); bwa_destroy(e); return 1;
    }

    /* bwa_get_speakers conventions: xyz=NULL -> the total (== channel count); with a buffer it
     * returns the count FILLED (min(cap, count) — the bwa_get_bus_levels convention). */
    {
        float xyz3[3 * 3];
        if (bwa_get_speakers(e, NULL, 0) != bwa_get_channel_count(e)) {
            fprintf(stderr, "FAIL[%s]: get_speakers(NULL) != channel count\n", name); bwa_destroy(e); return 1;
        }
        if (bwa_get_speakers(e, xyz3, 3) != 3) {
            fprintf(stderr, "FAIL[%s]: get_speakers must return the FILLED count under a small cap\n", name);
            bwa_destroy(e); return 1;
        }
    }

    bwa_sound  snd = bwa_load_sound(e, "footsteps.wav"); /* may be 0 if absent — fine here */
    bwa_source s   = bwa_source_create(e);
    if (s == 0) { fprintf(stderr, "FAIL[%s]: bwa_source_create returned 0\n", name); bwa_destroy(e); return 1; }

    /* asset metadata: an invalid handle reads 0/0; a real load reports mono + a nonzero length
     * (the exact values are pinned rt-side — this is the ABI surface) */
    if (bwa_sound_get_frames(e, 0) != 0 || bwa_sound_get_channels(e, 0) != 0) {
        fprintf(stderr, "FAIL[%s]: metadata of an invalid handle is not 0\n", name); bwa_destroy(e); return 1;
    }
    if (snd && (bwa_sound_get_frames(e, snd) == 0 || bwa_sound_get_channels(e, snd) != 1)) {
        fprintf(stderr, "FAIL[%s]: loaded-sound metadata\n", name); bwa_destroy(e); return 1;
    }

    /* ASIO enumeration: engine-free and load-nothing, so it must be consistent on ANY machine —
     * every index below the count yields a non-empty name, the count itself is out of range, and
     * a zero-cap buffer is refused (0 drivers on a machine with none installed is fine) */
    {
        char nm[64];
        uint32_t nd = bwa_get_asio_driver_count();
        for (uint32_t i = 0; i < nd && i < 4; ++i)
            if (!bwa_get_asio_driver_name(i, nm, sizeof nm) || !nm[0]) {
                fprintf(stderr, "FAIL[%s]: driver %u unreadable\n", name, i); bwa_destroy(e); return 1;
            }
        if (bwa_get_asio_driver_name(nd, nm, sizeof nm) ||
            bwa_get_asio_driver_name(0, nm, 0)) {
            fprintf(stderr, "FAIL[%s]: driver enumeration bounds\n", name); bwa_destroy(e); return 1;
        }
    }

    bwa_source_play(e, s, snd, /*loop*/ true);
    bwa_source_set_gain(e, s, 0.8f);
    bwa_source_set_pos(e, s, 1.0f, 0.0f, -0.5f);
    bwa_source_set_attenuation_override(e, s, 1.0f, 0.0f, 0.0f);   /* exercise the export (rt pins behavior) */
    bwa_source_set_attenuation_override(e, s, 0.0f, 0.0f, 0.0f);
    bwa_set_listener_pose(e, 0.f, 1.5f, 0.f, 0.f, 0.f, 0.f, 1.f);   /* the default grid's ear point */
    bwa_commit(e);
    /* The oneshot's return is its only signal (no handle to poll), so both directions are ABI:
     * a loaded asset is accepted, and a never-loaded handle reports the drop rather than
     * looking like it played. `snd` is legitimately 0 when footsteps.wav is absent — which is
     * itself the invalid-handle case, so the assertion follows the handle. */
    if (snd) {
        if (!bwa_play_oneshot(e, snd, 0.f, 1.f, 0.f, 1.0f)) {
            fprintf(stderr, "FAIL[%s]: bwa_play_oneshot dropped a valid oneshot: %s\n", name, bwa_last_error(e));
            bwa_destroy(e); return 1;
        }
    }
    if (bwa_play_oneshot(e, 0, 0.f, 1.f, 0.f, 1.0f)) {
        fprintf(stderr, "FAIL[%s]: bwa_play_oneshot accepted an invalid sound handle\n", name);
        bwa_destroy(e); return 1;
    }

    /* the bwa_bed_* facade forwards to the same per-voice machinery — exercise every export on a
     * live engine (no AmbiX asset here, so no bed_play; the rest is enqueue-only and must be safe
     * on an idle bed handle) */
    bwa_bed bed = bwa_bed_create(e);
    if (bed == 0) { fprintf(stderr, "FAIL[%s]: bwa_bed_create returned 0\n", name); bwa_destroy(e); return 1; }
    bwa_bed_set_gain(e, bed, 0.5f);
    bwa_bed_set_orientation(e, bed, 0.5f, 0.f, 0.f);
    bwa_bed_set_priority(e, bed, 255);
    bwa_bed_set_group(e, bed, 1);
    bwa_bed_fade_to(e, bed, 0.2f, 0.1f);
    bwa_bed_set_paused(e, bed, true);
    bwa_bed_set_paused(e, bed, false);
    bwa_bed_seek(e, bed, 0);
    if (bwa_bed_is_playing(e, bed)) { fprintf(stderr, "FAIL[%s]: idle bed reads as playing\n", name); bwa_destroy(e); return 1; }
    bwa_bed_fade_out(e, bed, 0.05f);
    bwa_bed_stop(e, bed);
    bwa_bed_destroy(e, bed);

    Sleep(30);                          /* let the audio thread(s) run several blocks */

    /* device clock pair + output latency through the full dll: the null sink stamps every block
     * from QPC, so a (sample, host-time) pair must exist, sit at/behind the dsp clock, and advance
     * with it; the null sink has no DAC, so its reported output latency must be 0. */
    {
        uint64_t cs = 0, ct = 0;
        if (!bwa_get_clock(e, &cs, &ct) || ct == 0) {
            fprintf(stderr, "FAIL[%s]: bwa_get_clock has no pair after 30 ms of blocks\n", name);
            bwa_destroy(e); return 1;
        }
        if (cs > bwa_get_dsp_time(e)) {   /* the pair is a rendered block's start — never ahead of the clock */
            fprintf(stderr, "FAIL[%s]: clock-pair sample runs ahead of the dsp clock\n", name);
            bwa_destroy(e); return 1;
        }
        Sleep(15);                        /* > 2 blocks at 256/48k: the pair must move */
        uint64_t cs2 = 0, ct2 = 0;
        if (!bwa_get_clock(e, &cs2, &ct2) || cs2 <= cs || ct2 <= ct) {
            fprintf(stderr, "FAIL[%s]: clock pair does not advance with rendered blocks\n", name);
            bwa_destroy(e); return 1;
        }
        if (bwa_get_output_latency_frames(e) != 0) {
            fprintf(stderr, "FAIL[%s]: null sink reports a nonzero output latency\n", name);
            bwa_destroy(e); return 1;
        }
    }

    /* device health through the full dll. The null sink has a real thread on a real deadline, so it
     * MEASURES — and a quiet 30 ms of blocks must come back clean. The point of asserting `measured`
     * is that a 0 xrun count is only meaningful once it is true. */
    {
        bwa_health h;
        if (!bwa_get_health(e, &h)) {
            fprintf(stderr, "FAIL[%s]: the null sink has a deadline, so health must be measured\n", name);
            bwa_destroy(e); return 1;
        }
        if (h.blocks == 0) {
            fprintf(stderr, "FAIL[%s]: health counted no blocks after 45 ms of audio\n", name);
            bwa_destroy(e); return 1;
        }
        if (h.xruns != 0 || h.dropped_frames != 0 || bwa_get_xruns(e) != 0) {
            fprintf(stderr, "FAIL[%s]: %llu xruns (%llu frames) on an idle null-sink run\n", name,
                    (unsigned long long)h.xruns, (unsigned long long)h.dropped_frames);
            bwa_destroy(e); return 1;
        }
        if (h.stream_starves != 0) {   /* nothing streamed here */
            fprintf(stderr, "FAIL[%s]: stream starves reported with no streamed voice\n", name);
            bwa_destroy(e); return 1;
        }
        if (!(h.peak_load > 0.f)) {    /* a rendered block always takes SOME time */
            fprintf(stderr, "FAIL[%s]: peak_load is zero after rendering blocks\n", name);
            bwa_destroy(e); return 1;
        }
    }

    bwa_source_stop(e, s);
    bwa_source_destroy(e, s);
    bwa_unload_sound(e, snd);
    bwa_stop(e);
    bwa_destroy(e);
    return 0;
}

/* a valid grid layout whose speaker 0 carries a room_eq section (static-listener room correction) */
static int write_room_eq_layout(const char* path) {
    FILE* f = fopen(path, "wb");
    if (!f) return 0;
    const float ax[3] = { -1.5f, 0.f, 1.5f };
    int k = 0;
    fprintf(f, "{ \"speakers\": [\n");
    for (int yi = 0; yi < 3; ++yi) for (int xi = 0; xi < 3; ++xi) for (int zi = 0; zi < 3; ++zi) {
        if (ax[xi] == 0 && ax[yi] == 0 && ax[zi] == 0) continue;
        fprintf(f, "  {\"index\":%d,\"position\":[%g,%g,%g]%s}%s\n", k, ax[xi], ax[yi], ax[zi],
                k == 0 ? ",\"room_eq\":[{\"fc\":62.5,\"gain_db\":-6.0,\"q\":4.0}]" : "",
                (k == 25) ? "" : ",");
        ++k;
    }
    fprintf(f, "] }\n");
    fclose(f);
    return k == 26;
}

/* the room_eq start guard: a room_eq'd layout must refuse a moving-listener session (DBAP is the
 * default panner) and start cleanly once the session is a fixed-listener one (SPCAP/VBAP). */
static int run_room_eq_guard(void) {
    const char* LAY = "bwa_smoke_room_eq.json";
    if (!write_room_eq_layout(LAY)) { fprintf(stderr, "FAIL[guard]: cannot write layout\n"); return 1; }
    bwa_desc cfg = {
        .profile     = BWA_PROFILE_CAVE,
        .layout_path = LAY,
        .hrtf_path   = NULL,
        .sample_rate = 48000,
        .block_size  = 256,
        .sink        = BWA_SINK_NULL,          /* hermetic: no real device */
    };
    int rc = 1;
    bwa_engine* e = bwa_create(&cfg);
    if (!e) { fprintf(stderr, "FAIL[guard]: bwa_create returned NULL\n"); goto done; }
    if (bwa_start(e) == 0) { fprintf(stderr, "FAIL[guard]: bwa_start accepted room_eq + DBAP (moving listener)\n"); goto done; }
    {
        const char* err = bwa_last_error(e);
        if (!err || !strstr(err, "room_eq")) { fprintf(stderr, "FAIL[guard]: error does not name room_eq: %s\n", err ? err : "(none)"); goto done; }
    }
    bwa_set_panner(e, BWA_PAN_SPCAP);                   /* fixed-listener session: the same layout is fine */
    if (bwa_start(e) != 0) {
        fprintf(stderr, "FAIL[guard]: bwa_start rejected room_eq + SPCAP: %s\n", bwa_last_error(e));
        goto done;
    }
    bwa_stop(e);
    rc = 0;
done:
    if (e) bwa_destroy(e);
    remove(LAY);
    return rc;
}

/* the push-API kind guards, at the ABI: pushing to a live NON-push source must accept nothing AND
 * report (a bare 0 reads exactly like ring backpressure), a real push must work, and a pending play
 * must read as playing before the first rendered block (the poll-then-destroy window). */
/* the strict-layout guard: an EXPLICIT layout_path that fails to load must leave bwa_create
 * usable (fallback grid, reason in bwa_last_error, readable immediately) but refuse bwa_start
 * with BWA_ERR_LAYOUT — a named layout must never silently run as the 26-grid default. Also
 * covers the zero-default resolution (sample_rate/block_size 0 -> 48000/256). */
static int run_layout_strict(void) {
    bwa_desc cfg = {
        .profile     = BWA_PROFILE_CAVE,
        .layout_path = "bwa_smoke_no_such_layout.json",   /* deliberately nonexistent */
        .sink        = BWA_SINK_NULL,                     /* rate/block 0: resolve to defaults */
    };
    bwa_engine* e = bwa_create(&cfg);
    if (!e) { fprintf(stderr, "FAIL[layout]: create must survive a failed layout load\n"); return 1; }
    if (!bwa_last_error(e)) {
        fprintf(stderr, "FAIL[layout]: failed layout load must be readable after create\n");
        bwa_destroy(e); return 1;
    }
    if (bwa_get_sample_rate(e) != 48000 || bwa_get_block_size(e) != 256) {
        fprintf(stderr, "FAIL[layout]: zero desc fields must resolve to 48000/256\n");
        bwa_destroy(e); return 1;
    }
    if (bwa_get_channel_count(e) != 26) {   /* the engine sits on the fallback grid... */
        fprintf(stderr, "FAIL[layout]: fallback channel count is not 26\n");
        bwa_destroy(e); return 1;
    }
    for (int i = 0; i < 2; ++i) {           /* ...but must refuse to start, repeatably */
        if (bwa_start(e) != BWA_ERR_LAYOUT || !bwa_last_error(e)) {
            fprintf(stderr, "FAIL[layout]: start #%d must fail BWA_ERR_LAYOUT with a reason\n", i + 1);
            bwa_destroy(e); return 1;
        }
    }
    bwa_destroy(e);
    return 0;
}

static int run_push_guard(void) {
    bwa_desc cfg = {
        .profile     = BWA_PROFILE_CAVE,
        .sample_rate = 48000,
        .block_size  = 256,
        .sink        = BWA_SINK_NULL,          /* hermetic: no real device */
    };
    int rc = 1;
    float blk[64] = {0};
    bwa_engine* e = bwa_create(&cfg);
    if (!e) { fprintf(stderr, "FAIL[push]: bwa_create returned NULL\n"); return 1; }
    if (bwa_start(e) != 0) { fprintf(stderr, "FAIL[push]: bwa_start: %s\n", bwa_last_error(e)); goto done; }
    {
        bwa_source plain = bwa_source_create(e);
        bwa_source push  = bwa_source_create_push(e);
        if (!plain || !push) { fprintf(stderr, "FAIL[push]: source create\n"); goto done; }
        if (!bwa_source_is_playing(e, push)) { fprintf(stderr, "FAIL[push]: pending play must read as playing\n"); goto done; }
        if (bwa_source_push(e, plain, blk, 64) != 0 || !bwa_last_error(e)) {
            fprintf(stderr, "FAIL[push]: push on a non-push source must accept nothing AND set bwa_last_error\n"); goto done;
        }
        if (bwa_source_push_space(e, plain) != 0) { fprintf(stderr, "FAIL[push]: push_space on a non-push source\n"); goto done; }
        if (bwa_source_push(e, push, blk, 64) != 64) { fprintf(stderr, "FAIL[push]: push source refused frames\n"); goto done; }
        bwa_source_destroy(e, push);
        bwa_source_destroy(e, plain);
    }
    bwa_stop(e);
    rc = 0;
done:
    bwa_destroy(e);
    return rc;
}

/* binaural laterality through the REAL dll, end to end: bwa_create's own construction (layout,
 * monitor at the sink block size), render_binaural on the sink thread, whichever monitor the build
 * carries (steam HRTF or simple-pan — same convention either way). The null sink's test tap
 * observes the device-bound stereo the sink otherwise discards. The room frame is RH, +y up,
 * +z ahead: the identity listener's RIGHT ear is at -x (bw_audio.h BWA_ROOM_RIGHT), so a source
 * at (-1.5, 1.5, 0) must reach device channel 1 (R) — and channel 0 (L) after a 180-degree turn. */
__declspec(dllimport) extern void (*bwa_null_sink_tap)(const float* bus, unsigned channels, unsigned block_size);
static double tap_sum[2];
static void stereo_tap(const float* bus, unsigned channels, unsigned n) {
    if (channels != 2) return;
    for (unsigned i = 0; i < n; ++i) { tap_sum[0] += fabs(bus[i]); tap_sum[1] += fabs(bus[n + i]); }
}
/* the PUBLIC capture path (bwa_set_output_capture) — fires per block in render_binaural with the same
 * final device-bound stereo, planar. Same accumulation as the tap; asserted to carry the laterality. */
static double cap_sum[2];
static void capture_probe(void* user, const float* planar, uint32_t channels, uint32_t n) {
    (void)user;
    if (channels != 2) return;
    for (uint32_t i = 0; i < n; ++i) { cap_sum[0] += fabs(planar[i]); cap_sum[1] += fabs(planar[n + i]); }
}
/* Runs for BOTH headphone profiles: BINAURAL exercises the direct per-source render (the SH
 * encode + the cardioid fallback decode, or the phonon decode with the SDK), CAVE_SIM the
 * virtual-speaker audition. Same physics, same assertions. */
static int run_binaural_laterality(bwa_profile profile, const char* name) {
    bwa_desc cfg = {
        .profile     = profile,
        .sample_rate = 48000,
        .block_size  = 256,
        .sink        = BWA_SINK_NULL,
    };
    int rc = 1;
    bwa_engine* e = bwa_create(&cfg);
    if (!e) { fprintf(stderr, "FAIL[lat:%s]: bwa_create returned NULL\n", name); return 1; }
    {
        bwa_source s = bwa_source_create_push(e);
        if (!s) { fprintf(stderr, "FAIL[lat]: create_stream: %s\n", bwa_last_error(e)); goto done; }
        /* a TONE, never DC: the HRTF's per-ear DC gains are laterally opposite its audible ILD, so a
         * DC-driven laterality check passes exactly when the image is MIRRORED (steam_decode.c 2b) */
        float blk[4096];
        for (int k = 0; k < 16; ++k) {
            for (int i = 0; i < 4096; ++i)
                blk[i] = 0.25f * sinf(6.2831853f * 660.0f * (float)(k * 4096 + i) / 48000.0f);
            bwa_source_push(e, s, blk, 4096);                            /* ~1.4 s of signal */
        }
        bwa_source_set_pos(e, s, -1.5f, 1.5f, 0.0f);        /* the identity listener's RIGHT (-x) */
        bwa_set_listener_pose(e, 0.f, 1.5f, 0.f, 0.f, 0.f, 0.f, 1.f);
        bwa_commit(e);
        bwa_null_sink_tap = stereo_tap;
        bwa_set_output_capture(e, capture_probe, NULL);     /* the public capture path, alongside the test tap */
        tap_sum[0] = tap_sum[1] = 0.0;
        cap_sum[0] = cap_sum[1] = 0.0;
        if (bwa_start(e) != 0) { fprintf(stderr, "FAIL[lat]: bwa_start: %s\n", bwa_last_error(e)); goto done; }
        char backend[96];
        snprintf(backend, sizeof backend, "%s", bwa_get_audio_backend(e));   /* read while the sink is open */
        Sleep(400);
        bwa_stop(e);                                        /* sink thread joined: tap_sum is safe to read */
        printf("smoke[lat] %s: -x ident L=%.3g R=%.3g\n", backend, tap_sum[0], tap_sum[1]);
        if (!(tap_sum[1] > tap_sum[0] * 1.1)) {
            fprintf(stderr, "FAIL[lat]: a -x source must reach the RIGHT device channel at identity\n"); goto done;
        }
        printf("smoke[cap] bwa_set_output_capture: L=%.3g R=%.3g\n", cap_sum[0], cap_sum[1]);
        if (!(cap_sum[0] > 0.0) || !(cap_sum[1] > cap_sum[0] * 1.1)) {   /* fired + carried the final stereo */
            fprintf(stderr, "FAIL[cap]: bwa_set_output_capture must deliver the final binaural output (right-biased for a -x source)\n"); goto done;
        }
        bwa_set_listener_pose(e, 0.f, 1.5f, 0.f, 0.f, 1.f, 0.f, 0.f);   /* yaw 180: -x is now the LEFT */
        bwa_commit(e);
        tap_sum[0] = tap_sum[1] = 0.0;
        if (bwa_start(e) != 0) { fprintf(stderr, "FAIL[lat]: restart: %s\n", bwa_last_error(e)); goto done; }
        Sleep(400);
        bwa_stop(e);
        printf("smoke[lat] yaw180: L=%.3g R=%.3g\n", tap_sum[0], tap_sum[1]);
        if (!(tap_sum[0] > tap_sum[1] * 1.1)) {
            fprintf(stderr, "FAIL[lat]: after a 180-degree turn the -x source must reach the LEFT channel\n"); goto done;
        }
    }
    rc = 0;
done:
    bwa_null_sink_tap = NULL;
    bwa_destroy(e);
    return rc;
}

/* headphone correction EQ (bwa_load_headphone_eq): parse an AutoEq fixture and verify the
 * correction actually shapes the headphone stereo — a deep PK notch at the tone frequency plus
 * the preamp must drop the output by roughly their combined gain (~ -24 dB); the ramped A/B
 * restores the dry level; a garbage file fails loudly and KEEPS the working EQ. The MANUAL sink
 * makes the pump deterministic (and single-threaded, so no ack waits). */
static uint64_t hp_phase;
static void hp_topup(bwa_engine* eng, bwa_source s) {   /* keep the push ring fed, phase-continuous
                                                         * (660 Hz is integer-periodic in 48000) */
    float b[1024];
    while (bwa_source_push_space(eng, s) >= 1024) {
        for (int i = 0; i < 1024; ++i)
            b[i] = 0.25f * sinf(6.2831853f * 660.0f * (float)((hp_phase + (uint64_t)i) % 48000u) / 48000.0f);
        hp_phase += 1024;
        if (bwa_source_push(eng, s, b, 1024) == 0) break;
    }
}
static double hp_blocks(bwa_engine* eng, bwa_source s, int nblocks) {   /* pump + sum |stereo| */
    double sum = 0;
    for (int b = 0; b < nblocks; ++b) {
        hp_topup(eng, s);
        uint32_t ch = 0, nf = 0;
        const float* p = bwa_render_block(eng, &ch, &nf);
        if (!p || ch != 2) return -1.0;
        for (uint32_t i = 0; i < 2 * nf; ++i) sum += fabs(p[i]);
    }
    return sum;
}
static int run_headphone_eq(void) {
    const char* EQF = "bwa_hpeq_test.txt";
    const char* BAD = "bwa_hpeq_bad.txt";
    { FILE* f = fopen(EQF, "wb");
      if (!f) { fprintf(stderr, "FAIL[hpeq]: fixture write\n"); return 1; }
      fputs("Preamp: -6.0 dB\nFilter 1: ON PK Fc 660 Hz Gain -18.0 dB Q 4.00\n", f);
      fclose(f); }
    { FILE* f = fopen(BAD, "wb"); if (f) { fputs("not an eq file\n", f); fclose(f); } }

    bwa_desc cfg = { .profile = BWA_PROFILE_CAVE_SIM, .sample_rate = 48000, .block_size = 256,
                     .sink = BWA_SINK_MANUAL };
    int rc = 1;
    bwa_engine* eng = bwa_create(&cfg);
    if (!eng) { fprintf(stderr, "FAIL[hpeq]: bwa_create\n"); return 1; }
    {
        bwa_source s = bwa_source_create_push(eng);
        if (!s) { fprintf(stderr, "FAIL[hpeq]: push source: %s\n", bwa_last_error(eng)); goto done; }
        hp_phase = 0;
        bwa_source_set_pos(eng, s, -1.5f, 1.5f, 0.0f);
        bwa_set_listener_pose(eng, 0.f, 1.5f, 0.f, 0.f, 0.f, 0.f, 1.f);
        bwa_commit(eng);
        if (bwa_start(eng) != BWA_OK) { fprintf(stderr, "FAIL[hpeq]: start: %s\n", bwa_last_error(eng)); goto done; }
        hp_blocks(eng, s, 4);                              /* voice ramp-in */
        double e_dry = hp_blocks(eng, s, 8);
        if (e_dry <= 0) { fprintf(stderr, "FAIL[hpeq]: no baseline output\n"); goto done; }

        if (bwa_load_headphone_eq(eng, EQF) != BWA_OK) {
            fprintf(stderr, "FAIL[hpeq]: load: %s\n", bwa_last_error(eng)); goto done; }
        hp_blocks(eng, s, 3);                              /* adopt + the one-block ramp-in + settle */
        double e_eq = hp_blocks(eng, s, 8);
        double ratio = e_eq / e_dry;                       /* -6 dB preamp x -18 dB notch ~ -24 dB */
        printf("smoke[hpeq] dry=%.4g eq=%.4g ratio=%.4f\n", e_dry, e_eq, ratio);
        if (!(ratio > 0.01 && ratio < 0.20)) {
            fprintf(stderr, "FAIL[hpeq]: corrected/dry ratio %.4f outside the ~-24 dB bounds\n", ratio); goto done; }

        bwa_set_headphone_eq(eng, false);                  /* the ramped A/B back to dry */
        hp_blocks(eng, s, 3);
        double e_off = hp_blocks(eng, s, 8);
        if (!(e_off > 0.8 * e_dry && e_off < 1.2 * e_dry)) {
            fprintf(stderr, "FAIL[hpeq]: disable did not restore the dry level (%.4g vs %.4g)\n", e_off, e_dry); goto done; }

        if (bwa_load_headphone_eq(eng, BAD) != BWA_ERR_CONFIG || !bwa_last_error(eng)) {
            fprintf(stderr, "FAIL[hpeq]: a garbage file must fail with BWA_ERR_CONFIG + a reason\n"); goto done; }
        bwa_set_headphone_eq(eng, true);                   /* the failed load kept the working EQ */
        hp_blocks(eng, s, 3);
        double e_back = hp_blocks(eng, s, 8);
        if (!(e_back < 0.2 * e_dry)) {
            fprintf(stderr, "FAIL[hpeq]: the previous EQ must survive a failed load\n"); goto done; }
    }
    printf("smoke[hpeq] load/correct/A-B/keep-on-failure OK\n");
    rc = 0;
done:
    bwa_destroy(eng);
    remove(EQF); remove(BAD);
    return rc;
}

/* the material table's fill / release / reuse contract: define fills the 64-slot table, release frees a
 * slot, and the NEXT define reuses that exact slot instead of overflowing — the churn escape hatch. */
static int run_material_release(void) {
    bwa_desc cfg; memset(&cfg, 0, sizeof cfg);
    cfg.profile = BWA_PROFILE_CAVE; cfg.sample_rate = 48000; cfg.block_size = 256; cfg.sink = BWA_SINK_NULL;
    bwa_engine* e = bwa_create(&cfg);
    if (!e) { fprintf(stderr, "FAIL[mat]: bwa_create\n"); return 1; }
    int rc = 1;
    const float a[3] = { 0.1f, 0.1f, 0.1f }, t[3] = { 0.05f, 0.05f, 0.05f };

    bwa_material tok[128]; int n = 0;
    for (int i = 0; i < 128; ++i) { bwa_material m = bwa_material_define(e, a, 0.5f, t); if (m == 0) break; tok[n++] = m; }
    /* slot 0 is the reserved default, so a 64-slot table mints 63 custom tokens, then it's full */
    if (n != 63)                                  { fprintf(stderr, "FAIL[mat]: expected 63 tokens, got %d\n", n); goto done; }
    if (bwa_material_define(e, a, 0.5f, t) != 0)  { fprintf(stderr, "FAIL[mat]: full table should return 0\n"); goto done; }

    bwa_material freed = tok[20];
    bwa_material_release(e, freed);
    bwa_material reused = bwa_material_define(e, a, 0.5f, t);   /* must reuse the freed slot, not overflow */
    if (reused != freed)                          { fprintf(stderr, "FAIL[mat]: reuse gave %u, expected the freed %u\n", reused, freed); goto done; }

    bwa_material_release(e, 0);                    /* refused: token 0 is the default */
    if (bwa_last_error(e) == NULL)                { fprintf(stderr, "FAIL[mat]: releasing token 0 must set an error\n"); goto done; }

    printf("smoke[mat] 63 tokens, release+reuse hit slot %u, token-0 release refused\n", freed);
    rc = 0;
done:
    bwa_destroy(e);
    return rc;
}

/* bwa_bed_gains_batch (the public offline bed evaluation, no engine handle): drives the REAL
 * AllRAD/EPAD builds over a caller layout. A cube layout's gains for an upward plane wave must
 * carry energy and favor the upper speakers; a clustered (hull-degenerate) layout must still
 * decode finitely via the sampling fallback rather than fail. */
static int run_bed_batch(void) {
    float pos[8 * 3]; int k = 0;
    for (int ix = -1; ix <= 1; ix += 2)               /* a 2 m cube of speakers about (0, 1.4, 0) */
        for (int iy = -1; iy <= 1; iy += 2)
            for (int iz = -1; iz <= 1; iz += 2) {
                pos[k*3] = (float)ix; pos[k*3+1] = 1.4f + (float)iy; pos[k*3+2] = (float)iz; ++k;
            }
    float g[8];
    const float up[3] = { 0.f, 1.f, 0.f };            /* room axes: straight up */
    if (bwa_bed_gains_batch(BWA_DECODE_ALLRAD, false, pos, 8, up, 1, g) != 1u) {
        fprintf(stderr, "FAIL[bed]: AllRAD batch did not return ndir\n"); return 1; }
    float e = 0.f, e_up = 0.f; int fin = 1;
    for (int s = 0; s < 8; ++s) {
        fin &= isfinite(g[s]) ? 1 : 0;
        e += g[s] * g[s];
        if (pos[s*3+1] > 1.4f) e_up += g[s] * g[s];
    }
    if (!fin || e <= 1e-6f) { fprintf(stderr, "FAIL[bed]: AllRAD gains empty or non-finite\n"); return 1; }
    if (e_up <= 0.5f * e)   { fprintf(stderr, "FAIL[bed]: an upward wave must favor the upper speakers\n"); return 1; }
    if (bwa_bed_gains_batch(BWA_DECODE_EPAD, true, pos, 8, up, 1, g) != 1u) {
        fprintf(stderr, "FAIL[bed]: EPAD + max-rE batch did not return ndir\n"); return 1; }
    e = 0.f; for (int s = 0; s < 8; ++s) e += g[s] * g[s];
    if (e <= 1e-6f) { fprintf(stderr, "FAIL[bed]: EPAD + max-rE gains carry no energy\n"); return 1; }
    float clus[4 * 3];                                /* all ~one direction: defeats the hull */
    for (int s = 0; s < 4; ++s) {
        clus[s*3] = 2.f; clus[s*3+1] = 1.4f + 0.01f * (float)s; clus[s*3+2] = 0.01f * (float)s;
    }
    if (bwa_bed_gains_batch(BWA_DECODE_ALLRAD, false, clus, 4, up, 1, g) != 1u) {
        fprintf(stderr, "FAIL[bed]: degenerate layout must fall back, not fail\n"); return 1; }
    e = 0.f; fin = 1;
    for (int s = 0; s < 4; ++s) { fin &= isfinite(g[s]) ? 1 : 0; e += g[s] * g[s]; }
    if (!fin || e <= 1e-9f) { fprintf(stderr, "FAIL[bed]: fallback gains empty or non-finite\n"); return 1; }
    printf("smoke[bed] AllRAD/EPAD/max-rE batch + degenerate fallback OK\n");
    return 0;
}

int main(void) {
    if (run_profile(BWA_PROFILE_CAVE,      "cave"))      return 1;
    if (run_profile(BWA_PROFILE_BINAURAL,  "binaural"))  return 1;
    if (run_profile(BWA_PROFILE_CAVE_SIM,  "cave_sim"))  return 1;
    if (run_profile(BWA_PROFILE_CAVE_BOTH, "cave_both")) return 1;
    if (run_room_eq_guard())                          return 1;
    if (run_layout_strict())                          return 1;
    if (run_push_guard())                             return 1;
    if (run_binaural_laterality(BWA_PROFILE_BINAURAL, "binaural")) return 1;   /* the direct render */
    if (run_binaural_laterality(BWA_PROFILE_CAVE_SIM, "cave_sim")) return 1;   /* the array audition */
    if (run_headphone_eq())                           return 1;
    if (run_material_release())                       return 1;
    if (run_bed_batch())                              return 1;
    printf("smoke OK (cave, binaural, cave_sim, cave_both lifecycles; room_eq start guard; push kind guards; binaural + cave_sim laterality; headphone EQ; material release/reuse; bed gains batch)\n");
    return 0;
}
