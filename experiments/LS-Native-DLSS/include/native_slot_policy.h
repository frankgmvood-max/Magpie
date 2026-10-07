#pragma once
#include <cstdint>
#include <limits>

// Research contract only. This does NOT hook LS, call NGX or perform a GPU copy.
// LS owns capture, the output slot, its deadline and every Present.
namespace ls_native {

struct Identity {
    uint64_t device_epoch = 0;
    uint64_t adapter_luid = 0;
    uint64_t previous_frame = 0;
    uint64_t current_frame = 0;
    uint64_t slot_sequence = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t format = 0;
    uint32_t color_space = 0;
    uint32_t phase_bits = 0; // Exact decoded float bits; never inferred from Present counts.
};

inline bool operator==(const Identity& a, const Identity& b) {
    return a.device_epoch == b.device_epoch && a.adapter_luid == b.adapter_luid &&
        a.previous_frame == b.previous_frame && a.current_frame == b.current_frame &&
        a.slot_sequence == b.slot_sequence && a.width == b.width && a.height == b.height &&
        a.format == b.format && a.color_space == b.color_space && a.phase_bits == b.phase_bits;
}

struct VerifiedContract {
    bool dll_and_shader_hashes = false;
    bool source_pair_and_phase = false;
    bool output_slot_and_resource_lifetime = false;
    bool fixed_x2_sdr = false;
};

struct CompletedImage {
    Identity identity;
    uint64_t fence_value = 0;
    bool succeeded = false;
    bool interpolation_disabled = true;
    bool copy_lease_held = false; // Held through the caller's GPU copy + retirement fence.
};

enum class Decision {
    NativeUnverified, NativeWrongPhase, NativeInvalidSlot, NativeFailure,
    NativeNotReady, NativeMismatch, NativeAlreadyConsumed, UseReplacement
};

inline Decision Choose(const VerifiedContract& contract, const Identity& native_slot,
                       const CompletedImage& candidate, uint64_t completed_fence,
                       uint64_t last_consumed_sequence) {
    if (!contract.dll_and_shader_hashes || !contract.source_pair_and_phase ||
        !contract.output_slot_and_resource_lifetime || !contract.fixed_x2_sdr)
        return Decision::NativeUnverified;
    // Initial NGX experiment is midpoint only. Adaptive and other multipliers stay native.
    if (native_slot.phase_bits != 0x3f000000u) // IEEE 754 float 0.5
        return Decision::NativeWrongPhase;
    if (!native_slot.device_epoch || !native_slot.adapter_luid || !native_slot.slot_sequence ||
        !native_slot.width || !native_slot.height || !native_slot.format ||
        native_slot.previous_frame == std::numeric_limits<uint64_t>::max() ||
        native_slot.previous_frame + 1 != native_slot.current_frame)
        return Decision::NativeInvalidSlot;
    if (native_slot.slot_sequence <= last_consumed_sequence)
        return Decision::NativeAlreadyConsumed;
    if (!candidate.succeeded || candidate.interpolation_disabled || !candidate.copy_lease_held)
        return Decision::NativeFailure;
    if (!candidate.fence_value || completed_fence == std::numeric_limits<uint64_t>::max() ||
        completed_fence < candidate.fence_value)
        return Decision::NativeNotReady; // Never wait here; run the original LS dispatch.
    if (!(candidate.identity == native_slot))
        return Decision::NativeMismatch;
    return Decision::UseReplacement;
}

// The caller marks a slot consumed only AFTER a successful GPU copy submission.
// A texture lease must outlive that copy, not just NGX completion. Device loss,
// focus/resize/restart and a source discontinuity invalidate all old epochs/jobs.
// Native fallback does not itself guarantee seamless quality: repeated switches
// require a measured session-level fallback/cooldown policy before release.
} // namespace ls_native
