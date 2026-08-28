/*
 * assets_test.c — the shared-ownership asset cache (bwa_sound_acquire / _release /
 * _acquire_async / _is_ready) against the real dll, off-hardware on the manual sink.
 *
 *   - dedup: the same (path, flags) returns the SAME handle;
 *   - path normalization: case and slash direction do not make a second entry;
 *   - flags are part of the key: streamed and in-RAM are separate handles for one path;
 *   - impossible flag combinations are REFUSED with a message, not silently narrowed;
 *   - refcount balance: only the LAST release unloads (through the retire-ack, so it is safe
 *     while a voice is on it);
 *   - the two ownership tiers stay separable: bwa_unload_sound on an acquired handle and
 *     bwa_sound_release on a loaded one are both refused;
 *   - async: the handle is usable IMMEDIATELY, a play issued against it is held and starts once
 *     the decode lands, and releasing an in-flight load is safe;
 *   - async KIND: a still-decoding handle reports 0 channels, which passes both of the ABI's
 *     kind guards, so the flags it was acquired with decide instead - a bed is refused at
 *     bwa_source_play and a mono asset at bwa_bed_play, and the matching pairs still work.
 *
 * Links bwa_core alongside the dll for dr_wav's write API (the implementation lives in sound.c),
 * exactly as test_tools_api does.
 */
#include "bw_audio.h"
#include "dr_wav.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define SR   48000u
#define BLK  256u
#define LEN  (4u * BLK)          /* test asset length in frames */

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", (msg)); ++fails; } } while (0)

static int write_wav(const char* path, uint16_t channels, float value, uint32_t frames) {
    drwav_data_format fmt = { drwav_container_riff, DR_WAVE_FORMAT_IEEE_FLOAT, channels, SR, 32 };
    drwav wav;
    if (!drwav_init_file_write(&wav, path, &fmt, NULL)) return 0;
    float* buf = (float*)malloc((size_t)frames * channels * sizeof(float));
    if (!buf) { drwav_uninit(&wav); return 0; }
    for (uint32_t i = 0; i < frames * channels; ++i) buf[i] = value;
    drwav_uint64 wrote = drwav_write_pcm_frames(&wav, frames, buf);
    free(buf); drwav_uninit(&wav);
    return wrote == frames;
}

/* Render one block and accumulate |sample| over every channel. */
static double render_energy(bwa_engine* e, uint32_t blocks) {
    double acc = 0.0;
    for (uint32_t b = 0; b < blocks; ++b) {
        uint32_t ch = 0, n = 0;
        const float* out = bwa_render_block(e, &ch, &n);
        if (!out) return -1.0;
        for (uint32_t i = 0; i < ch * n; ++i) acc += fabs((double)out[i]);
        bwa_commit(e);
    }
    return acc;
}

/* Pump until the async decode lands (or give up). Commit is the client's normal adopt point. */
static int wait_ready(bwa_engine* e, bwa_sound snd) {
    for (int i = 0; i < 4000; ++i) {          /* ~4 s ceiling; a few hundred KB decodes in <1 ms */
        bwa_commit(e);
        if (bwa_sound_is_ready(e, snd)) return 1;
        /* is_ready leaves the error clear while a decode is merely in progress, so a message here
         * means the load FAILED (or the handle is not the cache's): it will never land. */
        if (bwa_last_error(e) != NULL) return 0;
        Sleep(1);
    }
    return 0;
}

int main(void) {
    const char* MONO = "bwa_assets_mono.wav";
    const char* BED  = "bwa_assets_bed.wav";
    if (!write_wav(MONO, 1, 0.5f, LEN) || !write_wav(BED, 4, 0.25f, LEN)) {
        printf("FAIL: could not write the test wavs\n");
        return 1;
    }

    bwa_desc cfg; memset(&cfg, 0, sizeof cfg);
    cfg.profile = BWA_PROFILE_CAVE; cfg.sample_rate = SR; cfg.block_size = BLK; cfg.sink = BWA_SINK_MANUAL;
    bwa_engine* e = bwa_create(&cfg);
    if (!e) { printf("FAIL: bwa_create\n"); return 1; }
    bwa_set_listener_pose(e, 0.f, 1.5f, 0.f, 0.f, 0.f, 0.f, 1.f);
    bwa_commit(e);
    if (bwa_start(e) != 0) { printf("FAIL: bwa_start: %s\n", bwa_last_error(e)); bwa_destroy(e); return 1; }

    float spk[26 * 3] = { 0 };
    bwa_get_speakers(e, spk, 26);

    /* ---- dedup + refcount ---- */
    bwa_sound a1 = bwa_sound_acquire(e, MONO, 0);
    CHECK(a1 != 0, "acquire mono");
    CHECK(bwa_sound_get_frames(e, a1) == LEN, "acquired asset has the right length");

    bwa_sound a2 = bwa_sound_acquire(e, MONO, 0);
    CHECK(a2 == a1, "same path + flags dedups to the same handle");

    /* ---- path normalization: case and slash direction ---- */
    bwa_sound a3 = bwa_sound_acquire(e, "BWA_ASSETS_MONO.WAV", 0);
    CHECK(a3 == a1, "case-insensitive path dedups");
    bwa_sound a4 = bwa_sound_acquire(e, ".\\bwa_assets_mono.wav", 0);
    CHECK(a4 != 0, "backslash spelling acquires");
    /* "./x" and "x" are different STRINGS - only the separator and case are normalized - so this
     * is a distinct entry. What it proves is that '\' does not break the loader. */
    bwa_sound a5 = bwa_sound_acquire(e, "./bwa_assets_mono.wav", 0);
    CHECK(a5 == a4, "'.\\x' and './x' normalize to one entry");
    bwa_sound_release(e, a4);
    bwa_sound_release(e, a5);

    /* ---- flags are part of the key ---- */
    bwa_sound st = bwa_sound_acquire(e, MONO, BWA_LOAD_STREAM);
    CHECK(st != 0, "acquire the same path STREAMED");
    CHECK(st != a1, "streamed and in-RAM are different entries for one path");
    bwa_sound bed = bwa_sound_acquire(e, BED, BWA_LOAD_AMBIX);
    CHECK(bed != 0, "acquire an AmbiX bed");
    CHECK(bwa_sound_get_channels(e, bed) == 4, "the bed kept its 4 channels");

    /* ---- refused flag combinations ---- */
    CHECK(bwa_sound_acquire(e, MONO, BWA_LOAD_AMBIX | BWA_LOAD_FUMA) == 0, "AMBIX|FUMA is refused");
    CHECK(bwa_last_error(e) != NULL, "AMBIX|FUMA reports why");
    CHECK(bwa_sound_acquire(e, MONO, BWA_LOAD_STREAM | BWA_LOAD_AMBIX) == 0, "STREAM|AMBIX is refused");
    CHECK(bwa_last_error(e) != NULL, "STREAM|AMBIX reports why");
    CHECK(bwa_sound_acquire(e, MONO, 1u << 7) == 0, "an unknown flag bit is refused");
    CHECK(bwa_last_error(e) != NULL, "an unknown flag bit reports why");

    /* ---- mixing the two ownership tiers ---- */
    bwa_unload_sound(e, a1);
    CHECK(bwa_last_error(e) != NULL, "bwa_unload_sound on an acquired handle reports an error");
    CHECK(bwa_sound_get_frames(e, a1) == LEN, "...and does NOT unload it");

    bwa_sound owned = bwa_load_sound(e, MONO);
    CHECK(owned != 0, "bwa_load_sound still works alongside the cache");
    CHECK(owned != a1, "an explicit load is its own handle, not a cache hit");
    bwa_sound_release(e, owned);
    CHECK(bwa_last_error(e) != NULL, "bwa_sound_release on a loaded handle reports an error");
    CHECK(bwa_sound_get_frames(e, owned) == LEN, "...and does NOT unload it");
    bwa_unload_sound(e, owned);

    /* ---- release at zero actually unloads ---- */
    /* a1 was acquired 3 times (a1, a2, a3). Two releases must leave it alive. */
    bwa_sound_release(e, a1);
    bwa_sound_release(e, a1);
    CHECK(bwa_sound_get_frames(e, a1) == LEN, "the asset survives while references remain");
    bwa_sound_release(e, a1);
    render_energy(e, 2);                  /* let the audio side see CMD_SOUND_RETIRE and ack it */
    bwa_commit(e);
    CHECK(bwa_sound_get_channels(e, a1) == 0, "the last release unloads (handle goes stale)");

    bwa_sound again = bwa_sound_acquire(e, MONO, 0);
    CHECK(again != 0, "the path can be acquired again after a full release");
    CHECK(again != a1, "...and gets a fresh handle, not the retired one");

    /* ---- async: valid at once, held play, becomes ready ---- */
    bwa_source src = bwa_source_create(e);
    CHECK(src != 0, "source create");
    bwa_source_set_pos(e, src, spk[0], spk[1], spk[2]);
    bwa_commit(e);

    /* A decode that fails is reported at is_ready, not at acquire: the handle is already out. */
    bwa_sound bad = bwa_sound_acquire_async(e, MONO, BWA_LOAD_FUMA);   /* a MONO file is not B-format */
    CHECK(bad != 0, "async acquire hands out a handle before the decode is judged");
    CHECK(wait_ready(e, bad) == 0, "a failed decode never becomes ready");
    CHECK(bwa_last_error(e) != NULL, "...and says why at bwa_sound_is_ready");
    bwa_sound_release(e, bad);

    /* The mono asset under a second name, so this is a fresh cache entry rather than a hit. */
    CHECK(write_wav("bwa_assets_async.wav", 1, 0.5f, LEN) != 0, "write the async test wav");
    bwa_sound as = bwa_sound_acquire_async(e, "bwa_assets_async.wav", 0);
    CHECK(as != 0, "async acquire returns a handle immediately");

    /* DETERMINISM BY ORDERING, not by timing. A finished decode is adopted ONLY at a pump point -
     * acquire, is_ready, find, release, or commit - and bwa_source_play is not one of them. So as
     * long as nothing between the acquire above and the play below pumps, the slot is still
     * reserved and this play is GUARANTEED to take the held path. Do not insert an is_ready or a
     * release here "to check something first": that would adopt the decode and silently turn this
     * into an ordinary bind, which is exactly how the case used to go untested while passing. */
    bwa_source_play(e, src, as, false);
    CHECK(bwa_sound_get_channels(e, as) == 0, "the asset is still unpublished, so the play was HELD");
    CHECK(bwa_source_is_playing(e, src) == false, "a held play does not read as playing yet");

    double quiet = render_energy(e, 2);
    CHECK(quiet >= 0.0, "render_block works with a voice bound to a not-yet-ready asset");

    /* the dedup check moves AFTER the play, since acquiring again would have pumped */
    CHECK(bwa_sound_acquire_async(e, "bwa_assets_async.wav", 0) == as, "async acquire dedups too");
    bwa_sound_release(e, as);             /* drop that second reference */

    CHECK(wait_ready(e, as) != 0, "the async load becomes ready");
    CHECK(bwa_sound_get_frames(e, as) == LEN, "the async asset landed at full length");
    double loud = render_energy(e, 2);
    CHECK(loud > 0.0, "the held play started once the decode landed");

    /* ---- async KIND: a still-decoding asset reports 0 channels, so the acquire FLAGS decide ----
     * The kind guards on the play calls read the channel count, and a pending slot has none, so
     * both of them used to pass a not-yet-decoded asset of either kind. The cache knows better:
     * BWA_LOAD_AMBIX / BWA_LOAD_FUMA means a bed, anything else means a point source. */
    CHECK(write_wav("bwa_assets_kbed.wav", 4, 0.25f, LEN) != 0, "write the async bed wav");
    CHECK(write_wav("bwa_assets_kmono.wav", 1, 0.5f, LEN) != 0, "write the async mono wav");

    bwa_source ksrc = bwa_source_create(e);
    bwa_bed    kbed = bwa_bed_create(e);
    CHECK(ksrc != 0 && kbed != 0, "kind: source + bed create");
    bwa_source_set_pos(e, ksrc, spk[0], spk[1], spk[2]);
    bwa_commit(e);

    /* an AmbiX asset played as a POINT SOURCE is refused at the call, not silently bound later */
    bwa_sound kb = bwa_sound_acquire_async(e, "bwa_assets_kbed.wav", BWA_LOAD_AMBIX);
    CHECK(kb != 0, "kind: async acquire of an AmbiX bed");
    CHECK(bwa_sound_get_channels(e, kb) == 0, "kind: a still-decoding asset reports 0 channels");
    bwa_source_play(e, ksrc, kb, true);
    {   /* match the message, not just non-NULL: bwa_last_error is sticky across calls */
        const char* m = bwa_last_error(e);
        CHECK(m && strstr(m, "acquired as an ambisonic bed") != NULL,
              "kind: bwa_source_play refuses a still-decoding AmbiX asset, and says why");
    }
    /* the same handle on a BED is the legitimate case: held, then bound when the decode lands */
    bwa_bed_play(e, kbed, kb, true);
    CHECK(wait_ready(e, kb) != 0, "kind: the async bed becomes ready");
    CHECK(bwa_source_is_playing(e, ksrc) == false, "kind: the refused play never binds when the data lands");
    CHECK(bwa_bed_is_playing(e, kbed), "kind: the held BED play does bind");
    CHECK(render_energy(e, 2) > 0.0, "kind: ...and the bed is audible");

    /* symmetrically: a mono asset played as a BED is refused, and plays as a point source */
    bwa_sound km = bwa_sound_acquire_async(e, "bwa_assets_kmono.wav", 0);
    CHECK(km != 0, "kind: async acquire of a mono asset");
    bwa_bed_play(e, kbed, km, true);
    {
        const char* m = bwa_last_error(e);
        CHECK(m && strstr(m, "acquired as mono") != NULL,
              "kind: bwa_bed_play refuses a still-decoding mono asset, and says why");
    }
    bwa_source_play(e, ksrc, km, true);
    CHECK(wait_ready(e, km) != 0, "kind: the async mono asset becomes ready");
    CHECK(bwa_source_is_playing(e, ksrc), "kind: the held POINT-SOURCE play binds");
    CHECK(bwa_bed_is_playing(e, kbed), "kind: ...and the refused bed play left the bed on its own asset");
    CHECK(render_energy(e, 2) > 0.0, "kind: ...both audible");

    bwa_source_stop(e, ksrc);
    bwa_bed_stop(e, kbed);
    render_energy(e, 2);
    bwa_source_destroy(e, ksrc);
    bwa_bed_destroy(e, kbed);
    bwa_sound_release(e, kb);
    bwa_sound_release(e, km);
    render_energy(e, 2);

    /* ---- async: the documented call order survives the adoption ---- */
    /* Every layer says to set the region AFTER the play, because the bounds resolve against the
     * bound asset. A HELD play has not reached the audio thread, so a plain enqueue would be
     * consumed against a voice with nothing bound and the adoption would then reset the region from
     * the play call - losing it silently, on the only order the docs allow. Same DETERMINISM BY
     * ORDERING as the held-play case above: neither bwa_source_play nor bwa_source_set_region is a
     * pump point, so the play below is guaranteed held. Do not insert an is_ready between them. */
    CHECK(write_wav("bwa_assets_region.wav", 1, 0.5f, 8 * BLK) != 0, "write the async region wav");
    bwa_source rsrc = bwa_source_create(e);
    bwa_source_set_pos(e, rsrc, spk[0], spk[1], spk[2]);
    bwa_commit(e);
    bwa_sound rsnd = bwa_sound_acquire_async(e, "bwa_assets_region.wav", 0);
    CHECK(rsnd != 0, "async region: acquire hands back a handle");
    bwa_source_play(e, rsrc, rsnd, true);                 /* looping the whole 8-block clip ... */
    bwa_source_set_region(e, rsrc, 0, 2 * BLK);           /* ... narrowed to the first 2 blocks */
    CHECK(bwa_sound_get_channels(e, rsnd) == 0, "async region: the play was HELD");
    CHECK(wait_ready(e, rsnd) != 0, "async region: the decode lands");
    render_energy(e, 3);                                  /* 0->256, 256->512 (the end), wrap then 0->256 */
    CHECK(bwa_source_get_playhead_frames(e, rsrc) == BLK,
          "async region: the region survives the adoption (=> 256; a lost region reads 768)");
    bwa_source_stop(e, rsrc);
    render_energy(e, 2);
    bwa_source_destroy(e, rsrc);
    bwa_sound_release(e, rsnd);
    render_energy(e, 2);

    /* ---- a scene stop cancels the HELD plays it owns ---- */
    /* bwa_stop_all always dropped every held play, or one would start by itself the moment its
     * decode landed - a sound beginning after the caller said stop. bwa_group_stop now does the
     * same for its own members, so the rule is the same whichever stop you reach for.
     *
     * DETERMINISM BY ORDERING: neither bwa_source_play nor bwa_group_stop is a pump point, and
     * bwa_sound_get_channels forwards straight to the sound table, so the plays below are
     * GUARANTEED still held when the stop runs. ONE asset feeds both sources on purpose: a second
     * bwa_sound_acquire_async would pump on entry and could adopt this decode before the plays,
     * which would silently turn a held play into an ordinary bind and delete the coverage. Both
     * held entries then resolve in the SAME publish, so the group is the only thing separating
     * them.
     *
     * The KEPT arm is not decoration. Without it an engine that dropped EVERY held play, whatever
     * its group, would pass the stopped arm. */
    CHECK(write_wav("bwa_assets_gstop.wav", 1, 0.5f, LEN) != 0, "write the group-stop wav");
    bwa_source gs = bwa_source_create(e), gk = bwa_source_create(e);
    CHECK(gs != 0 && gk != 0, "group stop: two sources");
    bwa_source_set_pos(e, gs, spk[0], spk[1], spk[2]);
    bwa_source_set_pos(e, gk, spk[3], spk[4], spk[5]);
    bwa_source_set_group(e, gs, 1);            /* the group that gets stopped */
    bwa_source_set_group(e, gk, 2);            /* the group that does not */
    bwa_commit(e);
    bwa_sound gsn = bwa_sound_acquire_async(e, "bwa_assets_gstop.wav", 0);
    CHECK(gsn != 0, "group stop: async acquire hands back a handle");
    bwa_source_play(e, gs, gsn, true);
    bwa_source_play(e, gk, gsn, true);
    CHECK(bwa_sound_get_channels(e, gsn) == 0, "group stop: unpublished, so BOTH plays were HELD");
    bwa_group_stop(e, 1);
    CHECK(wait_ready(e, gsn) != 0, "group stop: the decode lands");
    CHECK(render_energy(e, 4) > 0.0, "group stop: the surviving play is audible");
    CHECK(bwa_source_is_playing(e, gs) == false,
          "group stop: the held play in the stopped group never starts");
    CHECK(bwa_source_is_playing(e, gk),
          "group stop: a held play in ANOTHER group is untouched and starts");
    for (int i = 0; i < 20; ++i) { bwa_commit(e); }   /* pump well past the adoption */
    render_energy(e, 4);
    CHECK(bwa_source_is_playing(e, gs) == false, "group stop: ...and it does not start later either");
    bwa_source_stop(e, gk);
    render_energy(e, 2);
    bwa_source_destroy(e, gs);
    bwa_source_destroy(e, gk);
    bwa_sound_release(e, gsn);
    render_energy(e, 2);

    /* ---- releasing an in-flight async load ---- */
    /* Unlike the held-play case above, this one canNOT be made deterministic by ordering:
     * bwa_sound_release pumps on entry, so a decode that already finished is adopted before the
     * release runs and what gets exercised is "release an already-landed load". Both branches are
     * safe and the assertions hold either way, which is the point - but do not read this as proof
     * that the in-flight cancel path ran. */
    CHECK(write_wav("bwa_assets_cancel.wav", 1, 0.5f, LEN * 8) != 0, "write the cancel test wav");
    bwa_sound cx = bwa_sound_acquire_async(e, "bwa_assets_cancel.wav", 0);
    CHECK(cx != 0, "async acquire (to be cancelled) returns a handle");
    bwa_sound_release(e, cx);             /* cancel; may or may not still be in flight */
    CHECK(bwa_sound_is_ready(e, cx) == false, "a cancelled load never reports ready");
    for (int i = 0; i < 200; ++i) { bwa_commit(e); Sleep(1); }   /* let the worker's result arrive and be dropped */
    CHECK(render_energy(e, 2) >= 0.0, "the engine still renders after a cancelled load");

    /* re-acquiring the cancelled path must work and must produce a live asset */
    bwa_sound cx2 = bwa_sound_acquire(e, "bwa_assets_cancel.wav", 0);
    CHECK(cx2 != 0, "the cancelled path re-acquires");
    CHECK(bwa_sound_get_frames(e, cx2) == LEN * 8, "...with the right content");
    bwa_sound_release(e, cx2);

    /* ---- bwa_sound_find: pure lookup, no load, no reference ---- */
    /* MONO's entry is `again` by now: a1 was fully released above and the path re-acquired, so a
     * find must report the LIVE handle, not the retired one. */
    CHECK(bwa_sound_find(e, MONO, 0) == again, "find returns the resident handle");
    CHECK(bwa_sound_find(e, "BWA_ASSETS_MONO.WAV", 0) == again, "find normalizes the path like acquire");
    CHECK(bwa_sound_find(e, MONO, 0) != a1, "find never reports a retired handle");
    CHECK(bwa_sound_find(e, MONO, BWA_LOAD_STREAM) == st, "find keys on flags too");
    CHECK(bwa_sound_find(e, BED, BWA_LOAD_AMBIX) == bed, "find locates the ambisonic entry");
    CHECK(bwa_sound_find(e, BED, 0) == 0, "find does not answer across flags");
    CHECK(bwa_sound_find(e, "bwa_assets_never_loaded.wav", 0) == 0, "find misses without loading");
    CHECK(bwa_sound_find(e, "bwa_assets_never_loaded.wav", 0) == 0, "...and still misses on a second ask");
    CHECK(bwa_sound_find(e, NULL, 0) == 0 && bwa_sound_find(NULL, MONO, 0) == 0,
          "find is NULL-safe on both arguments");
    CHECK(bwa_sound_find(e, MONO, 0xF0u) == 0, "find rejects unknown flag bits");
    /* the probe took no reference: one release still leaves the entry resident */
    CHECK(bwa_sound_find(e, MONO, 0) == again, "find is repeatable and takes no reference");
    CHECK(bwa_sound_get_frames(e, again) == LEN, "the probed asset is untouched");

    bwa_source_stop(e, src);
    bwa_source_destroy(e, src);
    bwa_sound_release(e, again);
    bwa_sound_release(e, as);
    bwa_sound_release(e, st);
    bwa_sound_release(e, bed);
    render_energy(e, 2);
    bwa_commit(e);

    bwa_stop(e);
    bwa_destroy(e);                       /* joins the loader thread; clean under ASan */

    remove(MONO); remove(BED);
    remove("bwa_assets_async.wav"); remove("bwa_assets_cancel.wav");
    remove("bwa_assets_kbed.wav"); remove("bwa_assets_kmono.wav"); remove("bwa_assets_region.wav");

    if (fails) { printf("assets: %d FAILED\n", fails); return 1; }
    printf("assets: OK\n");
    return 0;
}
