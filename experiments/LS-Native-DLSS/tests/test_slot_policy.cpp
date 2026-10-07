#include "../include/native_slot_policy.h"
#include <cassert>
#include <iostream>
using namespace ls_native;

int main() {
    VerifiedContract verified{true, true, true, true};
    Identity slot{7, 42, 100, 101, 5, 3440, 1440, 28, 0, 0x3f000000};
    CompletedImage image{slot, 12, true, false, true};
    assert(Choose(verified, slot, image, 12, 4) == Decision::UseReplacement);
    assert(Choose({}, slot, image, 12, 4) == Decision::NativeUnverified);
    assert(Choose(verified, slot, image, 11, 4) == Decision::NativeNotReady);
    assert(Choose(verified, slot, image, UINT64_MAX, 4) == Decision::NativeNotReady);
    assert(Choose(verified, slot, image, 12, 5) == Decision::NativeAlreadyConsumed);
    auto changed = image;
    changed.identity.device_epoch++;
    assert(Choose(verified, slot, changed, 12, 4) == Decision::NativeMismatch);
    changed = image; changed.identity.adapter_luid++;
    assert(Choose(verified, slot, changed, 12, 4) == Decision::NativeMismatch);
    changed = image; changed.identity.previous_frame--;
    assert(Choose(verified, slot, changed, 12, 4) == Decision::NativeMismatch);
    changed = image; changed.identity.width++;
    assert(Choose(verified, slot, changed, 12, 4) == Decision::NativeMismatch);
    changed = image; changed.identity.format++;
    assert(Choose(verified, slot, changed, 12, 4) == Decision::NativeMismatch);
    changed = image; changed.identity.color_space++;
    assert(Choose(verified, slot, changed, 12, 4) == Decision::NativeMismatch);
    changed = image; changed.interpolation_disabled = true;
    assert(Choose(verified, slot, changed, 12, 4) == Decision::NativeFailure);
    changed = image; changed.copy_lease_held = false;
    assert(Choose(verified, slot, changed, 12, 4) == Decision::NativeFailure);
    changed = image; changed.succeeded = false;
    assert(Choose(verified, slot, changed, 12, 4) == Decision::NativeFailure);
    auto other_slot = slot; other_slot.phase_bits = 0x3e800000; // 0.25
    assert(Choose(verified, other_slot, image, 12, 4) == Decision::NativeWrongPhase);
    other_slot = slot; other_slot.current_frame = 105;
    assert(Choose(verified, other_slot, image, 12, 4) == Decision::NativeInvalidSlot);
    other_slot = slot; other_slot.previous_frame = UINT64_MAX; other_slot.current_frame = 0;
    assert(Choose(verified, other_slot, image, 12, 4) == Decision::NativeInvalidSlot);
    std::cout << "native slot policy: all checks passed\n";
}
