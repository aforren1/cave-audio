/*
 * profile.h — Tracy instrumentation, compiled to nothing unless BW_TRACY is defined
 * (set by `cmake -DBWAUDIO_TRACY=ON`, which also fetches Tracy + defines TRACY_ENABLE).
 *
 * Usage (works in both C and C++ — we use Tracy's C API everywhere for one code path):
 *   BW_ZONE_BEGIN(z, "name"); ... BW_ZONE_END(z);   // a timed scope; `z` names the context var
 *   BW_FRAME_MARK();                                 // one per audio block (a "frame" in Tracy)
 *   BW_PLOT("voices", n);                            // a scalar time-series
 *   BW_THREAD_NAME("bw-audio");                       // name the current thread in the profiler
 *   BW_ALLOC(ptr, size) / BW_FREE(ptr);              // track a heap block in the memory view
 *
 * Connect the standalone Tracy profiler (github.com/wolfpld/tracy) to a running engine to view.
 */
#ifndef BW_PROFILE_H
#define BW_PROFILE_H

#if defined(BW_TRACY)
  #include "tracy/TracyC.h"
  #define BW_ZONE_BEGIN(ctx, name) TracyCZoneN(ctx, name, 1)
  #define BW_ZONE_END(ctx)         TracyCZoneEnd(ctx)
  #define BW_FRAME_MARK()          TracyCFrameMark
  #define BW_PLOT(name, val)       TracyCPlot(name, (double)(val))
  #define BW_THREAD_NAME(name)     TracyCSetThreadName(name)
  #define BW_ALLOC(ptr, size)      TracyCAlloc((ptr), (size))
  #define BW_FREE(ptr)             TracyCFree((ptr))
  #define BW_MSG(txt)              TracyCMessageL(txt)
#else
  #define BW_ZONE_BEGIN(ctx, name) ((void)0)
  #define BW_ZONE_END(ctx)         ((void)0)
  #define BW_FRAME_MARK()          ((void)0)
  #define BW_PLOT(name, val)       ((void)(val))        /* consume val so a counter isn't "set but unused" */
  #define BW_THREAD_NAME(name)     ((void)0)
  #define BW_ALLOC(ptr, size)      ((void)(ptr), (void)(size))
  #define BW_FREE(ptr)             ((void)(ptr))
  #define BW_MSG(txt)              ((void)0)
#endif

#endif /* BW_PROFILE_H */
