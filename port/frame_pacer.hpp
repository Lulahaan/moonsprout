// Present limiter and frame-slot sleeper for the host loops (fps_design 5.1). Header-only so the
// pacer self-test (tests/frame_pacer_test.cpp) builds it without the game.
#pragma once

#include <SDL3/SDL_timer.h>

#include <chrono>
#include <cstdint>

class FramePacer {
public:
    using clock = std::chrono::steady_clock;

    // Whether a present is due now at `interval` spacing. Never sleeps. An absolute schedule
    // (5 of 6 ticks at a 50 cap) with a quarter-interval tolerance, so a 59.94 Hz tick stream
    // keeps every tick at a 60 cap.
    bool due(clock::time_point now, std::chrono::nanoseconds interval) {
        if (interval.count() <= 0) {
            return true;
        }
        if (now + interval / 4 < mNext) {
            return false;
        }
        mNext += interval;
        if (mNext < now) {
            mNext = now + interval; // more than one interval behind: restart the schedule from now
        }
        return true;
    }

    // Sleeps until the next slot of `interval`, but not past `limit`. At interval 0 it yields
    // 250 us so lower-priority threads get time. The caller brackets it with
    // p2_host_block_begin / p2_host_block_end where game threads exist.
    void wait(std::chrono::nanoseconds interval, clock::time_point limit = clock::time_point::max()) {
        const clock::time_point now = clock::now();
        if (interval.count() <= 0) {
            SDL_DelayNS(250000);
            mNext = now;
            return;
        }
        mNext += interval;
        if (mNext < now - interval) {
            mNext = now;
        }
        const clock::time_point until = mNext < limit ? mNext : limit;
        if (until > now) {
            SDL_DelayPrecise(static_cast<Uint64>(std::chrono::duration_cast<std::chrono::nanoseconds>(until - now).count()));
        }
    }

    void reset() { mNext = clock::time_point{}; }

    // The next present slot of the `due` schedule (epoch after reset: due at once).
    clock::time_point next() const { return mNext; }

    // Starts the `due` schedule over with its next slot at `next` (a present made outside it).
    void restart(clock::time_point next) { mNext = next; }

    // Sleeps until `until` (no-op when it has passed). For loops that poll another condition
    // (a tick becoming due) between present slots.
    static void sleep_until(clock::time_point until) {
        const clock::time_point now = clock::now();
        if (until > now) {
            SDL_DelayPrecise(static_cast<Uint64>(std::chrono::duration_cast<std::chrono::nanoseconds>(until - now).count()));
        }
    }

private:
    clock::time_point mNext{};
};
