// SPDX-License-Identifier: GPL-3.0-or-later
//
// Monotonic time and a condition variable that waits on it.
//
// The unit's toolchain (GCC 6.5, glibc 2.12 headers) builds std::condition_variable on
// system_clock: wait_for() ends at a wall-clock deadline, so a clock step during a wait ends it
// at once (forward) or stretches it by the step (backward). MonoCond waits with
// pthread_cond_timedwait on a CLOCK_MONOTONIC condition (pthread_condattr_setclock, glibc 2.3.3),
// with the std::mutex the caller already holds (a pthread mutex underneath).
#ifndef AA_BRIDGE_MONO_H
#define AA_BRIDGE_MONO_H

#include <cerrno>
#include <cstdint>
#include <ctime>
#include <mutex>

#include <pthread.h>

namespace aa {

inline std::uint64_t mono_us() {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return std::uint64_t(t.tv_sec) * 1000000u + std::uint64_t(t.tv_nsec) / 1000u;
}

class MonoCond {
public:
    MonoCond() {
        pthread_condattr_t a;
        pthread_condattr_init(&a);
        pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
        pthread_cond_init(&c_, &a);
        pthread_condattr_destroy(&a);
    }
    ~MonoCond() { pthread_cond_destroy(&c_); }
    MonoCond(const MonoCond &) = delete;
    MonoCond &operator=(const MonoCond &) = delete;

    void notify_one() { pthread_cond_signal(&c_); }
    void notify_all() { pthread_cond_broadcast(&c_); }

    // Waits until pred() holds or `us` microseconds have passed; returns pred().
    template <typename Pred>
    bool wait_for_us(std::unique_lock<std::mutex> &lk, std::uint64_t us, Pred pred) {
        timespec end;
        clock_gettime(CLOCK_MONOTONIC, &end);
        end.tv_sec += static_cast<time_t>(us / 1000000u);
        end.tv_nsec += static_cast<long>(us % 1000000u) * 1000;
        if (end.tv_nsec >= 1000000000L) {
            end.tv_sec += 1;
            end.tv_nsec -= 1000000000L;
        }
        while (!pred()) {
            if (pthread_cond_timedwait(&c_, lk.mutex()->native_handle(), &end) == ETIMEDOUT) {
                return pred();
            }
        }
        return true;
    }

private:
    pthread_cond_t c_;
};

} // namespace aa

#endif // AA_BRIDGE_MONO_H
