#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace ls_native {

// These are object identities, NOT source frame IDs or completed GPU fences.
struct TextureObservation {
    uint64_t object = 0;
    uint32_t width = 0, height = 0, format = 0, view_format = 0;
    uint32_t mip = 0, array_size = 0, samples = 0, bind_flags = 0;
};

// A CPU-observed input Dispatch submission, not a game frame or GPU completion.
struct SourceWriteObservation {
    uint64_t epoch = 0, generation = 0, sequence = 0, qpc = 0;
    uint64_t command_epoch = 0, device = 0, context = 0, adapter_luid = 0;
    uint32_t thread = 0;
    TextureObservation texture;
};

struct DispatchObservation {
    uint64_t sequence = 0, qpc = 0, device = 0, context = 0, adapter_luid = 0;
    uint32_t thread = 0, groups_x = 0, groups_y = 0, groups_z = 0;
    uint32_t cb_byte_width = 0, first_constant = 0, num_constants = 0;
    uint32_t phase_bits = 0, resolution_scale_bits = 0;
    bool constants_known = false;
    std::array<TextureObservation, 5> inputs{};
    TextureObservation output;
    bool input_update = false, pair_matches_observed_updates = false;
    SourceWriteObservation source_write, previous_write, current_write;
};

struct ConstantBytes {
    uint32_t byte_width = 0;
    std::array<uint8_t, 64> bytes{};
};

// CPU bytes from successful CreateBuffer. The Windows observer must invalidate
// a DYNAMIC snapshot on every subsequent write/deferred execution. This decoder
// does not by itself establish snapshot provenance or immutability.
inline bool DecodeConstants(const ConstantBytes& snapshot,
                            uint32_t first_constant, uint32_t num_constants,
                            DispatchObservation& out) {
    out.constants_known = false;
    out.phase_bits = out.resolution_scale_bits = 0;
    out.cb_byte_width = snapshot.byte_width;
    out.first_constant = first_constant;
    out.num_constants = num_constants;
    const uint64_t start = uint64_t(first_constant) * 16;
    if (num_constants < 2 || snapshot.byte_width > snapshot.bytes.size() ||
        start + 32 > snapshot.byte_width) return false;
    std::memcpy(&out.resolution_scale_bits, snapshot.bytes.data() + start + 24, 4);
    std::memcpy(&out.phase_bits, snapshot.bytes.data() + start + 28, 4);
    out.constants_known = true;
    return true;
}

// A contended/full observer drops its record rather than waiting on the native
// render thread. Supports multiple producers. Formatting and I/O belong to the
// consumer. Record must own no COM references, pointers or dynamically sized data.
template <class Record, size_t Capacity> class ObservationQueue {
    static_assert(Capacity > 0, "nonempty queue required");
    std::array<Record, Capacity> records_{};
    std::mutex mutex_;
    size_t head_ = 0, size_ = 0;
    std::atomic<uint64_t> dropped_{0};
public:
    bool TryPush(const Record& record) {
        std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock() || size_ == Capacity) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        records_[(head_ + size_) % Capacity] = record;
        ++size_;
        return true;
    }
    bool TryPop(Record& record) {
        std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock() || !size_) return false;
        record = records_[head_];
        head_ = (head_ + 1) % Capacity;
        --size_;
        return true;
    }
    uint64_t Dropped() const { return dropped_.load(std::memory_order_relaxed); }
};
} // namespace ls_native
