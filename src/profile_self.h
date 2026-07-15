/*
 * profile_self.h — a tiny in-process zone profiler: the headless, dependency-free alternative to the
 * Tracy CLI tools (which need a full server dep stack to build). When the dll is compiled with
 * -DBWA_PROFILE_SELF, profile.h routes BWA_ZONE_BEGIN/END/FRAME_MARK here, and each named zone
 * accumulates (total time, call count) into a fixed table across all threads. bwa_prof_report() prints
 * a per-zone breakdown — the same "where does the block time go" view tracy-csvexport gives, minus the
 * flame graph. Profiling-build only; a normal build compiles the accumulation out.
 */
#ifndef BWA_PROFILE_SELF_H
#define BWA_PROFILE_SELF_H

#include <stdint.h>

typedef struct { const char* name; uint64_t t0; } bwa_prof_zone;

/* zone hooks (wired by profile.h under BWA_PROFILE_SELF; not called otherwise) */
bwa_prof_zone bwa_prof__begin(const char* name);
void          bwa_prof__end(bwa_prof_zone* z);
void          bwa_prof__frame(void);

/* readout — exported from the dll (a consumer declares them dllimport). reset clears the table and
 * returns 1 if the dll was built with -DBWA_PROFILE_SELF (0 = accumulation compiled out — nothing to
 * report). report prints one row per zone (calls, total ms, mean us, us/block) and returns the count. */
#if defined(BWA_BUILD_DLL)
  #define BWA_PROF_API __declspec(dllexport)
#else
  #define BWA_PROF_API __declspec(dllimport)
#endif
BWA_PROF_API int bwa_prof_reset(void);
BWA_PROF_API int bwa_prof_report(void);

#endif /* BWA_PROFILE_SELF_H */
