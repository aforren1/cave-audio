/* hpeq.c — AutoEq ParametricEQ parser + the stereo biquad cascade. See hpeq.h. */
#include "hpeq.h"
#include "biquad.h"

#include <stdio.h>
#include <string.h>

/* uppercase a line in place so the token matching is case-insensitive (numbers unaffected) */
static void s_upper(char* s) {
    for (; *s; ++s) if (*s >= 'a' && *s <= 'z') *s = (char)(*s - 'a' + 'A');
}

static void s_err(char* err, size_t cap, const char* msg, int lineno) {
    if (err && cap) snprintf(err, cap, "headphone EQ: %s (line %d)", msg, lineno);
}

int hpeq_parse(const char* path, uint32_t sample_rate, HpEqDesign* d, char* err, size_t errcap) {
    memset(d, 0, sizeof *d);
    d->preamp = 1.0f;
    FILE* f = fopen(path, "rb");
    if (!f) {
        if (err && errcap) snprintf(err, errcap, "headphone EQ: cannot open %s", path);
        return 0;
    }
    char line[256];
    int lineno = 0;
    while (fgets(line, sizeof line, f)) {
        ++lineno;
        s_upper(line);
        const char* p = line + strspn(line, " \t");
        if (strncmp(p, "PREAMP:", 7) == 0) {
            float db = 0.f;
            if (sscanf(p + 7, "%f", &db) == 1) d->preamp = powf(10.0f, db / 20.0f);
            continue;
        }
        if (strncmp(p, "FILTER", 6) != 0) continue;          /* lenient: unknown lines skipped */
        const char* colon = strchr(p, ':');
        if (!colon) continue;
        char state[8] = {0}, type[12] = {0};
        float fc = 0.f, gain = 0.f, q = 0.707f;
        int got = sscanf(colon + 1, " %7s %11s FC %f HZ GAIN %f DB Q %f",
                         state, type, &fc, &gain, &q);
        if (got >= 1 && strcmp(state, "OFF") == 0) continue; /* disabled filter: dropped */
        if (got < 4) {                                       /* a Filter line that doesn't parse is a
                                                              * wrong-format file — fail LOUDLY, not
                                                              * with a silently partial correction */
            s_err(err, errcap, "malformed Filter line", lineno);
            fclose(f); return 0;
        }
        int bt;
        if      (strcmp(type, "PK") == 0 || strcmp(type, "PEQ") == 0) bt = BWA_BIQUAD_PEAK;
        else if (strcmp(type, "LSC") == 0 || strcmp(type, "LS") == 0) bt = BWA_BIQUAD_LOWSHELF;
        else if (strcmp(type, "HSC") == 0 || strcmp(type, "HS") == 0) bt = BWA_BIQUAD_HIGHSHELF;
        else { s_err(err, errcap, "unknown filter type", lineno); fclose(f); return 0; }
        if (fc <= 0.f || q <= 0.f) { s_err(err, errcap, "non-positive Fc/Q", lineno); fclose(f); return 0; }
        if (fc >= 0.49f * (float)sample_rate) continue;      /* at/above Nyquist: skip (header) */
        if (d->nsec >= BWA_HPEQ_MAX_SEC) {
            s_err(err, errcap, "too many filters", lineno);
            fclose(f); return 0;
        }
        bwa_biquad_rbj_hz(bt, fc, q, gain, (double)sample_rate, d->sec[d->nsec]);
        ++d->nsec;
    }
    fclose(f);
    if (d->nsec == 0) {                                      /* nothing parsed (a preamp alone is
                                                              * not a correction): wrong file */
        if (err && errcap) snprintf(err, errcap, "headphone EQ: no filters in %s", path);
        return 0;
    }
    return 1;
}

void hpeq_state_reset(HpEqState* st) { memset(st, 0, sizeof *st); }

void hpeq_apply(const HpEqDesign* d, HpEqState* st, float* s, uint32_t n, float mix_tgt) {
    if (n == 0) return;
    const float step = (mix_tgt - st->mix) / (float)n;
    for (int ch = 0; ch < 2; ++ch) {
        float mix = st->mix;
        float* p  = s + (size_t)ch * n;
        float* x1 = st->x1[ch]; float* x2 = st->x2[ch];
        float* y1 = st->y1[ch]; float* y2 = st->y2[ch];
        for (uint32_t i = 0; i < n; ++i) {
            const float dry = p[i];
            float v = dry * d->preamp;
            for (int k = 0; k < d->nsec; ++k) {
                const float* c = d->sec[k];
                const float y = c[0]*v + c[1]*x1[k] + c[2]*x2[k] - c[3]*y1[k] - c[4]*y2[k];
                x2[k] = x1[k]; x1[k] = v;
                y2[k] = y1[k]; y1[k] = y;
                v = y;
            }
            p[i] = dry + mix * (v - dry);                    /* dry/wet: the ramped A/B + load swap */
            mix += step;
        }
    }
    st->mix = mix_tgt;                                       /* land exactly (invariant 4) */
}
