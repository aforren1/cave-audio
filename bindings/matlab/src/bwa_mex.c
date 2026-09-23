/*
 * bwa_mex.c - the gateway of the MATLAB and Octave binding: bwa_mex.
 *
 * ONE MEX file, dispatching on a subcommand string, the shape this audience already knows from
 * PsychPortAudio:
 *
 *     h = bwa_mex('create', cfg);
 *     bwa_mex('source_play', h, src, snd, false);
 *     [dsp, host] = bwa_mex('get_clock', h);
 *
 * One subcommand per BWA_API entry point, named by its C name minus the `bwa_` prefix, with the
 * header's own argument order and units. Nothing is renamed, reordered or re-unitted here; the
 * ergonomics live one layer up, in the +bwa package. This mirrors bindings/python's raw layer call
 * for call - read include/bw_audio.h or docs/api.md and you are reading this layer.
 *
 * WHY THE CLASSIC C MEX API. Octave implements it; the MATLAB-only C++ Data API it does not. So
 * this file uses mxGetData/mxGetPr, mxCreateNumericMatrix, mxCreateString, mxIsChar, mxIsNumeric,
 * mexErrMsgIdAndTxt, mexLock/mexUnlock and mexAtExit, and nothing else. It compiles with
 * `mex -R2017b` under MATLAB and with `mkoctfile --mex` under Octave, from this one source.
 *
 * Departures from a literal 1:1, each of which a literal binding could not express:
 *
 *   - `set_output_capture` is present but REFUSES, with the reason. Its callback runs on the AUDIO
 *     thread, and an interpreter must never run there - CLAUDE.md invariant 1, and
 *     docs/backends.md's own rule for this binding. The offline path is the manual sink plus
 *     `render_block`, which hands back the same post-limiter samples on the caller's own thread.
 *   - Out-parameters become extra outputs: [dsp, host] = bwa_mex('get_clock', h), and a bool plus
 *     out-struct call returns [] on false and a struct on true. A char* out-buffer (device name or
 *     id) returns a char row, or '' when the index is out of range.
 *   - `render_block` returns a COPY, as an [nframes, channels] single matrix. An mxArray cannot
 *     alias engine memory, so where Python hands back a zero-copy view this one memcpy's. The
 *     engine's planar block is channel-major with a channel stride of nframes, which IS MATLAB's
 *     column-major layout for [nframes, channels], so the copy is one memcpy and no transpose.
 *   - `constants` is the one subcommand with no C twin: it returns the enum values, the caps and
 *     the room basis this file compiled against, so the +bwa package does not re-hardcode the ABI.
 *
 * LIFETIME. Live engines are held in a registry, keyed by the uint64 the caller holds, so a stale
 * handle raises instead of dereferencing freed memory. The MEX is mexLock'd while any engine is
 * live, so `clear mex` cannot unload the gateway out from under a running audio thread, and a
 * mexAtExit handler stops and destroys whatever is still live, so `clear all` or an interpreter
 * exit never leaves a device open.
 *
 * THREADING. The engine's one-control-thread rule is satisfied by construction: a MEX call runs on
 * the interpreter's main thread. Parallel-pool workers are separate processes with their own
 * address space, so an engine handle does not travel to one; do not try.
 *
 * Every message this file can print is ASCII. It reaches a console (CLAUDE.md, Traps).
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "mex.h"
#include "bw_audio.h"

/* ------------------------------------------------------------------ engine registry */

/* Small and fixed: a session drives one engine, or two for an A/B. A linear scan of this is the
 * whole handle check, and it runs once per call on the control thread. */
#define BWA_MEX_MAX_ENGINES 32

static bwa_engine* g_engines[BWA_MEX_MAX_ENGINES];
static int         g_engine_count = 0;
static int         g_atexit_done = 0;

/* Cleared with `clear mex`, or at interpreter exit. Anything still live owns a device and an audio
 * thread, so stop before destroy and leave nothing open. */
static void mex_atexit(void)
{
    int i;
    for (i = 0; i < BWA_MEX_MAX_ENGINES; ++i) {
        if (g_engines[i]) {
            bwa_stop(g_engines[i]);
            bwa_destroy(g_engines[i]);
            g_engines[i] = NULL;
        }
    }
    g_engine_count = 0;
}

static void mex_register(bwa_engine* e)
{
    int i;
    if (!g_atexit_done) { mexAtExit(mex_atexit); g_atexit_done = 1; }
    for (i = 0; i < BWA_MEX_MAX_ENGINES; ++i) {
        if (!g_engines[i]) {
            g_engines[i] = e;
            ++g_engine_count;
            /* One lock per live engine. mexLock is refcounted, so the matching unlock in
             * mex_unregister leaves the file unlocked exactly when the last engine goes. */
            mexLock();
            return;
        }
    }
    bwa_destroy(e);
    mexErrMsgIdAndTxt("bwa:tooManyEngines",
        "bwa_mex: more than %d live engines. Destroy one before creating another.",
        BWA_MEX_MAX_ENGINES);
}

static void mex_unregister(bwa_engine* e)
{
    int i;
    for (i = 0; i < BWA_MEX_MAX_ENGINES; ++i) {
        if (g_engines[i] == e) {
            g_engines[i] = NULL;
            --g_engine_count;
            mexUnlock();
            return;
        }
    }
}

/* ------------------------------------------------------------------ argument helpers */

static void need(int nrhs, int want, const char* cmd)
{
    if (nrhs != want)
        mexErrMsgIdAndTxt("bwa:usage",
            "bwa_mex('%s'): expected %d argument(s) after the command, got %d.", cmd, want, nrhs);
}

static void need_range(int nrhs, int lo, int hi, const char* cmd)
{
    if (nrhs < lo || nrhs > hi)
        mexErrMsgIdAndTxt("bwa:usage",
            "bwa_mex('%s'): expected %d to %d argument(s) after the command, got %d.",
            cmd, lo, hi, nrhs);
}

static void want_scalar(const mxArray* a, const char* cmd)
{
    if (!a || mxIsEmpty(a) || (!mxIsNumeric(a) && !mxIsLogical(a)) || mxGetNumberOfElements(a) != 1)
        mexErrMsgIdAndTxt("bwa:usage", "bwa_mex('%s'): expected a numeric or logical scalar.", cmd);
}

static uint64_t get_u64(const mxArray* a, const char* cmd)
{
    want_scalar(a, cmd);
    /* A uint64 wider than 2^53 (an engine pointer may be) survives only if it is read as itself
     * rather than through the double mxGetScalar returns. */
    if (mxIsUint64(a)) return *(const uint64_t*)mxGetData(a);
    if (mxIsInt64(a))  return (uint64_t)(*(const int64_t*)mxGetData(a));
    {
        double d = mxGetScalar(a);
        if (d < 0.0) d = 0.0;
        return (uint64_t)d;
    }
}

static uint32_t get_u32(const mxArray* a, const char* cmd) { return (uint32_t)get_u64(a, cmd); }

static int32_t get_i32(const mxArray* a, const char* cmd)
{
    want_scalar(a, cmd);
    return (int32_t)mxGetScalar(a);
}

static float get_f32(const mxArray* a, const char* cmd)
{
    want_scalar(a, cmd);
    return (float)mxGetScalar(a);
}

static bool get_bool(const mxArray* a, const char* cmd)
{
    want_scalar(a, cmd);
    return mxGetScalar(a) != 0.0;
}

/* NULL for [] or '', which is how the ABI's "the engine's default" is spelled here. An empty
 * layout path is a load failure, not a default grid, so the two must stay distinguishable. */
static char* get_str_or_null(const mxArray* a, const char* cmd)
{
    if (!a || mxIsEmpty(a)) return NULL;
    if (!mxIsChar(a))
        mexErrMsgIdAndTxt("bwa:usage",
            "bwa_mex('%s'): expected a char row (a string), or [] for none.", cmd);
    return mxArrayToString(a);
}

static char* get_str(const mxArray* a, const char* cmd)
{
    char* s = get_str_or_null(a, cmd);
    if (!s)
        mexErrMsgIdAndTxt("bwa:usage", "bwa_mex('%s'): expected a non-empty char row (a string).", cmd);
    return s;
}

/* A float view of any real numeric array. `single` is handed back as-is; everything else is
 * converted into an mxMalloc buffer the caller frees when *owned. */
static const float* get_f32_array(const mxArray* a, size_t* n_out, int* owned, const char* cmd)
{
    size_t n, i;
    *owned = 0;
    *n_out = 0;
    if (!a || mxIsEmpty(a)) return NULL;
    if (!mxIsNumeric(a) || mxIsComplex(a))
        mexErrMsgIdAndTxt("bwa:usage", "bwa_mex('%s'): expected a real numeric array.", cmd);
    n = mxGetNumberOfElements(a);
    *n_out = n;
    if (mxIsSingle(a)) return (const float*)mxGetData(a);
    {
        float* buf = (float*)mxMalloc(n * sizeof(float));
        *owned = 1;
        if (mxIsDouble(a)) {
            const double* s = mxGetPr(a);
            for (i = 0; i < n; ++i) buf[i] = (float)s[i];
        } else if (mxIsInt32(a)) {
            const int32_t* s = (const int32_t*)mxGetData(a);
            for (i = 0; i < n; ++i) buf[i] = (float)s[i];
        } else if (mxIsUint32(a)) {
            const uint32_t* s = (const uint32_t*)mxGetData(a);
            for (i = 0; i < n; ++i) buf[i] = (float)s[i];
        } else if (mxIsInt16(a)) {
            const int16_t* s = (const int16_t*)mxGetData(a);
            for (i = 0; i < n; ++i) buf[i] = (float)s[i];
        } else {
            mxFree(buf);
            mexErrMsgIdAndTxt("bwa:usage",
                "bwa_mex('%s'): pass single or double samples (int16/int32/uint32 also convert).", cmd);
            return NULL;
        }
        return buf;
    }
}

static const int32_t* get_i32_array(const mxArray* a, size_t* n_out, int* owned, const char* cmd)
{
    size_t n, i;
    *owned = 0;
    *n_out = 0;
    if (!a || mxIsEmpty(a)) return NULL;
    if (!mxIsNumeric(a) || mxIsComplex(a))
        mexErrMsgIdAndTxt("bwa:usage", "bwa_mex('%s'): expected a real numeric array.", cmd);
    n = mxGetNumberOfElements(a);
    *n_out = n;
    if (mxIsInt32(a)) return (const int32_t*)mxGetData(a);
    {
        int32_t* buf = (int32_t*)mxMalloc(n * sizeof(int32_t));
        *owned = 1;
        if (mxIsDouble(a)) {
            const double* s = mxGetPr(a);
            for (i = 0; i < n; ++i) buf[i] = (int32_t)s[i];
        } else if (mxIsSingle(a)) {
            const float* s = (const float*)mxGetData(a);
            for (i = 0; i < n; ++i) buf[i] = (int32_t)s[i];
        } else if (mxIsUint32(a)) {
            const uint32_t* s = (const uint32_t*)mxGetData(a);
            for (i = 0; i < n; ++i) buf[i] = (int32_t)s[i];
        } else {
            mxFree(buf);
            mexErrMsgIdAndTxt("bwa:usage",
                "bwa_mex('%s'): pass int32, uint32, single or double triangle indices.", cmd);
            return NULL;
        }
        return buf;
    }
}

static const uint32_t* get_u32_array(const mxArray* a, size_t* n_out, int* owned, const char* cmd)
{
    size_t n, i;
    *owned = 0;
    *n_out = 0;
    if (!a || mxIsEmpty(a)) return NULL;
    if (!mxIsNumeric(a) || mxIsComplex(a))
        mexErrMsgIdAndTxt("bwa:usage", "bwa_mex('%s'): expected a real numeric array.", cmd);
    n = mxGetNumberOfElements(a);
    *n_out = n;
    if (mxIsUint32(a)) return (const uint32_t*)mxGetData(a);
    {
        uint32_t* buf = (uint32_t*)mxMalloc(n * sizeof(uint32_t));
        *owned = 1;
        if (mxIsDouble(a)) {
            const double* s = mxGetPr(a);
            for (i = 0; i < n; ++i) buf[i] = (uint32_t)s[i];
        } else if (mxIsSingle(a)) {
            const float* s = (const float*)mxGetData(a);
            for (i = 0; i < n; ++i) buf[i] = (uint32_t)s[i];
        } else if (mxIsInt32(a)) {
            const int32_t* s = (const int32_t*)mxGetData(a);
            for (i = 0; i < n; ++i) buf[i] = (uint32_t)s[i];
        } else {
            mxFree(buf);
            mexErrMsgIdAndTxt("bwa:usage",
                "bwa_mex('%s'): pass uint32, int32, single or double material tokens.", cmd);
            return NULL;
        }
        return buf;
    }
}

/* An (n,3) array of room-space triples, as the flat xyz stream the C side wants. The caller
 * mxFree's the result.
 *
 * This transpose is not a convenience. MATLAB is COLUMN-MAJOR, so an (n,3) matrix sits in memory
 * as x1..xn y1..yn z1..zn, and handing that straight to a call that reads xyz triples garbles
 * every position after the first. It is the kind of bug that produces a plausible-looking gain
 * vector rather than an error. Rows are also the shape everything here HANDS BACK - get_speakers
 * and box_mesh both return (n,3) - so the round trip a caller will try first has to work.
 * A 3-element vector is one row, whichever way round it is written. */
static float* get_xyz_rows(const mxArray* a, uint32_t* n_out, const char* cmd)
{
    size_t rows, cols, i;
    size_t nsrc; int owned;
    const float* p;
    float* out;
    *n_out = 0;
    if (!a || mxIsEmpty(a)) return NULL;
    if (mxGetNumberOfDimensions(a) > 2)
        mexErrMsgIdAndTxt("bwa:usage", "bwa_mex('%s'): expected an (n,3) array, not an N-D one.", cmd);
    rows = mxGetM(a);
    cols = mxGetN(a);
    if (mxGetNumberOfElements(a) == 3 && (rows == 1 || cols == 1)) { rows = 1; cols = 3; }
    if (cols != 3)
        mexErrMsgIdAndTxt("bwa:usage",
            "bwa_mex('%s'): expected an (n,3) array, one xyz position per ROW; got %dx%d.",
            cmd, (int)mxGetM(a), (int)mxGetN(a));
    p = get_f32_array(a, &nsrc, &owned, cmd);
    out = (float*)mxMalloc(rows * 3 * sizeof(float));
    for (i = 0; i < rows; ++i) {
        out[i * 3 + 0] = p[i];
        out[i * 3 + 1] = p[rows + i];
        out[i * 3 + 2] = p[2 * rows + i];
    }
    if (owned) mxFree((void*)p);
    *n_out = (uint32_t)rows;
    return out;
}

/* The index counterpart: an (m,3) array of CCW triangle indices, transposed for the same reason. */
static int32_t* get_tri_rows(const mxArray* a, uint32_t* n_out, const char* cmd)
{
    size_t rows, cols, i;
    size_t nsrc; int owned;
    const int32_t* p;
    int32_t* out;
    *n_out = 0;
    if (!a || mxIsEmpty(a)) return NULL;
    rows = mxGetM(a);
    cols = mxGetN(a);
    if (mxGetNumberOfElements(a) == 3 && (rows == 1 || cols == 1)) { rows = 1; cols = 3; }
    if (cols != 3)
        mexErrMsgIdAndTxt("bwa:usage",
            "bwa_mex('%s'): expected an (m,3) array, one triangle per ROW; got %dx%d.",
            cmd, (int)mxGetM(a), (int)mxGetN(a));
    p = get_i32_array(a, &nsrc, &owned, cmd);
    out = (int32_t*)mxMalloc(rows * 3 * sizeof(int32_t));
    for (i = 0; i < rows; ++i) {
        out[i * 3 + 0] = p[i];
        out[i * 3 + 1] = p[rows + i];
        out[i * 3 + 2] = p[2 * rows + i];
    }
    if (owned) mxFree((void*)p);
    *n_out = (uint32_t)rows;
    return out;
}

/* A fixed-length float vector (a triple, a quad, six face tokens as floats). */
static void get_f32_n(const mxArray* a, float* out, size_t want, const char* cmd)
{
    size_t n; int owned; const float* p = get_f32_array(a, &n, &owned, cmd);
    if (n != want) {
        if (owned) mxFree((void*)p);
        mexErrMsgIdAndTxt("bwa:usage",
            "bwa_mex('%s'): expected %d elements, got %d.", cmd, (int)want, (int)n);
    }
    memcpy(out, p, want * sizeof(float));
    if (owned) mxFree((void*)p);
}

static void get_faces(const mxArray* a, bwa_material* out, const char* cmd)
{
    size_t n; int owned; const uint32_t* p = get_u32_array(a, &n, &owned, cmd);
    if (n != 6) {
        if (owned) mxFree((void*)p);
        mexErrMsgIdAndTxt("bwa:usage",
            "bwa_mex('%s'): faces needs 6 material tokens, in the order (-x,+x,-y,+y,-z,+z).", cmd);
    }
    memcpy(out, p, 6 * sizeof(bwa_material));
    if (owned) mxFree((void*)p);
}

/* ------------------------------------------------------------------ engine handle */

static bwa_engine* get_engine(const mxArray* a, const char* cmd)
{
    uint64_t v = get_u64(a, cmd);
    bwa_engine* e = (bwa_engine*)(uintptr_t)v;
    int i;
    if (v == 0)
        mexErrMsgIdAndTxt("bwa:deadEngine",
            "bwa_mex('%s'): the engine handle is 0. It was never created, or it has been destroyed.",
            cmd);
    for (i = 0; i < BWA_MEX_MAX_ENGINES; ++i)
        if (g_engines[i] == e) return e;
    mexErrMsgIdAndTxt("bwa:deadEngine",
        "bwa_mex('%s'): no live engine with that handle. It was destroyed, or it came from an "
        "interpreter session that has since ended.", cmd);
    return NULL;
}

/* ------------------------------------------------------------------ return helpers */

static mxArray* ret_u64(uint64_t v)
{
    mxArray* a = mxCreateNumericMatrix(1, 1, mxUINT64_CLASS, mxREAL);
    *(uint64_t*)mxGetData(a) = v;
    return a;
}

static mxArray* ret_dbl(double v) { return mxCreateDoubleScalar(v); }
static mxArray* ret_bool(bool v)  { return mxCreateLogicalScalar(v ? 1 : 0); }
static mxArray* ret_str(const char* s) { return mxCreateString(s ? s : ""); }
static mxArray* ret_empty(void) { return mxCreateDoubleMatrix(0, 0, mxREAL); }

static mxArray* ret_f32(const float* src, size_t rows, size_t cols)
{
    mxArray* a = mxCreateNumericMatrix((mwSize)rows, (mwSize)cols, mxSINGLE_CLASS, mxREAL);
    if (src && rows && cols) memcpy(mxGetData(a), src, rows * cols * sizeof(float));
    return a;
}

static void sf_dbl (mxArray* s, const char* f, double v)   { mxSetField(s, 0, f, mxCreateDoubleScalar(v)); }
static void sf_u64 (mxArray* s, const char* f, uint64_t v) { mxSetField(s, 0, f, ret_u64(v)); }
static void sf_bool(mxArray* s, const char* f, bool v)     { mxSetField(s, 0, f, mxCreateLogicalScalar(v ? 1 : 0)); }

/* Read a numeric struct field, or `dflt` when it is absent or empty. Missing fields are the ABI's
 * zero defaults, which is what makes a partly-filled struct a legal desc. */
static double gf(const mxArray* s, const char* name, double dflt, const char* cmd)
{
    const mxArray* f;
    if (!mxIsStruct(s))
        mexErrMsgIdAndTxt("bwa:usage", "bwa_mex('%s'): expected a struct.", cmd);
    f = mxGetField(s, 0, name);
    if (!f || mxIsEmpty(f)) return dflt;
    if (!mxIsNumeric(f) && !mxIsLogical(f))
        mexErrMsgIdAndTxt("bwa:usage",
            "bwa_mex('%s'): field '%s' must be numeric or logical.", cmd, name);
    return mxGetScalar(f);
}

/* A string field, or NULL when absent or empty. The caller mxFree's it. */
static char* gfs(const mxArray* s, const char* name, const char* cmd)
{
    const mxArray* f;
    if (!mxIsStruct(s))
        mexErrMsgIdAndTxt("bwa:usage", "bwa_mex('%s'): expected a struct.", cmd);
    f = mxGetField(s, 0, name);
    if (!f || mxIsEmpty(f)) return NULL;
    if (!mxIsChar(f))
        mexErrMsgIdAndTxt("bwa:usage", "bwa_mex('%s'): field '%s' must be a char row.", cmd, name);
    return mxArrayToString(f);
}

/* ------------------------------------------------------------------ handler shapes
 *
 * Most entry points are one of a dozen argument shapes. Generating those keeps the hand-written
 * code to the calls that really are different, which is where a binding bug would hide.
 */

#define CMD(name) static void h_##name(int nlhs, mxArray** plhs, int nrhs, const mxArray** prhs)
#define NOOUT (void)nlhs; (void)plhs;
#define E     bwa_engine* e = get_engine(prhs[0], cmd)

#define GEN_E_VOID(n)      CMD(n){const char*cmd=#n;NOOUT need(nrhs,1,cmd);{E;bwa_##n(e);}}
#define GEN_E_F(n)         CMD(n){const char*cmd=#n;NOOUT need(nrhs,2,cmd);{E;bwa_##n(e,get_f32(prhs[1],cmd));}}
#define GEN_E_FF(n)        CMD(n){const char*cmd=#n;NOOUT need(nrhs,3,cmd);{E;bwa_##n(e,get_f32(prhs[1],cmd),get_f32(prhs[2],cmd));}}
#define GEN_E_FFF(n)       CMD(n){const char*cmd=#n;NOOUT need(nrhs,4,cmd);{E;bwa_##n(e,get_f32(prhs[1],cmd),get_f32(prhs[2],cmd),get_f32(prhs[3],cmd));}}
#define GEN_E_B(n)         CMD(n){const char*cmd=#n;NOOUT need(nrhs,2,cmd);{E;bwa_##n(e,get_bool(prhs[1],cmd));}}
#define GEN_E_U32(n,T)     CMD(n){const char*cmd=#n;NOOUT need(nrhs,2,cmd);{E;bwa_##n(e,(T)get_u32(prhs[1],cmd));}}
#define GEN_E_U32F(n)      CMD(n){const char*cmd=#n;NOOUT need(nrhs,3,cmd);{E;bwa_##n(e,get_u32(prhs[1],cmd),get_f32(prhs[2],cmd));}}
#define GEN_E_U32B(n)      CMD(n){const char*cmd=#n;NOOUT need(nrhs,3,cmd);{E;bwa_##n(e,get_u32(prhs[1],cmd),get_bool(prhs[2],cmd));}}
#define GEN_EH_VOID(n)     CMD(n){const char*cmd=#n;NOOUT need(nrhs,2,cmd);{E;bwa_##n(e,get_u32(prhs[1],cmd));}}
#define GEN_EH_F(n)        CMD(n){const char*cmd=#n;NOOUT need(nrhs,3,cmd);{E;bwa_##n(e,get_u32(prhs[1],cmd),get_f32(prhs[2],cmd));}}
#define GEN_EH_FF(n)       CMD(n){const char*cmd=#n;NOOUT need(nrhs,4,cmd);{E;bwa_##n(e,get_u32(prhs[1],cmd),get_f32(prhs[2],cmd),get_f32(prhs[3],cmd));}}
#define GEN_EH_FFF(n)      CMD(n){const char*cmd=#n;NOOUT need(nrhs,5,cmd);{E;bwa_##n(e,get_u32(prhs[1],cmd),get_f32(prhs[2],cmd),get_f32(prhs[3],cmd),get_f32(prhs[4],cmd));}}
#define GEN_EH_FFFF(n)     CMD(n){const char*cmd=#n;NOOUT need(nrhs,6,cmd);{E;bwa_##n(e,get_u32(prhs[1],cmd),get_f32(prhs[2],cmd),get_f32(prhs[3],cmd),get_f32(prhs[4],cmd),get_f32(prhs[5],cmd));}}
#define GEN_EH_B(n)        CMD(n){const char*cmd=#n;NOOUT need(nrhs,3,cmd);{E;bwa_##n(e,get_u32(prhs[1],cmd),get_bool(prhs[2],cmd));}}
#define GEN_EH_U32(n,T)    CMD(n){const char*cmd=#n;NOOUT need(nrhs,3,cmd);{E;bwa_##n(e,get_u32(prhs[1],cmd),(T)get_u32(prhs[2],cmd));}}
#define GEN_EH_I32(n)      CMD(n){const char*cmd=#n;NOOUT need(nrhs,3,cmd);{E;bwa_##n(e,get_u32(prhs[1],cmd),get_i32(prhs[2],cmd));}}
#define GEN_EH_U64(n)      CMD(n){const char*cmd=#n;NOOUT need(nrhs,3,cmd);{E;bwa_##n(e,get_u32(prhs[1],cmd),get_u64(prhs[2],cmd));}}
#define GEN_EH_U64U64(n)   CMD(n){const char*cmd=#n;NOOUT need(nrhs,4,cmd);{E;bwa_##n(e,get_u32(prhs[1],cmd),get_u64(prhs[2],cmd),get_u64(prhs[3],cmd));}}
#define GEN_EH_SB(n)       CMD(n){const char*cmd=#n;NOOUT need(nrhs,4,cmd);{E;bwa_##n(e,get_u32(prhs[1],cmd),get_u32(prhs[2],cmd),get_bool(prhs[3],cmd));}}
#define GEN_EH_SBU64(n)    CMD(n){const char*cmd=#n;NOOUT need(nrhs,5,cmd);{E;bwa_##n(e,get_u32(prhs[1],cmd),get_u32(prhs[2],cmd),get_bool(prhs[3],cmd),get_u64(prhs[4],cmd));}}
#define GEN_EH_SU64U64(n)  CMD(n){const char*cmd=#n;NOOUT need(nrhs,5,cmd);{E;bwa_##n(e,get_u32(prhs[1],cmd),get_u32(prhs[2],cmd),get_u64(prhs[3],cmd),get_u64(prhs[4],cmd));}}
#define GEN_E_RET_DBL(n)   CMD(n){const char*cmd=#n;(void)nlhs; need(nrhs,1,cmd);{E;plhs[0]=ret_dbl((double)bwa_##n(e));}}
#define GEN_E_RET_U64(n)   CMD(n){const char*cmd=#n;(void)nlhs; need(nrhs,1,cmd);{E;plhs[0]=ret_u64(bwa_##n(e));}}
#define GEN_E_RET_HANDLE(n)CMD(n){const char*cmd=#n;(void)nlhs; need(nrhs,1,cmd);{E;plhs[0]=ret_u64((uint64_t)bwa_##n(e));}}
#define GEN_EH_RET_DBL(n)  CMD(n){const char*cmd=#n;(void)nlhs; need(nrhs,2,cmd);{E;plhs[0]=ret_dbl((double)bwa_##n(e,get_u32(prhs[1],cmd)));}}
#define GEN_EH_RET_U64(n)  CMD(n){const char*cmd=#n;(void)nlhs; need(nrhs,2,cmd);{E;plhs[0]=ret_u64(bwa_##n(e,get_u32(prhs[1],cmd)));}}
#define GEN_EH_RET_BOOL(n) CMD(n){const char*cmd=#n;(void)nlhs; need(nrhs,2,cmd);{E;plhs[0]=ret_bool(bwa_##n(e,get_u32(prhs[1],cmd)));}}
#define GEN_LOADER(n)      CMD(n){const char*cmd=#n;(void)nlhs; need(nrhs,2,cmd);{E;char*p=get_str(prhs[1],cmd);bwa_sound s=bwa_##n(e,p);mxFree(p);plhs[0]=ret_u64((uint64_t)s);}}
#define GEN_ACQUIRE(n)     CMD(n){const char*cmd=#n;(void)nlhs; need(nrhs,3,cmd);{E;char*p=get_str(prhs[1],cmd);bwa_sound s=bwa_##n(e,p,get_u32(prhs[2],cmd));mxFree(p);plhs[0]=ret_u64((uint64_t)s);}}

/* ------------------------------------------------------------------ lifecycle */

CMD(create)
{
    const char* cmd = "create";
    bwa_desc c;
    bwa_engine* p;
    char *layout = NULL, *hrtf = NULL, *device = NULL;
    (void)nlhs;
    need_range(nrhs, 0, 1, cmd);
    memset(&c, 0, sizeof c);
    if (nrhs == 1) {
        if (!mxIsStruct(prhs[0]))
            mexErrMsgIdAndTxt("bwa:usage",
                "bwa_mex('create', cfg): cfg must be a struct mirroring bwa_desc. Every field is "
                "optional and every missing field is the ABI's zero default.");
        layout = gfs(prhs[0], "layout_path", cmd);
        hrtf   = gfs(prhs[0], "hrtf_path", cmd);
        device = gfs(prhs[0], "device", cmd);
        c.profile        = (bwa_profile)(int)gf(prhs[0], "profile", 0, cmd);
        c.layout_path    = layout;
        c.hrtf_path      = hrtf;
        c.sample_rate    = (uint32_t)gf(prhs[0], "sample_rate", 0, cmd);
        c.block_size     = (uint32_t)gf(prhs[0], "block_size", 0, cmd);
        c.sink           = (bwa_sink_type)(int)gf(prhs[0], "sink", 0, cmd);
        c.device         = device;
        c.embree         = gf(prhs[0], "embree", 0, cmd) != 0.0;
        c.enable_pathing = gf(prhs[0], "enable_pathing", 0, cmd) != 0.0;
        c.bed_decoder    = (bwa_bed_decoder)(int)gf(prhs[0], "bed_decoder", 0, cmd);
        c.sink_flags     = (uint32_t)gf(prhs[0], "sink_flags", 0, cmd);
    }
    p = bwa_create(&c);
    if (layout) mxFree(layout);
    if (hrtf)   mxFree(hrtf);
    if (device) mxFree(device);
    if (!p) {
        const char* why = bwa_last_error(NULL);
        mexErrMsgIdAndTxt("bwa:create", "bwa_create failed: %s", why ? why : "no reason reported");
    }
    mex_register(p);
    plhs[0] = ret_u64((uint64_t)(uintptr_t)p);
}

CMD(start)  { const char* cmd = "start"; (void)nlhs; need(nrhs, 1, cmd); { E; plhs[0] = ret_dbl((double)bwa_start(e)); } }
CMD(stop)   { const char* cmd = "stop";  (void)nlhs; need(nrhs, 1, cmd); { E; plhs[0] = ret_dbl((double)bwa_stop(e)); } }

CMD(destroy)
{
    const char* cmd = "destroy";
    uint64_t v;
    NOOUT
    need(nrhs, 1, cmd);
    v = get_u64(prhs[0], cmd);
    if (v == 0) return;                      /* idempotent, like the Python raw layer's destroy */
    {
        bwa_engine* e = (bwa_engine*)(uintptr_t)v;
        int i;
        for (i = 0; i < BWA_MEX_MAX_ENGINES; ++i) {
            if (g_engines[i] == e) {
                mex_unregister(e);
                bwa_destroy(e);
                return;
            }
        }
    }
    /* Unknown handle: silently done, because a double destroy is the normal shape of a cleanup
     * path that also ran in a `delete` method. */
}

CMD(last_error)
{
    const char* cmd = "last_error";
    const char* s;
    (void)nlhs;
    need_range(nrhs, 0, 1, cmd);
    if (nrhs == 0 || mxIsEmpty(prhs[0])) {
        s = bwa_last_error(NULL);            /* how a failed create reports its reason */
    } else {
        s = bwa_last_error(get_engine(prhs[0], cmd));
    }
    plhs[0] = ret_str(s);
}

CMD(get_version) { const char* cmd = "get_version"; (void)nlhs; need(nrhs, 0, cmd); plhs[0] = ret_dbl((double)bwa_get_version()); }

CMD(get_audio_backend)
{
    const char* cmd = "get_audio_backend";
    (void)nlhs; need(nrhs, 1, cmd);
    { E; plhs[0] = ret_str(bwa_get_audio_backend(e)); }
}

GEN_E_RET_DBL(get_sample_rate)
GEN_E_RET_DBL(get_block_size)
GEN_E_RET_DBL(get_sink_type)

/* ------------------------------------------------------------------ device query (no engine) */

CMD(get_device_count)
{
    const char* cmd = "get_device_count";
    (void)nlhs; need(nrhs, 1, cmd);
    plhs[0] = ret_dbl((double)bwa_get_device_count((bwa_sink_type)(int)get_u32(prhs[0], cmd)));
}

CMD(get_device_name)
{
    const char* cmd = "get_device_name";
    char buf[512];
    (void)nlhs; need(nrhs, 2, cmd);
    if (!bwa_get_device_name((bwa_sink_type)(int)get_u32(prhs[0], cmd), get_u32(prhs[1], cmd),
                             buf, (uint32_t)sizeof buf))
        plhs[0] = ret_str("");
    else
        plhs[0] = ret_str(buf);
}

CMD(get_device_id)
{
    const char* cmd = "get_device_id";
    char buf[512];
    (void)nlhs; need(nrhs, 2, cmd);
    if (!bwa_get_device_id((bwa_sink_type)(int)get_u32(prhs[0], cmd), get_u32(prhs[1], cmd),
                           buf, (uint32_t)sizeof buf))
        plhs[0] = ret_str("");
    else
        plhs[0] = ret_str(buf);
}

CMD(get_asio_driver_count)
{
    const char* cmd = "get_asio_driver_count";
    (void)nlhs; need(nrhs, 0, cmd);
    plhs[0] = ret_dbl((double)bwa_get_asio_driver_count());
}

CMD(get_asio_driver_name)
{
    const char* cmd = "get_asio_driver_name";
    char buf[512];
    (void)nlhs; need(nrhs, 1, cmd);
    if (!bwa_get_asio_driver_name(get_u32(prhs[0], cmd), buf, (uint32_t)sizeof buf))
        plhs[0] = ret_str("");
    else
        plhs[0] = ret_str(buf);
}

/* ------------------------------------------------------------------ assets */

GEN_LOADER(load_sound)
GEN_LOADER(load_sound_streaming)
GEN_LOADER(load_ambix)
GEN_LOADER(load_fuma)
GEN_EH_VOID(unload_sound)
GEN_EH_RET_U64(sound_get_frames)
GEN_EH_RET_DBL(sound_get_channels)
GEN_ACQUIRE(sound_acquire)
GEN_ACQUIRE(sound_acquire_async)
GEN_ACQUIRE(sound_find)
GEN_EH_VOID(sound_release)
GEN_EH_RET_BOOL(sound_is_ready)

/* ------------------------------------------------------------------ sources */

GEN_E_RET_HANDLE(source_create)
GEN_EH_VOID(source_destroy)
GEN_EH_I32(source_set_priority)
GEN_EH_FFF(source_set_pos)
GEN_EH_F(source_set_gain)
GEN_EH_FF(source_fade_to)
GEN_EH_F(source_fade_out)
GEN_EH_U32(source_set_group, uint32_t)
GEN_E_U32F(group_set_gain)
GEN_E_U32B(group_set_paused)
GEN_E_U32(group_stop, uint32_t)
GEN_E_VOID(stop_all)
GEN_EH_F(source_set_pitch)
GEN_EH_SB(source_play)
GEN_EH_SBU64(source_play_at)
GEN_EH_SU64U64(source_play_loop)
GEN_EH_VOID(source_stop)
GEN_EH_U64(source_stop_at)
GEN_EH_SB(source_queue)
GEN_EH_VOID(source_clear_queue)
GEN_EH_B(source_set_paused)
GEN_EH_U64(source_seek)
GEN_EH_U64U64(source_set_region)
GEN_EH_RET_BOOL(source_is_playing)
GEN_EH_RET_U64(source_get_playhead_frames)

static void poll_common(int nlhs, mxArray** plhs, int nrhs, const mxArray** prhs, const char* cmd,
                        uint32_t (*fn)(bwa_engine*, bwa_source*, uint32_t, uint64_t*))
{
    uint32_t cap = 64, n;
    uint64_t dropped = 0;
    bwa_source* buf;
    mxArray* out;
    uint64_t* dst;
    uint32_t i;
    (void)nlhs;
    need_range(nrhs, 1, 2, cmd);
    {
        E;
        if (nrhs == 2) cap = get_u32(prhs[1], cmd);
        if (cap == 0) cap = 1;
        buf = (bwa_source*)mxMalloc(cap * sizeof(bwa_source));
        n = fn(e, buf, cap, &dropped);
        out = mxCreateNumericMatrix((mwSize)n, n ? 1 : 0, mxUINT64_CLASS, mxREAL);
        dst = (uint64_t*)mxGetData(out);
        for (i = 0; i < n; ++i) dst[i] = (uint64_t)buf[i];
        mxFree(buf);
        plhs[0] = out;
        if (nlhs > 1) plhs[1] = ret_u64(dropped);
    }
}

CMD(poll_ended)  { poll_common(nlhs, plhs, nrhs, prhs, "poll_ended",  bwa_poll_ended); }
CMD(poll_looped) { poll_common(nlhs, plhs, nrhs, prhs, "poll_looped", bwa_poll_looped); }

CMD(play_oneshot)
{
    const char* cmd = "play_oneshot";
    (void)nlhs; need(nrhs, 6, cmd);
    {
        E;
        plhs[0] = ret_bool(bwa_play_oneshot(e, get_u32(prhs[1], cmd),
                                            get_f32(prhs[2], cmd), get_f32(prhs[3], cmd),
                                            get_f32(prhs[4], cmd), get_f32(prhs[5], cmd)));
    }
}

/* ------------------------------------------------------------------ clock */

GEN_E_RET_U64(get_dsp_time_frames)
GEN_E_RET_DBL(get_output_latency_frames)

/* Engine-free, and the one clock call that is: the monotonic clock every bwa_get_clock pair's
 * host_time_ns sits on. Read it either side of your own clock to measure the constant offset
 * between the two (see bwa.ClockBridge). */
CMD(host_time_ns)
{
    (void)nlhs; (void)prhs;
    need(nrhs, 0, "host_time_ns");
    plhs[0] = ret_u64(bwa_host_time_ns());
}

CMD(get_clock)
{
    const char* cmd = "get_clock";
    uint64_t s = 0, t = 0;
    (void)nlhs; need(nrhs, 1, cmd);
    {
        E;
        if (!bwa_get_clock(e, &s, &t)) {
            plhs[0] = ret_empty();
            if (nlhs > 1) plhs[1] = ret_empty();
            return;
        }
        plhs[0] = ret_u64(s);
        if (nlhs > 1) plhs[1] = ret_u64(t);
    }
}

CMD(get_clock_model)
{
    const char* cmd = "get_clock_model";
    static const char* f[] = { "ppm", "ppm_sigma", "rate_hz", "span_s", "jitter_ns", "stamps" };
    bwa_clock_model cm;
    (void)nlhs; need(nrhs, 1, cmd);
    {
        E;
        memset(&cm, 0, sizeof cm);
        if (!bwa_get_clock_model(e, &cm)) { plhs[0] = ret_empty(); return; }
        {
            mxArray* s = mxCreateStructMatrix(1, 1, 6, f);
            sf_dbl(s, "ppm", cm.ppm);
            sf_dbl(s, "ppm_sigma", cm.ppm_sigma);
            sf_dbl(s, "rate_hz", cm.rate_hz);
            sf_dbl(s, "span_s", cm.span_s);
            sf_dbl(s, "jitter_ns", cm.jitter_ns);
            sf_dbl(s, "stamps", (double)cm.stamps);
            plhs[0] = s;
        }
    }
}

/* ------------------------------------------------------------------ push sources */

GEN_E_RET_HANDLE(source_create_push)
GEN_EH_RET_DBL(source_push_space)
GEN_EH_VOID(source_push_end)

CMD(source_push)
{
    const char* cmd = "source_push";
    size_t n; int owned; const float* p;
    uint32_t took;
    (void)nlhs; need(nrhs, 3, cmd);
    {
        E;
        bwa_source s = get_u32(prhs[1], cmd);
        p = get_f32_array(prhs[2], &n, &owned, cmd);
        if (n == 0) { plhs[0] = ret_dbl(0.0); return; }
        if (mxGetNumberOfDimensions(prhs[2]) > 2 ||
            (mxGetM(prhs[2]) != 1 && mxGetN(prhs[2]) != 1))
            mexErrMsgIdAndTxt("bwa:usage",
                "bwa_mex('source_push'): the samples must be a MONO vector (a column or a row).");
        took = bwa_source_push(e, s, p, (uint32_t)n);
        if (owned) mxFree((void*)p);
        plhs[0] = ret_dbl((double)took);
    }
}

/* ------------------------------------------------------------------ global mix */

GEN_E_F(set_master_gain)
GEN_E_B(set_paused)

/* ------------------------------------------------------------------ ambisonic beds */

GEN_E_RET_HANDLE(bed_create)
GEN_EH_SB(bed_play)
GEN_EH_SBU64(bed_play_at)
GEN_EH_SU64U64(bed_play_loop)
GEN_EH_F(bed_set_gain)
GEN_EH_FFF(bed_set_orientation)
GEN_EH_VOID(bed_stop)
GEN_EH_VOID(bed_destroy)
GEN_EH_U64(bed_stop_at)
GEN_EH_FF(bed_fade_to)
GEN_EH_F(bed_fade_out)
GEN_EH_B(bed_set_paused)
GEN_EH_U64(bed_seek)
GEN_EH_U64U64(bed_set_region)
GEN_EH_I32(bed_set_priority)
GEN_EH_U32(bed_set_group, uint32_t)
GEN_EH_RET_BOOL(bed_is_playing)
GEN_EH_RET_U64(bed_get_playhead_frames)

/* ------------------------------------------------------------------ materials and geometry */

CMD(material_preset)
{
    const char* cmd = "material_preset";
    (void)nlhs; need(nrhs, 2, cmd);
    { E; plhs[0] = ret_u64((uint64_t)bwa_material_preset(e, (bwa_material_type)(int)get_u32(prhs[1], cmd))); }
}

CMD(material_define)
{
    const char* cmd = "material_define";
    float absorption[3], transmission[3];
    (void)nlhs; need(nrhs, 4, cmd);
    {
        E;
        get_f32_n(prhs[1], absorption, 3, cmd);
        get_f32_n(prhs[3], transmission, 3, cmd);
        plhs[0] = ret_u64((uint64_t)bwa_material_define(e, absorption, get_f32(prhs[2], cmd), transmission));
    }
}

GEN_EH_VOID(material_release)

CMD(scene_set_mesh_mat)
{
    const char* cmd = "scene_set_mesh_mat";
    uint32_t nv = 0, nt = 0;
    size_t nm = 0;
    int om = 0;
    float* vp; int32_t* tp; const uint32_t* mp;
    NOOUT
    need_range(nrhs, 3, 4, cmd);
    {
        E;
        vp = get_xyz_rows(prhs[1], &nv, cmd);
        tp = get_tri_rows(prhs[2], &nt, cmd);
        mp = (nrhs == 4) ? get_u32_array(prhs[3], &nm, &om, cmd) : NULL;
        if (!vp || !tp) {                      /* the documented CLEAR form */
            bwa_scene_set_mesh_mat(e, NULL, 0, NULL, 0, NULL);
        } else {
            if (mp && nm != nt) {
                mxFree(vp); mxFree(tp);
                if (om) mxFree((void*)mp);
                mexErrMsgIdAndTxt("bwa:usage",
                    "bwa_mex('scene_set_mesh_mat'): tri_material needs one token per triangle.");
            }
            bwa_scene_set_mesh_mat(e, vp, (int)nv, (const int*)tp, (int)nt, mp);
        }
        if (vp) mxFree(vp);
        if (tp) mxFree(tp);
        if (om) mxFree((void*)mp);
    }
}

CMD(scene_set_box)
{
    const char* cmd = "scene_set_box";
    bwa_material faces[6];
    NOOUT need(nrhs, 5, cmd);
    {
        E;
        get_faces(prhs[4], faces, cmd);
        bwa_scene_set_box(e, get_f32(prhs[1], cmd), get_f32(prhs[2], cmd), get_f32(prhs[3], cmd), faces);
    }
}

CMD(scene_set_ism_room)
{
    const char* cmd = "scene_set_ism_room";
    bwa_material faces[6];
    NOOUT need(nrhs, 5, cmd);
    {
        E;
        get_faces(prhs[4], faces, cmd);
        bwa_scene_set_ism_room(e, get_f32(prhs[1], cmd), get_f32(prhs[2], cmd), get_f32(prhs[3], cmd), faces);
    }
}

CMD(box_mesh)
{
    const char* cmd = "box_mesh";
    bwa_material faces[6];
    float verts[24];
    int tris[36];
    bwa_material mats[12];
    (void)nlhs; need(nrhs, 4, cmd);
    get_faces(prhs[3], faces, cmd);
    if (!bwa_box_mesh(get_f32(prhs[0], cmd), get_f32(prhs[1], cmd), get_f32(prhs[2], cmd),
                      faces, verts, tris, mats)) {
        plhs[0] = ret_empty();
        if (nlhs > 1) plhs[1] = ret_empty();
        if (nlhs > 2) plhs[2] = ret_empty();
        return;
    }
    /* The C side writes xyz triples and index triples contiguously, which is column-major (3, n)
     * here. Transposing to (n, 3) is what a caller expects, so do it rather than hand back the
     * transpose and a caveat. */
    {
        mxArray* v = mxCreateNumericMatrix(8, 3, mxSINGLE_CLASS, mxREAL);
        float* vd = (float*)mxGetData(v);
        int i, j;
        for (i = 0; i < 8; ++i) for (j = 0; j < 3; ++j) vd[j * 8 + i] = verts[i * 3 + j];
        plhs[0] = v;
        if (nlhs > 1) {
            mxArray* t = mxCreateNumericMatrix(12, 3, mxINT32_CLASS, mxREAL);
            int32_t* td = (int32_t*)mxGetData(t);
            for (i = 0; i < 12; ++i) for (j = 0; j < 3; ++j) td[j * 12 + i] = (int32_t)tris[i * 3 + j];
            plhs[1] = t;
        }
        if (nlhs > 2) {
            mxArray* m = mxCreateNumericMatrix(12, 1, mxUINT32_CLASS, mxREAL);
            memcpy(mxGetData(m), mats, sizeof mats);
            plhs[2] = m;
        }
    }
}

CMD(scene_set_ground)
{
    const char* cmd = "scene_set_ground";
    NOOUT need(nrhs, 4, cmd);
    { E; bwa_scene_set_ground(e, get_f32(prhs[1], cmd), (bwa_material)get_u32(prhs[2], cmd),
                              get_bool(prhs[3], cmd)); }
}

GEN_E_U32(scene_set_pressure_release, uint32_t)

CMD(scene_add_dynamic_mesh)
{
    const char* cmd = "scene_add_dynamic_mesh";
    uint32_t nv = 0, nt = 0;
    float* vp; int32_t* tp;
    (void)nlhs; need(nrhs, 4, cmd);
    {
        E;
        vp = get_xyz_rows(prhs[1], &nv, cmd);
        tp = get_tri_rows(prhs[2], &nt, cmd);
        if (!vp || !tp) {
            if (vp) mxFree(vp);
            if (tp) mxFree(tp);
            mexErrMsgIdAndTxt("bwa:usage",
                "bwa_mex('scene_add_dynamic_mesh'): verts (n,3) and tris (m,3) must be non-empty.");
        }
        {
            int h = bwa_scene_add_dynamic_mesh(e, vp, (int)nv, (const int*)tp, (int)nt,
                                               (bwa_material)get_u32(prhs[3], cmd));
            mxFree(vp);
            mxFree(tp);
            plhs[0] = ret_dbl((double)h);
        }
    }
}

CMD(scene_set_dynamic_transform)
{
    const char* cmd = "scene_set_dynamic_transform";
    NOOUT need(nrhs, 9, cmd);
    {
        E;
        bwa_scene_set_dynamic_transform(e, get_i32(prhs[1], cmd),
            get_f32(prhs[2], cmd), get_f32(prhs[3], cmd), get_f32(prhs[4], cmd),
            get_f32(prhs[5], cmd), get_f32(prhs[6], cmd), get_f32(prhs[7], cmd), get_f32(prhs[8], cmd));
    }
}

CMD(scene_remove_dynamic_mesh)
{
    const char* cmd = "scene_remove_dynamic_mesh";
    NOOUT need(nrhs, 2, cmd);
    { E; bwa_scene_remove_dynamic_mesh(e, get_i32(prhs[1], cmd)); }
}

/* ------------------------------------------------------------------ occlusion, reverb, propagation */

GEN_EH_B(source_set_occlusion)

CMD(source_set_occlusion_manual)
{
    const char* cmd = "source_set_occlusion_manual";
    float bands[3];
    int have_bands = 0;
    NOOUT need_range(nrhs, 3, 4, cmd);
    {
        E;
        if (nrhs == 4 && !mxIsEmpty(prhs[3])) { get_f32_n(prhs[3], bands, 3, cmd); have_bands = 1; }
        bwa_source_set_occlusion_manual(e, get_u32(prhs[1], cmd), get_f32(prhs[2], cmd),
                                        have_bands ? bands : NULL);
    }
}

CMD(reflections_config)
{
    const char* cmd = "reflections_config";
    bwa_reflections_desc c;
    NOOUT need(nrhs, 2, cmd);
    {
        E;
        memset(&c, 0, sizeof c);
        c.ir_seconds  = (float)gf(prhs[1], "ir_seconds", 0, cmd);
        c.order       = (uint32_t)gf(prhs[1], "order", 0, cmd);
        c.num_rays    = (uint32_t)gf(prhs[1], "num_rays", 0, cmd);
        c.num_bounces = (uint32_t)gf(prhs[1], "num_bounces", 0, cmd);
        c.enabled     = (int)gf(prhs[1], "enabled", 0, cmd);
        c.bake        = (int)gf(prhs[1], "bake", 0, cmd);
        bwa_reflections_config(e, &c);
    }
}

GEN_E_F(set_reverb_gain)

CMD(fdn_config)
{
    const char* cmd = "fdn_config";
    bwa_fdn_desc c;
    NOOUT need(nrhs, 2, cmd);
    {
        E;
        const mxArray* dir;
        memset(&c, 0, sizeof c);
        c.enabled      = (int)gf(prhs[1], "enabled", 0, cmd);
        c.rt60_low_s   = (float)gf(prhs[1], "rt60_low_s", 0, cmd);
        c.rt60_high_s  = (float)gf(prhs[1], "rt60_high_s", 0, cmd);
        c.xover_hz     = (float)gf(prhs[1], "xover_hz", 0, cmd);
        c.decay_factor = (float)gf(prhs[1], "decay_factor", 0, cmd);
        dir = mxGetField(prhs[1], 0, "decay_dir");
        if (dir && !mxIsEmpty(dir)) get_f32_n(dir, c.decay_dir, 3, cmd);
        bwa_fdn_config(e, &c);
    }
}

GEN_E_FFF(fdn_set_decay)
GEN_EH_B(source_set_early_reflections)
GEN_E_F(set_early_reflections_gain)
GEN_EH_B(source_set_reverb)
GEN_EH_F(source_set_reverb_send)
GEN_EH_B(source_set_reverb_distance)
GEN_EH_B(source_set_pathing)
GEN_EH_RET_DBL(source_get_occlusion)
GEN_EH_FFFF(source_set_orientation)
GEN_EH_FF(source_set_directivity)
GEN_EH_U32(source_set_directivity_preset, bwa_directivity)
GEN_EH_RET_DBL(source_get_directivity)
GEN_EH_B(source_set_doppler)
GEN_EH_B(source_set_air_absorption)
GEN_EH_B(source_set_loudness_comp)
GEN_EH_B(source_set_proximity)
GEN_E_F(set_speed_of_sound)
GEN_EH_FFF(source_set_attenuation_override)
GEN_EH_F(source_set_spread)
GEN_EH_FF(source_set_extent)
GEN_EH_F(source_set_size)

/* ------------------------------------------------------------------ the source_desc tier */

static const char* SRC_FIELDS[] = {
    "struct_size", "gain", "pitch", "priority", "group", "spread", "extent_height", "size_m",
    "reverb_send", "atten_ref_dist", "atten_rolloff", "atten_min_gain", "directivity_weight",
    "directivity_power", "doppler", "air_absorption", "loudness_comp", "proximity", "occlusion",
    "early_reflections", "reverb", "reverb_distance", "pathing"
};
#define SRC_NFIELDS 23

static mxArray* src_desc_to_mx(const bwa_source_desc* d)
{
    mxArray* s = mxCreateStructMatrix(1, 1, SRC_NFIELDS, SRC_FIELDS);
    sf_dbl(s, "struct_size", (double)d->struct_size);
    sf_dbl(s, "gain", d->gain);
    sf_dbl(s, "pitch", d->pitch);
    sf_dbl(s, "priority", (double)d->priority);
    sf_dbl(s, "group", (double)d->group);
    sf_dbl(s, "spread", d->spread);
    sf_dbl(s, "extent_height", d->extent_height);
    sf_dbl(s, "size_m", d->size_m);
    sf_dbl(s, "reverb_send", d->reverb_send);
    sf_dbl(s, "atten_ref_dist", d->atten_ref_dist);
    sf_dbl(s, "atten_rolloff", d->atten_rolloff);
    sf_dbl(s, "atten_min_gain", d->atten_min_gain);
    sf_dbl(s, "directivity_weight", d->directivity_weight);
    sf_dbl(s, "directivity_power", d->directivity_power);
    sf_bool(s, "doppler", d->doppler);
    sf_bool(s, "air_absorption", d->air_absorption);
    sf_bool(s, "loudness_comp", d->loudness_comp);
    sf_bool(s, "proximity", d->proximity);
    sf_bool(s, "occlusion", d->occlusion);
    sf_bool(s, "early_reflections", d->early_reflections);
    sf_bool(s, "reverb", d->reverb);
    sf_bool(s, "reverb_distance", d->reverb_distance);
    sf_bool(s, "pathing", d->pathing);
    return s;
}

/* struct_size defaults to sizeof(bwa_source_desc) rather than to the struct's own field, because a
 * caller who built the struct by hand has no way to know it and the ABI refuses a wrong one. A
 * struct that came from source_preset carries the right value already. */
static void mx_to_src_desc(const mxArray* s, bwa_source_desc* d, const char* cmd)
{
    memset(d, 0, sizeof *d);
    d->struct_size        = (uint32_t)gf(s, "struct_size", (double)sizeof(bwa_source_desc), cmd);
    d->gain               = (float)gf(s, "gain", 1.0, cmd);
    d->pitch              = (float)gf(s, "pitch", 1.0, cmd);
    d->priority           = (int32_t)gf(s, "priority", 128, cmd);
    d->group              = (uint32_t)gf(s, "group", 0, cmd);
    d->spread             = (float)gf(s, "spread", 0, cmd);
    d->extent_height      = (float)gf(s, "extent_height", -1.0, cmd);
    d->size_m             = (float)gf(s, "size_m", 0, cmd);
    d->reverb_send        = (float)gf(s, "reverb_send", 1.0, cmd);
    d->atten_ref_dist     = (float)gf(s, "atten_ref_dist", 0, cmd);
    d->atten_rolloff      = (float)gf(s, "atten_rolloff", 0, cmd);
    d->atten_min_gain     = (float)gf(s, "atten_min_gain", 0, cmd);
    d->directivity_weight = (float)gf(s, "directivity_weight", 0, cmd);
    d->directivity_power  = (float)gf(s, "directivity_power", 1.0, cmd);
    d->doppler            = gf(s, "doppler", 0, cmd) != 0.0;
    d->air_absorption     = gf(s, "air_absorption", 0, cmd) != 0.0;
    d->loudness_comp      = gf(s, "loudness_comp", 0, cmd) != 0.0;
    d->proximity          = gf(s, "proximity", 0, cmd) != 0.0;
    d->occlusion          = gf(s, "occlusion", 0, cmd) != 0.0;
    d->early_reflections  = gf(s, "early_reflections", 0, cmd) != 0.0;
    d->reverb             = gf(s, "reverb", 0, cmd) != 0.0;
    d->reverb_distance    = gf(s, "reverb_distance", 0, cmd) != 0.0;
    d->pathing            = gf(s, "pathing", 0, cmd) != 0.0;
}

CMD(source_preset)
{
    const char* cmd = "source_preset";
    bwa_source_desc d;
    (void)nlhs; need(nrhs, 1, cmd);
    memset(&d, 0, sizeof d);
    bwa_source_preset((bwa_source_kind)(int)get_u32(prhs[0], cmd), &d);
    plhs[0] = src_desc_to_mx(&d);
}

CMD(source_create_desc)
{
    const char* cmd = "source_create_desc";
    bwa_source_desc d;
    (void)nlhs; need(nrhs, 2, cmd);
    { E; mx_to_src_desc(prhs[1], &d, cmd); plhs[0] = ret_u64((uint64_t)bwa_source_create_desc(e, &d)); }
}

CMD(source_apply)
{
    const char* cmd = "source_apply";
    bwa_source_desc d;
    (void)nlhs; need(nrhs, 3, cmd);
    {
        E;
        mx_to_src_desc(prhs[2], &d, cmd);
        plhs[0] = ret_bool(bwa_source_apply(e, get_u32(prhs[1], cmd), &d));
    }
}

CMD(source_get_desc)
{
    const char* cmd = "source_get_desc";
    bwa_source_desc d;
    (void)nlhs; need(nrhs, 2, cmd);
    {
        E;
        memset(&d, 0, sizeof d);
        if (!bwa_source_get_desc(e, get_u32(prhs[1], cmd), &d)) { plhs[0] = ret_empty(); return; }
        plhs[0] = src_desc_to_mx(&d);
    }
}

/* ------------------------------------------------------------------ diagnostics, meters, health */

CMD(set_test_signal)
{
    const char* cmd = "set_test_signal";
    NOOUT need(nrhs, 4, cmd);
    { E; bwa_set_test_signal(e, get_u32(prhs[1], cmd), (bwa_test_kind)(int)get_u32(prhs[2], cmd),
                             get_f32(prhs[3], cmd)); }
}

GEN_EH_I32(source_set_channel)

CMD(get_speakers)
{
    const char* cmd = "get_speakers";
    (void)nlhs; need(nrhs, 1, cmd);
    {
        E;
        uint32_t n = bwa_get_speakers(e, NULL, 0);
        float* xyz;
        mxArray* out;
        float* dst;
        uint32_t i;
        if (n == 0) { plhs[0] = ret_f32(NULL, 0, 3); return; }
        xyz = (float*)mxMalloc((size_t)n * 3 * sizeof(float));
        bwa_get_speakers(e, xyz, n);
        out = mxCreateNumericMatrix((mwSize)n, 3, mxSINGLE_CLASS, mxREAL);
        dst = (float*)mxGetData(out);
        for (i = 0; i < n; ++i) {            /* xyz triples -> (n,3) column-major */
            dst[i]         = xyz[i * 3 + 0];
            dst[n + i]     = xyz[i * 3 + 1];
            dst[2 * n + i] = xyz[i * 3 + 2];
        }
        mxFree(xyz);
        plhs[0] = out;
    }
}

CMD(get_bus_levels)
{
    const char* cmd = "get_bus_levels";
    (void)nlhs; need(nrhs, 1, cmd);
    {
        E;
        uint32_t n = bwa_get_channel_count(e), got;
        float* peaks;
        if (n == 0) { plhs[0] = ret_f32(NULL, 0, 1); return; }
        peaks = (float*)mxMalloc(n * sizeof(float));
        got = bwa_get_bus_levels(e, peaks, n);
        plhs[0] = ret_f32(peaks, got, got ? 1 : 0);
        mxFree(peaks);
    }
}

GEN_E_RET_DBL(get_active_voices)
GEN_E_RET_U64(get_xruns)

CMD(get_health)
{
    const char* cmd = "get_health";
    static const char* f[] = { "blocks", "xruns", "dropped_frames", "driver_resyncs", "late_blocks",
                               "stream_starves", "peak_load", "device_lost" };
    bwa_health hh;
    (void)nlhs; need(nrhs, 1, cmd);
    {
        E;
        memset(&hh, 0, sizeof hh);
        if (!bwa_get_health(e, &hh)) { plhs[0] = ret_empty(); return; }
        {
            mxArray* s = mxCreateStructMatrix(1, 1, 8, f);
            sf_u64(s, "blocks", hh.blocks);
            sf_u64(s, "xruns", hh.xruns);
            sf_u64(s, "dropped_frames", hh.dropped_frames);
            sf_u64(s, "driver_resyncs", hh.driver_resyncs);
            sf_u64(s, "late_blocks", hh.late_blocks);
            sf_u64(s, "stream_starves", hh.stream_starves);
            sf_dbl(s, "peak_load", hh.peak_load);
            sf_dbl(s, "device_lost", (double)hh.device_lost);
            plhs[0] = s;
        }
    }
}

CMD(set_output_capture)
{
    (void)nlhs; (void)plhs; (void)nrhs; (void)prhs;
    mexErrMsgIdAndTxt("bwa:notBound",
        "bwa_set_output_capture is deliberately not exposed to MATLAB or Octave. Its callback runs "
        "on the AUDIO thread, where an interpreter must never run: entering the interpreter there "
        "allocates, takes locks and runs arbitrary code inside the device callback, which is the "
        "engine's first invariant. Use the manual sink and bwa_mex('render_block', h) instead - it "
        "hands back the same post-limiter samples on your own thread. See docs/backends.md.");
}

CMD(render_block)
{
    const char* cmd = "render_block";
    uint32_t ch = 0, n = 0;
    const float* out;
    (void)nlhs; need(nrhs, 1, cmd);
    {
        E;
        out = bwa_render_block(e, &ch, &n);
        if (!out) { plhs[0] = ret_empty(); return; }
        /* The engine's block is PLANAR channel-major with a channel stride of n, which is exactly
         * MATLAB's column-major layout for an [n, ch] matrix. So this is one memcpy and no
         * transpose. It IS a copy, unlike the Python binding's view: an mxArray cannot alias
         * memory the engine owns and overwrites on the next call. */
        plhs[0] = ret_f32(out, n, ch);
    }
}

/* ------------------------------------------------------------------ panner selection and tuning */

GEN_E_U32(set_panner, bwa_panner)
GEN_E_FF(set_spcap_focus)
GEN_E_RET_DBL(get_channel_count)
GEN_E_B(set_dual_band)
GEN_E_B(set_dual_band_cap)
GEN_E_B(set_max_re)
GEN_E_B(set_max_re_split)
GEN_E_U32(set_spread_mode, bwa_spread_mode)
GEN_E_B(set_decorrelation)
GEN_E_F(set_near_spread)
GEN_E_F(set_hole_spread)
GEN_E_B(set_limiter)
GEN_E_F(set_limiter_ceiling)
GEN_E_B(set_headphone_eq)
GEN_E_U32(set_bed_renderer, bwa_bed_renderer)
GEN_E_B(set_tracked_room_eq)
GEN_E_B(set_tracked_align)
GEN_E_FF(set_tracked_align_guards)

CMD(spcap_focus_default)
{
    const char* cmd = "spcap_focus_default";
    uint32_t n = 0;
    float* p;
    (void)nlhs; need(nrhs, 1, cmd);
    p = get_xyz_rows(prhs[0], &n, cmd);
    if (!p)
        mexErrMsgIdAndTxt("bwa:usage",
            "bwa_mex('spcap_focus_default'): positions must be a non-empty (n,3) array.");
    {
        float f = bwa_spcap_focus_default(p, n);
        mxFree(p);
        plhs[0] = ret_dbl((double)f);
    }
}

CMD(load_headphone_eq)
{
    const char* cmd = "load_headphone_eq";
    (void)nlhs; need(nrhs, 2, cmd);
    {
        E;
        char* p = get_str(prhs[1], cmd);
        bwa_result r = bwa_load_headphone_eq(e, p);
        mxFree(p);
        plhs[0] = ret_dbl((double)r);
    }
}

static const char* TUNING_FIELDS[] = {
    "struct_size", "panner", "spcap_focus", "spcap_density", "dual_band", "dual_band_cap",
    "spread_mode", "decorrelation", "near_spread", "hole_spread", "max_re", "max_re_split",
    "bed_renderer", "tracked_room_eq", "tracked_align", "align_dead_zone_m",
    "align_slew_frames_per_s"
};
#define TUNING_NFIELDS 17

static mxArray* tuning_to_mx(const bwa_tuning* t)
{
    mxArray* s = mxCreateStructMatrix(1, 1, TUNING_NFIELDS, TUNING_FIELDS);
    sf_dbl(s, "struct_size", (double)t->struct_size);
    sf_dbl(s, "panner", (double)t->panner);
    sf_dbl(s, "spcap_focus", t->spcap_focus);
    sf_dbl(s, "spcap_density", t->spcap_density);
    sf_bool(s, "dual_band", t->dual_band);
    sf_bool(s, "dual_band_cap", t->dual_band_cap);
    sf_dbl(s, "spread_mode", (double)t->spread_mode);
    sf_bool(s, "decorrelation", t->decorrelation);
    sf_dbl(s, "near_spread", t->near_spread);
    sf_dbl(s, "hole_spread", t->hole_spread);
    sf_bool(s, "max_re", t->max_re);
    sf_bool(s, "max_re_split", t->max_re_split);
    sf_dbl(s, "bed_renderer", (double)t->bed_renderer);
    sf_bool(s, "tracked_room_eq", t->tracked_room_eq);
    sf_bool(s, "tracked_align", t->tracked_align);
    sf_dbl(s, "align_dead_zone_m", t->align_dead_zone_m);
    sf_dbl(s, "align_slew_frames_per_s", t->align_slew_frames_per_s);
    return s;
}

CMD(tuning_preset)
{
    const char* cmd = "tuning_preset";
    bwa_tuning t;
    (void)nlhs; need(nrhs, 1, cmd);
    memset(&t, 0, sizeof t);
    bwa_tuning_preset((bwa_setup)(int)get_u32(prhs[0], cmd), &t);
    plhs[0] = tuning_to_mx(&t);
}

CMD(apply_tuning)
{
    const char* cmd = "apply_tuning";
    bwa_tuning t;
    (void)nlhs; need(nrhs, 2, cmd);
    {
        E;
        memset(&t, 0, sizeof t);
        t.struct_size             = (uint32_t)gf(prhs[1], "struct_size", (double)sizeof(bwa_tuning), cmd);
        t.panner                  = (bwa_panner)(int)gf(prhs[1], "panner", 0, cmd);
        t.spcap_focus             = (float)gf(prhs[1], "spcap_focus", 0, cmd);
        t.spcap_density           = (float)gf(prhs[1], "spcap_density", 0, cmd);
        t.dual_band               = gf(prhs[1], "dual_band", 0, cmd) != 0.0;
        t.dual_band_cap           = gf(prhs[1], "dual_band_cap", 0, cmd) != 0.0;
        t.spread_mode             = (bwa_spread_mode)(int)gf(prhs[1], "spread_mode", 0, cmd);
        t.decorrelation           = gf(prhs[1], "decorrelation", 0, cmd) != 0.0;
        t.near_spread             = (float)gf(prhs[1], "near_spread", 0, cmd);
        t.hole_spread             = (float)gf(prhs[1], "hole_spread", 0, cmd);
        t.max_re                  = gf(prhs[1], "max_re", 0, cmd) != 0.0;
        t.max_re_split            = gf(prhs[1], "max_re_split", 0, cmd) != 0.0;
        t.bed_renderer            = (bwa_bed_renderer)(int)gf(prhs[1], "bed_renderer", 0, cmd);
        t.tracked_room_eq         = gf(prhs[1], "tracked_room_eq", 0, cmd) != 0.0;
        t.tracked_align           = gf(prhs[1], "tracked_align", 0, cmd) != 0.0;
        t.align_dead_zone_m       = (float)gf(prhs[1], "align_dead_zone_m", 0, cmd);
        t.align_slew_frames_per_s = (float)gf(prhs[1], "align_slew_frames_per_s", 0, cmd);
        plhs[0] = ret_bool(bwa_apply_tuning(e, &t));
    }
}

CMD(get_tuning)
{
    const char* cmd = "get_tuning";
    bwa_tuning t;
    (void)nlhs; need(nrhs, 1, cmd);
    {
        E;
        memset(&t, 0, sizeof t);
        if (!bwa_get_tuning(e, &t)) { plhs[0] = ret_empty(); return; }
        plhs[0] = tuning_to_mx(&t);
    }
}

/* ------------------------------------------------------------------ offline panner evaluation */

CMD(panner_gains_batch)
{
    const char* cmd = "panner_gains_batch";
    uint32_t n = 0, nsrc = 0;
    float *pp, *sp;
    float lis[3];
    float focus = 0.0f, density = 0.0f;
    (void)nlhs; need_range(nrhs, 4, 6, cmd);
    pp = get_xyz_rows(prhs[1], &n, cmd);
    sp = get_xyz_rows(prhs[3], &nsrc, cmd);
    if (!pp || !sp) {
        if (pp) mxFree(pp);
        if (sp) mxFree(sp);
        mexErrMsgIdAndTxt("bwa:usage",
            "bwa_mex('panner_gains_batch'): positions and srcs must be non-empty (n,3) arrays.");
    }
    get_f32_n(prhs[2], lis, 3, cmd);
    if (nrhs >= 5) focus = get_f32(prhs[4], cmd);
    if (nrhs >= 6) density = get_f32(prhs[5], cmd);
    {
        /* The C side writes out[i*n + s]: source i's n speaker gains contiguously. Column-major,
         * that IS an (n, nsrc) matrix with one source per COLUMN, so the buffer is the result. */
        mxArray* out = mxCreateNumericMatrix((mwSize)n, (mwSize)nsrc, mxSINGLE_CLASS, mxREAL);
        bwa_panner_gains_batch((bwa_panner)(int)get_u32(prhs[0], cmd), pp, n, lis, sp, nsrc,
                               focus, density, (float*)mxGetData(out));
        mxFree(pp);
        mxFree(sp);
        plhs[0] = out;
    }
}

CMD(bed_gains_batch)
{
    const char* cmd = "bed_gains_batch";
    uint32_t n = 0, ndir = 0;
    float *pp, *dp;
    (void)nlhs; need(nrhs, 4, cmd);
    pp = get_xyz_rows(prhs[2], &n, cmd);
    dp = get_xyz_rows(prhs[3], &ndir, cmd);
    if (!pp || !dp) {
        if (pp) mxFree(pp);
        if (dp) mxFree(dp);
        mexErrMsgIdAndTxt("bwa:usage",
            "bwa_mex('bed_gains_batch'): positions and dirs must be non-empty (n,3) arrays.");
    }
    {
        mxArray* out = mxCreateNumericMatrix((mwSize)n, (mwSize)ndir, mxSINGLE_CLASS, mxREAL);
        bwa_bed_gains_batch((bwa_bed_decoder)(int)get_u32(prhs[0], cmd), get_bool(prhs[1], cmd),
                            pp, n, dp, ndir, (float*)mxGetData(out));
        mxFree(pp);
        mxFree(dp);
        plhs[0] = out;
    }
}

/* ------------------------------------------------------------------ listener and tracking */

CMD(set_listener_pose)
{
    const char* cmd = "set_listener_pose";
    NOOUT need_range(nrhs, 4, 8, cmd);
    {
        E;
        float q[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        if (nrhs >= 5) q[0] = get_f32(prhs[4], cmd);
        if (nrhs >= 6) q[1] = get_f32(prhs[5], cmd);
        if (nrhs >= 7) q[2] = get_f32(prhs[6], cmd);
        if (nrhs >= 8) q[3] = get_f32(prhs[7], cmd);
        bwa_set_listener_pose(e, get_f32(prhs[1], cmd), get_f32(prhs[2], cmd), get_f32(prhs[3], cmd),
                              q[0], q[1], q[2], q[3]);
    }
}

CMD(get_listener_pose)
{
    const char* cmd = "get_listener_pose";
    float p[3] = { 0, 0, 0 }, q[4] = { 0, 0, 0, 1 };
    (void)nlhs; need(nrhs, 1, cmd);
    {
        E;
        bwa_get_listener_pose(e, p, q);
        plhs[0] = ret_f32(p, 1, 3);
        if (nlhs > 1) plhs[1] = ret_f32(q, 1, 4);
    }
}

CMD(tracker_connect)
{
    const char* cmd = "tracker_connect";
    bwa_tracker_desc c;
    char *mc, *sv, *li, *rb;
    (void)nlhs; need(nrhs, 2, cmd);
    {
        E;
        memset(&c, 0, sizeof c);
        mc = gfs(prhs[1], "multicast", cmd);
        sv = gfs(prhs[1], "server", cmd);
        li = gfs(prhs[1], "local_iface", cmd);
        rb = gfs(prhs[1], "rigid_body_name", cmd);
        c.multicast       = mc;
        c.server          = sv;
        c.local_iface     = li;
        c.data_port       = (uint16_t)gf(prhs[1], "data_port", 0, cmd);
        c.command_port    = (uint16_t)gf(prhs[1], "command_port", 0, cmd);
        c.rigid_body_id   = (int32_t)gf(prhs[1], "rigid_body_id", 0, cmd);
        c.rigid_body_name = rb;
        c.version_major   = (int32_t)gf(prhs[1], "version_major", 0, cmd);
        c.version_minor   = (int32_t)gf(prhs[1], "version_minor", 0, cmd);
        {
            bwa_result r = bwa_tracker_connect(e, &c);
            if (mc) mxFree(mc);
            if (sv) mxFree(sv);
            if (li) mxFree(li);
            if (rb) mxFree(rb);
            plhs[0] = ret_dbl((double)r);
        }
    }
}

GEN_E_VOID(tracker_disconnect)
GEN_E_RET_DBL(tracker_status)
GEN_E_F(set_pose_prediction)

CMD(set_extra_listeners)
{
    const char* cmd = "set_extra_listeners";
    uint32_t n = 0;
    float* p;
    NOOUT need(nrhs, 2, cmd);
    {
        E;
        p = get_xyz_rows(prhs[1], &n, cmd);
        if (!p) { bwa_set_extra_listeners(e, NULL, 0); return; }
        bwa_set_extra_listeners(e, p, n);
        mxFree(p);
    }
}

GEN_E_VOID(commit)

/* ------------------------------------------------------------------ constants (no C twin) */

/* The ABI values this file COMPILED against, so the +bwa package reads them instead of repeating
 * them. bwa_mex('get_version') is the library's; VERSION here is the header's, and the two
 * disagreeing means the MEX and the engine library are from different builds. */
CMD(constants)
{
    static const char* f[] = {
        "VERSION", "VERSION_MAJOR", "VERSION_MINOR", "VERSION_PATCH",
        "GROUPS", "EXTRA_LIS", "CHANNEL_AUTO", "MAX_CHANNELS", "DEFAULT_GRID",
        "SINK_FLAG_EXCLUSIVE", "SINK_FLAG_EXACT_RATE", "SINK_FLAG_TIGHT_BUFFER",
        "ROOM_AHEAD", "ROOM_UP", "ROOM_RIGHT",
        "PROFILE_CAVE", "PROFILE_BINAURAL", "PROFILE_CAVE_SIM", "PROFILE_CAVE_BOTH",
        "DECODE_DEFAULT", "DECODE_ALLRAD", "DECODE_EPAD",
        "SINK_AUTO", "SINK_ASIO", "SINK_NULL", "SINK_MANUAL", "SINK_WASAPI", "SINK_COREAUDIO",
        "SINK_ALSA", "SINK_AAUDIO", "SINK_JACK",
        "OK", "ERR_CONFIG", "ERR_DEVICE", "ERR_LAYOUT", "ERR_HRTF", "ERR_STATE", "ERR_INTERNAL",
        "ERR_TRACKER",
        "LOAD_STREAM", "LOAD_AMBIX", "LOAD_FUMA",
        "MAT_GENERIC", "MAT_BRICK", "MAT_CONCRETE", "MAT_CERAMIC", "MAT_GRAVEL", "MAT_CARPET",
        "MAT_GLASS", "MAT_PLASTER", "MAT_WOOD", "MAT_METAL", "MAT_ROCK",
        "DIR_OMNI", "DIR_CARDIOID", "DIR_FIGURE8",
        "SRC_DEFAULT", "SRC_PROP", "SRC_VOICE", "SRC_AMBIENCE", "SRC_UI",
        "TEST_OFF", "TEST_SINE", "TEST_NOISE",
        "PAN_DBAP", "PAN_SPCAP", "PAN_VBAP",
        "SPREAD_LOBE", "SPREAD_MDAP", "SPREAD_SPECTRAL",
        "BED_MATRIX", "BED_PARAMETRIC",
        "SETUP_DEFAULT", "SETUP_SEATED", "SETUP_ROAMING",
        "TRACKER_DISCONNECTED", "TRACKER_NO_DATA", "TRACKER_NO_BODY", "TRACKER_LIVE"
    };
    const int nf = (int)(sizeof f / sizeof f[0]);
    mxArray* s;
    (void)nlhs; (void)prhs;
    need(nrhs, 0, "constants");
    s = mxCreateStructMatrix(1, 1, nf, f);
    sf_dbl(s, "VERSION", (double)BWA_VERSION);
    sf_dbl(s, "VERSION_MAJOR", BWA_VERSION_MAJOR);
    sf_dbl(s, "VERSION_MINOR", BWA_VERSION_MINOR);
    sf_dbl(s, "VERSION_PATCH", BWA_VERSION_PATCH);
    sf_dbl(s, "GROUPS", BWA_GROUPS);
    sf_dbl(s, "EXTRA_LIS", BWA_EXTRA_LIS);
    sf_dbl(s, "CHANNEL_AUTO", BWA_CHANNEL_AUTO);
    sf_dbl(s, "MAX_CHANNELS", BWA_MAX_CHANNELS);
    sf_dbl(s, "DEFAULT_GRID", BWA_DEFAULT_GRID);
    sf_dbl(s, "SINK_FLAG_EXCLUSIVE", BWA_SINK_FLAG_EXCLUSIVE);
    sf_dbl(s, "SINK_FLAG_EXACT_RATE", BWA_SINK_FLAG_EXACT_RATE);
    sf_dbl(s, "SINK_FLAG_TIGHT_BUFFER", BWA_SINK_FLAG_TIGHT_BUFFER);
    mxSetField(s, 0, "ROOM_AHEAD", ret_f32(BWA_ROOM_AHEAD, 1, 3));
    mxSetField(s, 0, "ROOM_UP",    ret_f32(BWA_ROOM_UP, 1, 3));
    mxSetField(s, 0, "ROOM_RIGHT", ret_f32(BWA_ROOM_RIGHT, 1, 3));
    sf_dbl(s, "PROFILE_CAVE", BWA_PROFILE_CAVE);
    sf_dbl(s, "PROFILE_BINAURAL", BWA_PROFILE_BINAURAL);
    sf_dbl(s, "PROFILE_CAVE_SIM", BWA_PROFILE_CAVE_SIM);
    sf_dbl(s, "PROFILE_CAVE_BOTH", BWA_PROFILE_CAVE_BOTH);
    sf_dbl(s, "DECODE_DEFAULT", BWA_DECODE_DEFAULT);
    sf_dbl(s, "DECODE_ALLRAD", BWA_DECODE_ALLRAD);
    sf_dbl(s, "DECODE_EPAD", BWA_DECODE_EPAD);
    sf_dbl(s, "SINK_AUTO", BWA_SINK_AUTO);
    sf_dbl(s, "SINK_ASIO", BWA_SINK_ASIO);
    sf_dbl(s, "SINK_NULL", BWA_SINK_NULL);
    sf_dbl(s, "SINK_MANUAL", BWA_SINK_MANUAL);
    sf_dbl(s, "SINK_WASAPI", BWA_SINK_WASAPI);
    sf_dbl(s, "SINK_COREAUDIO", BWA_SINK_COREAUDIO);
    sf_dbl(s, "SINK_ALSA", BWA_SINK_ALSA);
    sf_dbl(s, "SINK_AAUDIO", BWA_SINK_AAUDIO);
    sf_dbl(s, "SINK_JACK", BWA_SINK_JACK);
    sf_dbl(s, "OK", BWA_OK);
    sf_dbl(s, "ERR_CONFIG", BWA_ERR_CONFIG);
    sf_dbl(s, "ERR_DEVICE", BWA_ERR_DEVICE);
    sf_dbl(s, "ERR_LAYOUT", BWA_ERR_LAYOUT);
    sf_dbl(s, "ERR_HRTF", BWA_ERR_HRTF);
    sf_dbl(s, "ERR_STATE", BWA_ERR_STATE);
    sf_dbl(s, "ERR_INTERNAL", BWA_ERR_INTERNAL);
    sf_dbl(s, "ERR_TRACKER", BWA_ERR_TRACKER);
    sf_dbl(s, "LOAD_STREAM", BWA_LOAD_STREAM);
    sf_dbl(s, "LOAD_AMBIX", BWA_LOAD_AMBIX);
    sf_dbl(s, "LOAD_FUMA", BWA_LOAD_FUMA);
    sf_dbl(s, "MAT_GENERIC", BWA_MAT_GENERIC);
    sf_dbl(s, "MAT_BRICK", BWA_MAT_BRICK);
    sf_dbl(s, "MAT_CONCRETE", BWA_MAT_CONCRETE);
    sf_dbl(s, "MAT_CERAMIC", BWA_MAT_CERAMIC);
    sf_dbl(s, "MAT_GRAVEL", BWA_MAT_GRAVEL);
    sf_dbl(s, "MAT_CARPET", BWA_MAT_CARPET);
    sf_dbl(s, "MAT_GLASS", BWA_MAT_GLASS);
    sf_dbl(s, "MAT_PLASTER", BWA_MAT_PLASTER);
    sf_dbl(s, "MAT_WOOD", BWA_MAT_WOOD);
    sf_dbl(s, "MAT_METAL", BWA_MAT_METAL);
    sf_dbl(s, "MAT_ROCK", BWA_MAT_ROCK);
    sf_dbl(s, "DIR_OMNI", BWA_DIR_OMNI);
    sf_dbl(s, "DIR_CARDIOID", BWA_DIR_CARDIOID);
    sf_dbl(s, "DIR_FIGURE8", BWA_DIR_FIGURE8);
    sf_dbl(s, "SRC_DEFAULT", BWA_SRC_DEFAULT);
    sf_dbl(s, "SRC_PROP", BWA_SRC_PROP);
    sf_dbl(s, "SRC_VOICE", BWA_SRC_VOICE);
    sf_dbl(s, "SRC_AMBIENCE", BWA_SRC_AMBIENCE);
    sf_dbl(s, "SRC_UI", BWA_SRC_UI);
    sf_dbl(s, "TEST_OFF", BWA_TEST_OFF);
    sf_dbl(s, "TEST_SINE", BWA_TEST_SINE);
    sf_dbl(s, "TEST_NOISE", BWA_TEST_NOISE);
    sf_dbl(s, "PAN_DBAP", BWA_PAN_DBAP);
    sf_dbl(s, "PAN_SPCAP", BWA_PAN_SPCAP);
    sf_dbl(s, "PAN_VBAP", BWA_PAN_VBAP);
    sf_dbl(s, "SPREAD_LOBE", BWA_SPREAD_LOBE);
    sf_dbl(s, "SPREAD_MDAP", BWA_SPREAD_MDAP);
    sf_dbl(s, "SPREAD_SPECTRAL", BWA_SPREAD_SPECTRAL);
    sf_dbl(s, "BED_MATRIX", BWA_BED_MATRIX);
    sf_dbl(s, "BED_PARAMETRIC", BWA_BED_PARAMETRIC);
    sf_dbl(s, "SETUP_DEFAULT", BWA_SETUP_DEFAULT);
    sf_dbl(s, "SETUP_SEATED", BWA_SETUP_SEATED);
    sf_dbl(s, "SETUP_ROAMING", BWA_SETUP_ROAMING);
    sf_dbl(s, "TRACKER_DISCONNECTED", BWA_TRACKER_DISCONNECTED);
    sf_dbl(s, "TRACKER_NO_DATA", BWA_TRACKER_NO_DATA);
    sf_dbl(s, "TRACKER_NO_BODY", BWA_TRACKER_NO_BODY);
    sf_dbl(s, "TRACKER_LIVE", BWA_TRACKER_LIVE);
    plhs[0] = s;
}

/* ------------------------------------------------------------------ dispatch */

typedef void (*bwa_handler)(int, mxArray**, int, const mxArray**);

typedef struct { const char* name; bwa_handler fn; } bwa_cmd_entry;

#define ENT(n) { #n, h_##n }

static const bwa_cmd_entry g_cmds[] = {
    /* lifecycle */
    ENT(create), ENT(start), ENT(stop), ENT(destroy), ENT(last_error), ENT(get_version),
    ENT(get_audio_backend), ENT(get_sample_rate), ENT(get_block_size), ENT(get_sink_type),
    /* device query */
    ENT(get_device_count), ENT(get_device_name), ENT(get_device_id),
    ENT(get_asio_driver_count), ENT(get_asio_driver_name),
    /* assets */
    ENT(load_sound), ENT(load_sound_streaming), ENT(load_ambix), ENT(load_fuma),
    ENT(unload_sound), ENT(sound_get_frames), ENT(sound_get_channels),
    ENT(sound_acquire), ENT(sound_release), ENT(sound_acquire_async), ENT(sound_is_ready),
    ENT(sound_find),
    /* sources */
    ENT(source_create), ENT(source_destroy), ENT(source_set_priority), ENT(source_set_pos),
    ENT(source_set_gain), ENT(source_fade_to), ENT(source_fade_out), ENT(source_set_group),
    ENT(group_set_gain), ENT(group_set_paused), ENT(group_stop), ENT(stop_all),
    ENT(source_set_pitch), ENT(source_play), ENT(source_play_at), ENT(source_play_loop),
    ENT(source_stop), ENT(source_stop_at), ENT(source_queue), ENT(source_clear_queue),
    ENT(source_set_paused), ENT(source_seek), ENT(source_set_region), ENT(source_is_playing),
    ENT(poll_ended), ENT(poll_looped), ENT(source_get_playhead_frames), ENT(play_oneshot),
    /* clock */
    ENT(get_dsp_time_frames), ENT(get_clock), ENT(get_clock_model),
    ENT(get_output_latency_frames), ENT(host_time_ns),
    /* push sources */
    ENT(source_create_push), ENT(source_push), ENT(source_push_space), ENT(source_push_end),
    /* global mix */
    ENT(set_master_gain), ENT(set_paused),
    /* beds */
    ENT(bed_create), ENT(bed_play), ENT(bed_play_at), ENT(bed_play_loop), ENT(bed_set_gain),
    ENT(bed_set_orientation), ENT(bed_stop), ENT(bed_destroy), ENT(bed_stop_at),
    ENT(bed_fade_to), ENT(bed_fade_out), ENT(bed_set_paused), ENT(bed_seek),
    ENT(bed_set_region), ENT(bed_set_priority), ENT(bed_set_group), ENT(bed_is_playing),
    ENT(bed_get_playhead_frames),
    /* materials and geometry */
    ENT(material_preset), ENT(material_define), ENT(material_release),
    ENT(scene_set_mesh_mat), ENT(scene_set_box), ENT(scene_set_ism_room), ENT(box_mesh),
    ENT(scene_set_ground), ENT(scene_set_pressure_release), ENT(scene_add_dynamic_mesh),
    ENT(scene_set_dynamic_transform), ENT(scene_remove_dynamic_mesh),
    /* occlusion, reverb, propagation */
    ENT(source_set_occlusion), ENT(source_set_occlusion_manual), ENT(reflections_config),
    ENT(set_reverb_gain), ENT(fdn_config), ENT(fdn_set_decay),
    ENT(source_set_early_reflections), ENT(set_early_reflections_gain), ENT(source_set_reverb),
    ENT(source_set_reverb_send), ENT(source_set_reverb_distance), ENT(source_set_pathing),
    ENT(source_get_occlusion), ENT(source_set_orientation), ENT(source_set_directivity),
    ENT(source_set_directivity_preset), ENT(source_get_directivity), ENT(source_set_doppler),
    ENT(source_set_air_absorption), ENT(source_set_loudness_comp), ENT(source_set_proximity),
    ENT(set_speed_of_sound), ENT(source_set_attenuation_override), ENT(source_set_spread),
    ENT(source_set_extent), ENT(source_set_size),
    /* the source_desc tier */
    ENT(source_preset), ENT(source_create_desc), ENT(source_apply), ENT(source_get_desc),
    /* diagnostics */
    ENT(set_test_signal), ENT(source_set_channel), ENT(get_speakers), ENT(get_bus_levels),
    ENT(get_active_voices), ENT(get_health), ENT(get_xruns), ENT(set_output_capture),
    ENT(render_block),
    /* panner selection and tuning */
    ENT(set_panner), ENT(set_spcap_focus), ENT(spcap_focus_default), ENT(get_channel_count),
    ENT(set_dual_band), ENT(set_dual_band_cap), ENT(set_max_re), ENT(set_max_re_split),
    ENT(set_spread_mode), ENT(set_decorrelation), ENT(set_near_spread), ENT(set_hole_spread),
    ENT(set_limiter), ENT(set_limiter_ceiling), ENT(load_headphone_eq), ENT(set_headphone_eq),
    ENT(set_bed_renderer), ENT(set_tracked_room_eq), ENT(set_tracked_align),
    ENT(set_tracked_align_guards), ENT(tuning_preset), ENT(apply_tuning), ENT(get_tuning),
    ENT(panner_gains_batch), ENT(bed_gains_batch),
    /* listener and tracking */
    ENT(set_listener_pose), ENT(get_listener_pose), ENT(tracker_connect),
    ENT(tracker_disconnect), ENT(tracker_status), ENT(set_pose_prediction),
    ENT(set_extra_listeners), ENT(commit),
    /* the one subcommand with no C twin */
    ENT(constants)
};

#define BWA_NCMDS ((int)(sizeof g_cmds / sizeof g_cmds[0]))

void mexFunction(int nlhs, mxArray* plhs[], int nrhs, const mxArray* prhs[])
{
    char cmd[64];
    int i;

    if (nrhs < 1 || !mxIsChar(prhs[0]))
        mexErrMsgIdAndTxt("bwa:usage",
            "bwa_mex: the first argument is the subcommand, a char row. "
            "For example bwa_mex('create', cfg). bwa_mex('commands') lists them.");

    if (mxGetString(prhs[0], cmd, (mwSize)sizeof cmd) != 0)
        mexErrMsgIdAndTxt("bwa:usage", "bwa_mex: the subcommand name is too long to be one.");

    /* A listing, so a user at a prompt can find a call without leaving the interpreter. Not an ABI
     * call and not counted as one. */
    if (strcmp(cmd, "commands") == 0) {
        mxArray* c = mxCreateCellMatrix(BWA_NCMDS, 1);
        for (i = 0; i < BWA_NCMDS; ++i) mxSetCell(c, i, mxCreateString(g_cmds[i].name));
        plhs[0] = c;
        return;
    }

    for (i = 0; i < BWA_NCMDS; ++i) {
        if (strcmp(cmd, g_cmds[i].name) == 0) {
            g_cmds[i].fn(nlhs, plhs, nrhs - 1, prhs + 1);
            return;
        }
    }

    mexErrMsgIdAndTxt("bwa:unknownCommand",
        "bwa_mex: no subcommand '%s'. Every subcommand is a bw_audio.h entry point without its "
        "bwa_ prefix. bwa_mex('commands') lists them all.", cmd);
}
