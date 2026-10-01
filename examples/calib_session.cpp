/*
 * calib_session.cpp - the rig-day calibration session (calib_session.h): the model, the session file,
 * the command lines, and the subprocess runner.
 */
#include "calib_session.h"

extern "C" {
#include "core/layout.h"
#include "calib/calib.h"           /* calib_read_sos */
}
#include <cJSON.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- the step table ---------------------------------------------------------------------------- */

static const char* const IDS[SES_NSTEPS]    = { "frame", "localize", "capsules", "aim", "trims", "verify", "grid", "validate" };
static const char* const TITLES[SES_NSTEPS] = { "Motive frame check", "Speaker positions (ZM-1)", "Capsule survey",
                                                "Live aiming", "Trims (ZM-1)", "Verify", "Room-EQ grid", "Validate" };
static const char* const ARTS[SES_NSTEPS]   = { "frame_check.csv", "as_built.json", "capsules.json", "",
                                                "trims.json", "", "grid.json", "validate.csv" };

/* What each step needs. NEEDS: the step reads that step's file, so it must have PASSED. AFTER: the
 * runbook's order, satisfied by a pass or by a skip the installer wrote a note for. Bits are step ids. */
#define B(k) (1u << (k))
static const unsigned NEEDS[SES_NSTEPS] = {
    0, 0, B(SES_LOCALIZE), B(SES_LOCALIZE) | B(SES_CAPSULES), B(SES_LOCALIZE), B(SES_TRIMS), B(SES_TRIMS), B(SES_TRIMS) };
static const unsigned AFTER[SES_NSTEPS] = {
    0, B(SES_FRAME), 0, 0, B(SES_CAPSULES) | B(SES_AIM), 0, B(SES_VERIFY), B(SES_VERIFY) | B(SES_GRID) | B(SES_CAPSULES) };

const char* ses_step_id(int k)       { return (k >= 0 && k < SES_NSTEPS) ? IDS[k] : "?"; }
const char* ses_step_title(int k)    { return (k >= 0 && k < SES_NSTEPS) ? TITLES[k] : "?"; }
const char* ses_step_artifact(int k) { return (k >= 0 && k < SES_NSTEPS) ? ARTS[k] : ""; }

const char* ses_status_name(SesStatus s) {
    switch (s) {
    case SES_PENDING: return "pending";
    case SES_RUNNING: return "running";
    case SES_PASSED:  return "passed";
    case SES_FAILED:  return "failed";
    case SES_SKIPPED: return "skipped";
    }
    return "?";
}
static SesStatus status_from(const char* s) {
    for (int v = SES_PENDING; v <= SES_SKIPPED; ++v)
        if (s && !strcmp(s, ses_status_name((SesStatus)v))) return (SesStatus)v;
    return SES_PENDING;
}

/* The tools' exit codes (bwa_calibrate, bwa_validate, bwa_speaker_survey) in one place */
const char* ses_exit_meaning(int k, int code) {
    switch (code) {
    case 0: return "ok";
    case 1: return "error (see the log)";
    case 2: return k == SES_FRAME ? "no Motive data (see the log)" : "refused before measuring (see the log)";
    case 3: return k == SES_FRAME ? "the frame does not agree, or the check failed"
                 : k == SES_VERIFY ? "speakers flagged" : "the tool's own check failed";
    case 4: return "BUMP: the stand moved or turned";
    case 5: return "the track self-check failed";
    case 6: return "a speaker does not fit the rest (leave-one-out: its direction; the range check: its distance): "
                   "check its position and its Dante latency, or leave it out of the capsule speakers";
    }
    return "unexpected exit code";
}

/* ---- small helpers ------------------------------------------------------------------------------ */

static std::wstring wide(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, NULL, 0);
    std::wstring w((size_t)(n > 0 ? n : 1), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    w.resize(wcslen(w.c_str()));
    return w;
}
static std::string narrow(const std::wstring& w) {
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, NULL, 0, NULL, NULL);
    std::string s((size_t)(n > 0 ? n : 1), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, NULL, NULL);
    s.resize(strlen(s.c_str()));
    return s;
}
static FILE* open_u8(const std::string& path, const wchar_t* mode) { return _wfopen(wide(path).c_str(), mode); }

std::string ses_now(void) {
    time_t t = time(NULL);
    struct tm lt;
    localtime_s(&lt, &t);
    char b[32];
    strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%S", &lt);
    return b;
}

/* The tab asks for staleness every frame, and staleness is a hash per file read: cache each hash
 * against the file's size and last write time, so a frame costs one attribute query per file rather
 * than a read. UI thread only. */
struct HashMemo { unsigned long long size, mtime; std::string hash; };
static std::vector<std::pair<std::string, HashMemo>> g_hash_memo;

static std::string hash_file_uncached(const std::string& path);
std::string ses_hash_file(const std::string& path) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(wide(path).c_str(), GetFileExInfoStandard, &a)) return std::string();
    const unsigned long long size = ((unsigned long long)a.nFileSizeHigh << 32) | a.nFileSizeLow;
    const unsigned long long mt = ((unsigned long long)a.ftLastWriteTime.dwHighDateTime << 32) | a.ftLastWriteTime.dwLowDateTime;
    for (auto& e : g_hash_memo)
        if (e.first == path) {
            if (e.second.size != size || e.second.mtime != mt) e.second = { size, mt, hash_file_uncached(path) };
            return e.second.hash;
        }
    g_hash_memo.push_back({ path, { size, mt, hash_file_uncached(path) } });
    return g_hash_memo.back().second.hash;
}

static std::string hash_file_uncached(const std::string& path) {
    FILE* f = open_u8(path, L"rb");
    if (!f) return std::string();
    unsigned long long h = 1469598103934665603ULL;
    unsigned char buf[16384];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0)
        for (size_t i = 0; i < n; ++i) { h ^= buf[i]; h *= 1099511628211ULL; }
    fclose(f);
    char o[24];
    snprintf(o, sizeof o, "%016llx", h);
    return o;
}

std::string ses_abs(const std::string& path) {
    if (path.empty()) return path;
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = GetFullPathNameW(wide(path).c_str(), (DWORD)(sizeof buf / sizeof buf[0]), buf, NULL);
    if (!n || n >= sizeof buf / sizeof buf[0]) return path;
    return narrow(buf);
}

std::string ses_join(const std::string& folder, const std::string& name) {
    if (folder.empty()) return name;
    const char e = folder.back();
    return (e == '\\' || e == '/') ? folder + name : folder + "\\" + name;
}

bool ses_file_exists(const std::string& path) {
    if (path.empty()) return false;
    const DWORD a = GetFileAttributesW(wide(path).c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static bool same_path(const std::string& a, const std::string& b) {
    return !a.empty() && !b.empty() && _stricmp(ses_abs(a).c_str(), ses_abs(b).c_str()) == 0;
}

static std::string quote(const std::string& s) { return "\"" + s + "\""; }

bool ses_layout_ref(const std::string& path, float ref[3], int* count) {
    static Layout L;                     /* never a stack local (layout.h); UI thread only */
    char err[256];
    if (!layout_load(path.c_str(), 48000, &L, err, sizeof err)) return false;
    memcpy(ref, L.ref, 3 * sizeof(float));
    if (count) *count = (int)L.count;
    return true;
}

int ses_parse_speakers(const std::string& str, int n, int* out, int cap) {
    const char* q = str.c_str();
    int k = 0;
    std::vector<unsigned char> seen((size_t)(n > 0 ? n : 1), 0);
    while (*q) {
        char* end = NULL;
        const long a = strtol(q, &end, 10);
        if (end == q) return -1;
        long b = a;
        q = end;
        if (*q == '-') { ++q; b = strtol(q, &end, 10); if (end == q) return -1; q = end; }
        if (a < 0 || b < a || b >= n) return -1;
        for (long v = a; v <= b; ++v) {
            if (seen[(size_t)v] || k >= cap) return -1;
            seen[(size_t)v] = 1;
            out[k++] = (int)v;
        }
        if (*q == ',') ++q;
        else if (*q) return -1;
    }
    return k;
}

/* ---- the session file --------------------------------------------------------------------------- */

static void put_str(cJSON* o, const char* k, const std::string& v) { cJSON_AddStringToObject(o, k, v.c_str()); }
static std::string get_str(const cJSON* o, const char* k) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) && v->valuestring ? v->valuestring : "";
}
static double get_num(const cJSON* o, const char* k, double def) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? v->valuedouble : def;
}
static bool get_bool(const cJSON* o, const char* k, bool def) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsBool(v) ? cJSON_IsTrue(v) != 0 : def;
}

static cJSON* files_json(const std::vector<SesFile>& v) {
    cJSON* a = cJSON_CreateArray();
    for (const SesFile& f : v) {
        cJSON* o = cJSON_CreateObject();
        put_str(o, "what", f.what); put_str(o, "path", f.path); put_str(o, "hash", f.hash);
        cJSON_AddNumberToObject(o, "run", f.run);
        cJSON_AddItemToArray(a, o);
    }
    return a;
}
static void files_read(const cJSON* a, std::vector<SesFile>* v) {
    v->clear();
    const cJSON* o;
    cJSON_ArrayForEach(o, a) {
        SesFile f;
        f.what = get_str(o, "what"); f.path = get_str(o, "path"); f.hash = get_str(o, "hash");
        f.run = (int)get_num(o, "run", 0);
        v->push_back(f);
    }
}

bool ses_save(const Session& S, std::string* err) {
    if (S.folder.empty()) { if (err) *err = "no session folder"; return false; }
    cJSON* root = cJSON_CreateObject();
    put_str(root, "format", "bwa_calib_session");
    cJSON_AddNumberToObject(root, "version", 1);
    put_str(root, "created", S.created);
    put_str(root, "saved", ses_now());
    cJSON_AddBoolToObject(root, "simulate", S.simulate);
    cJSON_AddNumberToObject(root, "next_run", S.next_run);
    cJSON* in = cJSON_AddObjectToObject(root, "inputs");
    const SesInputs& I = S.in;
    put_str(in, "plan", I.plan); put_str(in, "driver", I.driver);
    cJSON_AddNumberToObject(in, "input_first", I.input_first);
    put_str(in, "natnet_server", I.nn_server); put_str(in, "natnet_multicast", I.nn_multicast);
    put_str(in, "stand_body", I.body); put_str(in, "mount_offset", I.mount_offset); put_str(in, "temp", I.temp);
    put_str(in, "localize_rows", I.localize_rows); put_str(in, "grid_rows", I.grid_rows);
    put_str(in, "frame_speakers", I.frame_speakers); put_str(in, "capsule_speakers", I.capsule_speakers);
    cJSON_AddNumberToObject(in, "baffle_offset_m", I.baffle_offset_m);
    put_str(in, "aim_speakers", I.aim_speakers); put_str(in, "validate_positions", I.validate_positions);
    cJSON_AddNumberToObject(in, "validate_azimuths", I.validate_azimuths);
    cJSON_AddBoolToObject(in, "validate_reference", I.validate_reference);
    cJSON_AddNumberToObject(in, "place_timeout_s", I.place_timeout_s);
    put_str(in, "sim_truth", I.sim_truth);
    cJSON* st = cJSON_AddObjectToObject(root, "steps");
    for (int k = 0; k < SES_NSTEPS; ++k) {
        const SesStep& P = S.step[k];
        cJSON* o = cJSON_AddObjectToObject(st, IDS[k]);
        put_str(o, "status", ses_status_name(P.status));
        cJSON_AddNumberToObject(o, "run", P.run);
        cJSON_AddNumberToObject(o, "exit_code", P.exit_code);
        put_str(o, "started", P.started); put_str(o, "finished", P.finished);
        put_str(o, "note", P.note); put_str(o, "command", P.command); put_str(o, "summary", P.summary);
        cJSON_AddItemToObject(o, "consumed", files_json(P.consumed));
        cJSON_AddItemToObject(o, "produced", files_json(P.produced));
        if (P.have_latency) cJSON_AddNumberToObject(o, "latency_m", P.latency_m);
        if (P.have_bg) cJSON_AddNumberToObject(o, "background_dbfs", P.bg_dbfs);
        if (P.have_center) {
            const double c[3] = { P.center[0], P.center[1], P.center[2] };
            cJSON_AddItemToObject(o, "center", cJSON_CreateDoubleArray(c, 3));
        }
        if (k == SES_AIM) {
            cJSON* a = cJSON_AddArrayToObject(o, "aim");
            for (const SesAimRec& r : P.aim) {
                cJSON* e = cJSON_CreateObject();
                cJSON_AddNumberToObject(e, "speaker", r.spk);
                cJSON_AddNumberToObject(e, "below_peak_db", r.below_db);
                put_str(e, "off_axis", r.angle);
                if (r.have_pos) cJSON_AddNumberToObject(e, "position_mm", r.pos_mm);
                put_str(e, "move", r.words); put_str(e, "accepted", r.when);
                cJSON_AddItemToArray(a, e);
            }
        }
    }
    char* txt = cJSON_Print(root);
    cJSON_Delete(root);
    if (!txt) { if (err) *err = "out of memory"; return false; }
    const std::string path = ses_join(S.folder, "session.json"), tmp = path + ".tmp";
    FILE* f = open_u8(tmp, L"wb");
    bool ok = f && fwrite(txt, 1, strlen(txt), f) == strlen(txt);
    if (f) { ok = fputc('\n', f) != EOF && ok; ok = fclose(f) == 0 && ok; }
    cJSON_free(txt);
    if (ok) ok = MoveFileExW(wide(tmp).c_str(), wide(path).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    if (!ok && err) *err = "cannot write " + path;
    return ok;
}

bool ses_load(Session* S, const std::string& folder, std::string* err) {
    const std::string path = ses_join(folder, "session.json");
    FILE* f = open_u8(path, L"rb");
    if (!f) { if (err) *err = "no session.json in " + folder; return false; }
    std::string txt;
    char buf[16384];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) txt.append(buf, n);
    fclose(f);
    cJSON* root = cJSON_Parse(txt.c_str());
    if (!root || get_str(root, "format") != "bwa_calib_session") {
        if (root) cJSON_Delete(root);
        if (err) *err = path + " is not a calibration session file";
        return false;
    }
    Session N;
    N.folder = folder;
    N.created = get_str(root, "created");
    N.simulate = get_bool(root, "simulate", true);
    N.next_run = (int)get_num(root, "next_run", 1);
    const cJSON* in = cJSON_GetObjectItemCaseSensitive(root, "inputs");
    SesInputs& I = N.in;
    I.plan = get_str(in, "plan"); I.driver = get_str(in, "driver");
    I.input_first = (int)get_num(in, "input_first", 0);
    I.nn_server = get_str(in, "natnet_server"); I.nn_multicast = get_str(in, "natnet_multicast");
    I.body = get_str(in, "stand_body"); I.mount_offset = get_str(in, "mount_offset"); I.temp = get_str(in, "temp");
    I.localize_rows = get_str(in, "localize_rows"); I.grid_rows = get_str(in, "grid_rows");
    I.frame_speakers = get_str(in, "frame_speakers"); I.capsule_speakers = get_str(in, "capsule_speakers");
    I.baffle_offset_m = get_num(in, "baffle_offset_m", 0.0);
    if (!(fabs(I.baffle_offset_m) <= SES_BAFFLE_MAX_M)) I.baffle_offset_m = 0.0;   /* NaN or absurd: the default */
    I.aim_speakers = get_str(in, "aim_speakers"); I.validate_positions = get_str(in, "validate_positions");
    I.validate_azimuths = (int)get_num(in, "validate_azimuths", 12);
    I.validate_reference = get_bool(in, "validate_reference", true);
    I.place_timeout_s = (int)get_num(in, "place_timeout_s", 300);
    I.sim_truth = get_str(in, "sim_truth");
    const cJSON* st = cJSON_GetObjectItemCaseSensitive(root, "steps");
    for (int k = 0; k < SES_NSTEPS; ++k) {
        const cJSON* o = cJSON_GetObjectItemCaseSensitive(st, IDS[k]);
        if (!o) continue;
        SesStep& P = N.step[k];
        P.status = status_from(get_str(o, "status").c_str());
        P.run = (int)get_num(o, "run", 0);
        P.exit_code = (int)get_num(o, "exit_code", 0);
        P.started = get_str(o, "started"); P.finished = get_str(o, "finished");
        P.note = get_str(o, "note"); P.command = get_str(o, "command"); P.summary = get_str(o, "summary");
        files_read(cJSON_GetObjectItemCaseSensitive(o, "consumed"), &P.consumed);
        files_read(cJSON_GetObjectItemCaseSensitive(o, "produced"), &P.produced);
        const cJSON* lat = cJSON_GetObjectItemCaseSensitive(o, "latency_m");
        if (cJSON_IsNumber(lat)) { P.have_latency = true; P.latency_m = lat->valuedouble; }
        const cJSON* bgv = cJSON_GetObjectItemCaseSensitive(o, "background_dbfs");
        if (cJSON_IsNumber(bgv)) { P.have_bg = true; P.bg_dbfs = bgv->valuedouble; }
        const cJSON* c = cJSON_GetObjectItemCaseSensitive(o, "center");
        if (cJSON_IsArray(c) && cJSON_GetArraySize(c) == 3) {
            P.have_center = true;
            for (int a = 0; a < 3; ++a) P.center[a] = (float)cJSON_GetArrayItem(c, a)->valuedouble;
        }
        const cJSON* e;
        cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(o, "aim")) {
            SesAimRec r;
            r.spk = (int)get_num(e, "speaker", -1);
            r.below_db = (float)get_num(e, "below_peak_db", 0.0);
            r.angle = get_str(e, "off_axis");
            const cJSON* pm = cJSON_GetObjectItemCaseSensitive(e, "position_mm");
            if (cJSON_IsNumber(pm)) { r.have_pos = true; r.pos_mm = (float)pm->valuedouble; }
            r.words = get_str(e, "move"); r.when = get_str(e, "accepted");
            P.aim.push_back(r);
        }
        /* a subprocess the window was running when it closed did not finish: say so, never resume it */
        if (P.status == SES_RUNNING) {
            if (k == SES_AIM) { P.status = SES_PENDING; P.note = "aiming was in progress when the session closed; start it again"; }
            else {
                P.status = SES_FAILED; P.exit_code = -1;
                P.note = "interrupted: the window closed while it ran";
            }
        }
    }
    cJSON_Delete(root);
    *S = N;
    return true;
}

/* ---- inputs, artifacts, prerequisites ------------------------------------------------------------ */

/* the input a file record names, resolved the way a command would use it now */
static std::string input_path(const Session& S, const std::string& what) {
    const SesInputs& I = S.in;
    if (what == "plan")               return ses_abs(I.plan);
    if (what == "localize_rows")      return ses_abs(I.localize_rows);
    if (what == "grid_rows")          return ses_abs(I.grid_rows);
    if (what == "validate_positions") return ses_abs(I.validate_positions);
    if (what == "sim_truth")          return ses_abs(I.sim_truth);
    return std::string();
}
/* a VALUE input a step read, as the text its record keeps; false = `what` is not a value input */
static bool input_value(const Session& S, const std::string& what, std::string* txt) {
    if (what != "baffle_offset_m") return false;
    char b[32];
    snprintf(b, sizeof b, "%.4f", S.in.baffle_offset_m);
    *txt = b;
    return true;
}
static int step_index(const std::string& id) {
    for (int k = 0; k < SES_NSTEPS; ++k) if (id == IDS[k]) return k;
    return -1;
}
static std::string art_path(const Session& S, int k) { return ses_join(S.folder, ARTS[k]); }

std::string ses_stale(const Session& S, int k) {
    const SesStep& P = S.step[k];
    if (P.status == SES_PENDING || P.status == SES_RUNNING || P.status == SES_SKIPPED) return std::string();
    std::string val;
    for (const SesFile& f : P.consumed) {
        const int j = step_index(f.what);
        if (j >= 0) {
            const SesStep& Q = S.step[j];
            if (Q.run != f.run)
                return std::string(IDS[j]) + " ran again (run " + std::to_string(Q.run) + ") after this step used its " +
                       f.path + " (run " + std::to_string(f.run) + ")";
            if (ses_hash_file(ses_join(S.folder, f.path)) != f.hash) return f.path + " changed on disk since this step used it";
            const std::string up = ses_stale(S, j);
            if (!up.empty()) return std::string(IDS[j]) + " is stale";
        } else if (input_value(S, f.what, &val)) {
            if (val != f.path) return "the " + f.what + " input changed since this step ran (" + f.path + " then, " + val + " now)";
        } else {
            const std::string now = input_path(S, f.what);
            if (_stricmp(now.c_str(), f.path.c_str()) != 0) return "the " + f.what + " input changed since this step ran";
            if (ses_hash_file(now) != f.hash) return f.path + " changed on disk since this step used it";
        }
    }
    return std::string();
}

std::string ses_blocked(const Session& S, int k) {
    if (S.folder.empty()) return "no session is open";
    for (int j = 0; j < SES_NSTEPS; ++j) {
        if (!(NEEDS[k] & B(j))) continue;
        const SesStep& Q = S.step[j];
        const std::string name = IDS[j];
        if (Q.status == SES_FAILED)
            return "needs " + name + "'s " + ARTS[j] + ", and " + name + " failed (exit " + std::to_string(Q.exit_code) +
                   "): fix it and run it again";
        if (Q.status == SES_SKIPPED) return "needs " + name + "'s " + ARTS[j] + ", and " + name + " was skipped";
        if (Q.status != SES_PASSED) return "needs " + name + "'s " + ARTS[j] + ": run " + name + " first";
        if (!ses_stale(S, j).empty()) return name + " is stale (" + ses_stale(S, j) + "): run it again first";
    }
    for (int j = 0; j < SES_NSTEPS; ++j) {
        if (!(AFTER[k] & B(j))) continue;
        const SesStep& Q = S.step[j];
        const std::string name = IDS[j];
        if (Q.status == SES_FAILED)
            return name + " failed (exit " + std::to_string(Q.exit_code) + ", " + ses_exit_meaning(j, Q.exit_code) +
                   "): fix it and run it again, or mark it skipped";
        if (Q.status == SES_PENDING || Q.status == SES_RUNNING) return name + " comes first: run it, or mark it skipped";
        if (Q.status == SES_PASSED && !ses_stale(S, j).empty()) return name + " is stale: run it again first";
    }
    const SesInputs& I = S.in;
    if ((k == SES_FRAME || k == SES_LOCALIZE) && !ses_file_exists(ses_abs(I.plan))) return "the plan layout is not a file: set it";
    for (int j = 0; j < SES_NSTEPS; ++j)       /* the session's own outputs can never be the plan */
        if (ARTS[j][0] && same_path(I.plan, art_path(S, j)))
            return "the plan is this session's own " + std::string(ARTS[j]) + ": pick the plan outside the session's outputs";
    if (k == SES_LOCALIZE && !ses_file_exists(ses_abs(I.localize_rows))) return "the localize rows are not a file: set them";
    if (k == SES_GRID && !ses_file_exists(ses_abs(I.grid_rows))) return "the grid rows are not a file: set them";
    if (k == SES_VALIDATE && !I.validate_positions.empty() && !ses_file_exists(ses_abs(I.validate_positions)))
        return "the validate placements are not a file";
    if (S.simulate && !I.sim_truth.empty() && k != SES_FRAME && k != SES_AIM && k != SES_VALIDATE &&
        !ses_file_exists(ses_abs(I.sim_truth)))
        return "the simulated truth is not a file";
    if (k == SES_FRAME && !S.simulate && I.nn_server.empty())
        return "needs Motive's IP (NatNet server): the speaker bodies' names come from its model definitions";
    if (k == SES_AIM && I.aim_speakers.empty()) return "no speakers to aim: list them, or mark the step skipped";
    return std::string();
}

/* ---- the command lines ---------------------------------------------------------------------------- */

static bool tracked(const Session& S) { return S.simulate || !S.in.body.empty(); }

/* the placement flags every tracked bwa_calibrate / bwa_validate step takes */
static std::string track_flags(const Session& S) {
    const SesInputs& I = S.in;
    std::string c;
    if (S.simulate) c += " --track-sim";
    else if (!I.body.empty()) {
        c += " --track " + quote(I.body);
        if (!I.nn_server.empty()) c += " --natnet-server " + I.nn_server;
        if (!I.nn_multicast.empty()) c += " --natnet-multicast " + I.nn_multicast;
    } else return c;
    c += " --place-timeout " + std::to_string(I.place_timeout_s > 0 ? I.place_timeout_s : 300);
    return c;
}
/* the device and simulator flags of every bwa_calibrate step */
static std::string device_flags(const Session& S, std::vector<SesFile>* consumed) {
    const SesInputs& I = S.in;
    std::string c;
    if (S.simulate) {
        c += " --simulate";
        if (!I.sim_truth.empty()) {
            const std::string p = ses_abs(I.sim_truth);
            c += " --sim-truth " + quote(p);
            consumed->push_back({ "sim_truth", p, ses_hash_file(p), 0 });
        }
    } else {
        if (!I.driver.empty()) c += " --driver " + quote(I.driver);
        c += " --input " + std::to_string(I.input_first);
    }
    if (!I.temp.empty()) c += " --temp " + I.temp;
    return c;
}
static void use_art(const Session& S, int j, std::vector<SesFile>* consumed) {
    const std::string p = art_path(S, j);
    consumed->push_back({ IDS[j], ARTS[j], ses_hash_file(p), S.step[j].run });
}
static void use_input(const std::string& what, const std::string& p, std::vector<SesFile>* consumed) {
    consumed->push_back({ what, p, ses_hash_file(p), 0 });
}
/* the capsule table for the steps after the survey: a tracked run reads its offset from the body-frame
 * survey, an untracked one takes the room-axes survey and the acoustic center it printed as --mic */
static std::string survey_flags(const Session& S, bool mic_ok, const float ref[3], std::vector<SesFile>* consumed) {
    const SesStep& C = S.step[SES_CAPSULES];
    const bool have = C.status == SES_PASSED;
    std::string c;
    if (have) { c += " --survey capsules.json"; use_art(S, SES_CAPSULES, consumed); }
    if (tracked(S)) {
        if (!have && !S.in.mount_offset.empty()) c += " --mount-offset " + S.in.mount_offset;
    } else if (mic_ok) {
        const float* m = (have && C.have_center) ? C.center : ref;
        char b[96];
        snprintf(b, sizeof b, " --mic %.4f %.4f %.4f", m[0], m[1], m[2]);
        c += b;
    }
    return c;
}

bool ses_command(const Session& S, int k, const SesTools& T, std::string* cmd, std::vector<SesFile>* consumed,
                 std::string* why) {
    consumed->clear();
    const std::string blocked = ses_blocked(S, k);
    if (!blocked.empty()) { *why = blocked; return false; }
    const SesInputs& I = S.in;
    const std::string plan = ses_abs(I.plan);
    std::string c;
    float ref[3] = { 0.f, 0.f, 0.f };
    int n = 0;
    switch (k) {
    case SES_FRAME:
        if (T.survey.empty()) { *why = "bwa_speaker_survey was not found beside this tool"; return false; }
        c = quote(T.survey) + " " + quote(plan) + " --csv frame_check.csv --require-frame";
        {   /* layout positions are acoustic centers: the markers' centroid is moved back by this much */
            std::string v;
            input_value(S, "baffle_offset_m", &v);
            c += " --baffle-offset-m " + v;
            consumed->push_back({ "baffle_offset_m", v, std::string(), 0 });
        }
        if (S.simulate) {
            c += " --simulate";
            if (!I.frame_speakers.empty()) c += " --sim-speakers " + I.frame_speakers;
        } else {
            c += " --server " + I.nn_server;
            if (!I.nn_multicast.empty()) c += " --multicast " + I.nn_multicast;
        }
        use_input("plan", plan, consumed);
        break;
    case SES_LOCALIZE: {
        if (T.calibrate.empty()) { *why = "bwa_calibrate was not found beside this tool"; return false; }
        const std::string rows = ses_abs(I.localize_rows);
        c = quote(T.calibrate) + " --layout " + quote(plan) + " --out as_built.json --zylia --localize " + quote(rows);
        c += device_flags(S, consumed) + track_flags(S);
        if (tracked(S) && !I.mount_offset.empty()) c += " --mount-offset " + I.mount_offset;
        use_input("plan", plan, consumed);
        use_input("localize_rows", rows, consumed);
        break;
    }
    case SES_CAPSULES: {
        if (T.calibrate.empty()) { *why = "bwa_calibrate was not found beside this tool"; return false; }
        if (!ses_layout_ref(art_path(S, SES_LOCALIZE), ref, &n)) { *why = "as_built.json does not load"; return false; }
        char mic[96];
        snprintf(mic, sizeof mic, " --mic %.4f %.4f %.4f", ref[0], ref[1], ref[2]);
        c = quote(T.calibrate) + " --layout as_built.json --zylia --capsule-survey capsules.json" + mic;
        if (!I.capsule_speakers.empty()) c += " --speakers " + I.capsule_speakers;
        {   /* localize pins the latency (its mic rows move); the survey's own four-unknown solve cannot on
             * a dome, so hand it the one that is measured */
            const SesStep& Lz = S.step[SES_LOCALIZE];
            if (Lz.have_latency) { char lb[48]; snprintf(lb, sizeof lb, " --latency %.4f", Lz.latency_m); c += lb; }
        }
        c += device_flags(S, consumed) + track_flags(S);
        if (tracked(S) && !I.mount_offset.empty()) c += " --mount-offset " + I.mount_offset;
        use_art(S, SES_LOCALIZE, consumed);
        break;
    }
    case SES_TRIMS:
    case SES_VERIFY: {
        if (T.calibrate.empty()) { *why = "bwa_calibrate was not found beside this tool"; return false; }
        const int in = k == SES_TRIMS ? SES_LOCALIZE : SES_TRIMS;
        if (!ses_layout_ref(art_path(S, in), ref, &n)) { *why = std::string(ARTS[in]) + " does not load"; return false; }
        c = quote(T.calibrate) + " --layout " + ARTS[in] + (k == SES_TRIMS ? " --out trims.json --zylia --trims --room"
                                                                           : " --zylia --verify");
        use_art(S, in, consumed);
        c += device_flags(S, consumed) + track_flags(S) + survey_flags(S, true, ref, consumed);
        break;
    }
    case SES_GRID: {
        if (T.calibrate.empty()) { *why = "bwa_calibrate was not found beside this tool"; return false; }
        const std::string rows = ses_abs(I.grid_rows);
        c = quote(T.calibrate) + " --layout trims.json --out grid.json --zylia --room-eq-grid " + quote(rows);
        use_art(S, SES_TRIMS, consumed);
        use_input("grid_rows", rows, consumed);
        c += device_flags(S, consumed) + track_flags(S) + survey_flags(S, false, ref, consumed);
        break;
    }
    case SES_VALIDATE: {
        if (T.validate.empty()) { *why = "bwa_validate was not found beside this tool"; return false; }
        const int in = S.step[SES_GRID].status == SES_PASSED ? SES_GRID : SES_TRIMS;
        c = quote(T.validate) + " --layout " + ARTS[in] + " --out validate.csv --azimuths " +
            std::to_string(I.validate_azimuths >= 3 && I.validate_azimuths <= 36 ? I.validate_azimuths : 12);
        use_art(S, in, consumed);
        if (!I.validate_positions.empty()) {
            const std::string p = ses_abs(I.validate_positions);
            c += " --positions " + quote(p);
            use_input("validate_positions", p, consumed);
        }
        if (!I.validate_reference) c += " --no-reference";
        if (S.simulate) c += " --simulate";
        else {
            if (!I.driver.empty()) c += " --driver " + quote(I.driver);
            c += " --mic-in " + std::to_string(I.input_first);
        }
        c += track_flags(S);
        if (S.step[SES_CAPSULES].status == SES_PASSED) { c += " --survey capsules.json"; use_art(S, SES_CAPSULES, consumed); }
        else if (tracked(S) && !I.mount_offset.empty()) c += " --mount-offset " + I.mount_offset;
        break;
    }
    default:
        *why = "this step runs in the Aim tab, not as a tool";
        return false;
    }
    *cmd = c;
    return true;
}

/* ---- records -------------------------------------------------------------------------------------- */

void ses_begin(Session* S, int k, const std::string& cmd, const std::vector<SesFile>& consumed) {
    SesStep& P = S->step[k];
    P = SesStep();                       /* a new record starts empty */
    P.status = SES_RUNNING;
    P.run = S->next_run++;
    P.started = ses_now();
    P.command = cmd;
    P.consumed = consumed;
}

void ses_skip(Session* S, int k, const std::string& note) {
    SesStep& P = S->step[k];
    P = SesStep();
    P.status = SES_SKIPPED;
    P.run = S->next_run++;
    P.started = P.finished = ses_now();
    P.note = note;
}

void ses_reset(Session* S, int k) {
    SesStep& P = S->step[k];
    P = SesStep();
    P.run = S->next_run++;               /* anything that used this step's file reads stale now */
}

/* the numbers after `key` on the LAST line that has it; true when all `n` parsed */
static bool last_nums(const std::string& out, const char* key, const char* fmt, double* v, int n) {
    size_t at = std::string::npos, from = 0;
    for (size_t p; (p = out.find(key, from)) != std::string::npos; from = p + 1) at = p;
    if (at == std::string::npos) return false;
    double a[3] = { 0, 0, 0 };
    const int got = sscanf(out.c_str() + at + strlen(key), fmt, &a[0], &a[1], &a[2]);
    for (int i = 0; i < n && i < 3; ++i) v[i] = a[i];
    return got == n;
}
static std::string last_line_with(const std::string& out, const char* key) {
    size_t at = std::string::npos, from = 0;
    for (size_t p; (p = out.find(key, from)) != std::string::npos; from = p + 1) at = p;
    if (at == std::string::npos) return std::string();
    const size_t b = out.rfind('\n', at), e = out.find('\n', at);
    std::string l = out.substr(b == std::string::npos ? 0 : b + 1, e == std::string::npos ? std::string::npos : e - (b == std::string::npos ? 0 : b + 1));
    while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
    return l;
}

void ses_finish(Session* S, int k, int exit_code, const std::string& out, bool canceled) {
    SesStep& P = S->step[k];
    P.finished = ses_now();
    P.exit_code = canceled ? -1 : exit_code;
    P.status = (!canceled && exit_code == 0) ? SES_PASSED : SES_FAILED;
    P.note = canceled ? "canceled" : (exit_code ? ses_exit_meaning(k, exit_code) : "");
    P.produced.clear();
    if (ARTS[k][0]) {
        const std::string p = art_path(*S, k);
        const std::string h = ses_hash_file(p);
        if (!h.empty()) P.produced.push_back({ IDS[k], ARTS[k], h, P.run });
        else if (P.status == SES_PASSED) {     /* exit 0 is not a result without its file */
            P.status = SES_FAILED;
            P.note = std::string("exit 0, but ") + ARTS[k] + " was not written";
        }
    }
    char b[256];
    double v[3];
    P.summary.clear();
    /* the background: the loudest "background: <dBFS>" the tool printed (bwa_calibrate once a run,
     * bwa_validate once a placement), or silent */
    P.have_bg = false;
    for (size_t from = 0, p; (p = out.find("background: ", from)) != std::string::npos; from = p + 1) {
        double d;
        if (!out.compare(p + 12, 6, "silent")) d = -300.0;
        else if (sscanf(out.c_str() + p + 12, "%lf dBFS", &d) != 1) continue;
        if (!P.have_bg || d > P.bg_dbfs) P.bg_dbfs = d;
        P.have_bg = true;
    }
    switch (k) {
    case SES_FRAME:
        P.summary = last_line_with(out, "summary: ");
        break;
    case SES_LOCALIZE: {
        /* the per-speaker solves each carry the system latency; their median is the run's */
        std::vector<double> lat;
        for (size_t from = 0, p; (p = out.find("[system latency ", from)) != std::string::npos; from = p + 1) {
            double m;
            if (sscanf(out.c_str() + p + 16, "%lf", &m) == 1) lat.push_back(m);
        }
        if (!lat.empty()) {
            std::sort(lat.begin(), lat.end());
            const size_t h = lat.size() / 2;
            P.latency_m = (lat.size() & 1) ? lat[h] : 0.5 * (lat[h - 1] + lat[h]);
            P.have_latency = true;
            snprintf(b, sizeof b, "system latency %.3f m (median of %d speakers)", P.latency_m, (int)lat.size());
            P.summary = b;
        }
        /* the rig: the solved latency against the driver's own loop (the runbook's "residual sane"). The
         * session's localize is always the ZM-1, so the Dante Via leg is in the residual: the same rule
         * the tool applies (calib_latency_check) */
        if (last_nums(out, "-> residual ", "%lf", v, 1)) {
            const int lc = calib_latency_check(1, v[0] * 1e-3, 0.0, NULL);
            snprintf(b, sizeof b, "; residual %+.2f ms against the driver's loop%s", v[0],
                     lc == CALIB_LAT_IMPOSSIBLE ? " (NEGATIVE: wrong device or rate)"
                     : lc == CALIB_LAT_WARN ? " (LARGE: past the Via leg's allowance; an extra buffer?)" : "");
            P.summary += b;
        }
        break;
    }
    case SES_CAPSULES:
        if (last_nums(out, "capsule survey: residual ", "%lf us, radius %lf mm", v, 2)) {
            snprintf(b, sizeof b, "residual %.2f us, radius %.2f mm", v[0], v[1]);
            P.summary = b;
        }
        if (last_nums(out, "--latency ", "%lf", v, 1) && out.find("for live aiming") != std::string::npos) {
            P.have_latency = true; P.latency_m = v[0];
            snprintf(b, sizeof b, "; latency %.3f m", v[0]);
            P.summary += b;
        }
        if (last_nums(out, "array center as --mic: (", "%lf %lf %lf", v, 3)) {
            P.have_center = true;
            for (int a = 0; a < 3; ++a) P.center[a] = (float)v[a];
        }
        if (out.find("WARNING the acoustic center and the tracked one disagree") != std::string::npos)
            P.summary += "; the acoustic and tracked centers DISAGREE (see the log)";
        if (exit_code) {   /* exit 6: the survey refused, and its LAST flag lines name the speakers it refused on */
            std::string fl;
            for (const char* key : { "leave-one-out flagged speaker(s) ", "the range check flagged speaker(s) " }) {
                const std::string f = last_line_with(out, key);
                const size_t at = f.find(key[0] == 't' ? "range check flagged" : "leave-one-out flagged");
                if (at != std::string::npos) fl += (fl.empty() ? "" : "; ") + f.substr(at);
            }
            if (!fl.empty()) P.summary += (P.summary.empty() ? "" : "; ") + fl + ", nothing written";
        } else {
            const std::string f = last_line_with(out, "the survey leaves out speaker ");
            const size_t at = f.find("leaves out speaker");
            if (at != std::string::npos) P.summary += (P.summary.empty() ? "" : "; ") + f.substr(at);
        }
        break;
    case SES_TRIMS:
        P.summary = last_line_with(out, "trims: gain_db in ");
        if (last_nums(out, "room: mean RT60 ~ ", "%lf", v, 1)) { snprintf(b, sizeof b, "; RT60 %.2f s", v[0]); P.summary += b; }
        break;
    case SES_VERIFY:
        P.summary = last_line_with(out, "verify: arrival spread ");
        { const std::string f = last_line_with(out, "speaker(s) flagged");
          if (!f.empty()) P.summary += (P.summary.empty() ? "" : "; ") + f; }
        break;
    case SES_GRID:
        P.summary = last_line_with(out, "into room_eq_grid");
        break;
    case SES_VALIDATE:
        P.summary = last_line_with(out, "cells to ");
        break;
    default: break;
    }
}

std::string ses_background_line(const Session& S, int k) {
    if (k < 0 || k >= SES_NSTEPS || !S.step[k].have_bg) return "";
    const SesStep& P = S.step[k];
    char b[300];
    const bool silent = P.bg_dbfs <= SES_BG_SILENT_DBFS;
    int prev = -1;
    for (int j = k - 1; j >= 0 && prev < 0; --j) if (S.step[j].have_bg) prev = j;
    if (silent) snprintf(b, sizeof b, "background: %s silent (simulate)", IDS[k]);
    else        snprintf(b, sizeof b, "background: %s %.1f dBFS", IDS[k], P.bg_dbfs);
    std::string l = b;
    if (prev >= 0 && !silent && S.step[prev].bg_dbfs > SES_BG_SILENT_DBFS) {
        const double d = P.bg_dbfs - S.step[prev].bg_dbfs;
        snprintf(b, sizeof b, ", %+.1f dB against %s (%.1f dBFS)%s", d, IDS[prev], S.step[prev].bg_dbfs,
                 d > SES_BG_RISE_DB ? "  <-- RISING: something in the room got louder" : "");
        l += b;
    }
    return l;
}

/* ---- the runner ----------------------------------------------------------------------------------- */

/* one committed line: the shown log and the file */
static void commit_line(SesRunner* R, FILE* f, const std::string& l) {
    if (f) { fwrite(l.data(), 1, l.size(), f); fputc('\n', f); fflush(f); }
    R->lines.push_back(l);
    if (R->lines.size() > 4000) R->lines.erase(R->lines.begin(), R->lines.begin() + 1000);
}

void ses_log(SesRunner* R, const std::string& log_path, const std::string& line) {
    std::lock_guard<std::mutex> lk(R->mu);
    FILE* f = log_path.empty() ? NULL : open_u8(log_path, L"ab");
    commit_line(R, f, line);
    if (f) fclose(f);
}

void ses_log_reload(SesRunner* R, const std::string& log_path) {
    std::lock_guard<std::mutex> lk(R->mu);
    R->lines.clear();
    R->partial.clear();
    FILE* f = open_u8(log_path, L"rb");
    if (!f) return;
    std::string txt;
    char buf[16384];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) txt.append(buf, n);
    fclose(f);
    size_t from = txt.size() > 400000 ? txt.size() - 400000 : 0;   /* the tail */
    if (from) from = txt.find('\n', from) + 1;
    for (size_t e; (e = txt.find('\n', from)) != std::string::npos; from = e + 1) {
        std::string l = txt.substr(from, e - from);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        R->lines.push_back(l);
    }
    if (R->lines.size() > 4000) R->lines.erase(R->lines.begin(), R->lines.end() - 4000);
}

static void reader(SesRunner* R, HANDLE out_r) {
    FILE* f = open_u8(R->log_path, L"ab");
    char buf[4096];
    DWORD n = 0;
    bool cr = false;                     /* a \r waiting to learn whether \n follows */
    while (ReadFile(out_r, buf, sizeof buf, &n, NULL) && n > 0) {
        std::lock_guard<std::mutex> lk(R->mu);
        R->output.append(buf, n);
        for (DWORD i = 0; i < n; ++i) {
            const char c = buf[i];
            if (cr) {
                cr = false;
                if (c == '\n') { commit_line(R, f, R->partial); R->partial.clear(); continue; }
                R->partial.clear();      /* a bare \r: the live line rewrites itself */
            }
            if (c == '\r') cr = true;
            else if (c == '\n') { commit_line(R, f, R->partial); R->partial.clear(); }
            else R->partial.push_back(c);
        }
    }
    CloseHandle(out_r);
    WaitForSingleObject((HANDLE)R->proc, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess((HANDLE)R->proc, &code);
    {
        std::lock_guard<std::mutex> lk(R->mu);
        if (!R->partial.empty()) { commit_line(R, f, R->partial); R->partial.clear(); }
        R->exit_code = (int)code;
    }
    if (f) fclose(f);
    R->state.store(2, std::memory_order_release);
}

bool ses_run_start(SesRunner* R, const std::string& cmd, const std::string& cwd, const std::string& log_path,
                   std::string* why) {
    if (R->state.load() != 0) { *why = "a step is already running"; return false; }
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE out_r = NULL, out_w = NULL, in_r = NULL, in_w = NULL;
    if (!CreatePipe(&out_r, &out_w, &sa, 0) || !CreatePipe(&in_r, &in_w, &sa, 0)) {
        *why = "CreatePipe failed";
        if (out_r) { CloseHandle(out_r); CloseHandle(out_w); }
        return false;
    }
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);   /* our ends stay ours */
    SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
    /* only the two pipe ends reach the child: an inherited stray handle would keep a pipe open */
    SIZE_T asz = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &asz);
    std::vector<unsigned char> abuf(asz);
    LPPROC_THREAD_ATTRIBUTE_LIST al = (LPPROC_THREAD_ATTRIBUTE_LIST)abuf.data();
    HANDLE inherit[2] = { out_w, in_r };
    bool ok = InitializeProcThreadAttributeList(al, 1, 0, &asz) &&
              UpdateProcThreadAttribute(al, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof inherit, NULL, NULL);
    STARTUPINFOEXW si;
    memset(&si, 0, sizeof si);
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdOutput = out_w; si.StartupInfo.hStdError = out_w; si.StartupInfo.hStdInput = in_r;
    si.lpAttributeList = al;
    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof pi);
    std::wstring wcmd = wide(cmd), wcwd = wide(cwd);
    /* CREATE_NO_WINDOW: no console, so the tools' "press a key" never reads a stray key; the gate decides */
    if (ok) ok = CreateProcessW(NULL, &wcmd[0], NULL, NULL, TRUE, CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
                                NULL, wcwd.empty() ? NULL : wcwd.c_str(), &si.StartupInfo, &pi) != 0;
    DeleteProcThreadAttributeList(al);
    CloseHandle(out_w);
    CloseHandle(in_r);
    if (!ok) {
        char b[64];
        snprintf(b, sizeof b, "CreateProcess failed (error %lu)", (unsigned long)GetLastError());
        *why = b;
        CloseHandle(out_r); CloseHandle(in_w);
        return false;
    }
    CloseHandle(pi.hThread);
    {
        std::lock_guard<std::mutex> lk(R->mu);
        R->output.clear(); R->partial.clear();
        R->log_path = log_path;
    }
    R->proc = pi.hProcess;
    R->stdin_w = in_w;
    R->exit_code = 0;
    R->canceled.store(false);
    R->state.store(1, std::memory_order_release);
    R->th = std::thread(reader, R, out_r);
    return true;
}

void ses_run_cancel(SesRunner* R) {
    if (R->state.load() != 1 || !R->proc) return;
    R->canceled.store(true);
    TerminateProcess((HANDLE)R->proc, 1);
}

void ses_run_enter(SesRunner* R) {
    if (R->state.load() != 1 || !R->stdin_w) return;
    DWORD w = 0;
    WriteFile((HANDLE)R->stdin_w, "\n", 1, &w, NULL);
}

void ses_run_join(SesRunner* R) {
    if (R->th.joinable()) R->th.join();
    if (R->proc) { CloseHandle((HANDLE)R->proc); R->proc = nullptr; }
    if (R->stdin_w) { CloseHandle((HANDLE)R->stdin_w); R->stdin_w = nullptr; }
    R->state.store(0, std::memory_order_release);
}
