#pragma once
#include "observation.h"
#include <limits>

namespace ls_native {
// Metadata only. Writers not intercepted by the observer can invalidate the
// actual pixels without changing these stamps. This is not replacement approval.
struct SourcePairHistory {
    uint64_t invalidation_epoch = 0;
    SourceWriteObservation previous, current;
};
inline bool SameTexture(const TextureObservation& a, const TextureObservation& b) {
    return a.object == b.object && a.width == b.width && a.height == b.height &&
        a.format == b.format && a.view_format == b.view_format && a.mip == b.mip &&
        a.array_size == b.array_size && a.samples == b.samples && a.bind_flags == b.bind_flags;
}
inline bool SameSourceWrite(const SourceWriteObservation& a, const SourceWriteObservation& b) {
    return a.epoch == b.epoch && a.generation == b.generation && a.sequence == b.sequence &&
        a.qpc == b.qpc && a.command_epoch == b.command_epoch && a.device == b.device &&
        a.context == b.context && a.adapter_luid == b.adapter_luid && a.thread == b.thread &&
        SameTexture(a.texture, b.texture);
}
inline bool SourceWriteValid(const SourceWriteObservation& w) {
    const auto& t = w.texture;
    return w.epoch && w.generation && w.sequence && w.qpc && w.device && w.context &&
        w.adapter_luid && w.thread && t.object && t.width && t.height && t.format &&
        t.format == t.view_format && t.mip == 0 && t.array_size == 1 && t.samples == 1;
}
inline bool CanContinueSource(const SourceWriteObservation& a, const SourceWriteObservation& b) {
    return SourceWriteValid(a) && a.device == b.device && a.context == b.context &&
        a.adapter_luid == b.adapter_luid && a.thread == b.thread && a.command_epoch == b.command_epoch &&
        a.texture.object != b.texture.object && a.texture.width == b.texture.width &&
        a.texture.height == b.texture.height && a.texture.format == b.texture.format &&
        a.texture.view_format == b.texture.view_format && a.sequence < b.sequence && a.qpc <= b.qpc &&
        a.generation != std::numeric_limits<uint64_t>::max();
}
inline bool AppendSourceWrite(SourcePairHistory& h, const SourceWriteObservation& w) {
    if (!SourceWriteValid(w)) { h.previous = {}; h.current = {}; return false; }
    if (CanContinueSource(h.current, w) && h.current.epoch == w.epoch &&
        h.current.generation + 1 == w.generation) h.previous = h.current;
    else h.previous = {};
    h.current = w;
    return true;
}
inline bool MatchObservedSourcePair(const SourcePairHistory& h,
    const SourceWriteObservation& previous_tag, const SourceWriteObservation& current_tag,
    const DispatchObservation& synthesis) {
    const auto& p = h.previous; const auto& c = h.current;
    return SourceWriteValid(p) && SourceWriteValid(c) && CanContinueSource(p, c) &&
        p.epoch == c.epoch && p.generation + 1 == c.generation &&
        SameSourceWrite(p, previous_tag) && SameSourceWrite(c, current_tag) &&
        synthesis.device == c.device && synthesis.context == c.context &&
        synthesis.adapter_luid == c.adapter_luid && synthesis.thread == c.thread &&
        synthesis.sequence > c.sequence && synthesis.qpc >= c.qpc &&
        SameTexture(synthesis.inputs[0], p.texture) && SameTexture(synthesis.inputs[1], c.texture);
}
} // namespace ls_native
