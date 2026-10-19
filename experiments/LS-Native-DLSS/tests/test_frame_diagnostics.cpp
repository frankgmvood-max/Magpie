#include "../include/frame_diagnostics.h"
#include <cassert>
#include <limits>
using namespace ls_native;
int main() {
    MetricWindow<100> w;
    w.Add(-1); w.Add(std::numeric_limits<double>::infinity()); w.Add(std::numeric_limits<double>::quiet_NaN());
    assert(w.Snapshot().count == 0);
    for (unsigned i = 1; i <= 100; ++i) w.Add(i);
    auto s = w.Snapshot();
    assert(s.count == 100 && std::abs(s.mean - 50.5) < 1e-9 && s.p50 == 50 && s.p95 == 95 && s.p99 == 99 && s.maximum == 100);
    w.Add(1000); s = w.Snapshot();
    assert(s.count == 100 && s.p50 == 51 && s.p99 == 100 && s.maximum == 1000);
    w.Clear(); w.Add(0); s = w.Snapshot();
    assert(s.count == 1 && s.p50 == 0 && s.p99 == 0 && s.mean == 0);
    FrameEvent e;
    assert(e.flags == UINT32_MAX && e.cpu_ms == -1 && e.wait_api_cpu_ms == -1);
}
