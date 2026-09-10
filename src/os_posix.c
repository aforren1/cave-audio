/*
 * os_posix.c — the POSIX half of the os.h shim (Linux, macOS, Android). The Windows half is
 * os_win.c; exactly one of the two is compiled.
 *
 * Apple differs in two places only, both marked: the monotonic clock goes through
 * mach_absolute_time, and lowering a thread's priority has no per-thread nice.
 */
#include "os.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>       /* strcasecmp */
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>

#if defined(__APPLE__)
#include <mach/mach_time.h>
#else
#include <sys/resource.h>  /* setpriority: Linux applies PRIO_PROCESS/0 to the CALLING THREAD */
#endif

/* ---- threads ---- */

/* pthread_create's entry point returns void*, and the shim's returns void, so the pair (callback
 * plus its argument) rides in a small heap block the trampoline frees. One malloc per thread
 * START, on the control thread — never on any hot path. */
typedef struct { os_thread_fn fn; void* arg; } ThreadStart;

static void* os_thread_trampoline(void* p) {
    ThreadStart ts = *(ThreadStart*)p;
    free(p);
    ts.fn(ts.arg);
    return NULL;
}

int os_thread_create(os_thread* t, os_thread_fn fn, void* arg) {
    if (!t || !fn) return 1;
    t->live = 0;
    ThreadStart* ts = (ThreadStart*)malloc(sizeof *ts);
    if (!ts) return 1;
    ts->fn = fn; ts->arg = arg;
    if (pthread_create(&t->h, NULL, os_thread_trampoline, ts) != 0) { free(ts); return 1; }
    t->live = 1;
    return 0;
}

void os_thread_join(os_thread* t) {
    if (!t || !t->live) return;
    pthread_join(t->h, NULL);
    t->live = 0;
}

bool os_thread_valid(const os_thread* t) { return t && t->live != 0; }

void os_thread_lower_priority(void) {
#if defined(__APPLE__)
    /* No per-thread nice on Darwin. Stay in SCHED_OTHER and take the bottom of its band; a
     * failure is fine, the thread just runs at the default priority. */
    struct sched_param sp;
    int policy = SCHED_OTHER;
    if (pthread_getschedparam(pthread_self(), &policy, &sp) == 0) {
        const int lo = sched_get_priority_min(policy);
        if (sp.sched_priority > lo) { sp.sched_priority = lo; pthread_setschedparam(pthread_self(), policy, &sp); }
    }
#else
    /* Linux nice is per-thread, and PRIO_PROCESS with a 0 "who" means the calling thread. +5 is
     * the same intent as THREAD_PRIORITY_BELOW_NORMAL: yield to the audio thread, keep running. */
    (void)setpriority(PRIO_PROCESS, 0, 5);
#endif
}

/* ---- time ---- */

void os_sleep_ms(unsigned ms) {
    struct timespec ts;
    ts.tv_sec  = (time_t)(ms / 1000u);
    ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) { }   /* finish the remainder after a signal */
}

uint64_t os_monotonic_ns(void) {
#if defined(__APPLE__)
    /* mach_absolute_time counts in a unit the timebase names; on Apple silicon that is 125/3 ns
     * per tick, so the conversion is not optional. Split the scale (ticks * numer can overflow on
     * a long uptime) the same way the QPC path does. UNTESTED on a Mac; see docs/backends.md. */
    static mach_timebase_info_data_t tb;
    if (tb.denom == 0) mach_timebase_info(&tb);
    const uint64_t t = mach_absolute_time();
    return (t / tb.denom) * tb.numer + (t % tb.denom) * tb.numer / tb.denom;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

void os_sleep_until_ns(uint64_t deadline_ns) {
#if defined(__APPLE__)
    /* mach_wait_until takes mach ticks, so the deadline converts back through the same timebase
     * os_monotonic_ns used. A signal can cut the wait short, so re-arm on what is left.
     * UNTESTED on a Mac; see docs/backends.md. */
    static mach_timebase_info_data_t tb;
    if (tb.denom == 0) mach_timebase_info(&tb);
    for (int tries = 0; tries < 64; ++tries) {
        if (os_monotonic_ns() >= deadline_ns) return;
        const uint64_t ticks = (deadline_ns / tb.numer) * tb.denom
                             + (deadline_ns % tb.numer) * tb.denom / tb.numer;
        if (mach_wait_until(ticks) == KERN_SUCCESS) return;
    }
#else
    /* An ABSOLUTE deadline on the same clock os_monotonic_ns reads, so the kernel does the
     * comparison and a caller that was descheduled while computing an interval cannot oversleep
     * by that much. clock_nanosleep RETURNS the error rather than setting errno. */
    struct timespec ts;
    ts.tv_sec  = (time_t)(deadline_ns / 1000000000ull);
    ts.tv_nsec = (long)(deadline_ns % 1000000000ull);
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) { }
#endif
}

/* Windows-only: nanosleep already has the granularity timeBeginPeriod buys there. */
void os_timer_resolution_begin(void) { }
void os_timer_resolution_end(void)   { }

/* ---- mutex ---- */

int os_mutex_init(os_mutex* m) {
    if (!m) return 1;
    m->live = 0;
    pthread_mutexattr_t at;
    if (pthread_mutexattr_init(&at) != 0) return 1;
    /* RECURSIVE to match the CRITICAL_SECTION this replaces — see os.h. */
    pthread_mutexattr_settype(&at, PTHREAD_MUTEX_RECURSIVE);
    const int rc = pthread_mutex_init(&m->m, &at);
    pthread_mutexattr_destroy(&at);
    if (rc != 0) return 1;
    m->live = 1;
    return 0;
}

void os_mutex_destroy(os_mutex* m) {
    if (!m || !m->live) return;
    pthread_mutex_destroy(&m->m);
    m->live = 0;
}

void os_mutex_lock(os_mutex* m)   { if (m && m->live) pthread_mutex_lock(&m->m); }
void os_mutex_unlock(os_mutex* m) { if (m && m->live) pthread_mutex_unlock(&m->m); }

/* ---- reader/writer lock ---- */

int os_rwlock_init(os_rwlock* l) {
    if (!l) return 1;
    l->live = 0;
    if (pthread_rwlock_init(&l->rw, NULL) != 0) return 1;
    l->live = 1;
    return 0;
}
void os_rwlock_destroy(os_rwlock* l) { if (l && l->live) { pthread_rwlock_destroy(&l->rw); l->live = 0; } }
void os_rwlock_lock(os_rwlock* l)          { if (l && l->live) pthread_rwlock_wrlock(&l->rw); }
void os_rwlock_unlock(os_rwlock* l)        { if (l && l->live) pthread_rwlock_unlock(&l->rw); }
void os_rwlock_lock_shared(os_rwlock* l)   { if (l && l->live) pthread_rwlock_rdlock(&l->rw); }
void os_rwlock_unlock_shared(os_rwlock* l) { if (l && l->live) pthread_rwlock_unlock(&l->rw); }

/* ---- strings ---- */

char* os_strdup(const char* s) {
    if (!s) return NULL;
    const size_t n = strlen(s) + 1;
    char* p = (char*)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

int os_strcasecmp(const char* a, const char* b) { return strcasecmp(a, b); }

/* ---- UDP sockets ---- */

int  os_net_startup(void) { return 0; }     /* BSD sockets need no startup */
void os_net_cleanup(void) { }

os_socket os_udp_open(void) {
    const int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    return (s < 0) ? OS_INVALID_SOCKET : (os_socket)s;
}

void os_udp_close(os_socket s) { if (s != OS_INVALID_SOCKET) close(s); }

int os_udp_set_rcvtimeo_ms(os_socket s, unsigned ms) {
    struct timeval tv;                        /* BSD takes a timeval; Winsock takes a DWORD of ms */
    tv.tv_sec  = (time_t)(ms / 1000u);
    tv.tv_usec = (suseconds_t)((ms % 1000u) * 1000u);
    return setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) == 0 ? 0 : 1;
}

int os_udp_set_reuseaddr(os_socket s) {
    int on = 1;
    return setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on) == 0 ? 0 : 1;
}

int os_udp_bind_any(os_socket s, uint16_t port) {
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port   = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    return bind(s, (struct sockaddr*)&a, sizeof a) == 0 ? 0 : 1;
}

int os_udp_join_multicast(os_socket s, const char* group_ipv4, const char* iface_ipv4) {
    struct ip_mreq mreq; memset(&mreq, 0, sizeof mreq);
    if (!group_ipv4 || inet_pton(AF_INET, group_ipv4, &mreq.imr_multiaddr) != 1) return 1;
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    if (iface_ipv4 && iface_ipv4[0]) inet_pton(AF_INET, iface_ipv4, &mreq.imr_interface);
    return setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof mreq) == 0 ? 0 : 1;
}

int os_udp_sendto(os_socket s, const void* buf, size_t len, const char* ipv4, uint16_t port) {
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port   = htons(port);
    if (!ipv4 || inet_pton(AF_INET, ipv4, &a.sin_addr) != 1) return -1;
    const ssize_t n = sendto(s, buf, len, 0, (struct sockaddr*)&a, sizeof a);
    return (int)n;
}

int os_udp_recv(os_socket s, void* buf, size_t cap) {
    const ssize_t got = recvfrom(s, buf, cap, 0, NULL, NULL);
    if (got >= 0) return (int)got;
    return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? OS_UDP_TIMEOUT : OS_UDP_ERROR;
}

bool os_ipv4_valid(const char* s) {
    struct in_addr a;
    return s && inet_pton(AF_INET, s, &a) == 1;
}
