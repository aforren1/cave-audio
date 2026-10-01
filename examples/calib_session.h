/*
 * calib_session.h - the rig-day calibration SESSION behind bwa_calib_view's Session tab.
 *
 * One folder holds one session: session.json (the inputs and every step's record), session.log (every
 * step's command and output, appended), and the files the steps write. The steps are the runbook's
 * Stage 2 in order (docs/hardware-validation.md), each handing the next its files:
 *
 *   frame     bwa_speaker_survey <plan> --require-frame
 *             --baffle-offset-m <the input>                      -> frame_check.csv
 *   localize  bwa_calibrate --localize rows --zylia, <plan> in   -> as_built.json   (the plan is never written)
 *   capsules  bwa_calibrate --capsule-survey, as_built in        -> capsules.json
 *   aim       the Aim tab, in process, as_built + capsules       -> the readings the installer accepted
 *   trims     bwa_calibrate --zylia --trims --room, as_built in  -> trims.json
 *   verify    bwa_calibrate --zylia --verify, trims in           -> a status, no file
 *   grid      bwa_calibrate --room-eq-grid rows, trims in        -> grid.json
 *   validate  bwa_validate, grid (or trims) + capsules in        -> validate.csv
 *
 * Every step but aim runs as a SUBPROCESS of the tested tool, with its exit code as the result: the
 * session adds the file chain, the record and the log, never a second implementation of a measurement.
 * Each record keeps what the step consumed (the producing step's run number and the file's hash) and
 * what it produced, so a step whose input was re-made since it ran reads STALE, and a step whose
 * prerequisite failed, is stale or never ran is BLOCKED, with the reason.
 *
 * Windows-only (CreateProcessW, pipes), like the viewer. The model functions are UI-free; the viewer
 * draws the tab and drives the Aim tab for the one in-process step.
 */
#ifndef BWA_CALIB_SESSION_H
#define BWA_CALIB_SESSION_H

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

enum SesStepId { SES_FRAME, SES_LOCALIZE, SES_CAPSULES, SES_AIM, SES_TRIMS, SES_VERIFY, SES_GRID, SES_VALIDATE, SES_NSTEPS };
enum SesStatus { SES_PENDING, SES_RUNNING, SES_PASSED, SES_FAILED, SES_SKIPPED };

/* One file a step consumed or produced. `what` is the producing step's id ("localize"), or the input's
 * name ("plan", "localize_rows", ...) for a file the installer gave. Artifacts are paths relative to the
 * session folder, inputs absolute. `run` is the producing step's run number (0 for an input). A VALUE
 * input a step read ("baffle_offset_m") is recorded the same way, its value as text in `path` and no
 * hash, so changing it makes the step stale the way a changed file does. */
struct SesFile {
    std::string what, path, hash;
    int         run = 0;
};

/* One reading the installer accepted in the Aim tab for the aim step. */
struct SesAimRec {
    int         spk = -1;
    float       below_db = 0.f;          /* below the held peak, dB */
    std::string angle;                   /* the off-axis estimate as the tab shows it, or "-" */
    bool        have_pos = false;
    float       pos_mm = 0.f;            /* |d| against the plan, mm */
    std::string words;                   /* the move in room words */
    std::string when;
};

struct SesStep {
    SesStatus   status = SES_PENDING;
    int         run = 0;                 /* the session's run counter at the start of this record */
    int         exit_code = 0;
    std::string started, finished, note, command, summary;
    std::vector<SesFile> consumed, produced;
    bool        have_latency = false;    /* localize, capsules: the solved system latency */
    double      latency_m = 0.0;
    bool        have_center = false;     /* capsules, untracked: the acoustic center to pass as --mic */
    float       center[3] = { 0.f, 0.f, 0.f };
    std::vector<SesAimRec> aim;          /* aim: the accepted readings */
    bool        have_bg = false;         /* the room's background the step's tool read (the loudest it
                                          * printed: "background: <dBFS>"), so a rising one shows */
    double      bg_dbfs = 0.0;           /* <= SES_BG_SILENT_DBFS: silent (simulate) */
};
#define SES_BAFFLE_MAX_M 0.5             /* the baffle offset input's range, +/- */
#define SES_BG_SILENT_DBFS (-299.0)
#define SES_BG_RISE_DB 6.0                /* a step this much louder than the last one that read it: flagged */

struct SesInputs {
    std::string plan;                    /* the plan layout: read, never written */
    std::string driver;                  /* ASIO driver, empty = auto */
    int         input_first = 0;         /* the ZM-1's first capsule input */
    std::string nn_server, nn_multicast = "239.255.42.99", body;
    std::string mount_offset;            /* "ring", "x,y,z", or empty (0: the pivot is the center) */
    std::string temp;                    /* --temp, empty = the layout's or 20 C */
    std::string localize_rows, grid_rows;
    std::string frame_speakers;          /* simulate: --sim-speakers for the frame check, empty = the tool's 4 */
    double      baffle_offset_m = 0.0;   /* the frame check's --baffle-offset-m: layout positions are acoustic
                                          * centers, the markers sit on the baffle this far in front of them */
    std::string capsule_speakers;        /* --speakers for the capsule survey, empty = all */
    std::string aim_speakers;            /* the speakers to aim, empty = none (skip the step) */
    std::string validate_positions;      /* --positions, empty = the tool's default envelope */
    int         validate_azimuths = 12;
    bool        validate_reference = true;
    int         place_timeout_s = 300;
    std::string sim_truth;               /* simulate: where the speakers really stand (--sim-truth) */
};

struct Session {
    std::string folder;                  /* absolute, no trailing separator; empty = no session open */
    std::string created;
    bool        simulate = true;
    int         next_run = 1;
    SesInputs   in;
    SesStep     step[SES_NSTEPS];
};

/* The tool executables a session runs (absolute paths; empty = not found). */
struct SesTools { std::string calibrate, validate, survey; };

const char* ses_step_id(int k);          /* "frame", "localize", ... */
const char* ses_step_title(int k);       /* "Motive frame check", ... */
const char* ses_step_artifact(int k);    /* "as_built.json", or "" for aim and verify */
const char* ses_status_name(SesStatus s);
const char* ses_exit_meaning(int k, int code);

std::string ses_now(void);               /* local time, ISO 8601 to the second */
std::string ses_hash_file(const std::string& path);   /* FNV-1a 64 as 16 hex digits, "" = unreadable */
std::string ses_abs(const std::string& path);         /* absolute, from the current directory; "" stays "" */
std::string ses_join(const std::string& folder, const std::string& name);
bool        ses_file_exists(const std::string& path);

/* The session file: <folder>/session.json, written whole through a temp file and a rename. */
bool ses_save(const Session& S, std::string* err);
bool ses_load(Session* S, const std::string& folder, std::string* err);

/* Why step k cannot run now ("" = it can), and why its record is stale ("" = it is current). */
std::string ses_blocked(const Session& S, int k);
std::string ses_stale(const Session& S, int k);
/* The command line for a subprocess step, and the files it will consume. false + why = refused. */
bool ses_command(const Session& S, int k, const SesTools& T, std::string* cmd, std::vector<SesFile>* consumed,
                 std::string* why);
/* After a subprocess step: status from the exit code, the artifact checked and hashed, the values the
 * next steps read (the latency, the acoustic center) parsed out of its output, the one-line summary. */
void ses_finish(Session* S, int k, int exit_code, const std::string& output, bool canceled);
/* Begin a record (status running, a new run number, the start time, the consumed files). */
void ses_begin(Session* S, int k, const std::string& cmd, const std::vector<SesFile>& consumed);
/* Mark a step skipped (note required), or put it back to pending. */
void ses_skip(Session* S, int k, const std::string& note);
void ses_reset(Session* S, int k);
/* The background trend line the log gets after step k: its background against the last earlier step
 * (in step order) that read one, "RISING" past SES_BG_RISE_DB. "" when step k read none. */
std::string ses_background_line(const Session& S, int k);
/* The listening point (and speaker count) of a layout file, through the engine's loader. */
bool ses_layout_ref(const std::string& path, float ref[3], int* count);
/* The speaker list syntax bwa_calibrate takes ("0,3,5-9"); returns the count, -1 = malformed. */
int  ses_parse_speakers(const std::string& s, int n, int* out, int cap);

/* ---- the subprocess runner: one child at a time, its stdout and stderr streamed into the log ---- */
struct SesRunner {
    std::atomic<int>  state{ 0 };        /* 0 idle, 1 running, 2 exited (the UI collects it) */
    std::atomic<bool> canceled{ false };
    int               exit_code = 0;
    void*             proc = nullptr;    /* HANDLE */
    void*             stdin_w = nullptr; /* HANDLE: Enter for the untracked prompts */
    std::thread       th;
    std::mutex        mu;                /* guards everything below */
    std::vector<std::string> lines;      /* the log as shown: earlier runs, then this one */
    std::string       partial;           /* the line being written (a live \r line shows here) */
    std::string       output;            /* this run's whole output, for ses_finish */
    std::string       log_path;          /* <folder>/session.log, appended */
};
/* Start `cmd` with its working directory `cwd`. false + why when the process did not start. */
bool ses_run_start(SesRunner* R, const std::string& cmd, const std::string& cwd, const std::string& log_path,
                   std::string* why);
void ses_run_cancel(SesRunner* R);       /* TerminateProcess; the reader thread reaps it */
void ses_run_enter(SesRunner* R);        /* write "\n" to the child's stdin */
void ses_run_join(SesRunner* R);         /* after state 2: join the reader thread */
/* A line into the log file and the shown log, from the UI thread (headers, results, refusals). */
void ses_log(SesRunner* R, const std::string& log_path, const std::string& line);
/* Load the shown log from the tail of <folder>/session.log (a reopened session shows its history). */
void ses_log_reload(SesRunner* R, const std::string& log_path);

#endif
