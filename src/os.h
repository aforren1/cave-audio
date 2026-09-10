/*
 * os.h — the OS portability shim: the small set of platform calls the engine makes OUTSIDE the
 * device sinks. See docs/backends.md ("src/os.h") for the inventory this replaces.
 *
 * The rule that shapes this header: ASIO and WASAPI are C++ translation units that include
 * sink.h, and sink.h includes this file for BWA_EXPORT. So os.h must compile as C++ — which
 * means NO <stdatomic.h> and no `_Atomic` here, and no <windows.h> or <winsock2.h> either
 * (winsock2.h after windows.h is the classic redefinition break). Shared state that used to be
 * `Interlocked*` on `volatile LONG` now uses C11 atomics DIRECTLY IN THE .c FILES, which is why
 * the socket calls below are wrapped whole rather than exposing sockaddr to the caller: natnet.c
 * then needs no socket header of its own.
 *
 * Nothing here may be called from the audio thread. The one thing that looks close is the null
 * sink's pacing sleep, and that runs between render calls, never inside one.
 */
#ifndef BWA_OS_H
#define BWA_OS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if !defined(_WIN32)
#include <pthread.h>       /* C++-safe; the Windows branch pulls in no system header at all */
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Symbol visibility for the INTERNAL hooks the tests reach through the dll (sink.h's null-sink tap
 * and skip counter, profile_self.h's reset/report). The public ABI has its own BWA_API in
 * bw_audio.h; this one exists because those symbols are deliberately not in the public header.
 * Off Windows the library builds with -fvisibility=hidden, so the attribute is what makes them
 * reachable at all. */
#if defined(_WIN32)
  #define BWA_EXPORT __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
  #define BWA_EXPORT __attribute__((visibility("default")))
#else
  #define BWA_EXPORT
#endif

/* ---- threads ---------------------------------------------------------------------------- */

/* The engine's worker threads all ignore their return value (they end by returning from a stop
 * poll), so the shim's entry point returns void rather than carrying DWORD/void* through. */
typedef void (*os_thread_fn)(void*);

#if defined(_WIN32)
typedef struct { void* h; } os_thread;                  /* HANDLE, untyped to keep windows.h out */
#else
typedef struct { pthread_t h; int live; } os_thread;
#endif

int  os_thread_create(os_thread* t, os_thread_fn fn, void* arg);  /* 0 = ok, non-zero = failed */
void os_thread_join(os_thread* t);                 /* join + clear; a no-op on a cleared handle */
bool os_thread_valid(const os_thread* t);

/* Drop the CALLING thread below normal priority. The Steam Audio sim threads use it so a
 * ray-tracing burst can never preempt the audio callback. Best-effort: a platform that refuses
 * (a POSIX box with no permission to renice) simply runs the thread at normal priority. */
void os_thread_lower_priority(void);

/* ---- time ------------------------------------------------------------------------------- */

void     os_sleep_ms(unsigned ms);

/* The platform monotonic clock in nanoseconds: QPC on Windows, mach_absolute_time scaled by
 * mach_timebase_info on Apple, CLOCK_MONOTONIC elsewhere. Arbitrary epoch — only DIFFERENCES
 * mean anything, which is exactly what bwa_timestamp.system_time_ns promises. */
uint64_t os_monotonic_ns(void);

/* Sleep until an ABSOLUTE deadline on the os_monotonic_ns clock; returns at once when the deadline
 * has already passed. This is what a SELF-PACED render loop wants and os_sleep_ms is not: a
 * relative sleep is computed from a clock reading that is already stale by the time the kernel
 * sees it, so its error accumulates block after block.
 *
 * Precision comes from the platform primitive built for it - a high-resolution waitable timer on
 * Windows 10 1803 and later, clock_nanosleep(TIMER_ABSTIME) on Linux and Android, mach_wait_until
 * on Apple - so the caller does NOT have to raise the global timer resolution to get a
 * sub-millisecond wake. That matters because timeBeginPeriod is system-wide in effect: a
 * visual-only tool that happens to run the null sink was raising the whole machine's timer
 * resolution for as long as it ran. The pre-1803 fallback is the old coarse path (timeBeginPeriod
 * plus Sleep), and only that path still touches the global resolution.
 *
 * On the fallback the wake lands within the OS timer granularity of the deadline rather than on
 * it. Callers pace, they do not measure, so that is the same tolerance the loops always had. */
void os_sleep_until_ns(uint64_t deadline_ns);

/* Windows timer resolution (timeBeginPeriod/timeEndPeriod, link winmm): raise it around a loop
 * that paces itself with short sleeps, or Sleep(1) can take 15 ms. A no-op on every other
 * platform, where nanosleep already has the granularity. Calls must be paired.
 * Prefer os_sleep_until_ns, which needs none of this on a current Windows. */
void os_timer_resolution_begin(void);
void os_timer_resolution_end(void);

/* ---- mutex ------------------------------------------------------------------------------ */

/* RECURSIVE, matching the CRITICAL_SECTION these replace: the Steam scene lock is taken through
 * several layers and a plain mutex would deadlock where the old code merely re-entered.
 * Control-thread and sim-thread only — see the header comment about the audio thread. */
#if defined(_WIN32)
typedef struct { void* cs; } os_mutex;                  /* a heap CRITICAL_SECTION */
#else
typedef struct { pthread_mutex_t m; int live; } os_mutex;
#endif

int  os_mutex_init(os_mutex* m);        /* 0 = ok; allocates on Windows, so it CAN fail */
void os_mutex_destroy(os_mutex* m);
void os_mutex_lock(os_mutex* m);
void os_mutex_unlock(os_mutex* m);

/* ---- reader/writer lock ------------------------------------------------------------------ */

/* Not a convenience over os_mutex: steam_scene.c needs many concurrent READERS. Several sim
 * threads borrow one committed IPLScene for their ray traces while the scene owner takes the lock
 * exclusively around iplSceneCommit, and collapsing that to a plain mutex would serialize traces
 * that have no reason to wait on each other. NOT recursive, on either platform.
 * (SRWLOCK on Windows, pthread_rwlock_t elsewhere.) */
#if defined(_WIN32)
typedef struct { void* srw; } os_rwlock;   /* an SRWLOCK is exactly one pointer; see os_win.c */
#else
typedef struct { pthread_rwlock_t rw; int live; } os_rwlock;
#endif

int  os_rwlock_init(os_rwlock* l);      /* 0 = ok */
void os_rwlock_destroy(os_rwlock* l);
void os_rwlock_lock(os_rwlock* l);          /* exclusive (one writer) */
void os_rwlock_unlock(os_rwlock* l);
void os_rwlock_lock_shared(os_rwlock* l);   /* shared (many readers) */
void os_rwlock_unlock_shared(os_rwlock* l);

/* ---- strings ---------------------------------------------------------------------------- */

char* os_strdup(const char* s);              /* NULL in, NULL out; free() the result */
int   os_strcasecmp(const char* a, const char* b);

/* ---- UDP sockets (natnet.c) -------------------------------------------------------------- */

/* Wrapped WHOLE, not thinly: natnet.c only ever opens one UDP socket, points it at an IPv4
 * literal, and reads datagrams, so the shim takes address strings and keeps sockaddr_in,
 * ip_mreq and the Winsock-versus-BSD spelling entirely inside os_win.c / os_posix.c. */
#if defined(_WIN32)
typedef uintptr_t os_socket;                            /* SOCKET */
#define OS_INVALID_SOCKET ((os_socket)~(uintptr_t)0)    /* INVALID_SOCKET */
#else
typedef int os_socket;
#define OS_INVALID_SOCKET ((os_socket)-1)
#endif

/* os_udp_recv's two failure kinds, kept apart because the caller must treat them differently: a
 * receive timeout is the NORMAL way the loop re-polls its stop flag, and a hard error has to back
 * off instead of hot-spinning a core. */
#define OS_UDP_TIMEOUT (-1)
#define OS_UDP_ERROR   (-2)

int  os_net_startup(void);      /* WSAStartup on Windows; nothing to do elsewhere. 0 = ok */
void os_net_cleanup(void);

os_socket os_udp_open(void);                            /* OS_INVALID_SOCKET on failure */
void os_udp_close(os_socket s);
int  os_udp_set_rcvtimeo_ms(os_socket s, unsigned ms);  /* 0 = ok */
int  os_udp_set_reuseaddr(os_socket s);                 /* 0 = ok */
int  os_udp_bind_any(os_socket s, uint16_t port);       /* bind INADDR_ANY:port; 0 = ok */
/* Join an IPv4 multicast group. iface_ipv4 NULL/"" = INADDR_ANY. 0 = ok. */
int  os_udp_join_multicast(os_socket s, const char* group_ipv4, const char* iface_ipv4);
/* Send one datagram to an IPv4 literal. Returns bytes sent, or negative on failure. */
int  os_udp_sendto(os_socket s, const void* buf, size_t len, const char* ipv4, uint16_t port);
/* Receive one datagram (the sender address is discarded — natnet never inspects it). Returns the
 * byte count, OS_UDP_TIMEOUT when the receive timeout expired, or OS_UDP_ERROR. */
int  os_udp_recv(os_socket s, void* buf, size_t cap);

bool os_ipv4_valid(const char* s);   /* a numeric IPv4 literal, not a hostname */

#ifdef __cplusplus
}
#endif

#endif /* BWA_OS_H */
