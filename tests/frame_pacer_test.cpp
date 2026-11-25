// Pacer self-test (fps_design 10, T1): runs FramePacer::wait for 2 s at several rates and checks
// the mean interval (within 0.2%) and the p95 lateness (under 1 ms). Exit status 0 on pass.
#include "frame_pacer.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

int main() {
    using namespace std::chrono;
    const int rates[] = { 30, 60, 144, 240, 1000 };
    bool ok = true;
    for (int hz : rates) {
        const nanoseconds interval(1000000000LL / hz);
        FramePacer pacer;
        pacer.wait(interval); // establishes the schedule
        auto start = FramePacer::clock::now();
        auto ideal = start;
        std::vector<double> late;
        int frames = 0;
        while (FramePacer::clock::now() - start < seconds(2)) {
            pacer.wait(interval);
            ideal += interval;
            late.push_back(duration<double, std::milli>(FramePacer::clock::now() - ideal).count());
            frames++;
        }
        const double total = duration<double, std::milli>(FramePacer::clock::now() - start).count();
        const double mean  = total / frames;
        const double want  = 1000.0 / hz;
        std::sort(late.begin(), late.end());
        const double p95 = late.empty() ? 0.0 : late[late.size() * 95 / 100];
        const bool pass  = std::fabs(mean - want) / want < 0.002 && p95 < 1.0;
        std::printf("%5d Hz: mean %.4f ms (want %.4f), p95 late %.3f ms %s\n", hz, mean, want, p95, pass ? "ok" : "FAIL");
        ok = ok && pass;
    }
    return ok ? 0 : 1;
}
