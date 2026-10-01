/*
 * survey.h - the optical speaker survey's math (bwa_speaker_survey). Pure, alloc-free, control
 * side only; unit-tested by test/survey_test.c.
 *
 * The acoustic survey (bwa_calibrate --zylia) places every speaker, but nothing checks a speaker's
 * AIM to better than about 10 degrees, and nothing checks that Motive's frame IS the room frame the
 * layout is written in. The few speakers the cameras can see answer both: give each a rigid body
 * in Motive, read the bodies off the NatNet stream, and compare.
 *
 * The aim comes from the MARKERS, not from the body's local axes: with 3 or more markers stuck
 * flat on the front baffle, the baffle plane is the best-fit plane through the body's marker
 * offsets (NatNet's MODELDEF carries them in the body's frame), rotated into the room by the
 * body's pose, and the aim is that plane's normal. The sign is the one that faces the layout's
 * listening point. So no Motive orientation convention enters, and a body created at any
 * orientation measures the same. A configured local axis is the fallback for a body whose markers
 * are not on the baffle.
 *
 * The frame check fits a rigid transform (rotation + translation, no scale) from the optical
 * positions onto the layout's, by Horn's quaternion method (the closed-form Kabsch solution that
 * can only return a proper rotation), and fits it a second time with the optical x axis mirrored.
 * A mirrored Motive frame fits the mirrored solve better. Three points, or any coplanar set, fit a
 * mirror EXACTLY as well as the truth (reflect across their own plane), so positions alone decide
 * handedness only when the matched speakers leave a plane; otherwise the aims decide it, and when
 * they lie in that plane too the verdict is UNDETERMINED rather than a guess.
 */
#ifndef BWA_SURVEY_H
#define BWA_SURVEY_H

#include <stdbool.h>
#include <stddef.h>

/* ---- thresholds (the defaults; SurveyOpts overrides the per-body ones) ---- */
#define SURVEY_PLANE_MAX_RMS_M  0.003f  /* markers off their best plane by more than this RMS: not flat on one baffle */
#define SURVEY_MIN_ASPECT       0.15f   /* sqrt(l_mid / l_max) of the marker spread below this: colinear */
#define SURVEY_MIN_WIDTH_M      0.005f  /* or an RMS extent across the long axis below this: colinear */
#define SURVEY_MAX_SPREAD_M     0.002f  /* averaged pose: position RMS spread past this refuses the body */
#define SURVEY_MAX_SPREAD_DEG   0.5f    /* averaged pose: rotation spread past this refuses the body */
#define SURVEY_MIN_FRAMES       10      /* fewer tracking-valid frames than this refuses the body */

/* ---- one speaker from one (averaged) body pose ---- */
enum {
    SURVEY_AIM_OK = 0,          /* plane normal, sign toward the listening point */
    SURVEY_AIM_AXIS,            /* the configured local axis (SurveyOpts.use_axis) */
    SURVEY_AIM_FEW_MARKERS,     /* fewer than 3 marker offsets */
    SURVEY_AIM_COLINEAR,        /* the markers are (nearly) on a line: no plane */
    SURVEY_AIM_NOT_FLAT,        /* the markers are not on one plane within max_plane_rms_m */
    SURVEY_AIM_BAD_AXIS         /* use_axis with a zero or non-finite axis */
};
const char* survey_aim_status_str(int status);   /* short ASCII phrase for a report */

typedef struct {
    float max_plane_rms_m;   /* 0 -> SURVEY_PLANE_MAX_RMS_M */
    float baffle_offset_m;   /* the layout's point for a speaker sits this far BEHIND the baffle
                              * (along -aim); 0 = the marker centroid on the baffle itself */
    bool  use_axis;          /* aim = R(q) * axis_local instead of the plane normal */
    float axis_local[3];     /* the body-frame axis along the speaker's acoustic axis */
} SurveyOpts;

typedef struct {
    float pos[3];            /* room point: marker centroid, moved behind the baffle when the aim is good */
    float centroid[3];       /* the marker centroid on the baffle (body position with no markers) */
    float aim[3];            /* unit; meaningful when aim_ok */
    bool  aim_ok;
    int   aim_status;        /* SURVEY_AIM_* */
    bool  aim_away;          /* the chosen aim points AWAY from the listening point (axis mode only;
                              * the plane mode flips it instead) */
    int   n_markers;
    float plane_rms_m;       /* RMS out-of-plane distance of the markers (0 with fewer than 3) */
    float aspect;            /* sqrt(l_mid / l_max): 0 = colinear, 1 = evenly spread */
    float width_m;           /* RMS extent across the markers' long axis */
} SurveySpeaker;

/* Evaluate one body. body_q is xyzw (normalized here). markers are the body-frame offsets from the
 * MODELDEF, n of them (0 ok). ref is the layout's listening point. o NULL = defaults. */
void survey_speaker(const float body_pos[3], const float body_q[4], const float (*markers)[3], int n,
                    const float ref[3], const SurveyOpts* o, SurveySpeaker* out);

/* ---- averaging N frames of one body ---- */
typedef struct {
    int    n;
    double p0[3];            /* first sample: the rest accumulate relative to it (no cancellation) */
    double sp[3], spp;       /* sum and sum of squares of (p - p0) */
    double sq[4];            /* sign-aligned quaternion sum */
} SurveyAvg;

void survey_avg_reset(SurveyAvg* a);
/* Add one pose. A non-finite pose or a zero quaternion is skipped (returns false). */
bool survey_avg_add(SurveyAvg* a, const float p[3], const float q[4]);
/* Mean position, mean rotation (normalized sign-aligned sum), the position RMS spread (m) and the
 * rotation spread (deg): 2 acos(|mean of unit quats|), which is the RMS angle from the mean for a
 * small spread. False with no samples. */
bool survey_avg_result(const SurveyAvg* a, float p[3], float q[4], float* spread_m, float* spread_deg);

/* ---- frame agreement: optical (Motive) -> layout (room) ---- */
enum { SURVEY_HAND_UNDETERMINED = 0, SURVEY_HAND_OK, SURVEY_HAND_MIRRORED };
enum { SURVEY_HAND_BY_NONE = 0, SURVEY_HAND_BY_POSITIONS, SURVEY_HAND_BY_AIMS };

typedef struct {
    int   n;                 /* matched speakers used */
    float R[3][3], t[3];     /* layout = R * optical + t (the PROPER fit, reported either way) */
    float angle_deg;         /* rotation angle of R */
    float axis[3];           /* its unit axis ((0,1,0) when the angle is 0) */
    float rms_m, max_m;      /* position residual of the proper fit */
    int   max_i;             /* which point has max_m */
    float rms_mirror_m;      /* position residual of the fit after mirroring the optical x axis */
    float aim_deg;           /* mean aim disagreement under the proper fit (-1: no usable aim) */
    float aim_mirror_deg;    /* same under the mirrored fit */
    float thickness_m;       /* RMS distance of the layout points from their best plane */
    int   hand;              /* SURVEY_HAND_* */
    int   hand_by;           /* SURVEY_HAND_BY_*: which evidence decided it */
} SurveyFrameFit;

/* Fit with 3 <= n <= 64 matched points. opt_aim/lay_aim/aim_ok may be NULL (no aim evidence); an entry
 * with aim_ok[i] false is left out of the aim numbers. False with n out of range, a non-finite
 * point, or layout points on (nearly) one line, where the rotation about that line is free. */
bool survey_fit_frame(const float (*opt)[3], const float (*lay)[3],
                      const float (*opt_aim)[3], const float (*lay_aim)[3], const bool* aim_ok,
                      int n, SurveyFrameFit* out);

/* The two-match check: the distance between the two optical points minus the distance between the
 * two layout points (m). A rigid frame change leaves it 0; a scale or a gross mismatch does not. */
float survey_pair_delta(const float opt0[3], const float opt1[3], const float lay0[3], const float lay1[3]);

/* ---- the baffle depth: the optical points against ACOUSTIC ones ----
 * Layout positions are acoustic centers. Once --localize has MEASURED them, an optical point (the
 * marker centroid on the baffle, less any baffle offset already applied) sits in front of its acoustic
 * center by the baffle-to-acoustic-center depth, along the box's aim. A plain frame fit cannot read
 * that depth off afterward: a rigid fit of points all pushed along their aims absorbs much of the push
 * into its translation (a 60 mm depth read back as anywhere from 14 to 61 mm on four dome_24 subsets
 * of 4 and 6 speakers, and the lopsided subsets then failed the 10 mm frame check on the depth alone).
 * So the depth is a FOURTH unknown of the fit:
 *
 *   layout_i = R (optical_i - depth aim_i) + t
 *
 * solved by alternating Horn's rotation (depth fixed) with the closed-form (t, depth) least squares
 * (rotation fixed) until the depth moves less than a nanometer. depth > 0 = the acoustic center sits
 * BEHIND the baffle. The depth is separable from the translation only through the spread of the aims
 * (b_i = R aim_i): its standard error is the position noise over `leverage`, so boxes that all face
 * one way cannot measure it. */
#define SURVEY_DEPTH_AGREE_M     0.020f  /* PROVISIONAL: a speaker whose depth is this far off the median,
                                          * or whose difference has this much ACROSS its aim, says the two
                                          * surveys disagree (the acoustic survey's simulated worst is
                                          * about 12 mm, the optical one's about 1 mm) */
#define SURVEY_DEPTH_MIN_LEVERAGE 0.5f   /* PROVISIONAL: below this the depth moves more than 2 mm per mm
                                          * of position error; the report warns */
typedef struct {
    int   n;
    float R[3][3], t[3];     /* layout = R * (optical - depth * aim) + t */
    float angle_deg, t_m;    /* the rotation's angle and |t|: the frame check's two numbers, depth solved */
    float depth_m;           /* the common depth, least squares */
    float leverage;          /* sqrt(sum |b_i - mean b|^2): the depth's standard error is noise / this */
    float rms_m;             /* the joint fit's position residual */
    int   iters;
} SurveyDepthFit;
/* Fit with 3 <= n <= 64 speakers. opt: the optical points; aim: their unit OPTICAL aims, same frame;
 * lay: the acoustic positions. depth_i / across_i (n each, NULL ok) receive, per speaker,
 * (R opt_i + t - lay_i) . (R aim_i), and the length of what is left across the aim. False with n out of
 * range, a non-finite input, layout points on (nearly) one line, or aims too alike to separate the depth
 * from the translation. */
bool survey_fit_depth(const float (*opt)[3], const float (*aim)[3], const float (*lay)[3], int n,
                      SurveyDepthFit* out, float* depth_i, float* across_i);

/* ---- rigid-body name -> layout index ---- */

/* The default rule: `spk` or `speaker`, any case, then at most one separator (`_`, `-`, `.` or a
 * space), then decimal digits (leading zeros ok) and nothing else. spk07, Speaker_7, SPK-7 -> 7.
 * Returns the number, or -1 when the name does not follow the rule. */
int survey_name_index(const char* name);

typedef struct { const char* name; int index; } SurveyMapEntry;   /* one --map name=index pair */

enum {
    SURVEY_MATCH_OK = 0,
    SURVEY_MATCH_NO_RULE,        /* not in --map and not spk<N>/speaker<N>: ignored */
    SURVEY_MATCH_OUT_OF_RANGE,   /* the index is not a speaker in this layout */
    SURVEY_MATCH_DUPLICATE       /* another body already holds that index */
};
typedef struct {
    int  index;                  /* layout index when status == OK, else the index it asked for (or -1) */
    int  status;                 /* SURVEY_MATCH_* */
    bool by_map;                 /* matched through --map */
    int  other;                  /* DUPLICATE: the body index that holds the speaker */
} SurveyMatch;

/* Match n body names against a layout of `count` speakers. --map entries (case-insensitive exact
 * name) beat the default rule; within one kind the first body in wire order wins. map_used[k]
 * (NULL ok) reports whether map entry k named a body at all, so a typo in --map is visible. */
void survey_match_names(const char* const* names, int n, const SurveyMapEntry* map, int nmap,
                        int count, SurveyMatch* out, bool* map_used);

/* ---- small shared helpers (exported for the tool and the tests) ---- */
void  survey_quat_to_mat(const float q[4], float R[3][3]);            /* unit xyzw -> rotation matrix */
float survey_angle_deg(const float a[3], const float b[3]);            /* angle between two vectors */

#endif /* BWA_SURVEY_H */
