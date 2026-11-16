// SPDX-License-Identifier: GPL-3.0-or-later
#include "watchdog.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include <fcntl.h>
#include <unistd.h>

#include "mono.h"

namespace aa {

namespace {

// Monotonic milliseconds of the last tick (wraps after 49 days; differences stay right), and
// whether the loop is serving (false while it waits for a connection).
std::atomic<std::uint32_t> g_tick_ms(0);
std::atomic<bool> g_armed(false);
std::atomic<const char *> g_stage("start");

std::uint32_t now_ms() { return static_cast<std::uint32_t>(mono_us() / 1000u); }

unsigned timeout_s() {
    const char *v = std::getenv("AA_WATCHDOG_S");
    if (!v || !*v) return 10;
    const long s = std::strtol(v, nullptr, 10);
    return s > 0 ? static_cast<unsigned>(s) : 0;
}

// Leaves at once, whatever the other threads hold: the message goes out with a plain write on
// a non-blocking stderr (not stdio, whose lock the stuck thread may hold), then _exit.
void bail(const char *stage, unsigned stuck_s) {
    char msg[200];
    const int n = std::snprintf(msg, sizeof(msg),
                                "[aa-bridge] watchdog: the main loop has been stuck in %s for %u s; "
                                "exiting for procguard to restart the bridge\n", stage, stuck_s);
    const int fl = fcntl(2, F_GETFL);
    if (fl >= 0) fcntl(2, F_SETFL, fl | O_NONBLOCK);
    if (n > 0) {
        ssize_t w = write(2, msg, static_cast<std::size_t>(n) < sizeof(msg) ? static_cast<std::size_t>(n) : sizeof(msg) - 1);
        (void) w;
    }
    _exit(0);
}

} // namespace

void watchdog_start() {
    static std::atomic<bool> started(false);
    const unsigned limit = timeout_s();
    if (limit == 0 || started.exchange(true)) {
        if (limit == 0) std::fprintf(stderr, "[aa-bridge] watchdog off (AA_WATCHDOG_S=0)\n");
        return;
    }
    std::thread([limit] {
        for (;;) {
            usleep(250 * 1000);
            if (!g_armed.load()) continue;
            const std::uint32_t stuck_ms = now_ms() - g_tick_ms.load();
            if (stuck_ms >= limit * 1000u) bail(g_stage.load(), stuck_ms / 1000u);
        }
    }).detach();
}

void watchdog_tick(const char *stage) {
    g_stage.store(stage);
    g_tick_ms.store(now_ms());
    g_armed.store(true);
}

void watchdog_idle() {
    g_armed.store(false);
    g_stage.store("idle");
}

} // namespace aa
