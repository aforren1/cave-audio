/*
 * os_win.c — the Windows half of the os.h shim. The POSIX half is os_posix.c; exactly one of the
 * two is compiled. Nothing here is new behavior: each function is the Win32 call the engine used
 * to make inline, moved behind the seam so the same source builds elsewhere.
 */
#include "os.h"

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>      /* MUST precede windows.h, or windows.h drags in the old winsock.h */
#include <ws2tcpip.h>
#include <windows.h>
#include <timeapi.h>       /* timeBeginPeriod / timeEndPeriod (link winmm) */
#include <avrt.h>          /* AvSetMmThreadCharacteristics: the "Pro Audio" MMCSS task (link avrt) */

#include <stdlib.h>
#include <string.h>

/* ---- threads ---- */

/* CreateThread takes one LPVOID, and the shim's entry point needs two values (the callback and
 * its argument), so the pair rides in a small heap block the trampoline frees. One malloc per
 * thread START, on the control thread — never on any hot path. */
typedef struct { os_thread_fn fn; void* arg; } ThreadStart;

static DWORD WINAPI os_thread_trampoline(LPVOID p) {
    ThreadStart ts = *(ThreadStart*)p;
    free(p);
    ts.fn(ts.arg);
    return 0;
}

int os_thread_create(os_thread* t, os_thread_fn fn, void* arg) {
    if (!t || !fn) return 1;
    t->h = NULL;
    ThreadStart* ts = (ThreadStart*)malloc(sizeof *ts);
    if (!ts) return 1;
    ts->fn = fn; ts->arg = arg;
    HANDLE h = CreateThread(NULL, 0, os_thread_trampoline, ts, 0, NULL);
    if (!h) { free(ts); return 1; }
    t->h = (void*)h;
    return 0;
}

void os_thread_join(os_thread* t) {
    if (!t || !t->h) return;
    WaitForSingleObject((HANDLE)t->h, INFINITE);
    CloseHandle((HANDLE)t->h);
    t->h = NULL;
}

bool os_thread_valid(const os_thread* t) { return t && t->h != NULL; }

void os_thread_lower_priority(void) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
}

/* MMCSS, not a raw priority. "Pro Audio" is the task class the OS reserves for audio work: it
 * lifts the thread above the normal scheduler classes for its share of each period, which is what
 * an audio thread is supposed to ask for and what the WASAPI sink asked for inline before this
 * moved behind the seam. THREAD_PRIORITY_TIME_CRITICAL is the fallback for a system where MMCSS
 * refuses (the service is disabled), because a self-paced render loop still needs to outrank
 * ordinary work. `period_ns` has no place in either: MMCSS takes a task name.
 *
 * Thread-local, because the handle belongs to the thread that joined the task and only that thread
 * may revert it. */
static __declspec(thread) HANDLE  g_mmcss      = NULL;
static __declspec(thread) int     g_prio_raised = 0;

int os_thread_set_realtime(uint64_t period_ns) {
    (void)period_ns;
    if (!g_mmcss) {
        DWORD index = 0;
        g_mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &index);
    }
    if (g_mmcss) return 0;
    if (SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL)) {
        g_prio_raised = 1;
        return 0;
    }
    return (int)GetLastError();
}

void os_thread_clear_realtime(void) {
    if (g_mmcss) { AvRevertMmThreadCharacteristics(g_mmcss); g_mmcss = NULL; }
    if (g_prio_raised) {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
        g_prio_raised = 0;
    }
}

bool os_thread_realtime_available(void) { return true; }

/* ---- time ---- */

void os_sleep_ms(unsigned ms) { Sleep((DWORD)ms); }

uint64_t os_monotonic_ns(void) {
    static uint64_t freq = 0;
    if (!freq) { LARGE_INTEGER f; freq = QueryPerformanceFrequency(&f) ? (uint64_t)f.QuadPart : 1; }
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    const uint64_t t = (uint64_t)c.QuadPart;
    /* Split the scale: t * 1e9 wraps uint64 after only ~30 min at a 10 MHz QPC. This form stays
     * exact for centuries of uptime. */
    return (t / freq) * 1000000000ull + (t % freq) * 1000000000ull / freq;
}

void os_timer_resolution_begin(void) { timeBeginPeriod(1); }
void os_timer_resolution_end(void)   { timeEndPeriod(1); }

/* Windows 10 1803 added the flag; older SDK headers do not define it. On an older OS the create
 * call fails with ERROR_INVALID_PARAMETER, which is what selects the fallback below. */
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

/* One timer per THREAD, in fiber-local storage. Fiber-local rather than __declspec(thread) for one
 * reason: FLS runs a destructor at thread exit, so a sink started and stopped a thousand times
 * closes a thousand handles instead of leaking them. OS_FLS_NO_TIMER caches the pre-1803 failure
 * so the create is not retried on every block. */
#define OS_FLS_NO_TIMER ((void*)(uintptr_t)1)

static DWORD     g_timer_fls  = FLS_OUT_OF_INDEXES;
static INIT_ONCE g_timer_once = INIT_ONCE_STATIC_INIT;

static void WINAPI timer_fls_free(void* p) {
    if (p && p != OS_FLS_NO_TIMER) CloseHandle((HANDLE)p);
}
static BOOL CALLBACK timer_fls_init(PINIT_ONCE once, PVOID param, PVOID* ctx) {
    (void)once; (void)param; (void)ctx;
    g_timer_fls = FlsAlloc(timer_fls_free);
    return TRUE;
}

/* NULL = this thread has no high-resolution timer (pre-1803, or FLS exhausted): use the fallback. */
static HANDLE hires_timer(void) {
    InitOnceExecuteOnce(&g_timer_once, timer_fls_init, NULL, NULL);
    if (g_timer_fls == FLS_OUT_OF_INDEXES) return NULL;
    void* v = FlsGetValue(g_timer_fls);
    if (v == OS_FLS_NO_TIMER) return NULL;
    if (v) return (HANDLE)v;
    HANDLE h = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                      TIMER_ALL_ACCESS);
    FlsSetValue(g_timer_fls, h ? (void*)h : OS_FLS_NO_TIMER);
    return h;
}

void os_sleep_until_ns(uint64_t deadline_ns) {
    const uint64_t now = os_monotonic_ns();
    if (now >= deadline_ns) return;
    const uint64_t remain_ns = deadline_ns - now;

    HANDLE t = hires_timer();
    if (t) {
        LARGE_INTEGER due;
        /* Negative = RELATIVE, in 100 ns units. Relative from a reading taken microseconds ago is
         * exact enough at this resolution, and it avoids converting the monotonic clock into the
         * FILETIME epoch an absolute due time would need. */
        due.QuadPart = -(LONGLONG)(remain_ns / 100ull);
        if (due.QuadPart == 0) due.QuadPart = -1;
        if (SetWaitableTimerEx(t, &due, 0, NULL, NULL, NULL, 0)) {
            WaitForSingleObject(t, INFINITE);
            return;
        }
    }

    /* Pre-1803 fallback: the coarse path, and the ONLY place the shim still moves the global timer
     * resolution. Sleep to within the OS granularity of the deadline - see the tolerance note in
     * os.h - rather than burning the last millisecond in a spin. */
    os_timer_resolution_begin();
    for (;;) {
        const uint64_t n = os_monotonic_ns();
        if (n + 300000ull >= deadline_ns) break;          /* within 0.3 ms: close enough */
        const uint64_t left_ms = (deadline_ns - n) / 1000000ull;
        Sleep(left_ms > 1 ? (DWORD)(left_ms - 1) : 0);
    }
    os_timer_resolution_end();
}

/* ---- mutex ---- */

/* A heap CRITICAL_SECTION rather than an inline one, because os.h cannot name the type without
 * pulling windows.h into every C++ translation unit that reaches sink.h. Recursive, which is what
 * CRITICAL_SECTION always was — see the note in os.h. */
int os_mutex_init(os_mutex* m) {
    if (!m) return 1;
    m->cs = NULL;
    CRITICAL_SECTION* cs = (CRITICAL_SECTION*)malloc(sizeof *cs);
    if (!cs) return 1;
    InitializeCriticalSection(cs);
    m->cs = (void*)cs;
    return 0;
}

void os_mutex_destroy(os_mutex* m) {
    if (!m || !m->cs) return;
    DeleteCriticalSection((CRITICAL_SECTION*)m->cs);
    free(m->cs);
    m->cs = NULL;
}

void os_mutex_lock(os_mutex* m)   { if (m && m->cs) EnterCriticalSection((CRITICAL_SECTION*)m->cs); }
void os_mutex_unlock(os_mutex* m) { if (m && m->cs) LeaveCriticalSection((CRITICAL_SECTION*)m->cs); }

/* ---- reader/writer lock ---- */

/* An SRWLOCK is a single pointer (RTL_SRWLOCK { PVOID Ptr; }), so it fits os_rwlock's one void*
 * exactly and needs no heap block the way the CRITICAL_SECTION above does. The static assert is
 * what makes that an assumption the compiler checks rather than one a reader has to trust. */
_Static_assert(sizeof(SRWLOCK) == sizeof(void*), "os_rwlock assumes SRWLOCK is one pointer wide");
static SRWLOCK* srw_of(os_rwlock* l) { return (SRWLOCK*)(void*)&l->srw; }

int  os_rwlock_init(os_rwlock* l)  { if (!l) return 1; InitializeSRWLock(srw_of(l)); return 0; }
void os_rwlock_destroy(os_rwlock* l) { (void)l; }        /* an SRWLOCK owns no resources */
void os_rwlock_lock(os_rwlock* l)          { if (l) AcquireSRWLockExclusive(srw_of(l)); }
void os_rwlock_unlock(os_rwlock* l)        { if (l) ReleaseSRWLockExclusive(srw_of(l)); }
void os_rwlock_lock_shared(os_rwlock* l)   { if (l) AcquireSRWLockShared(srw_of(l)); }
void os_rwlock_unlock_shared(os_rwlock* l) { if (l) ReleaseSRWLockShared(srw_of(l)); }

/* ---- strings ---- */

char* os_strdup(const char* s) { return s ? _strdup(s) : NULL; }
int   os_strcasecmp(const char* a, const char* b) { return _stricmp(a, b); }

/* ---- UDP sockets ---- */

int os_net_startup(void) {
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0 ? 0 : 1;
}

void os_net_cleanup(void) { WSACleanup(); }

os_socket os_udp_open(void) {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    return (s == INVALID_SOCKET) ? OS_INVALID_SOCKET : (os_socket)s;
}

void os_udp_close(os_socket s) { if (s != OS_INVALID_SOCKET) closesocket((SOCKET)s); }

int os_udp_set_rcvtimeo_ms(os_socket s, unsigned ms) {
    DWORD tmo = (DWORD)ms;                    /* Winsock takes milliseconds; BSD takes a timeval */
    return setsockopt((SOCKET)s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof tmo) == 0 ? 0 : 1;
}

int os_udp_set_reuseaddr(os_socket s) {
    BOOL on = TRUE;
    return setsockopt((SOCKET)s, SOL_SOCKET, SO_REUSEADDR, (const char*)&on, sizeof on) == 0 ? 0 : 1;
}

int os_udp_bind_any(os_socket s, uint16_t port) {
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port   = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    return bind((SOCKET)s, (struct sockaddr*)&a, sizeof a) == 0 ? 0 : 1;
}

int os_udp_join_multicast(os_socket s, const char* group_ipv4, const char* iface_ipv4) {
    struct ip_mreq mreq; memset(&mreq, 0, sizeof mreq);
    if (!group_ipv4 || inet_pton(AF_INET, group_ipv4, &mreq.imr_multiaddr) != 1) return 1;
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    if (iface_ipv4 && iface_ipv4[0]) inet_pton(AF_INET, iface_ipv4, &mreq.imr_interface);
    return setsockopt((SOCKET)s, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                      (const char*)&mreq, sizeof mreq) == 0 ? 0 : 1;
}

int os_udp_sendto(os_socket s, const void* buf, size_t len, const char* ipv4, uint16_t port) {
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port   = htons(port);
    if (!ipv4 || inet_pton(AF_INET, ipv4, &a.sin_addr) != 1) return -1;
    return sendto((SOCKET)s, (const char*)buf, (int)len, 0, (struct sockaddr*)&a, sizeof a);
}

int os_udp_recv(os_socket s, void* buf, size_t cap) {
    int got = recvfrom((SOCKET)s, (char*)buf, (int)cap, 0, NULL, NULL);
    if (got != SOCKET_ERROR) return got;
    const int e = WSAGetLastError();
    return (e == WSAETIMEDOUT || e == WSAEWOULDBLOCK) ? OS_UDP_TIMEOUT : OS_UDP_ERROR;
}

bool os_ipv4_valid(const char* s) {
    struct in_addr a;
    return s && inet_pton(AF_INET, s, &a) == 1;
}
