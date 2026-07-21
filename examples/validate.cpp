/*
 * validate.cpp — bwa_validate: measure where the array actually puts a phantom source.
 *
 * Render a source in a known direction, capture it with the ZM-1 at a known listening position,
 * estimate the direction of arrival, report the angular miss. Sweep that over directions, over
 * panners, over tracked-vs-fixed rendering, and over listener positions, and you have graded the
 * layout, the panner and the calibration with numbers instead of opinions.
 *
 * THE SESSION SHAPE, and why it is cheap. Phantom sources are rendered, not carried, so a whole
 * direction grid sweeps electronically from one microphone placement. Only the MICROPHONE moves.
 * That inverts the usual cost of this measurement: a study that moves a physical reference speaker
 * needs one session per direction, where this needs one per listening position and gets every
 * direction for free. Half a dozen placements covers the walking envelope.
 *
 *   for each microphone placement (you move the ZM-1, the tool waits):
 *       check the capsules once, report anything faulty, exclude it for the rest of the placement
 *       for each panner x {tracked, fixed} x direction:  render -> capture -> score
 *
 * --simulate runs the identical flow with the analytic field substituted for the capture, so the
 * plan, the scoring, the statistics and the report are exercised without the rig. The ONLY thing it
 * cannot give you is the room, which is precisely the term hardware is for (see valid.h).
 *
 * Usage:
 *   bwa_validate --simulate                          the whole flow, no hardware
 *   bwa_validate --driver "ASIO MADIface USB" --mic-in 26
 *   bwa_validate --layout cave_layout.json --azimuths 24 --out cells.csv
 */
/* valid.h / layout.h / zylia.h are C headers with no extern "C" of their own (the codebase keeps
 * them that way and wraps at the C++ call site — see calib_capture.h). */
extern "C" {
#include "valid.h"
}
#include "valid_capture.h"
#include "bw_audio.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_LIS   32
#define MAX_TGT   (36 * 3)
#define NPAN      3

static const char* pan_name(int p) {
    return p == BWA_PAN_DBAP ? "DBAP" : (p == BWA_PAN_SPCAP ? "SPCAP" : "VBAP");
}

/* the default walking envelope: the sweet spot, three horizontal steps, and — separately, because it
 * is a different failure mode — a height sweep. See valid_test.c's note on why these are not pooled. */
static int default_listeners(float (*out)[3], const float ref[3]) {
    const float dx = 0.7f, dy = 0.4f;
    float p[7][3] = {
        { ref[0],      ref[1],      ref[2]      },
        { ref[0]+dx,   ref[1],      ref[2]      },
        { ref[0]-dx,   ref[1],      ref[2]      },
        { ref[0],      ref[1],      ref[2]+dx   },
        { ref[0],      ref[1]-dy,   ref[2]      },
        { ref[0],      ref[1]+dy,   ref[2]      },
        { ref[0]+0.6f, ref[1]+dy,   ref[2]+0.5f },
    };
    memcpy(out, p, sizeof p);
    return 7;
}

int main(int argc, char** argv) {
    const char* layout_path = NULL;
    const char* driver = NULL;
    const char* csv = NULL;
    int simulate = 0, mic_in = 0, naz = 12;
    float radius = 1.4f;

    for (int i = 1; i < argc; ++i) {
        if      (!strcmp(argv[i], "--simulate"))                    simulate = 1;
        else if (!strcmp(argv[i], "--layout")   && i+1 < argc)      layout_path = argv[++i];
        else if (!strcmp(argv[i], "--driver")   && i+1 < argc)      driver = argv[++i];
        else if (!strcmp(argv[i], "--out")      && i+1 < argc)      csv = argv[++i];
        else if (!strcmp(argv[i], "--mic-in")   && i+1 < argc)      mic_in = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--azimuths") && i+1 < argc)      naz = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--radius")   && i+1 < argc)      radius = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            printf("bwa_validate — measure where the array actually puts a phantom source\n\n"
                   "  --simulate            run the whole flow with no hardware (analytic field)\n"
                   "  --layout <path>       cave_layout.json (default: the built-in grid)\n"
                   "  --driver <name>       ASIO driver (default: first with enough channels)\n"
                   "  --mic-in <n>          first Zylia input channel (default 0)\n"
                   "  --azimuths <n>        azimuths per elevation row (default 12)\n"
                   "  --radius <m>          source distance from the sweet spot (default 1.4)\n"
                   "  --out <file.csv>      write every cell\n");
            return 0;
        } else { fprintf(stderr, "unknown argument: %s (try --help)\n", argv[i]); return 2; }
    }
    if (naz < 3 || naz > 36) { fprintf(stderr, "--azimuths must be 3..36\n"); return 2; }

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

    float elev[3] = { -25.0f, 0.0f, 25.0f };
    static float tg[MAX_TGT][3];
    int ntgt = valid_target_grid(naz, elev, 3, tg, MAX_TGT);
    if (!ntgt) { fprintf(stderr, "target grid too large\n"); return 2; }

    static float lis[MAX_LIS][3];
    int nlis = default_listeners(lis, L.ref);

    const int panners[NPAN] = { BWA_PAN_DBAP, BWA_PAN_SPCAP, BWA_PAN_VBAP };
    const int ncell = NPAN * 2 * nlis * ntgt;
    printf("plan: %d directions x %d panners x 2 modes x %d placements = %d cells\n",
           ntgt, NPAN, nlis, ncell);

#ifdef BWA_HAVE_ASIO
    int have_hw = 0;
    if (!simulate) {
        if (valid_asio_open(driver, mic_in, (int)L.count) != 0) {
            fprintf(stderr, "\nNo capture device. Re-run with --simulate to exercise the flow "
                            "without hardware.\n");
            return 1;
        }
        have_hw = 1;
    }
#else
    if (!simulate) {
        fprintf(stderr, "this build has no ASIO SDK — only --simulate is available\n");
        return 1;
    }
#endif

    ValidCell* cells = (ValidCell*)calloc((size_t)ncell, sizeof(ValidCell));
    float* feeds = (float*)malloc(sizeof(float) * (size_t)BWA_CHANNELS * VAL_CAPLEN);
    float* cap   = (float*)malloc(sizeof(float) * (size_t)ZYLIA_MICS  * VAL_ANALYZE);
    if (!cells || !feeds || !cap) { fprintf(stderr, "out of memory\n"); return 1; }

    int w = 0;
    for (int li = 0; li < nlis; ++li) {
        unsigned char flags[ZYLIA_MICS] = { 0 };
        printf("\n--- placement %d/%d: (%.2f, %.2f, %.2f) ---\n",
               li + 1, nlis, lis[li][0], lis[li][1], lis[li][2]);
#ifdef BWA_HAVE_ASIO
        if (have_hw) {
            printf("Place the ZM-1 there, then press ENTER (measure the position, do not eyeball it).\n");
            (void)getchar();
        }
#endif
        int checked = 0;

        for (int p = 0; p < NPAN; ++p)
            for (int tracked = 1; tracked >= 0; --tracked)
                for (int t = 0; t < ntgt; ++t) {
                    float src[3] = { L.ref[0] + radius*tg[t][0],
                                     L.ref[1] + radius*tg[t][1],
                                     L.ref[2] + radius*tg[t][2] };
                    const float* solve = tracked ? lis[li] : L.ref;
                    ValidCell* c = &cells[w];

                    int got = 0;
#ifdef BWA_HAVE_ASIO
                    if (have_hw) {
                        if (valid_speaker_feeds(&L, panners[p], solve, src, VAL_FS, feeds, VAL_CAPLEN)
                            && valid_asio_capture(feeds, cap)) {
                            /* One capsule check per placement, on the first real capture: a fault is
                             * a property of the session, and re-checking every cell would only cost
                             * time. Anything flagged is excluded from every cell that follows. */
                            if (!checked) {
                                const float* ptr[ZYLIA_MICS];
                                for (int j = 0; j < ZYLIA_MICS; ++j) ptr[j] = cap + (size_t)j * VAL_ANALYZE;
                                int nb = zylia_check_capsules(ptr, VAL_ANALYZE, flags);
                                if (nb > 0) {
                                    printf("  capsule check: %d FAULTY —", nb);
                                    for (int j = 0; j < ZYLIA_MICS; ++j)
                                        if (flags[j]) printf(" ch%d(0x%02X)", j, flags[j]);
                                    printf("  (excluded for this placement)\n");
                                } else printf("  capsule check: all %d healthy\n", ZYLIA_MICS);
                                checked = 1;
                            }
                            got = valid_score(&L, panners[p], tracked, lis[li], src,
                                              cap, VAL_ANALYZE, VAL_FS, 343.0, flags, c);
                        }
                    } else
#endif
                    {
                        got = valid_cell(&L, panners[p], tracked, lis[li], src,
                                         VAL_FS, 343.0, VAL_ANALYZE, c);
                    }
                    if (!got) { memset(c, 0, sizeof *c); c->panner = panners[p]; c->tracked = tracked; }
                    c->lis = li; c->tgt = t;
                    ++w;
                }

        /* per-placement summary, so a bad placement is visible before you move the mic again */
        for (int p = 0; p < NPAN; ++p) {
            double mt[MAX_TGT], mf[MAX_TGT];
            int nt = 0, nf = 0;
            for (int i = 0; i < w; ++i) {
                ValidCell* c = &cells[i];
                if (c->lis != li || c->panner != panners[p] || !c->ok) continue;
                if (c->tracked) mt[nt++] = c->miss_deg; else mf[nf++] = c->miss_deg;
            }
            if (nt && nf)
                printf("  %-5s  tracked %5.1f deg   fixed %5.1f deg   (n=%d/%d)\n",
                       pan_name(panners[p]), valid_median(mt, nt), valid_median(mf, nf), nt, nf);
        }
    }

#ifdef BWA_HAVE_ASIO
    if (have_hw) valid_asio_close();
#endif

    /* ---- matched-cell contrasts, the claim worth making ---- */
    printf("\nmatched-cell contrast (fixed - tracked), median of paired differences\n");
    for (int p = 0; p < NPAN; ++p)
        for (int li = 0; li < nlis; ++li) {
            double a[MAX_TGT], b[MAX_TGT];
            int n = 0;
            for (int t = 0; t < ntgt; ++t) {
                ValidCell *ct = NULL, *cf = NULL;
                for (int i = 0; i < w; ++i) {
                    ValidCell* c = &cells[i];
                    if (c->lis != li || c->tgt != t || c->panner != panners[p]) continue;
                    if (c->tracked) ct = c; else cf = c;
                }
                if (ct && cf && ct->ok && cf->ok) { a[n] = ct->miss_deg; b[n] = cf->miss_deg; ++n; }
            }
            if (n < 8) continue;
            double md, lo, hi;
            if (!valid_contrast(a, b, n, 2000, 777u, &md, &lo, &hi)) continue;
            printf("  %-5s  (%+.2f,%+.2f,%+.2f)  %+6.1f  CI [%+.1f, %+.1f]%s\n",
                   pan_name(panners[p]), lis[li][0], lis[li][1], lis[li][2], md, lo, hi,
                   (lo > 0.0 || hi < 0.0) ? "  *" : "");
        }
    printf("  (* = interval excludes zero)\n");
    if (simulate)
        printf("\nSIMULATED: anechoic, so this is the RENDERING term only — the room adds to it.\n");

    if (csv) {
        FILE* f = fopen(csv, "w");
        if (!f) { fprintf(stderr, "cannot write %s\n", csv); }
        else {
            fprintf(f, "panner,tracked,lis,tgt,mic_x,mic_y,mic_z,"
                       "tgt_x,tgt_y,tgt_z,meas_x,meas_y,meas_z,miss_deg,diffuseness,ok\n");
            for (int i = 0; i < w; ++i) {
                ValidCell* c = &cells[i];
                fprintf(f, "%s,%d,%d,%d,%.4f,%.4f,%.4f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.3f,%.3f,%d\n",
                        pan_name(c->panner), c->tracked, c->lis, c->tgt,
                        c->mic[0], c->mic[1], c->mic[2],
                        c->target[0], c->target[1], c->target[2],
                        c->measured[0], c->measured[1], c->measured[2],
                        c->miss_deg, c->diffuseness, c->ok);
            }
            fclose(f);
            printf("wrote %d cells to %s\n", w, csv);
        }
    }

    free(cells); free(feeds); free(cap);
    return 0;
}
