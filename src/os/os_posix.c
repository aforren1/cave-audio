/*
 * os_posix.c — the POSIX half of the os.h shim (Linux, macOS, Android). The Windows half is
 * os_win.c; exactly one of the two is compiled.
 *
 * Apple differs in two places only, both marked: the monotonic clock goes through
 * mach_absolute_time, and lowering a thread's priority has no per-thread nice.
 */
#include "os/os.h"

#include <dlfcn.h>      /* os_dl_*: the two Linux device backends load their libraries at run time */
#include <errno.h>
#include <sched.h>       /* SCHED_FIFO + the priority range os_thread_set_realtime asks for */
#include <stdlib.h>
#include <string.h>
#include <strings.h>       /* strcasecmp */
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>     /* mkdir: the UTF-8 path family is plain POSIX here */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>

#if defined(__APPLE__)
#include <mach/mach_init.h>     /* mach_thread_self / mach_task_self */
#include <mach/mach_port.h>     /* mach_port_deallocate: mach_thread_self hands out a send right */
#include <mach/mach_time.h>
#include <mach/thread_act.h>    /* thread_policy_set */
#include <mach/thread_policy.h> /* THREAD_TIME_CONSTRAINT_POLICY */
#else
#include <sys/resource.h>  /* setpriority: Linux applies PRIO_PROCESS/0 to the CALLING THREAD.
                            * Also getrlimit(RLIMIT_RTPRIO), the SCHED_FIFO permission probe. */
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

#if defined(__APPLE__)
/* THREAD_TIME_CONSTRAINT_POLICY, which is what CoreAudio gives its own IO thread, and on Darwin
 * this is not an optimization: the kernel COALESCES timers for ordinary threads, so a correct
 * mach_wait_until on a plain thread still lands milliseconds late. CI on an Apple Silicon runner
 * measured p50 2.7 ms and p99 9.8 ms against a 2 ms deadline, and os_sleep_ms(50) taking 100 ms,
 * with the timebase conversions verified correct in both directions. A time-constraint thread is
 * exempt from that coalescing.
 *
 * The triple is the standard shape: `period` is the render period, `computation` the CPU time the
 * thread claims inside it, `constraint` the deadline it must finish by. A quarter and a half of
 * the period are the conventional values, and preemptible stays 1 because the render loop sleeps
 * rather than spins. */
static uint32_t mach_ticks_of_ns(uint64_t ns) {
    static mach_timebase_info_data_t tb;
    if (tb.denom == 0) mach_timebase_info(&tb);
    const uint64_t t = (ns / tb.numer) * tb.denom + (ns % tb.numer) * tb.denom / tb.numer;
    return (t > 0xFFFFFFFFull) ? 0xFFFFFFFFu : (uint32_t)t;
}

int os_thread_set_realtime(uint64_t period_ns) {
    if (period_ns == 0) period_ns = 5000000ull;     /* a nominal 5 ms if the caller has no period */
    thread_time_constraint_policy_data_t pol;
    pol.period      = mach_ticks_of_ns(period_ns);
    pol.computation = mach_ticks_of_ns(period_ns / 4u);
    pol.constraint  = mach_ticks_of_ns(period_ns / 2u);
    pol.preemptible = 1;
    const mach_port_t th = mach_thread_self();      /* a send right: it has to be given back */
    const kern_return_t kr = thread_policy_set(th, THREAD_TIME_CONSTRAINT_POLICY,
                                               (thread_policy_t)&pol,
                                               THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    mach_port_deallocate(mach_task_self(), th);
    return (kr == KERN_SUCCESS) ? 0 : (int)kr;
}

void os_thread_clear_realtime(void) {
    thread_standard_policy_data_t pol;
    memset(&pol, 0, sizeof pol);
    const mach_port_t th = mach_thread_self();
    thread_policy_set(th, THREAD_STANDARD_POLICY, (thread_policy_t)&pol,
                      THREAD_STANDARD_POLICY_COUNT);
    mach_port_deallocate(mach_task_self(), th);
}
#else
/* SCHED_FIFO, not a nice value: a blocking-write render loop misses its deadline the moment a
 * desktop task outranks it, and SCHED_OTHER has no way to say "always before that". min + 10 is
 * rtprio 11 on Linux - above every ordinary task, and deliberately well under jackd's own thread
 * (70 by default), which must always outrank a client. The policy is not deadline-based, so
 * `period_ns` has nothing to say here. pthread_setschedparam RETURNS the errno rather than
 * setting it. */
int os_thread_set_realtime(uint64_t period_ns) {
    (void)period_ns;
    struct sched_param sp;
    memset(&sp, 0, sizeof sp);
    const int lo = sched_get_priority_min(SCHED_FIFO);
    const int hi = sched_get_priority_max(SCHED_FIFO);
    int prio = lo + 10;
    if (prio > hi) prio = hi;
    sp.sched_priority = prio;
    return pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
}

void os_thread_clear_realtime(void) {
    struct sched_param sp;
    memset(&sp, 0, sizeof sp);
    (void)pthread_setschedparam(pthread_self(), SCHED_OTHER, &sp);
}
#endif

bool os_thread_realtime_available(void) {
#if defined(__linux__)
    /* Exactly what the kernel checks: an unprivileged SCHED_FIFO request is granted while the
     * requested priority fits inside RLIMIT_RTPRIO (the `audio` group's limits.d drop, or a
     * systemd LimitRTPRIO). Root bypasses the limit, and so does CAP_SYS_NICE - which cannot be
     * read from here, so a capability-only setup reads as "no" and merely produces a warning the
     * successful attempt then contradicts. */
    if (geteuid() == 0) return true;
    struct rlimit rl;
    if (getrlimit(RLIMIT_RTPRIO, &rl) != 0) return true;   /* cannot tell: let the attempt decide */
    return rl.rlim_cur > 0;
#else
    /* Apple's time-constraint policy needs no privilege, so the attempt is the only judge. */
    return true;
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

/* ---- events ---- */

/* A mutex + condvar + FLAG. The flag is not a detail: it is what makes a signal that arrives before
 * anyone waits survive, which a bare pthread_cond_signal does not. The timed wait is on
 * CLOCK_MONOTONIC (pthread_condattr_setclock), so a wall-clock step cannot stretch or cut a wait;
 * Darwin has no condattr_setclock, and uses the relative_np wait instead for the same reason. */
int os_event_init(os_event* e) {
    if (!e) return 1;
    e->flag = 0; e->live = 0;
    if (pthread_mutex_init(&e->m, NULL) != 0) return 1;
#if defined(__APPLE__)
    if (pthread_cond_init(&e->c, NULL) != 0) { pthread_mutex_destroy(&e->m); return 1; }
#else
    pthread_condattr_t at;
    if (pthread_condattr_init(&at) != 0) { pthread_mutex_destroy(&e->m); return 1; }
    pthread_condattr_setclock(&at, CLOCK_MONOTONIC);
    const int rc = pthread_cond_init(&e->c, &at);
    pthread_condattr_destroy(&at);
    if (rc != 0) { pthread_mutex_destroy(&e->m); return 1; }
#endif
    e->live = 1;
    return 0;
}

void os_event_destroy(os_event* e) {
    if (!e || !e->live) return;
    pthread_cond_destroy(&e->c);
    pthread_mutex_destroy(&e->m);
    e->live = 0;
}

void os_event_signal(os_event* e) {
    if (!e || !e->live) return;
    pthread_mutex_lock(&e->m);
    e->flag = 1;
    pthread_cond_signal(&e->c);
    pthread_mutex_unlock(&e->m);
}

bool os_event_wait(os_event* e, int timeout_ms) {
    if (!e || !e->live) return false;
    bool got = false;
    pthread_mutex_lock(&e->m);
    if (timeout_ms < 0) {
        while (!e->flag) if (pthread_cond_wait(&e->c, &e->m) != 0) break;
    } else {
        /* Loop to the DEADLINE rather than waiting once: a spurious wakeup must not be reported as
         * a timeout that has not happened yet, because the stream thread derives its refill cadence
         * from this return. os_monotonic_ns is CLOCK_MONOTONIC here, the same clock the condvar was
         * given, so the absolute timespec below is directly comparable. */
        const uint64_t deadline = os_monotonic_ns() + (uint64_t)timeout_ms * 1000000ull;
        while (!e->flag) {
            const uint64_t now = os_monotonic_ns();
            if (now >= deadline) break;
#if defined(__APPLE__)
            const uint64_t left = deadline - now;
            struct timespec rel;
            rel.tv_sec  = (time_t)(left / 1000000000ull);
            rel.tv_nsec = (long)  (left % 1000000000ull);
            const int rc = pthread_cond_timedwait_relative_np(&e->c, &e->m, &rel);
#else
            struct timespec ts;
            ts.tv_sec  = (time_t)(deadline / 1000000000ull);
            ts.tv_nsec = (long)  (deadline % 1000000000ull);
            const int rc = pthread_cond_timedwait(&e->c, &e->m, &ts);
#endif
            if (rc != 0 && rc != ETIMEDOUT) break;      /* a broken condvar must not spin a core */
        }
    }
    if (e->flag) { e->flag = 0; got = true; }
    pthread_mutex_unlock(&e->m);
    return got;
}

/* ---- files ---- */

/* Nothing to convert: a POSIX path is bytes, and the ABI's bytes are already UTF-8. The whole
 * family exists for the Windows half - see os.h. */
FILE* os_fopen(const char* utf8_path, const char* mode) {
    return (utf8_path && mode) ? fopen(utf8_path, mode) : NULL;
}

int os_mkdir(const char* utf8_path) {
    if (!utf8_path) return 1;
    if (mkdir(utf8_path, 0777) == 0) return 0;
    return (errno == EEXIST) ? 0 : 1;
}

int os_remove(const char* utf8_path) { return (utf8_path && remove(utf8_path) == 0) ? 0 : 1; }
int os_rmdir (const char* utf8_path) { return (utf8_path && rmdir (utf8_path) == 0) ? 0 : 1; }

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

/* ---- dynamic libraries ---- */

/* RTLD_NOW so a library missing one of the symbols a backend needs fails HERE rather than on the
 * first call into it, and RTLD_LOCAL so nothing it drags in joins the global namespace. */
os_dl os_dl_open(const char* name) {
    if (!name || !*name) return NULL;
    return dlopen(name, RTLD_NOW | RTLD_LOCAL);
}

void* os_dl_sym(os_dl h, const char* symbol) {
    if (!h || !symbol) return NULL;
    return dlsym(h, symbol);
}

void os_dl_close(os_dl h) { if (h) dlclose(h); }

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
