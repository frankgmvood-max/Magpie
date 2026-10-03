#include "policy.h"
#include <cstdio>
#include <cstdlib>
static void Check(bool ok, const char* label) {
    if (!ok) { std::fprintf(stderr, "%s\n", label); std::exit(1); }
}
int main() {
    fg::Timeline t;
    Check(!t.Observe(1), "first frame must prime history");
    Check(!t.Observe(1.015), "warmup 1");
    Check(!t.Observe(1.030), "warmup 2");
    Check(t.Observe(1.045), "warmup complete");
    const double step = t.Step(136);
    Check(std::abs(step - 1.0/136) < 1e-10, "136 Hz period");
    double previous = 0;
    for (int i = 0; i < 300; ++i) {
        const double now = 2.0 + i / 68.0 + (i % 9 == 0 ? 0.003 : 0);
        const double made = t.GeneratedDue(now, step);
        Check(made >= now && made <= now + 0.0021, "bounded generated wait");
        Check(made > previous, "monotonic schedule");
        const double real = t.RealDue(made, step);
        Check(std::abs(real - made - step) < 1e-10, "even pair spacing");
        previous = real;
    }
    Check(t.GeneratedDue(20, step) == 20, "late frame rebase, no catchup burst");
    Check(!t.Observe(30), "pause must reset history");
    Check(fg::SafePresent(0, 0x200), "tearing preserved");
    Check(fg::SafePresent(1, 0), "vsync allowed");
    Check(!fg::SafePresent(0, 1), "test present rejected");
    Check(!fg::SafePresent(0, 8), "nonblocking present rejected");
    Check(!fg::SafePresent(2, 0), "multi-vblank rejected");
    std::puts("presentation policy passed");
}
