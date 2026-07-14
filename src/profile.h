/*
 * profile.h — Tracy instrumentation, compiled to nothing unless BWA_TRACY is defined
 * (set by `cmake -DBWA_TRACY=ON`, which also fetches Tracy + defines TRACY_ENABLE).
 *
 * Usage (works in both C and C++ — we use Tracy's C API everywhere for one code path):
 *   BWA_ZONE_BEGIN(z, "name"); ... BWA_ZONE_END(z);   // a timed scope; `z` names the context var
 *   BWA_FRAME_MARK();                                 // one per audio block (a "frame" in Tracy)
 *   BWA_PLOT("voices", n);                            // a scalar time-series
 *   BWA_THREAD_NAME("bw-audio");                       // name the current thread in the profiler
 *   BWA_ALLOC(ptr, size) / BWA_FREE(ptr);              // track a heap block in the memory view
 *
 * Connect the standalone Tracy profiler (github.com/wolfpld/tracy) to a running engine to view.
 */
#ifndef BWA_PROFILE_H
#define BWA_PROFILE_H

#if defined(BWA_TRACY)
  #include "tracy/TracyC.h"
  #define BWA_ZONE_BEGIN(ctx, name) TracyCZoneN(ctx, name, 1)
  #define BWA_ZONE_END(ctx)         TracyCZoneEnd(ctx)
  #define BWA_FRAME_MARK()          TracyCFrameMark
  #define BWA_PLOT(name, val)       TracyCPlot(name, (double)(val))
  #define BWA_THREAD_NAME(name)     TracyCSetThreadName(name)
  #define BWA_ALLOC(ptr, size)      TracyCAlloc((ptr), (size))
  #define BWA_FREE(ptr)             TracyCFree((ptr))
  #define BWA_MSG(txt)              TracyCMessageL(txt)
#else
  #define BWA_ZONE_BEGIN(ctx, name) ((void)0)
  #define BWA_ZONE_END(ctx)         ((void)0)
  #define BWA_FRAME_MARK()          ((void)0)
  #define BWA_PLOT(name, val)       ((void)(val))        /* consume val so a counter isn't "set but unused" */
  #define BWA_THREAD_NAME(name)     ((void)0)
  #define BWA_ALLOC(ptr, size)      ((void)(ptr), (void)(size))
  #define BWA_FREE(ptr)             ((void)(ptr))
  #define BWA_MSG(txt)              ((void)0)
#endif

#endif /* BWA_PROFILE_H */
