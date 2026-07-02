/* asio_session.cpp — see asio_session.h. */
#include "asio_session.h"

#include <atomic>
#include <cstdio>

static std::atomic<const char*> g_owner{ nullptr };

int asio_session_acquire(const char* who) {
    const char* expected = nullptr;
    if (!g_owner.compare_exchange_strong(expected, who)) {
        fprintf(stderr, "asio: the driver slot is held by %s — close it first (one ASIO device at a time)\n",
                expected ? expected : "?");
        return 0;
    }
    return 1;
}

void asio_session_release(void) { g_owner.store(nullptr); }
