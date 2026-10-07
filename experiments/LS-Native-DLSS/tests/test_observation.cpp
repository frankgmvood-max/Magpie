#include "../include/observation.h"
#include <cassert>
#include <iostream>
#include <limits>
#include <thread>
#include <vector>

int main() {
    using namespace ls_native;
    ConstantBytes cb;
    cb.byte_width = 48; // Actual CPU allocation; RDEF only describes 32 bytes.
    const uint32_t phase = 0x3f000000, scale = 0x3f800000;
    std::memcpy(cb.bytes.data() + 28, &phase, 4);
    std::memcpy(cb.bytes.data() + 24, &scale, 4);
    DispatchObservation out;
    assert(DecodeConstants(cb, 0, 4096, out));
    assert(out.constants_known && out.phase_bits == phase && out.resolution_scale_bits == scale);
    assert(!DecodeConstants(cb, 2, 4096, out) && !out.constants_known && out.phase_bits == 0);
    assert(!DecodeConstants(cb, 0, 1, out));
    assert(!DecodeConstants(cb, std::numeric_limits<uint32_t>::max(), 4096, out));
    cb.byte_width = 31;
    assert(!DecodeConstants(cb, 0, 2, out));
    cb.byte_width = 65;
    assert(!DecodeConstants(cb, 0, 2, out));
    cb.byte_width = 64;
    std::memcpy(cb.bytes.data() + 44, &phase, 4);
    assert(DecodeConstants(cb, 1, 2, out) && out.phase_bits == phase);

    ObservationQueue<uint64_t, 2> q;
    uint64_t value = 0;
    assert(!q.TryPop(value));
    assert(q.TryPush(11) && q.TryPush(12));
    assert(!q.TryPush(13) && q.Dropped() == 1);
    assert(q.TryPop(value) && value == 11);
    assert(q.TryPush(14));
    assert(q.TryPop(value) && value == 12);
    assert(q.TryPop(value) && value == 14);
    assert(!q.TryPop(value));

    // Every submitted record is either retained once or counted as dropped.
    ObservationQueue<uint64_t, 4096> concurrent;
    std::vector<std::thread> workers;
    for (uint64_t t = 0; t != 4; ++t)
        workers.emplace_back([&, t] { for (uint64_t i = 0; i != 500; ++i) concurrent.TryPush(t * 500 + i); });
    for (auto& thread : workers) thread.join();
    std::array<bool, 2000> seen{};
    uint64_t retained = 0;
    while (concurrent.TryPop(value)) {
        assert(value < seen.size() && !seen[value]);
        seen[value] = true;
        ++retained;
    }
    assert(retained + concurrent.Dropped() == seen.size());
    std::cout << "observation snapshots and concurrent queue: all checks passed\n";
}
