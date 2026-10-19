#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace ls_native {
enum class FrameEventKind : uint32_t { Source, Composite, Completed };
enum class FrameReason : uint32_t {
    Invalid, History, RingBusy, Submitted, Failed, Unmatched, Warmup,
    NotReady, Ordered, Ready, Repeat, Ineligible, Destination, LockBusy
};
inline const char* FrameReasonName(FrameReason v) {
    switch (v) {
    case FrameReason::Invalid: return "invalid";
    case FrameReason::History: return "history";
    case FrameReason::RingBusy: return "ring_busy";
    case FrameReason::Submitted: return "submitted";
    case FrameReason::Failed: return "failed";
    case FrameReason::Unmatched: return "unmatched";
    case FrameReason::Warmup: return "warmup";
    case FrameReason::NotReady: return "not_ready";
    case FrameReason::Ordered: return "gpu_dependency";
    case FrameReason::Ready: return "already_ready";
    case FrameReason::Repeat: return "repeat";
    case FrameReason::Ineligible: return "ineligible";
    case FrameReason::Destination: return "destination";
    case FrameReason::LockBusy: return "lock_busy";
    }
    return "unknown";
}
// Plain metadata only. A ready fence or a queued composite is NOT a display
// measurement. Negative durations mean unavailable, never a measured zero.
struct FrameEvent {
    FrameEventKind kind = FrameEventKind::Source;
    FrameReason reason = FrameReason::Invalid;
    uint64_t qpc = 0, context = 0, epoch = 0, generation = 0, previous = 0;
    uint64_t source_qpc = 0, source_sequence = 0, texture = 0, device = 0;
    uint64_t ready = 0, completed = 0, copied = 0, copied_completed = 0;
    uint64_t producer = 0, producer_completed = 0;
    uint32_t thread = 0, slot = UINT32_MAX, flags = UINT32_MAX;
    bool consumed = false, reset = false;
    double cpu_ms = -1, interval_ms = -1, source_age_ms = -1, submit_cpu_ms = -1;
    double of_cpu_ms = -1, ngx_cpu_ms = -1, wait_api_cpu_ms = -1;
    double conversion_ms = -1, flow_ms = -1, generation_ms = -1, post_ms = -1, total_ms = -1;
};
struct MetricSnapshot {
    uint64_t count = 0;
    double mean = 0, p50 = 0, p95 = 0, p99 = 0, maximum = 0;
};
// Worker-only, bounded rolling window. Percentiles use nearest rank; the
// reported count is the retained window, not a lifetime sample count.
template<size_t Capacity = 512> class MetricWindow {
public:
    void Add(double v) {
        if (!std::isfinite(v) || v < 0) return;
        values_[next_] = v; next_ = (next_ + 1) % Capacity;
        count_ = std::min(count_ + 1, Capacity);
    }
    MetricSnapshot Snapshot() const {
        MetricSnapshot s; s.count = count_;
        if (!count_) return s;
        auto sorted = values_;
        std::sort(sorted.begin(), sorted.begin() + count_);
        for (size_t i = 0; i != count_; ++i) s.mean += sorted[i] / double(count_);
        const auto rank = [&](size_t p) { return sorted[(count_ * p + 99) / 100 - 1]; };
        s.p50 = rank(50); s.p95 = rank(95); s.p99 = rank(99); s.maximum = sorted[count_ - 1];
        return s;
    }
    void Clear() { count_ = next_ = 0; }
private:
    static_assert(Capacity > 0);
    std::array<double, Capacity> values_{};
    size_t next_ = 0, count_ = 0;
};
} // namespace ls_native
