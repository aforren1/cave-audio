/*
 * source_desc_test.c — the fill-then-apply source configuration (bwa_source_preset /
 * bwa_source_create_desc / bwa_source_apply / bwa_source_get_desc), against the DLL's real
 * exported entry points so every call crosses the guards a client's calls do.
 *
 * The contract under test is the header's, and it has four load-bearing halves:
 *   - the preset is PURE (no engine, deterministic, sets struct_size);
 *   - this struct's zero is NOT its default, so a zero-init or a wrong struct_size must be
 *     REFUSED rather than silently applied (the bwa_tuning argument, and here gain 0 / pitch 0
 *     is what a zero-init would actually configure);
 *   - get_desc -> apply is a NO-OP, which is what lets a client edit a readback and hand it back;
 *   - a stale handle is dropped, not acted on.
 *
 * Determinism: BWA_SINK_MANUAL throughout (single-threaded, sample-counted), so the audio thread
 * is this thread and a pumped block proves the command actually drained.
 */
#include "bw_audio.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { ++fails;                                   \
    fprintf(stderr, "FAIL %s:%d: ", "source_desc_test.c", __LINE__);                    \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static bwa_engine* make_engine(void) {
    bwa_desc cfg = { .profile = BWA_PROFILE_CAVE, .sample_rate = 48000, .block_size = 256,
                     .sink = BWA_SINK_MANUAL };
    return bwa_create(&cfg);
}

/* 1 = rendered and every sample finite, 0 = the render was refused, -1 = a non-finite sample
 * reached the device-bound output. */
static int render_blocks(bwa_engine* e, int blocks) {
    for (int b = 0; b < blocks; ++b) {
        uint32_t ch = 0, nf = 0;
        const float* p = bwa_render_block(e, &ch, &nf);
        if (!p || ch == 0 || nf == 0) return 0;
        for (uint32_t i = 0; i < ch * nf; ++i)
            if (!isfinite(p[i])) return -1;
    }
    return 1;
}

/* Field-by-field rather than memcmp: the struct has tail padding between its bool run and
 * `reserved`, and padding bytes are not part of the value. */
static int desc_eq(const bwa_source_desc* a, const bwa_source_desc* b) {
    return a->struct_size        == b->struct_size        &&
           a->gain               == b->gain               &&
           a->pitch              == b->pitch              &&
           a->priority           == b->priority           &&
           a->group              == b->group              &&
           a->spread             == b->spread             &&
           a->extent_height      == b->extent_height      &&
           a->size_m             == b->size_m             &&
           a->reverb_send        == b->reverb_send        &&
           a->atten_ref_dist     == b->atten_ref_dist     &&
           a->atten_rolloff      == b->atten_rolloff      &&
           a->atten_min_gain     == b->atten_min_gain     &&
           a->directivity_weight == b->directivity_weight &&
           a->directivity_power  == b->directivity_power  &&
           a->doppler            == b->doppler            &&
           a->air_absorption     == b->air_absorption     &&
           a->loudness_comp      == b->loudness_comp      &&
           a->proximity          == b->proximity          &&
           a->occlusion          == b->occlusion          &&
           a->early_reflections  == b->early_reflections  &&
           a->reverb             == b->reverb             &&
           a->reverb_distance    == b->reverb_distance    &&
           a->pathing            == b->pathing;
}

static int desc_finite(const bwa_source_desc* d) {
    return isfinite(d->gain) && isfinite(d->pitch) && isfinite(d->spread) &&
           isfinite(d->extent_height) && isfinite(d->size_m) && isfinite(d->reverb_send) &&
           isfinite(d->atten_ref_dist) && isfinite(d->atten_rolloff) &&
           isfinite(d->atten_min_gain) && isfinite(d->directivity_weight) &&
           isfinite(d->directivity_power);
}

int main(void) {
    const bwa_source_kind KINDS[] = { BWA_SRC_DEFAULT, BWA_SRC_PROP, BWA_SRC_VOICE,
                                      BWA_SRC_AMBIENCE, BWA_SRC_UI };
    const int NKIND = (int)(sizeof KINDS / sizeof KINDS[0]);

    /* ---- the preset is pure: no engine, deterministic, and it sets struct_size ---- */
    bwa_source_preset(BWA_SRC_PROP, NULL);          /* documented no-op, must not crash */
    for (int k = 0; k < NKIND; ++k) {
        bwa_source_desc a, b;
        memset(&a, 0xAB, sizeof a);                 /* garbage in: the preset must fill everything */
        memset(&b, 0x00, sizeof b);
        bwa_source_preset(KINDS[k], &a);
        bwa_source_preset(KINDS[k], &b);
        CHECK(a.struct_size == (uint32_t)sizeof(bwa_source_desc),
              "preset %d must set struct_size", (int)KINDS[k]);
        CHECK(desc_eq(&a, &b), "preset %d must be deterministic (same in, same out)", (int)KINDS[k]);
        CHECK(desc_finite(&a), "preset %d must not contain a non-finite field", (int)KINDS[k]);
        CHECK(a.gain > 0.f && a.pitch > 0.f,
              "preset %d must not configure silence or a zero rate", (int)KINDS[k]);
        for (int i = 0; i < 4; ++i) CHECK(a.reserved[i] == 0, "preset %d must zero reserved", (int)KINDS[k]);
    }
    /* an unknown kind resolves to the default rather than filling garbage */
    { bwa_source_desc def, odd;
      bwa_source_preset(BWA_SRC_DEFAULT, &def);
      bwa_source_preset((bwa_source_kind)9999, &odd);
      CHECK(desc_eq(&def, &odd), "an unknown kind must resolve to BWA_SRC_DEFAULT"); }
    /* the kinds are not all the same struct — the enum would be decoration if they were */
    { bwa_source_desc def, prop, ui, amb;
      bwa_source_preset(BWA_SRC_DEFAULT,  &def);
      bwa_source_preset(BWA_SRC_PROP,     &prop);
      bwa_source_preset(BWA_SRC_UI,       &ui);
      bwa_source_preset(BWA_SRC_AMBIENCE, &amb);
      CHECK(!desc_eq(&def, &prop), "PROP must differ from DEFAULT");
      CHECK(prop.occlusion && prop.reverb && prop.proximity && prop.air_absorption,
            "PROP is the documented room-obeying source");
      CHECK(!prop.doppler, "PROP leaves Doppler off (the docs argue it is a fast-mover knob)");
      CHECK(!prop.early_reflections, "no preset spends the O(N) image-source path");
      CHECK(ui.priority == 255 && ui.atten_ref_dist > 0.f && ui.atten_rolloff == 0.f,
            "UI is the documented distance-free, protected source");
      CHECK(amb.spread == 1.f, "AMBIENCE takes the ABI's documented wide endpoint");
      CHECK(def.directivity_weight == 0.f && prop.directivity_weight == 0.f &&
            ui.directivity_weight == 0.f && amb.directivity_weight == 0.f,
            "no preset turns on a pattern the caller has not aimed"); }

    bwa_engine* e = make_engine();
    CHECK(e != NULL, "engine create");
    if (!e) return 1;
    CHECK(bwa_start(e) == BWA_OK, "engine start (manual sink)");

    /* ---- NULL handling ---- */
    { bwa_source_desc d; bwa_source_preset(BWA_SRC_DEFAULT, &d);
      bwa_source s = bwa_source_create(e);
      CHECK(s != 0, "source create");
      CHECK(!bwa_source_apply(NULL, s, &d), "apply on a NULL engine must be false");
      CHECK(!bwa_source_apply(e, s, NULL) && bwa_last_error(e) != NULL,
            "a NULL desc must be refused WITH a reason");
      CHECK(!bwa_source_get_desc(e, s, NULL), "get_desc with a NULL out must be false");
      CHECK(!bwa_source_get_desc(NULL, s, &d), "get_desc on a NULL engine must be false");
      CHECK(bwa_source_create_desc(NULL, &d) == 0, "create_desc on a NULL engine must be 0");
      CHECK(bwa_source_create_desc(e, NULL) == 0 && bwa_last_error(e) != NULL,
            "create_desc with a NULL desc must be 0 WITH a reason");
      bwa_source_destroy(e, s); }

    /* ---- the struct_size guard: a zero-init struct is what it exists to catch ---- */
    { bwa_source s = bwa_source_create(e);
      bwa_source_desc z;
      memset(&z, 0, sizeof z);
      CHECK(!bwa_source_apply(e, s, &z) && bwa_last_error(e) != NULL,
            "a zero-init bwa_source_desc must be refused with a reason (gain 0 / pitch 0 is not a default)");
      CHECK(bwa_source_create_desc(e, &z) == 0,
            "create_desc must refuse a zero-init desc too, without allocating a voice");

      bwa_source_desc d; bwa_source_preset(BWA_SRC_PROP, &d);
      d.struct_size += 4;
      CHECK(!bwa_source_apply(e, s, &d), "an oversized struct_size must be refused");
      d.struct_size -= 8;
      CHECK(!bwa_source_apply(e, s, &d), "an undersized struct_size must be refused");
      d.struct_size += 4;
      CHECK(bwa_source_apply(e, s, &d), "the corrected struct_size must be accepted");

      /* the refusals must have changed nothing: only the accepted apply is visible */
      bwa_source_desc back;
      CHECK(bwa_source_get_desc(e, s, &back), "get_desc on a live source");
      CHECK(desc_eq(&back, &d), "the accepted apply must be what the source reads back as");
      bwa_source_destroy(e, s); }

    /* ---- non-finite fields refuse the WHOLE apply (half a configuration is worse than none) ---- */
    { bwa_source s = bwa_source_create(e);
      bwa_source_desc base, probe, back;
      bwa_source_preset(BWA_SRC_DEFAULT, &base);
      CHECK(bwa_source_apply(e, s, &base), "seed the source with the default");

      const float BAD[2] = { NAN, INFINITY };
      for (int i = 0; i < 2; ++i) {
          probe = base; probe.gain = BAD[i]; probe.spread = 0.75f;
          CHECK(!bwa_source_apply(e, s, &probe) && bwa_last_error(e) != NULL,
                "a non-finite gain must refuse the whole apply, with a reason");
          probe = base; probe.size_m = BAD[i];
          CHECK(!bwa_source_apply(e, s, &probe), "a non-finite size must refuse the whole apply");
          probe = base; probe.directivity_power = BAD[i];
          CHECK(!bwa_source_apply(e, s, &probe), "a non-finite directivity power must refuse");
      }
      CHECK(bwa_source_get_desc(e, s, &back), "get_desc after the refused applies");
      CHECK(desc_eq(&back, &base), "a refused apply must land NOTHING, not the fields before the bad one");
      CHECK(render_blocks(e, 2) == 1, "renders finite across the refused-apply traffic");
      bwa_source_destroy(e, s); }

    /* ---- out-of-range FINITE values clamp (the individual setters' own behavior) ---- */
    { bwa_source s = bwa_source_create(e);
      bwa_source_desc d, back;
      bwa_source_preset(BWA_SRC_DEFAULT, &d);
      d.pitch          = 99.f;      /* rt clamps [0.25, 4] */
      d.spread         = 5.f;       /* [0, 1] */
      d.extent_height  = -3.f;      /* < 0 = isotropic, canonicalized to -1 */
      d.size_m         = -2.f;      /* [0, inf) */
      d.priority       = 4000;      /* [0, 255] */
      d.group          = 99;        /* out of range falls back to group 0 */
      d.atten_min_gain = 7.f;       /* [0, 1], but only once a ref enables the override */
      d.atten_ref_dist = 2.f;
      CHECK(bwa_source_apply(e, s, &d), "an out-of-range FINITE desc must be accepted, not refused");
      CHECK(bwa_source_get_desc(e, s, &back), "get_desc after the clamping apply");
      CHECK(back.pitch == 4.f && back.spread == 1.f && back.extent_height == -1.f &&
            back.size_m == 0.f && back.priority == 255 && back.group == 0 &&
            back.atten_min_gain == 1.f,
            "the readback must report the CLAMPED value, not what the caller asked for");
      CHECK(bwa_source_apply(e, s, &back), "the clamped readback must re-apply");
      { bwa_source_desc again;
        CHECK(bwa_source_get_desc(e, s, &again), "get_desc after the re-apply");
        CHECK(desc_eq(&again, &back), "clamping must be idempotent, or a round-trip drifts"); }
      bwa_source_destroy(e, s); }

    /* ---- get_desc -> apply is a NO-OP, for every preset ---- */
    for (int k = 0; k < NKIND; ++k) {
        bwa_source_desc d, r1, r2;
        bwa_source_preset(KINDS[k], &d);
        bwa_source s = bwa_source_create(e);
        CHECK(bwa_source_apply(e, s, &d), "apply preset %d", (int)KINDS[k]);
        CHECK(bwa_source_get_desc(e, s, &r1), "get_desc preset %d", (int)KINDS[k]);
        CHECK(desc_eq(&r1, &d), "apply(preset) must read back AS the preset (kind %d)", (int)KINDS[k]);
        CHECK(bwa_source_apply(e, s, &r1), "re-apply the readback (kind %d)", (int)KINDS[k]);
        CHECK(bwa_source_get_desc(e, s, &r2), "get_desc again (kind %d)", (int)KINDS[k]);
        CHECK(desc_eq(&r1, &r2), "get_desc -> apply must be a no-op (kind %d)", (int)KINDS[k]);
        CHECK(render_blocks(e, 1) == 1, "renders finite under preset %d", (int)KINDS[k]);
        bwa_source_destroy(e, s);
    }

    /* ---- preset, then a field override, then apply: the override lands ---- */
    { bwa_source_desc d, back;
      bwa_source_preset(BWA_SRC_PROP, &d);
      CHECK(!d.doppler, "the PROP preset leaves Doppler off (precondition for the override case)");
      d.doppler = true;                    /* "this prop moves fast" */
      d.gain    = 0.25f;
      d.group   = 3;
      bwa_source s = bwa_source_create(e);
      CHECK(bwa_source_apply(e, s, &d), "apply preset-plus-override");
      CHECK(bwa_source_get_desc(e, s, &back), "get_desc after preset-plus-override");
      CHECK(back.doppler && back.gain == 0.25f && back.group == 3,
            "the overridden fields must land");
      CHECK(back.occlusion && back.reverb && back.proximity,
            "and the preset's own fields must survive the override");
      CHECK(desc_eq(&back, &d), "preset + override must round-trip whole");
      bwa_source_destroy(e, s); }

    /* ---- create_desc == create + apply ---- */
    { bwa_source_desc d, a, b;
      bwa_source_preset(BWA_SRC_VOICE, &d);
      d.reverb_send = 0.4f;
      d.pitch       = 1.25f;
      bwa_source s1 = bwa_source_create_desc(e, &d);
      CHECK(s1 != 0, "create_desc must return a handle");
      bwa_source s2 = bwa_source_create(e);
      CHECK(bwa_source_apply(e, s2, &d), "the long-hand equivalent");
      CHECK(bwa_source_get_desc(e, s1, &a) && bwa_source_get_desc(e, s2, &b),
            "get_desc on both");
      CHECK(desc_eq(&a, &b), "create_desc must equal create-then-apply");
      CHECK(desc_eq(&a, &d), "and both must equal the desc that was passed");
      CHECK(render_blocks(e, 2) == 1, "renders finite with both configured");
      bwa_source_destroy(e, s1);
      bwa_source_destroy(e, s2); }

    /* ---- a fresh source starts at the DEFAULT preset, and a recycled slot does not inherit ---- */
    { bwa_source_desc loud, back, def;
      bwa_source_preset(BWA_SRC_DEFAULT, &def);
      bwa_source_preset(BWA_SRC_AMBIENCE, &loud);
      loud.gain = 0.125f; loud.priority = 7; loud.pathing = true;

      bwa_source s = bwa_source_create(e);
      CHECK(bwa_source_get_desc(e, s, &back), "get_desc on a fresh source");
      CHECK(desc_eq(&back, &def), "a fresh source must read back as BWA_SRC_DEFAULT");
      CHECK(bwa_source_apply(e, s, &loud), "configure it away from the default");
      CHECK(render_blocks(e, 1) == 1, "pump the configuration through");
      bwa_source_destroy(e, s);
      CHECK(!bwa_source_get_desc(e, s, &back), "a destroyed handle must have nothing to report");

      /* the freed slot is at the head of the free list, so the next create almost certainly
       * reuses it — and it must come back as the default, not as the previous occupant */
      bwa_source s2 = bwa_source_create(e);
      CHECK(s2 != 0, "re-create after destroy");
      CHECK(bwa_source_get_desc(e, s2, &back), "get_desc on the recycled slot");
      CHECK(desc_eq(&back, &def), "a recycled slot must not inherit the previous source's config");
      bwa_source_destroy(e, s2); }

    /* ---- a stale handle is DROPPED, not acted on ---- */
    { bwa_source_desc d, back, def;
      bwa_source_preset(BWA_SRC_DEFAULT, &def);
      bwa_source stale = bwa_source_create(e);
      bwa_source_destroy(e, stale);
      CHECK(render_blocks(e, 1) == 1, "let the destroy drain");

      bwa_source live = bwa_source_create(e);      /* very likely the same SLOT, new generation */
      CHECK(live != 0 && live != stale, "the recycled handle must carry a new generation");

      bwa_source_preset(BWA_SRC_AMBIENCE, &d);
      d.gain = 0.03125f; d.priority = 11; d.doppler = true;
      CHECK(bwa_source_apply(e, stale, &d),
            "apply on a stale handle is the documented silent no-op, so it still returns true");
      CHECK(!bwa_source_get_desc(e, stale, &back), "get_desc on a stale handle must be false");
      CHECK(bwa_source_get_desc(e, live, &back), "get_desc on the live occupant");
      CHECK(desc_eq(&back, &def),
            "the stale apply must NOT have reconfigured the slot's current occupant");
      CHECK(render_blocks(e, 2) == 1, "renders finite across the stale-handle traffic");
      bwa_source_destroy(e, live); }

    /* ---- the individual setters and the struct are ONE state, not two ---- */
    { bwa_source s = bwa_source_create(e);
      bwa_source_desc back;
      bwa_source_set_gain(e, s, 0.5f);
      bwa_source_set_pitch(e, s, 2.0f);
      bwa_source_set_priority(e, s, 200);
      bwa_source_set_group(e, s, 2);
      bwa_source_set_spread(e, s, 0.5f);
      bwa_source_set_size(e, s, 1.5f);
      bwa_source_set_doppler(e, s, true);
      bwa_source_set_air_absorption(e, s, true);
      bwa_source_set_loudness_comp(e, s, true);
      bwa_source_set_proximity(e, s, true);
      bwa_source_set_reverb(e, s, true);
      bwa_source_set_reverb_send(e, s, 0.25f);
      bwa_source_set_reverb_distance(e, s, true);
      bwa_source_set_attenuation_override(e, s, 2.f, 1.5f, 0.1f);
      bwa_source_set_directivity(e, s, 0.5f, 2.f);
      CHECK(bwa_source_get_desc(e, s, &back), "get_desc after the individual setters");
      CHECK(back.gain == 0.5f && back.pitch == 2.f && back.priority == 200 && back.group == 2 &&
            back.spread == 0.5f && back.size_m == 1.5f,
            "the struct readback must see what the individual setters set");
      CHECK(back.doppler && back.air_absorption && back.loudness_comp && back.proximity &&
            back.reverb && back.reverb_distance && back.reverb_send == 0.25f,
            "including every per-source effect toggle");
      CHECK(back.atten_ref_dist == 2.f && back.atten_rolloff == 1.5f && back.atten_min_gain == 0.1f,
            "and the attenuation override");
      CHECK(back.directivity_weight == 0.5f && back.directivity_power == 2.f,
            "and the directivity pattern");
      CHECK(bwa_source_apply(e, s, &back), "that readback must round-trip");
      { bwa_source_desc again;
        CHECK(bwa_source_get_desc(e, s, &again), "get_desc after the round-trip");
        CHECK(desc_eq(&again, &back), "setters -> get_desc -> apply must be a no-op"); }

      /* the extent/spread pair is one field pair on both sides: an extent sets the height, a
       * scalar spread resets it to isotropic (last call wins) */
      bwa_source_set_extent(e, s, 0.8f, 0.2f);
      CHECK(bwa_source_get_desc(e, s, &back) && back.spread == 0.8f && back.extent_height == 0.2f,
            "an extent must read back as width + height");
      bwa_source_set_spread(e, s, 0.3f);
      CHECK(bwa_source_get_desc(e, s, &back) && back.spread == 0.3f && back.extent_height < 0.f,
            "a scalar spread must reset the readback to isotropic");
      CHECK(render_blocks(e, 2) == 1, "renders finite after the setter traffic");
      bwa_source_destroy(e, s); }

    /* ---- fade_to is a gain setting, fade_out is a stop ---- */
    { bwa_source s = bwa_source_create(e);
      bwa_source_desc back;
      bwa_source_set_gain(e, s, 1.f);
      bwa_source_fade_to(e, s, 0.25f, 0.5f);
      CHECK(bwa_source_get_desc(e, s, &back) && back.gain == 0.25f,
            "a fade in flight reads back as its TARGET");
      bwa_source_fade_out(e, s, 0.5f);
      CHECK(bwa_source_get_desc(e, s, &back) && back.gain == 0.25f,
            "fade_out is a stop, not a gain setting: it must leave the configured level alone");
      bwa_source_destroy(e, s); }

    /* ---- a bed is a voice, so the same struct describes one (position-class fields aside) ---- */
    { bwa_bed b = bwa_bed_create(e);
      bwa_source_desc d, back;
      bwa_source_preset(BWA_SRC_AMBIENCE, &d);
      d.gain = 0.75f;
      CHECK(bwa_source_apply(e, b, &d), "apply on a bed handle");
      CHECK(bwa_source_get_desc(e, b, &back) && back.gain == 0.75f, "and read it back");
      bwa_bed_destroy(e, b); }

    CHECK(render_blocks(e, 4) == 1, "renders finite at the end of the run");
    bwa_stop(e);
    bwa_destroy(e);

    /* the preset is pure, so it still answers after the engine is gone */
    { bwa_source_desc d;
      bwa_source_preset(BWA_SRC_UI, &d);
      CHECK(d.struct_size == (uint32_t)sizeof(bwa_source_desc) && d.priority == 255,
            "bwa_source_preset must not depend on an engine existing"); }

    printf("source_desc_test: %s (%d failure%s)\n", fails ? "FAIL" : "ok", fails,
           fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
