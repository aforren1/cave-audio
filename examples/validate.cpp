/*
 * validate.cpp — bwa_validate: measure where the array actually puts a phantom source.
 *
 * Render a source in a known direction, capture it with the ZM-1 at a known listening position,
 * estimate the direction of arrival, report the angular miss. Sweep that over directions, over
 * panners, over tracked-vs-fixed rendering, over the engine's live A/B knobs, and over listener
 * positions, and you have graded the layout, the panner and the calibration with numbers instead of
 * opinions. The phantom arm renders through a REAL ENGINE CORE (valid.h), which is what makes the
 * knobs sweepable at all. See docs/validation.md.
 *
 * THE SESSION SHAPE, and why it is cheap. Phantom sources are rendered, not carried, so a whole
 * direction grid sweeps electronically from one microphone placement. Only the MICROPHONE moves.
 * That inverts the usual cost of this measurement: a study that moves a physical reference speaker
 * needs one session per direction, where this needs one per listening position and gets every
 * direction for free.
 *
 *   for each microphone placement (you move the ZM-1, the tool waits):
 *       check the capsules once, report anything faulty, exclude it for the rest of the placement
 *       for each condition x {tracked, fixed} x direction:  render -> capture -> score
 *
 * ONE session loop, two capture backends. The simulated path is not a shortcut around the hardware
 * path — it is the same loop with the analytic field substituted for the device, so --simulate
 * exercises the capsule check, the exclusion threading and the reporting exactly as the rig will run
 * them. Only the ~20 lines that actually talk to ASIO go untested, which is the irreducible part.
 *
 * Usage:
 *   bwa_validate --simulate                          the whole flow, no hardware
 *   bwa_validate --simulate --inject-fault 7         prove the integrity layer catches a bad capsule
 *   bwa_validate --driver "ASIO MADIface USB" --mic-in 26
 *   bwa_validate --layout cave_layout.json --positions mics.txt --out cells.csv
 */
/* valid.h / layout.h / zylia.h are C headers with no extern "C" of their own (the codebase keeps
 * them that way and wraps at the C++ call site — see calib_capture.h). */
extern "C" {
#include "valid.h"
#include "natnet.h"
}
#include "valid_capture.h"
#include "bw_audio.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>          /* Sleep, while waiting for a tracker pose */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_LIS   64
/* 36 azimuths x 3 elevations of grid, PLUS one speaker-matched target per channel for the physical
 * reference arm. The per-placement scratch arrays below are sized off this, so it has to cover both. */
#define MAX_TGT   (36 * 3 + BWA_CHANNELS)
#define NPAN      3
#define MAX_FOCUS 6
#define MAX_LIST  6        /* values per swept knob (--focus, --hole-spread, --near-spread) */
#define MAX_COND  96       /* measured conditions; a factorial past this is refused, with the count */
#define LBL       32
#define CLBL      48       /* condition-label width */

static const char* pan_name(int p) {
    return p == BWA_PAN_DBAP ? "DBAP" : (p == BWA_PAN_SPCAP ? "SPCAP" : "VBAP");
}

/* One measured CONDITION: a panner plus the render settings it is measured at (ValidRender — the
 * live engine knobs, valid.h). The phantom arm renders through a real engine core, so every one of
 * them reaches the speaker feeds and can be swept.
 *
 * Focus is inert under DBAP and VBAP, so those two never get more than one condition per focus value
 * however long the --focus list is — sweeping them would re-measure an identical cell and cost rig
 * time for nothing. The settings stored here are already RESOLVED (the <= 0 focus sentinel is applied
 * in main), so they compare field-for-field against what valid_score wrote into each cell, which is
 * how the reports below sort cells back into conditions. */
typedef struct { int panner; ValidRender r; } Cond;

static void tag_add(char* k, size_t cap, const char* s) {
    size_t at = strlen(k);
    if (at && at + 1 < cap) { k[at++] = '+'; k[at] = 0; }
    snprintf(k + at, cap - at, "%s", s);
}

/* "SPCAP f 12.70 dual+cap" / "DBAP          -". Two columns after the panner: the SPCAP focus, and
 * the knobs that are ON. The focus column is BLANK under DBAP/VBAP, because a number printed beside
 * a panner with no lobe would read as a setting that did something. A row with nothing on reads "-",
 * so the baseline is never confused with a truncated label. */
static const char* cond_label(const Cond* c, char* buf, int cap) {
    char f[12], k[64], t[24];
    k[0] = 0;
    if (c->panner == BWA_PAN_SPCAP) snprintf(f, sizeof f, "f%6.2f", c->r.focus);
    else                            snprintf(f, sizeof f, "       ");
    if (c->r.dual_band)             tag_add(k, sizeof k, c->r.cap ? "dual+cap" : "dual");
    if (c->r.hole_spread > 0.f)   { snprintf(t, sizeof t, "hole%.2f", c->r.hole_spread); tag_add(k, sizeof k, t); }
    if (c->r.tracked_align)         tag_add(k, sizeof k, "talign");
    if (c->r.spread_mode == 1)      tag_add(k, sizeof k, "mdap");
    else if (c->r.spread_mode == 2) tag_add(k, sizeof k, "spectral");
    if (c->r.decorrelation)         tag_add(k, sizeof k, "decorr");
    if (c->r.near_spread > 0.f)   { snprintf(t, sizeof t, "near%.2f", c->r.near_spread); tag_add(k, sizeof k, t); }
    if (c->r.spread > 0.f)        { snprintf(t, sizeof t, "spr%.2f", c->r.spread); tag_add(k, sizeof k, t); }
    if (!k[0]) snprintf(k, sizeof k, "-");
    snprintf(buf, (size_t)cap, "%-5s %s %-20s", pan_name(c->panner), f, k);
    return buf;
}

/* Append one condition, or count it as overflow. Split out because the builder in main emits from
 * eight places and a silently truncated condition list would be a measured session missing a row.
 *
 * DUPLICATES ARE DROPPED, and a factorial needs that: CAP implies dual-band, so the cross product
 * asks for (dual off, cap on) and gets (dual on, cap on), which some other cell of the product
 * already covers. A duplicate condition would measure the same cells twice and then match both of
 * them in every report below. */
static void cond_emit(Cond* cond, int* n, int* over, int panner, const ValidRender* r) {
    for (int i = 0; i < *n; ++i)
        if (cond[i].panner == panner && valid_render_equal(&cond[i].r, r)) return;
    if (*n < MAX_COND) { cond[*n].panner = panner; cond[*n].r = *r; ++(*n); }
    else ++(*over);
}

/* Does this cell belong to this condition? Panner plus every render setting, field for field. */
static int cell_in_cond(const ValidCell* c, const Cond* q) {
    return c->panner == q->panner && valid_render_equal(&c->render, &q->r);
}

/* ---- capture backends behind one shape ------------------------------------------------------ */

typedef struct {
    const Layout* L;
    float*        feeds;        /* scratch for the hardware path: [nspk][VAL_CAPLEN] */
    int           inject;       /* capsule to corrupt, or -1 — a self-check, see --inject-fault */
    unsigned int  rng;
} CapCtx;

typedef int (*CaptureFn)(CapCtx*, const Cond* q, const float solve[3], const float mic[3],
                         const float src[3], float* cap19);

/* Corrupt one capsule the way a real fault does: broadband self-noise, well above the array's own
 * level, at a capsule that is otherwise fine. Total array power still looks healthy, which is the
 * whole reason zylia_check_capsules has to look at the raw signals. Scaling the capture down first
 * leaves headroom so this reads as HOT rather than merely CLIPPED. */
static void inject_fault(CapCtx* ctx, float* cap19, int ch) {
    if (ch < 0 || ch >= ZYLIA_MICS) return;
    double s = 0.0;
    for (uint32_t i = 0; i < VAL_ANALYZE; ++i) s += (double)cap19[i] * cap19[i];
    double rms = sqrt(s / VAL_ANALYZE);
    for (size_t i = 0; i < (size_t)ZYLIA_MICS * VAL_ANALYZE; ++i) cap19[i] *= 0.03f;
    double amp = 20.0 * rms * 0.03 * 1.732;              /* ~26 dB over the array, uniform noise */
    for (uint32_t i = 0; i < VAL_ANALYZE; ++i) {
        ctx->rng = ctx->rng * 1664525u + 1013904223u;
        cap19[(size_t)ch * VAL_ANALYZE + i] =
            (float)(((double)(int)(ctx->rng >> 9) / (double)(1 << 22) - 1.0) * amp);
    }
}

static int cap_simulate(CapCtx* ctx, const Cond* q, const float solve[3], const float mic[3],
                        const float src[3], float* cap19) {
    if (!valid_simulate(ctx->L, q->panner, &q->r, solve, mic, src, VAL_FS, 343.0,
                        cap19, VAL_ANALYZE)) return 0;
    if (ctx->inject >= 0) inject_fault(ctx, cap19, ctx->inject);
    return 1;
}

#ifdef BWA_HAVE_ASIO
static int cap_asio(CapCtx* ctx, const Cond* q, const float solve[3], const float mic[3],
                    const float src[3], float* cap19) {
    (void)mic;                                            /* the room decides what the mic hears */
    if (!valid_speaker_feeds(ctx->L, q->panner, &q->r, solve, src, VAL_FS,
                             ctx->feeds, VAL_CAPLEN)) return 0;
    if (!valid_asio_capture(ctx->feeds, cap19)) return 0;
    if (ctx->inject >= 0) inject_fault(ctx, cap19, ctx->inject);
    return 1;
}
#endif

/* ---- listener placements -------------------------------------------------------------------- */

/* the default walking envelope: the sweet spot, three horizontal steps, and — separately, because it
 * is a different failure mode — a height sweep. See docs/validation.md. */
static int default_listeners(float (*out)[3], char (*names)[LBL], const float ref[3]) {
    const float dx = 0.7f, dy = 0.4f;
    struct { float p[3]; const char* n; } d[] = {
        {{ ref[0],      ref[1],      ref[2]      }, "sweet spot"     },
        {{ ref[0]+dx,   ref[1],      ref[2]      }, "right +0.7"     },
        {{ ref[0]-dx,   ref[1],      ref[2]      }, "left -0.7"      },
        {{ ref[0],      ref[1],      ref[2]+dx   }, "back +0.7"      },
        {{ ref[0],      ref[1]-dy,   ref[2]      }, "seated -0.4"    },
        {{ ref[0],      ref[1]+dy,   ref[2]      }, "tall +0.4"      },
        {{ ref[0]+0.6f, ref[1]+dy,   ref[2]+0.5f }, "tall + off-axis"},
    };
    int n = (int)(sizeof d / sizeof d[0]);
    for (int i = 0; i < n; ++i) {
        memcpy(out[i], d[i].p, sizeof d[i].p);
        snprintf(names[i], LBL, "%s", d[i].n);
    }
    return n;
}

static int parse_xyz(const char* s, float out[3]) {
    return (sscanf(s, "%f,%f,%f", &out[0], &out[1], &out[2]) == 3) ||
           (sscanf(s, "%f %f %f", &out[0], &out[1], &out[2]) == 3);
}

/* One placement per line: "x y z [label]" or "x,y,z [label]". Blank lines and # comments skipped.
 * MEASURE these — the mic position is an input to the scoring, not a caption. */
static int load_positions(const char* path, float (*out)[3], char (*names)[LBL], int cap, int at) {
    FILE* f = fopen(path, "r");
    if (!f) { fprintf(stderr, "cannot read %s\n", path); return -1; }
    char line[256];
    int n = at;
    while (fgets(line, sizeof line, f)) {
        char* p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p == '#' || *p == '\n' || *p == '\r' || !*p) continue;
        if (n >= cap) { fprintf(stderr, "too many placements (max %d)\n", cap); fclose(f); return -1; }
        float v[3];
        if (!parse_xyz(p, v)) { fprintf(stderr, "cannot parse placement: %s", line); fclose(f); return -1; }
        memcpy(out[n], v, sizeof v);
        /* an optional trailing label makes the report readable; skip the three numbers first */
        int skipped = 0;
        while (*p && skipped < 3) {
            while (*p == ' ' || *p == '\t' || *p == ',') ++p;
            while (*p && *p != ' ' && *p != '\t' && *p != ',' && *p != '\n') ++p;
            ++skipped;
        }
        while (*p == ' ' || *p == '\t' || *p == ',') ++p;
        /* the label lands in a CSV field, so a comma in it would shift every later column */
        for (char* q = p; *q; ++q) if (*q == ',' || *q == '"') *q = ' ';
        char* e = p + strlen(p);
        while (e > p && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ')) --e;
        *e = 0;
        if (*p) snprintf(names[n], LBL, "%s", p);
        else    snprintf(names[n], LBL, "pos %d", n);
        ++n;
    }
    fclose(f);
    return n;
}

/* ---- tracked mount: read where the microphone actually is -------------------------------------
 *
 * A typed placement is a tape-measure number, and at the 1.4 m source radius a 5 cm error injects
 * ~2 deg of direction error — the same size as the phantom penalties being measured. If the stand is
 * a tracked rigid body, both the position AND the mount orientation become measurements instead.
 *
 * The survey has to be a BODY-FRAME one (zylia.h): capsules in the stand's axes plus the probed
 * offset from the stand's body origin to the array's acoustic center. Then per placement:
 *   capsules_room = R(pose) . capsules_body      and      center = pose_pos + R(pose) . offset
 *
 * This is the easy use of the tracker — the mic is static during a capture, so one good pose per
 * placement is enough. No prediction, no velocity, no clock domain. A wrong reading is also obvious
 * rather than subtle, which is why the planned position is printed next to the measured one. */
typedef struct {
    NatNet*    nn;                          /* NULL => the synthetic self-check pose, see --track-sim */
    float      caps_body[ZYLIA_MICS][3];
    ZyliaMount mount;
    /* The PLANNED placements, kept in their own storage. They must not alias the array `place` writes
     * into: the call site passes lis[li] as the out-param, so reading the plan back out of lis after
     * writing it would compare the tracked position against itself, silently zeroing the delta and
     * disarming the wrong-rigid-body warning below. */
    float      planned[MAX_LIS][3];
} TrackCtx;

/* Apply one mount pose: re-aim the capsule table and derive the array center. Split out so the
 * synthetic self-check pose and a real tracker pose go through EXACTLY the same maths. */
static void track_apply(TrackCtx* T, const float p[3], const float q[4], int li, float mic_out[3]) {
    float R[9], caps_room[ZYLIA_MICS][3];
    zylia_quat_to_matrix(q, R);
    zylia_capsules_rotate(T->caps_body, R, 0, caps_room);
    zylia_set_capsules(caps_room);                       /* the array as it is turned RIGHT NOW */
    const float* o = T->mount.offset_m;
    float plan[3] = { T->planned[li][0], T->planned[li][1], T->planned[li][2] };
    mic_out[0] = p[0] + R[0]*o[0] + R[1]*o[1] + R[2]*o[2];
    mic_out[1] = p[1] + R[3]*o[0] + R[4]*o[1] + R[5]*o[2];
    mic_out[2] = p[2] + R[6]*o[0] + R[7]*o[1] + R[8]*o[2];
    double d = sqrt((double)(mic_out[0]-plan[0])*(mic_out[0]-plan[0]) +
                    (double)(mic_out[1]-plan[1])*(mic_out[1]-plan[1]) +
                    (double)(mic_out[2]-plan[2])*(mic_out[2]-plan[2]));
    printf("  tracked: (%.3f, %.3f, %.3f)  planned (%.2f, %.2f, %.2f)  delta %.0f mm\n",
           mic_out[0], mic_out[1], mic_out[2], plan[0], plan[1], plan[2], d * 1000.0);
    if (d > 0.5) printf("  WARNING: half a meter from the plan - right rigid body? right frame?\n");
}

static int track_place(void* user, int li, float mic_out[3]) {
    TrackCtx* T = (TrackCtx*)user;
    if (!T->nn) {                                        /* --track-sim: a fixed synthetic pose */
        const float p[3] = { 0.05f, 0.02f, -0.03f };
        const float q[4] = { 0.0f, 0.3826834f, 0.0f, 0.9238795f };   /* 45 deg yaw */
        track_apply(T, p, q, li, mic_out);
        return 1;
    }
    float p[3], q[4];
    for (int tries = 0; tries < 200; ++tries) {           /* ~2 s for a live pose to show up */
        /* pose_read alone is NOT enough. It returns the last PUBLISHED pose forever, and natnet only
         * publishes tracking-valid frames, so from the second placement on an occluded stand or a
         * wrong streaming id would hand back the PREVIOUS placement's pose and be accepted as this
         * one's measurement. Gate on liveness, which is what natnet_status is for. */
        if (natnet_status(T->nn) == NN_STATUS_LIVE && pose_read(natnet_pose(T->nn), p, q)) {
            track_apply(T, p, q, li, mic_out);
            return 1;
        }
        Sleep(10);
    }
    fprintf(stderr, "  no LIVE pose for the tracked rigid body (occluded, wrong id, or Motive not "
                    "streaming) - refusing to reuse a stale one\n");
    return 0;
}

/* ---- the session ---------------------------------------------------------------------------- */

/* Sweep every placement. Identical for both backends — that is the point: the hardware run executes
 * exactly the code --simulate already proved.
 * `place` (optional) resolves each placement's microphone position before its cells are measured, and
 * for a tracked mount also re-aims the capsule table. A placement it cannot resolve is SKIPPED, so
 * the caller's accounting has to come from what actually happened rather than from nlis: `nchecked`
 * counts placements that were measured, `nflagged` those where the injected capsule was caught. */
static int run_session(const Layout* L, CaptureFn cap, CapCtx* ctx,
                       const Cond* cond, int ncond,
                       float (*lis)[3], char (*lisname)[LBL], int nlis,
                       float (*tg)[3], int ntgt, int ngrid, float radius, int do_ref,
                       int prompt, int (*place)(void*, int, float[3]), void* place_user,
                       ValidCell* cells, int* nchecked, int* nflagged, int* nplaced) {
    float* cap19 = (float*)malloc(sizeof(float) * (size_t)ZYLIA_MICS * VAL_ANALYZE);
    if (!cap19) { fprintf(stderr, "out of memory\n"); return 0; }
    int w = 0;
    if (nflagged) *nflagged = 0;
    if (nchecked) *nchecked = 0;
    if (nplaced) *nplaced = 0;

    for (int li = 0; li < nlis; ++li) {
        unsigned char flags[ZYLIA_MICS] = { 0 };
        int checked = 0;
        printf("\n--- placement %d/%d: %s (%.2f, %.2f, %.2f) ---\n",
               li + 1, nlis, lisname[li], lis[li][0], lis[li][1], lis[li][2]);
        if (prompt) {
            printf(place ? "Move the ZM-1 to roughly there, then press ENTER (the tracker measures it).\n"
                         : "Place the ZM-1 there, then press ENTER (measure the position, do not eyeball it).\n");
            int c; while ((c = getchar()) != '\n' && c != EOF) { }
        }
        /* With a tracked mount the typed placement is only the PLAN: the pose is the measurement, and
         * it also re-aims the capsule table for however the stand ended up turned. */
        if (place) {
            if (!place(place_user, li, lis[li])) {
                fprintf(stderr, "  skipping placement %d/%d (no usable pose)\n", li + 1, nlis);
                continue;
            }
            if (nplaced) ++(*nplaced);            /* counts INVOCATIONS: 0 means the hook is unwired */
        }

        /* The physical baseline: drive each speaker alone. No panner, so this is measured once per
         * placement rather than per panner/mode. It is also the fastest possible sanity check on the
         * whole chain — if a directly driven speaker does not land on its surveyed position, nothing
         * measured afterwards at this placement means anything. */
        if (do_ref) {
            for (uint32_t sp = 0; sp < L->count; ++sp) {
                ValidCell* c = &cells[w];
                if (!valid_reference_cell(L, (int)sp, lis[li], VAL_FS, 343.0, VAL_ANALYZE, c)) {
                    memset(c, 0, sizeof *c); c->reference = 1; c->tgt = (int)sp;
                }
                c->lis = li;
                ++w;
            }
            double rm[BWA_CHANNELS]; int nr = 0;
            for (int i = 0; i < w; ++i)
                if (cells[i].lis == li && cells[i].reference == 1 && cells[i].ok) rm[nr++] = cells[i].miss_deg;
            if (nr) {
                double med = valid_median(rm, nr), wmax = 0.0;
                for (int i = 0; i < nr; ++i) if (rm[i] > wmax) wmax = rm[i];
                printf("  physical reference: median %.2f deg, worst %.2f deg over %d speakers%s\n",
                       med, wmax, nr, (med > 5.0) ? "   <-- SUSPECT, check layout/survey" : "");
            }
            /* The comb FLOOR. One speaker alone radiates one coherent copy, so whatever ripple this
             * shows is the stimulus's line structure, the analysis, and the room — never interference.
             * Every phantom comb depth below is only meaningful as an excess over it. */
            double rc[BWA_CHANNELS]; int nrc = 0;
            for (int i = 0; i < w; ++i)
                if (cells[i].lis == li && cells[i].reference == 1 && cells[i].comb_ok) rc[nrc++] = cells[i].comb_db;
            if (nrc) printf("  comb floor: %.2f dB over %d speakers driven alone (no interference to comb)\n",
                            valid_median(rc, nrc), nrc);
        }

        /* The MATCHED phantom for each reference: a rendered source at that speaker's own position.
         * Same direction, same room, same placement, so the pair differences cleanly. It is not
         * degenerate — a distance-blurred panner still spreads a source that sits on a speaker, and
         * how much it spreads is exactly the rendering cost being asked about. */
        if (do_ref)
            for (int ci = 0; ci < ncond; ++ci)
                for (int tracked = 1; tracked >= 0; --tracked)
                    for (uint32_t sp = 0; sp < L->count; ++sp) {
                        const float* q = L->speakers[sp].pos;
                        float src[3] = { q[0], q[1], q[2] };
                        const float* solve = tracked ? lis[li] : L->ref;
                        ValidCell* c = &cells[w];
                        int got = 0;
                        if (cap(ctx, &cond[ci], solve, lis[li], src, cap19))
                            got = valid_score(L, cond[ci].panner, &cond[ci].r, tracked, lis[li], src,
                                              cap19, VAL_ANALYZE, VAL_FS, 343.0, flags, c);
                        if (!got) {
                            memset(c, 0, sizeof *c);
                            c->panner = cond[ci].panner; c->tracked = tracked;
                            c->render = cond[ci].r;
                        }
                        c->lis = li; c->tgt = (int)sp; c->reference = 2;
                        ++w;
                    }

        for (int ci = 0; ci < ncond; ++ci)
            for (int tracked = 1; tracked >= 0; --tracked)
                for (int t = 0; t < ntgt; ++t) {
                    (void)ngrid;
                    float src[3] = { L->ref[0] + radius*tg[t][0],
                                     L->ref[1] + radius*tg[t][1],
                                     L->ref[2] + radius*tg[t][2] };
                    const float* solve = tracked ? lis[li] : L->ref;
                    ValidCell* c = &cells[w];
                    int got = 0;

                    if (cap(ctx, &cond[ci], solve, lis[li], src, cap19)) {
                        /* ONE capsule check per placement, on its first capture: a fault is a
                         * property of the session, and re-checking every cell would only cost time.
                         * Anything flagged is excluded from every cell that follows. */
                        if (!checked) {
                            const float* ptr[ZYLIA_MICS];
                            for (int j = 0; j < ZYLIA_MICS; ++j) ptr[j] = cap19 + (size_t)j * VAL_ANALYZE;
                            int nb = zylia_check_capsules(ptr, VAL_ANALYZE, flags);
                            if (nb > 0) {
                                printf("  capsule check: %d FAULTY  - ", nb);
                                for (int j = 0; j < ZYLIA_MICS; ++j)
                                    if (flags[j]) printf(" ch%d(0x%02X)", j, flags[j]);
                                printf("  (excluded for this placement)\n");
                            } else printf("  capsule check: all %d healthy\n", ZYLIA_MICS);
                            if (nflagged && ctx->inject >= 0 && flags[ctx->inject]) ++(*nflagged);
                            if (nchecked) ++(*nchecked);
                            checked = 1;
                        }
                        got = valid_score(L, cond[ci].panner, &cond[ci].r, tracked, lis[li], src,
                                          cap19, VAL_ANALYZE, VAL_FS, 343.0, flags, c);
                    }
                    if (!got) {
                        memset(c, 0, sizeof *c);
                        c->panner = cond[ci].panner; c->tracked = tracked;
                        c->render = cond[ci].r;
                    }
                    c->lis = li; c->tgt = t;
                    ++w;
                }

        /* per-placement summary, so a bad placement is visible before you move the mic again.
         * Comb depth sits beside the angular miss because they are different failures: a render can
         * aim perfectly and still comb the timbre to pieces, and only one of the two is audible as a
         * direction error. */
        for (int ci = 0; ci < ncond; ++ci) {
            static double mt[MAX_TGT], mf[MAX_TGT], ct[MAX_TGT], cf[MAX_TGT];
            int nt = 0, nf = 0, nct = 0, ncf = 0;
            for (int i = 0; i < w; ++i) {
                ValidCell* c = &cells[i];
                if (c->lis != li || c->reference || !cell_in_cond(c, &cond[ci])) continue;
                if (c->ok)      { if (c->tracked) mt[nt++]  = c->miss_deg; else mf[nf++]  = c->miss_deg; }
                if (c->comb_ok) { if (c->tracked) ct[nct++] = c->comb_db;  else cf[ncf++] = c->comb_db; }
            }
            if (!nt || !nf) continue;
            char cl[CLBL];
            printf("  %-36s miss tracked %5.1f  fixed %5.1f deg", cond_label(&cond[ci], cl, sizeof cl),
                   valid_median(mt, nt), valid_median(mf, nf));
            if (nct && ncf)
                printf("   comb tracked %5.2f  fixed %5.2f dB", valid_median(ct, nct), valid_median(cf, ncf));
            printf("   (n=%d/%d)\n", nt, nf);
        }
    }
    free(cap19);
    return w;
}

/* off|on|both -> {0} / {1} / {0,1}. Element 0 is the baseline, so "both" always measures OFF as the
 * baseline and ON as the one-knob variant. */
static int parse_onoff(const char* s, int* out, int* n) {
    if (!strcmp(s, "off"))  { out[0] = 0; *n = 1; return 1; }
    if (!strcmp(s, "on"))   { out[0] = 1; *n = 1; return 1; }
    if (!strcmp(s, "both")) { out[0] = 0; out[1] = 1; *n = 2; return 1; }
    return 0;
}

/* "1,2.5,4" -> a float list. Same shape as --focus, and the same rule: element 0 is the baseline. */
static int parse_flist(const char* s, float* out, int cap, int* n, int allow_zero) {
    int w = 0;
    while (*s && w < cap) {
        char* endp = NULL;
        double v = strtod(s, &endp);
        if (endp == s) return 0;
        if (!(v > 0.0) && !allow_zero) return 0;
        if (v < 0.0) return 0;
        out[w++] = (float)v;
        s = endp;
        while (*s == ',' || *s == ' ') ++s;
    }
    if (!w || *s) return 0;
    *n = w;
    return 1;
}

int main(int argc, char** argv) {
    const char* layout_path = NULL;
    const char* driver = NULL;
    const char* csv = NULL;
    const char* posfile = NULL;
    int simulate = 0, mic_in = 0, naz = 12, inject = -1, no_prompt = 0;
    const char* track_body = NULL;   /* rigid-body id or name for the ZM-1's stand */
    const char* survey_path = NULL;  /* body-frame capsule survey to follow it with */
    const char* nn_server = NULL;
    const char* nn_multicast = "239.255.42.99";
    int track_sim = 0;               /* synthetic mount pose: exercises the tracked path, no rig */
    int do_ref = 1;                  /* physical reference arm: each speaker driven alone */
    float radius = 1.4f;
    float focus_req[MAX_FOCUS] = { 0.f };   /* 0 = the array's derived default, resolved below */
    int   nfocus = 1;
    float density = 0.f;             /* 0 = the layout's own */
    /* The swept knob axes. Element 0 of each is the BASELINE value, and by default only element 0
     * appears in combination with the others: see the condition builder below. */
    int   ax_db[2] = { 0, 0 };  int n_db = 1;      /* dual-band */
    int   ax_cp[2] = { 0, 0 };  int n_cp = 1;      /* CAP on the dual-band low band */
    int   ax_ta[2] = { 0, 0 };  int n_ta = 1;      /* tracked alignment */
    int   ax_dc[2] = { 0, 0 };  int n_dc = 1;      /* decorrelation */
    int   ax_sm[3] = { 0, 0, 0 }; int n_sm = 1;    /* spread mode */
    float ax_hs[MAX_LIST] = { 0.f }; int n_hs = 1; /* hole-aware spread floor strength */
    float ax_ns[MAX_LIST] = { 0.f }; int n_ns = 1; /* near-listener widening radius */
    float spread = 0.f;              /* the source's own width; the spread knobs act on this */
    int   factorial = 0;             /* opt in to the full cross product (see the builder) */
    static float lis[MAX_LIS][3];
    static char  lisname[MAX_LIS][LBL];
    int nlis_cli = 0;

    for (int i = 1; i < argc; ++i) {
        if      (!strcmp(argv[i], "--simulate"))                    simulate = 1;
        else if (!strcmp(argv[i], "--no-prompt"))                   no_prompt = 1;
        else if (!strcmp(argv[i], "--layout")    && i+1 < argc)     layout_path = argv[++i];
        else if (!strcmp(argv[i], "--driver")    && i+1 < argc)     driver = argv[++i];
        else if (!strcmp(argv[i], "--out")       && i+1 < argc)     csv = argv[++i];
        else if (!strcmp(argv[i], "--positions") && i+1 < argc)     posfile = argv[++i];
        else if (!strcmp(argv[i], "--mic-in")    && i+1 < argc)     mic_in = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--azimuths")  && i+1 < argc)     naz = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--inject-fault") && i+1 < argc)  inject = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--radius")    && i+1 < argc)     radius = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--track")     && i+1 < argc)     track_body = argv[++i];
        else if (!strcmp(argv[i], "--survey")    && i+1 < argc)     survey_path = argv[++i];
        else if (!strcmp(argv[i], "--natnet-server") && i+1 < argc) nn_server = argv[++i];
        else if (!strcmp(argv[i], "--natnet-multicast") && i+1 < argc) nn_multicast = argv[++i];
        else if (!strcmp(argv[i], "--track-sim"))                   track_sim = 1;
        else if (!strcmp(argv[i], "--no-reference"))                do_ref = 0;
        else if (!strcmp(argv[i], "--density")   && i+1 < argc)     density = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--factorial"))                   factorial = 1;
        else if (!strcmp(argv[i], "--spread")    && i+1 < argc)     spread = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--dual-band") && i+1 < argc) {
            if (!parse_onoff(argv[++i], ax_db, &n_db)) { fprintf(stderr, "--dual-band wants off|on|both\n"); return 2; }
        }
        else if (!strcmp(argv[i], "--cap") && i+1 < argc) {
            if (!parse_onoff(argv[++i], ax_cp, &n_cp)) { fprintf(stderr, "--cap wants off|on|both\n"); return 2; }
        }
        else if (!strcmp(argv[i], "--tracked-align") && i+1 < argc) {
            if (!parse_onoff(argv[++i], ax_ta, &n_ta)) { fprintf(stderr, "--tracked-align wants off|on|both\n"); return 2; }
        }
        else if (!strcmp(argv[i], "--decorrelation") && i+1 < argc) {
            if (!parse_onoff(argv[++i], ax_dc, &n_dc)) { fprintf(stderr, "--decorrelation wants off|on|both\n"); return 2; }
        }
        else if (!strcmp(argv[i], "--spread-mode") && i+1 < argc) {
            const char* m = argv[++i];
            if      (!strcmp(m, "lobe"))     { ax_sm[0] = 0; n_sm = 1; }
            else if (!strcmp(m, "mdap"))     { ax_sm[0] = 1; n_sm = 1; }
            else if (!strcmp(m, "spectral")) { ax_sm[0] = 2; n_sm = 1; }
            else if (!strcmp(m, "all"))      { ax_sm[0] = 0; ax_sm[1] = 1; ax_sm[2] = 2; n_sm = 3; }
            else { fprintf(stderr, "--spread-mode wants lobe|mdap|spectral|all\n"); return 2; }
        }
        else if (!strcmp(argv[i], "--hole-spread") && i+1 < argc) {
            if (!parse_flist(argv[++i], ax_hs, MAX_LIST, &n_hs, 1)) {
                fprintf(stderr, "--hole-spread wants a value (or comma-separated list) in 0..2; 0 is off\n"); return 2; }
        }
        else if (!strcmp(argv[i], "--near-spread") && i+1 < argc) {
            if (!parse_flist(argv[++i], ax_ns, MAX_LIST, &n_ns, 1)) {
                fprintf(stderr, "--near-spread wants a radius in meters (or a list); 0 is off\n"); return 2; }
        }
        else if (!strcmp(argv[i], "--focus")     && i+1 < argc) {
            /* one value, or a comma-separated sweep. Each value is a separate measured condition for
             * SPCAP and is ignored by the other two panners (focus has no meaning there). */
            const char* s = argv[++i];
            nfocus = 0;
            while (*s && nfocus < MAX_FOCUS) {
                char* endp = NULL;
                double v = strtod(s, &endp);
                if (endp == s) { fprintf(stderr, "--focus wants a number or a comma-separated list\n"); return 2; }
                if (!(v > 0.0)) { fprintf(stderr, "--focus values must be > 0 (0 is the sentinel for "
                                                  "the array's derived default, which is what you get "
                                                  "by leaving --focus off)\n"); return 2; }
                focus_req[nfocus++] = (float)v;
                s = endp;
                while (*s == ',' || *s == ' ') ++s;
            }
            if (!nfocus) { fprintf(stderr, "--focus wants at least one value\n"); return 2; }
            if (*s) { fprintf(stderr, "--focus takes at most %d values\n", MAX_FOCUS); return 2; }
        }
        else if (!strcmp(argv[i], "--tone") && i+1 < argc) {
            double hz = atof(argv[++i]);
            if (!valid_set_stimulus(VALID_STIM_TONE, hz)) {
                fprintf(stderr, "--tone %.0f Hz is outside the array's first-order reach: its\n"
                                "+-1/6-octave band sits above the %.0f Hz ceiling (kr ~ 1 on a 49 mm\n"
                                "sphere). Pick a lower tone, or use the broadband default.\n",
                        hz, ZYLIA_FOA_FMAX);
                return 2;
            }
        }
        else if (!strcmp(argv[i], "--position")  && i+1 < argc) {
            if (nlis_cli >= MAX_LIS) { fprintf(stderr, "too many --position\n"); return 2; }
            if (!parse_xyz(argv[++i], lis[nlis_cli])) {
                fprintf(stderr, "--position wants x,y,z (got %s)\n", argv[i]); return 2; }
            snprintf(lisname[nlis_cli], LBL, "pos %d", nlis_cli + 1);
            ++nlis_cli;
        }
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            printf("bwa_validate - measure where the array actually puts a phantom source\n\n"
                   "  --simulate            run the whole flow with no hardware: the engine renders\n"
                   "                        the feeds, they propagate to the capsules analytically\n"
                   "  --layout <path>       cave_layout.json (default: the built-in grid)\n"
                   "  --driver <name>       ASIO driver (default: first with enough channels)\n"
                   "  --mic-in <n>          first Zylia input channel (default 0)\n"
                   "  --position x,y,z      a microphone placement; repeatable, replaces the defaults\n"
                   "  --positions <file>    placements from a file: 'x y z [label]' per line, # comments\n"
                   "  --azimuths <n>        azimuths per elevation row (default 12)\n"
                   "  --radius <m>          source distance from the sweet spot (default 1.4)\n"
                   "  --focus <v[,v,...]>   SPCAP lobe sharpness. A list SWEEPS it: each value is a\n"
                   "                        separate condition measured in the same session, at the\n"
                   "                        same placements and directions, so the focus contrasts are\n"
                   "                        matched-cell. Focus trades image tightness against COMB\n"
                   "                        DEPTH (fewer speakers interfere less), which is the number\n"
                   "                        this reports and the ear cannot put a figure on. Inert\n"
                   "                        under DBAP/VBAP, so those are measured once. Default: the\n"
                   "                        array's geometry-derived value.\n"
                   "  --density <v>          SPCAP placement-correction exponent (default: the\n"
                   "                        layout's, 2.0). One value for the whole run.\n"
                   "\n"
                   "  The phantom arm renders through a real engine, so these live A/B knobs are\n"
                   "  measurable. Each is a swept AXIS: the first value is the baseline, and by\n"
                   "  default every other value is measured ONE AT A TIME against that baseline\n"
                   "  (N extra conditions, not 2^N). --factorial measures the cross product.\n"
                   "  --dual-band off|on|both   ~700 Hz split: amplitude LF, power HF\n"
                   "  --cap off|on|both     ITD-exact low band. REQUIRES dual-band, and a cap\n"
                   "                        condition turns it on for you (labelled dual+cap).\n"
                   "                        Measured facing room-ahead only: this tool cannot see\n"
                   "                        CAP's head-rotation claim (docs/validation.md).\n"
                   "  --tracked-align off|on|both  re-reference the per-speaker delay/gain trims\n"
                   "                        onto the solve listener. Inert at the layout reference.\n"
                   "  --hole-spread <v[,v]> hole-aware spread floor strength (0 = off). Inert on\n"
                   "                        an array with no holes, by construction.\n"
                   "  --spread-mode lobe|mdap|spectral|all   how width renders\n"
                   "  --decorrelation off|on|both  velvet-noise decorrelation of the wide part\n"
                   "  --near-spread <v[,v]> near-listener widening radius, meters (0 = off)\n"
                   "  --spread <0..1>       the SOURCE's own width. The three knobs above act on\n"
                   "                        the wide part of a source, so with spread 0 and no floor\n"
                   "                        engaged they have nothing to do. Set it to sweep them.\n"
                   "  --factorial           measure the full cross product instead of one knob at\n"
                   "                        a time. Cells explode; the count is printed either way.\n"
                   "\n"
                   "  --out <file.csv>      write every cell\n"
                   "  --no-prompt           don't wait for ENTER between placements (unattended runs)\n"
                   "  --track <id|name>     follow the ZM-1's stand as a tracked rigid body: the pose\n"
                   "                        gives the mic POSITION and re-aims the capsule table, so a\n"
                   "                        remount costs nothing. Placements become plans, the tracker\n"
                   "                        measures. Needs --survey.\n"
                   "  --survey <file>       BODY-FRAME capsule survey (zylia_survey_save with a mount)\n"
                   "  --natnet-server <ip>  Motive host; REQUIRED to --track by name (the streaming id\n"
                   "                        is resolved from Motive's model definitions)\n"
                   "  --natnet-multicast <g>  NatNet group (default 239.255.42.99)\n"
                   "  --tone <hz>           measure with a sustained TONE instead of broadband. Two\n"
                   "                        mechanisms make this the hard case: room standing waves\n"
                   "                        (hardware only), AND the array's own frequency-dependent\n"
                   "                        interference, which shows up in --simulate too. A single\n"
                   "                        driven speaker stays content-independent either way, so\n"
                   "                        the reference arm is the control.\n"
                   "  --no-reference        skip the physical reference arm (each speaker driven\n"
                   "                        alone). That arm is what makes a phantom miss a CONTRAST\n"
                   "                        against a real source rather than an absolute number, and\n"
                   "                        it is the fastest check that the chain is sane at all  - \n"
                   "                        only skip it to save rig time.\n"
                   "  --track-sim           SELF-CHECK: drive the tracked path from a synthetic mount\n"
                   "                        pose, no rig. Still needs --survey. Nonzero exit if the\n"
                   "                        placement hook does not fire for every placement.\n"
                   "  --inject-fault <ch>   SELF-CHECK: corrupt capsule <ch> in every capture and\n"
                   "                        require the integrity layer to catch it (nonzero exit if\n"
                   "                        it doesn't). Proves the check + exclusion chain on YOUR\n"
                   "                        data before you trust it at the rig.\n");
            return 0;
        } else { fprintf(stderr, "unknown argument: %s (try --help)\n", argv[i]); return 2; }
    }
    if (naz < 3 || naz > 36) { fprintf(stderr, "--azimuths must be 3..36\n"); return 2; }
    if (inject != -1 && (inject < 0 || inject >= ZYLIA_MICS)) {
        fprintf(stderr, "--inject-fault must be 0..%d\n", ZYLIA_MICS-1); return 2; }

    Layout L = layout_default();
    if (layout_path) {
        char err[256] = { 0 };
        if (!layout_load(layout_path, (uint32_t)VAL_FS, &L, err, sizeof err)) {
            fprintf(stderr, "layout: %s\n", err);
            return 2;
        }
    }
    printf("layout: %u speakers, sweet spot (%.2f, %.2f, %.2f)%s\n",
           L.count, L.ref[0], L.ref[1], L.ref[2], layout_path ? "" : "  [built-in default grid]");
    {
        double f_lo, f_hi;
        valid_get_stimulus_band(&f_lo, &f_hi);
        printf("stimulus: %s   analysis band %.0f-%.0f Hz\n", valid_stimulus_name(), f_lo, f_hi);
    }

    float elev[3] = { -25.0f, 0.0f, 25.0f };
    static float tg[MAX_TGT][3];
    int ntgt = valid_target_grid(naz, elev, 3, tg, MAX_TGT);
    if (!ntgt) { fprintf(stderr, "target grid too large\n"); return 2; }

    int nlis = nlis_cli;
    if (posfile) {
        nlis = load_positions(posfile, lis, lisname, MAX_LIS, nlis);
        if (nlis < 0) return 2;
    }
    /* A file with nothing but comments leaves nlis == 0. Falling back to the built-in envelope while
     * announcing "(yours)" would run a whole session at the wrong places and say they were yours. */
    int user_pos = (nlis > 0);
    if (!nlis) {
        if (posfile) fprintf(stderr, "note: %s held no placements - falling back to the defaults\n", posfile);
        nlis = default_listeners(lis, lisname, L.ref);
    }
    printf("placements: %d %s\n", nlis,
           user_pos ? "(yours)" : "(defaults - pass --position/--positions for the real ones)");

    const int panners[NPAN] = { BWA_PAN_DBAP, BWA_PAN_SPCAP, BWA_PAN_VBAP };

    /* ---- the condition space ----
     *
     * Resolve the <= 0 focus sentinel HERE, once, against the layout the run actually loaded. From
     * this point every condition carries real numbers, which is what lets a cell be matched back to
     * its condition by plain field equality against what valid_score recorded.
     *
     * ONE KNOB AT A TIME IS THE DEFAULT, and the reason is rig time. A full factorial over these
     * axes is 2^N sessions' worth of cells and answers a question nobody asked: what settles a knob
     * is its contrast against a fixed baseline, measured on the same directions at the same
     * placements. So the builder emits the baseline plus one condition per non-baseline value, which
     * is N extra passes. --factorial takes the cross product when an interaction is genuinely
     * suspected, and the cell count is printed before anything is measured either way.
     *
     * CAP is the one dependency: it touches the dual-band low band and nothing else, so a cap
     * condition turns dual-band on with it rather than measuring a knob that provably did nothing. */
    if (n_cp == 1 && ax_cp[0] == 1 && n_db == 1 && ax_db[0] == 0) {
        fprintf(stderr, "--cap on with --dual-band off measures nothing: CAP replaces the dual-band\n"
                        "low band, and with no dual-band there is no low band to replace. Pass\n"
                        "--dual-band on. (A cap VARIANT condition turns dual-band on for itself; a\n"
                        "cap BASELINE cannot, because there is nothing left to vary against.)\n");
        return 2;
    }
    for (int fi = 0; fi < nfocus; ++fi)
        if (!(focus_req[fi] > 0.f)) focus_req[fi] = L.spcap_focus;
    if (!(density > 0.f)) density = L.spcap_density;   /* the same sentinel, resolved the same once */

    static Cond cond[MAX_COND];
    int ncond = 0, over = 0;
    ValidRender B;
    {
        valid_render_init(&B);
        B.density       = density;
        B.dual_band     = ax_db[0];
        B.cap           = ax_cp[0];
        B.hole_spread   = ax_hs[0];
        B.tracked_align = ax_ta[0];
        B.spread_mode   = ax_sm[0];
        B.decorrelation = ax_dc[0];
        B.near_spread   = ax_ns[0];
        B.spread        = spread;

        for (int p = 0; p < NPAN; ++p) {
            const int nf = (panners[p] == BWA_PAN_SPCAP) ? nfocus : 1;  /* no lobe, no focus */
            for (int fi = 0; fi < nf; ++fi) {
                ValidRender b = B;
                b.focus = focus_req[fi];
                if (factorial) {
                    for (int a = 0; a < n_db; ++a)
                    for (int k = 0; k < n_cp; ++k)
                    for (int h = 0; h < n_hs; ++h)
                    for (int t = 0; t < n_ta; ++t)
                    for (int m = 0; m < n_sm; ++m)
                    for (int d = 0; d < n_dc; ++d)
                    for (int e = 0; e < n_ns; ++e) {
                        ValidRender r = b;
                        r.dual_band = ax_db[a];
                        r.cap       = ax_cp[k];
                        if (r.cap) r.dual_band = 1;                     /* CAP implies dual-band */
                        r.hole_spread   = ax_hs[h];
                        r.tracked_align = ax_ta[t];
                        r.spread_mode   = ax_sm[m];
                        r.decorrelation = ax_dc[d];
                        r.near_spread   = ax_ns[e];
                        cond_emit(cond, &ncond, &over, panners[p], &r);
                    }
                    continue;
                }
                cond_emit(cond, &ncond, &over, panners[p], &b);         /* the baseline */
                for (int a = 1; a < n_db; ++a) { ValidRender r = b; r.dual_band = ax_db[a];
                                                 cond_emit(cond, &ncond, &over, panners[p], &r); }
                for (int a = 1; a < n_cp; ++a) { ValidRender r = b; r.cap = ax_cp[a];
                                                 if (r.cap) r.dual_band = 1;
                                                 cond_emit(cond, &ncond, &over, panners[p], &r); }
                for (int a = 1; a < n_hs; ++a) { ValidRender r = b; r.hole_spread = ax_hs[a];
                                                 cond_emit(cond, &ncond, &over, panners[p], &r); }
                for (int a = 1; a < n_ta; ++a) { ValidRender r = b; r.tracked_align = ax_ta[a];
                                                 cond_emit(cond, &ncond, &over, panners[p], &r); }
                for (int a = 1; a < n_sm; ++a) { ValidRender r = b; r.spread_mode = ax_sm[a];
                                                 cond_emit(cond, &ncond, &over, panners[p], &r); }
                for (int a = 1; a < n_dc; ++a) { ValidRender r = b; r.decorrelation = ax_dc[a];
                                                 cond_emit(cond, &ncond, &over, panners[p], &r); }
                for (int a = 1; a < n_ns; ++a) { ValidRender r = b; r.near_spread = ax_ns[a];
                                                 cond_emit(cond, &ncond, &over, panners[p], &r); }
            }
        }
    }
    if (over) {
        fprintf(stderr, "that asks for %d conditions and the cap is %d. Drop an axis, or run the\n"
                        "sweeps as separate sessions.\n", ncond + over, MAX_COND);
        return 2;
    }
    printf("SPCAP tuning: focus");
    for (int fi = 0; fi < nfocus; ++fi) printf(" %.2f", focus_req[fi]);
    printf("%s   density %.2f\n", nfocus > 1 ? "  (swept)" : "",
           density > 0.f ? density : L.spcap_density);
    /* Print the condition table BEFORE measuring: an operator asking for a factorial should see what
     * they just asked for while it is still cheap to change their mind. */
    printf("conditions: %d  (%s)\n", ncond,
           factorial ? "factorial: the full cross product"
                     : "one knob at a time against the baseline; --factorial for the cross product");
    for (int ci = 0; ci < ncond; ++ci) {
        char cl[CLBL];
        printf("  %2d  %s\n", ci + 1, cond_label(&cond[ci], cl, sizeof cl));
    }

    /* NOT a matched-cell design, deliberately. Rendering a phantom at a speaker's own position is
     * degenerate — the panner puts essentially all the gain on that one speaker, so the "phantom"
     * IS the speaker and the difference measures nothing (tried it: ~0.02 deg). A true matched
     * physical/phantom pair needs a real source at a direction BETWEEN the speakers, which is why
     * the published protocol had to physically move a loudspeaker. What the reference arm gives us
     * instead is the FLOOR: what the instrument, the layout survey and the room cost before any
     * panning happens. Phantom misses are then quoted above that floor rather than as absolutes. */
    const int ngrid = ntgt;
    const int ncell = ncond * 2 * nlis * ntgt
                    + (do_ref ? nlis * (int)L.count * (1 + ncond * 2) : 0);
    printf("plan: %d directions x %d conditions x 2 modes x %d placements%s = %d cells\n",
           ntgt, ncond, nlis,
           do_ref ? ", plus one physical reference per speaker" : "", ncell);
    if (inject >= 0) printf("SELF-CHECK: capsule %d will be corrupted in every capture\n", inject);

    /* ---- optional tracked mount ---- */
    TrackCtx trk;
    memset(&trk, 0, sizeof trk);
    int (*place_fn)(void*, int, float[3]) = NULL;
    void* place_user = NULL;
    if (track_body || track_sim) {
        if (!survey_path && !track_sim) {
            fprintf(stderr, "--track needs --survey <body-frame survey>: the pose gives the mount's\n"
                            "orientation, but only a survey knows how the capsules sit inside it.\n");
            return 2;
        }
        char e[192] = { 0 };
        if (!survey_path) {
            /* --track-sim with no survey: stand the built-in table in as the body frame. This
             * self-check is about whether the placement hook is WIRED, not about the loader, and
             * keeping it survey-free means it needs no committed fixture to run under ctest. */
            zylia_set_capsules(NULL);
            trk.mount.body_frame = 1;
            printf("track self-check: no --survey, using the built-in capsule table as the body frame\n");
        } else if (!zylia_survey_load(survey_path, &trk.mount, e, sizeof e)) {
            fprintf(stderr, "survey: %s\n", e);
            return 2;
        }
        if (!trk.mount.body_frame) {
            fprintf(stderr, "survey %s is in ROOM axes, not the mount's body frame - it is tied to one\n"
                            "orientation, so it cannot follow a moving stand. Re-save it with a mount.\n",
                    survey_path);
            return 2;
        }
        if (!trk.mount.have_offset)
            printf("NOTE: survey carries no mount offset; assuming the body origin IS the array center\n");
        zylia_capsules(trk.caps_body);            /* the loaded table, still in body axes */
        /* snapshot the plans: `place` writes into lis[], so they cannot be read back from there */
        for (int i = 0; i < nlis && i < MAX_LIS; ++i) memcpy(trk.planned[i], lis[i], sizeof lis[i]);
        place_fn = &track_place; place_user = &trk;

        if (track_sim) {                          /* self-check: synthetic pose, no tracker needed */
            printf("TRACK SELF-CHECK: a synthetic mount pose drives the same path a tracker would\n");
        } else {
            NatNetConfig nc;
            memset(&nc, 0, sizeof nc);
            nc.multicast = nn_multicast;
            nc.server = nn_server;
            nc.data_port = 1511; nc.command_port = 1510;
            char* endp = NULL;
            long id = strtol(track_body, &endp, 10);
            if (endp && *endp == 0) nc.rigid_body = (int32_t)id;      /* numeric => streaming id */
            else {
                if (!nn_server) {
                    fprintf(stderr, "--track by NAME needs --natnet-server <ip>: the streaming id is\n"
                                    "resolved from Motive's model definitions. Or pass the id directly.\n");
                    return 2;
                }
                nc.rigid_body_name = track_body;
            }
            trk.nn = natnet_open(&nc, e, sizeof e);
            if (!trk.nn) { fprintf(stderr, "tracker: %s\n", e); return 1; }
            printf("tracking rigid body '%s' - placements are PLANS, the pose is the measurement\n",
                   track_body);
        }
    }

    CapCtx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.L = &L; ctx.inject = inject; ctx.rng = 0xC0FFEEu;
    CaptureFn cap = &cap_simulate;
    int prompt = 0;

#ifdef BWA_HAVE_ASIO
    int have_hw = 0;
    if (!simulate) {
        ctx.feeds = (float*)malloc(sizeof(float) * (size_t)BWA_CHANNELS * VAL_CAPLEN);
        if (!ctx.feeds) { fprintf(stderr, "out of memory\n"); return 1; }
        if (valid_asio_open(driver, mic_in, (int)L.count) != 0) {
            fprintf(stderr, "\nNo capture device. Re-run with --simulate to exercise the flow "
                            "without hardware.\n");
            return 1;
        }
        have_hw = 1;
        cap = &cap_asio;
        prompt = !no_prompt;
    }
#else
    (void)driver; (void)mic_in; (void)no_prompt;
    if (!simulate) {
        fprintf(stderr, "this build has no ASIO SDK - only --simulate is available\n");
        return 1;
    }
#endif

    ValidCell* cells = (ValidCell*)calloc((size_t)ncell, sizeof(ValidCell));
    if (!cells) { fprintf(stderr, "out of memory\n"); return 1; }

    int nflagged = 0, nchecked = 0, nplaced = 0;
    int w = run_session(&L, cap, &ctx, cond, ncond, lis, lisname, nlis,
                        tg, ntgt, ngrid, radius, do_ref, prompt, place_fn, place_user,
                        cells, &nchecked, &nflagged, &nplaced);

#ifdef BWA_HAVE_ASIO
    if (have_hw) valid_asio_close();
#endif
    free(ctx.feeds);
    if (trk.nn) natnet_close(trk.nn);

    /* ---- matched-cell contrasts, the claim worth making ---- */
    printf("\nmatched-cell contrast (fixed - tracked), median of paired differences\n");
    for (int ci = 0; ci < ncond; ++ci)
        for (int li = 0; li < nlis; ++li) {
            static double a[MAX_TGT], b[MAX_TGT];
            int n = 0;
            for (int t = 0; t < ntgt; ++t) {
                ValidCell *ct = NULL, *cf = NULL;
                for (int i = 0; i < w; ++i) {
                    ValidCell* c = &cells[i];
                    if (c->lis != li || c->tgt != t || c->reference) continue;
                    if (!cell_in_cond(c, &cond[ci])) continue;
                    if (c->tracked) ct = c; else cf = c;
                }
                if (ct && cf && ct->ok && cf->ok) { a[n] = ct->miss_deg; b[n] = cf->miss_deg; ++n; }
            }
            if (n < 8) continue;
            double md, lo, hi;
            if (!valid_contrast(a, b, n, 2000, 777u, &md, &lo, &hi)) continue;
            char cl[32];
            printf("  %-13s  %-16s %+6.1f  CI [%+.1f, %+.1f]%s\n",
                   cond_label(&cond[ci], cl, sizeof cl), lisname[li], md, lo, hi,
                   (lo > 0.0 || hi < 0.0) ? "  *" : "");
        }
    printf("  (* = interval excludes zero)\n");
    /* ---- the knob sweep: what each A/B costs, matched-cell against the baseline ----
     *
     * Every condition after the first for a panner changes exactly one thing (unless --factorial was
     * asked for), so its paired difference against that panner's baseline IS the effect of that knob
     * on this array: same placement, same direction, same capture chain. Two columns, because a
     * render can fail two ways that do not track each other. Angular miss is where the phantom went;
     * comb depth is what making it cost in timbre, and read that one against the placement's comb
     * floor printed above rather than as an absolute.
     *
     * Focus is one of the swept knobs and reads here like the rest. What it trades is the NUMBER of
     * speakers carrying a source: fewer coherent copies interfering means a tighter lobe combs less,
     * a looser one buys a smoother, better-covered image and pays in ripple. That trade was dialed by
     * ear and the ear cannot put a figure on it. */
    if (ncond > NPAN) {
        int base_of[MAX_COND];
        for (int ci = 0; ci < ncond; ++ci) {
            base_of[ci] = ci;
            for (int cj = 0; cj < ncond; ++cj)      /* the panner's FIRST condition is its baseline */
                if (cond[cj].panner == cond[ci].panner) { base_of[ci] = cj; break; }
        }
        printf("\nknob sweep, matched-cell against each panner's baseline (tracked solve)\n");
        printf("  %-16s %-36s %9s %8s %9s %8s\n",
               "placement", "condition", "miss deg", "d miss", "comb dB", "d comb");
        for (int li = 0; li < nlis; ++li)
            for (int ci = 0; ci < ncond; ++ci) {
                if (base_of[ci] == ci) continue;                /* the baseline is the reference */
                static double m0[MAX_TGT], m1[MAX_TGT], c0[MAX_TGT], c1[MAX_TGT];
                int nm = 0, nc = 0;
                for (int t = 0; t < ntgt; ++t) {
                    ValidCell *ca = NULL, *cb = NULL;
                    for (int i = 0; i < w; ++i) {
                        ValidCell* c = &cells[i];
                        if (c->lis != li || c->tgt != t || c->reference || !c->tracked) continue;
                        if (cell_in_cond(c, &cond[base_of[ci]])) ca = c;
                        if (cell_in_cond(c, &cond[ci]))          cb = c;
                    }
                    if (!ca || !cb) continue;
                    if (ca->ok && cb->ok)           { m0[nm] = ca->miss_deg; m1[nm] = cb->miss_deg; ++nm; }
                    if (ca->comb_ok && cb->comb_ok) { c0[nc] = ca->comb_db;  c1[nc] = cb->comb_db;  ++nc; }
                }
                if (nm < 8) continue;
                double dm = 0, dmlo = 0, dmhi = 0, dc = 0, dclo = 0, dchi = 0;
                char cl[CLBL];
                valid_contrast(m0, m1, nm, 2000, 909u, &dm, &dmlo, &dmhi);
                printf("  %-16s %-36s %9.1f %+7.1f%-2s", lisname[li],
                       cond_label(&cond[ci], cl, sizeof cl),
                       valid_median(m1, nm), dm, (dmlo > 0.0 || dmhi < 0.0) ? " *" : "");
                if (nc >= 8 && valid_contrast(c0, c1, nc, 2000, 909u, &dc, &dclo, &dchi))
                    printf(" %9.2f %+7.2f%-2s", valid_median(c1, nc), dc,
                           (dclo > 0.0 || dchi < 0.0) ? " *" : "");
                printf("\n");
            }
        printf("  (* = bootstrap interval on the paired difference excludes zero)\n");
    }

    /* ---- the physical floor, and what panning costs above it ----
     * Driving one speaker alone is a real source at a known position, measured through the same
     * chain in the same room. That is the floor: instrument + layout survey + room, before any
     * panning. Quoting a phantom miss against it says something an absolute number cannot. */
    if (do_ref) {
        static double rr[MAX_TGT * 8];
        int nr = 0;
        /* reference == 1 ONLY. `reference` is also 2 for the MATCHED PHANTOM at a speaker's position,
         * so a truthiness test here would pool phantoms into the physical floor and inflate it by
         * whatever panning costs — which is the one thing the floor exists to be free of. */
        for (int i = 0; i < w && nr < (int)(sizeof rr / sizeof rr[0]); ++i)
            if (cells[i].reference == 1 && cells[i].ok) rr[nr++] = cells[i].miss_deg;
        if (nr >= 4) {
            double floor_med, lo, hi;
            floor_med = valid_median(rr, nr);
            valid_bootstrap_ci(rr, nr, 2000, 4242u, &lo, &hi);
            printf("\nphysical floor: %.2f deg  CI [%.2f, %.2f]  (%d speakers x placements,\n"
                   "  each driven alone - instrument + survey + room, before any panning)\n",
                   floor_med, lo, hi, nr);
            if (floor_med > 5.0)
                printf("  ** SUSPECT. A directly driven speaker should land near its surveyed\n"
                       "  ** position. Until this is small, no phantom number here is interpretable.\n");
        }

        /* Physical versus phantom, MATCHED per speaker: the published comparison, nothing moved.
         * Reported PER PLACEMENT and never pooled across them. Pooling is actively misleading here:
         * at the array center the penalty is ~0 by symmetry (a blurred phantom's energy vector still
         * points at the speaker), while off-center it is degrees. Pool the two and the median lands
         * in the empty middle and looks like "no effect", which is the opposite of the truth. */
        printf("\nphysical versus phantom, matched per speaker (phantom - real, same direction)\n");
        for (int li = 0; li < nlis; ++li)
        for (int ci = 0; ci < ncond; ++ci)
            for (int tracked = 1; tracked >= 0; --tracked) {
                static double ra[MAX_TGT * 8], pa[MAX_TGT * 8], rk[MAX_TGT * 8], pk[MAX_TGT * 8];
                int n = 0, nk = 0;
                for (int sp = 0; sp < (int)L.count; ++sp) {
                    ValidCell *rc = NULL, *pc = NULL;
                    for (int i = 0; i < w; ++i) {
                        ValidCell* c = &cells[i];
                        if (c->lis != li || c->tgt != sp) continue;
                        if (c->reference == 1) rc = c;
                        else if (c->reference == 2 && cell_in_cond(c, &cond[ci]) &&
                                 c->tracked == tracked) pc = c;
                    }
                    if (!rc || !pc) continue;
                    if (rc->ok && pc->ok && n < (int)(sizeof ra / sizeof ra[0])) {
                        ra[n] = rc->miss_deg; pa[n] = pc->miss_deg; ++n;
                    }
                    /* The comb EXCESS, matched the same way: same direction, same placement, one
                     * speaker against many. This is the pair that makes a comb depth mean something. */
                    if (rc->comb_ok && pc->comb_ok && nk < (int)(sizeof rk / sizeof rk[0])) {
                        rk[nk] = rc->comb_db; pk[nk] = pc->comb_db; ++nk;
                    }
                }
                if (n < 8) continue;
                double md, clo, chi;
                if (!valid_contrast(ra, pa, n, 2000, 4242u, &md, &clo, &chi)) continue;
                char cl[CLBL];
                printf("  %-16s %-36s %-8s  real %5.2f  phantom %5.2f   penalty %+6.2f  CI [%+.2f, %+.2f]%s",
                       lisname[li], cond_label(&cond[ci], cl, sizeof cl),
                       tracked ? "tracked" : "fixed",
                       valid_median(ra, n), valid_median(pa, n), md, clo, chi,
                       (clo > 0.0 || chi < 0.0) ? "  *" : "");
                double kd, klo, khi;
                if (nk >= 8 && valid_contrast(rk, pk, nk, 2000, 4242u, &kd, &klo, &khi))
                    printf("   comb %+6.2f dB CI [%+.2f, %+.2f]%s", kd, klo, khi,
                           (klo > 0.0 || khi < 0.0) ? " *" : "");
                printf("\n");
            }
        printf("  A source sitting ON a speaker is still spread by a distance-blurred panner, so this\n"
               "  penalty is real. Expect ~0 at the array center (symmetry) and degrees off-center;\n"
               "  read the off-center rows, and do not average them together. The comb column has no\n"
               "  such symmetry escape: many coherent copies interfere everywhere, center included.\n");
    }

    if (simulate)
        printf("\nSIMULATED: anechoic, so this is the RENDERING term only - the room adds to it.\n");

    if (csv) {
        FILE* f = fopen(csv, "w");
        if (!f) { fprintf(stderr, "cannot write %s\n", csv); }
        else {
            /* One column per swept knob, so a session's results can be re-analyzed against the
             * settings they came from without re-deriving them from a label. A reference row carries
             * zeros throughout: it ran no panner and no knob. */
            fprintf(f, "panner,focus,density,dual_band,cap,hole_spread,tracked_align,spread_mode,"
                       "decorrelation,near_spread,spread,reference,tracked,lis,lis_name,tgt,"
                       "mic_x,mic_y,mic_z,tgt_x,tgt_y,tgt_z,meas_x,meas_y,meas_z,miss_deg,"
                       "diffuseness,ok,comb_db,comb_q,comb_ok\n");
            for (int i = 0; i < w; ++i) {
                ValidCell* c = &cells[i];
                fprintf(f, "%s,%.4f,%.4f,%d,%d,%.4f,%d,%d,%d,%.4f,%.4f,%d,%d,%d,%s,%d,"
                           "%.4f,%.4f,%.4f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.3f,%.3f,%d,%.3f,%.3f,%d\n",
                        c->reference == 1 ? "-" : pan_name(c->panner),
                        c->render.focus, c->render.density, c->render.dual_band, c->render.cap,
                        c->render.hole_spread, c->render.tracked_align, c->render.spread_mode,
                        c->render.decorrelation, c->render.near_spread, c->render.spread,
                        c->reference, c->tracked, c->lis, lisname[c->lis], c->tgt,
                        c->mic[0], c->mic[1], c->mic[2],
                        c->target[0], c->target[1], c->target[2],
                        c->measured[0], c->measured[1], c->measured[2],
                        c->miss_deg, c->diffuseness, c->ok,
                        c->comb_db, c->comb_q, c->comb_ok);
            }
            fclose(f);
            printf("wrote %d cells to %s\n", w, csv);
        }
    }

    free(cells);

    /* the self-checks are TESTS, so they have to be able to fail */
    if (inject >= 0) {
        printf("\nself-check: capsule %d flagged at %d/%d MEASURED placements\n",
               inject, nflagged, nchecked);
        if (nchecked == 0 || nflagged != nchecked) {
            fprintf(stderr, "SELF-CHECK FAILED: the integrity layer missed the injected fault\n");
            return 3;
        }
        printf("self-check PASSED: integrity check + exclusion chain works end to end\n");
    }
    if (track_sim) {
        /* This exists because the placement hook was once passed into run_session and never called,
         * leaving --track silently inert while it printed that the tracker was measuring. Asserting
         * on the RESULT would not have caught that: in simulate the field is synthesized from the
         * same capsule table the estimator reads, so a wrong table cancels out and looks healthy.
         * So assert the hook FIRED, which is the thing that was actually broken. */
        printf("\ntrack self-check: placement hook fired %d/%d times\n", nplaced, nchecked);
        if (nchecked == 0 || nplaced != nchecked) {
            fprintf(stderr, "TRACK SELF-CHECK FAILED: the placement hook is not wired into the "
                            "session loop - --track would run with the survey's body-frame capsule "
                            "table installed as if it were room axes\n");
            return 4;
        }
        printf("track self-check PASSED: pose -> capsule re-aim -> mic position is wired\n");
    }
    return 0;
}
