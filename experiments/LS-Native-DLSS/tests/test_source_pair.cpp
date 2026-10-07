#include "../include/source_pair.h"
#include <cassert>
#include <iostream>

int main() {
    using namespace ls_native;
    SourceWriteObservation a;
    a.epoch = 10; a.generation = 1; a.sequence = 1; a.qpc = 100;
    a.device = 20; a.context = 30; a.adapter_luid = 40; a.thread = 50;
    a.texture = {60, 1920, 1080, 28, 28, 0, 1, 1, 136};
    auto b = a; b.generation = 2; b.sequence = 2; b.qpc = 200; b.texture.object = 61;
    SourcePairHistory h;
    assert(AppendSourceWrite(h, a));
    DispatchObservation final;
    final.sequence = 3; final.qpc = 300; final.device = a.device; final.context = a.context;
    final.adapter_luid = a.adapter_luid; final.thread = a.thread;
    final.inputs[0] = a.texture; final.inputs[1] = b.texture;
    assert(!MatchObservedSourcePair(h, a, b, final)); // One source is not a pair.
    assert(AppendSourceWrite(h, b) && MatchObservedSourcePair(h, a, b, final));
    assert(!MatchObservedSourcePair(h, b, a, final)); // Temporal order matters.
    auto wrong = final; std::swap(wrong.inputs[0], wrong.inputs[1]);
    assert(!MatchObservedSourcePair(h, a, b, wrong));
    wrong = final; ++wrong.context; assert(!MatchObservedSourcePair(h, a, b, wrong));
    wrong = final; ++wrong.thread; assert(!MatchObservedSourcePair(h, a, b, wrong));
    wrong = final; wrong.qpc = 199; assert(!MatchObservedSourcePair(h, a, b, wrong));
    wrong = final; wrong.sequence = 2; assert(!MatchObservedSourcePair(h, a, b, wrong));
    auto stale = b; --stale.generation; assert(!MatchObservedSourcePair(h, a, stale, final));
    auto c = b; c.generation = 3; c.sequence = 4; c.qpc = 400; c.texture.object = a.texture.object;
    assert(AppendSourceWrite(h, c)); // Same texture object, new image submission.
    final.sequence = 5; final.qpc = 500; final.inputs[0] = b.texture; final.inputs[1] = c.texture;
    assert(MatchObservedSourcePair(h, b, c, final));
    assert(!MatchObservedSourcePair(h, b, a, final));
    ++final.sequence; // Multiple synthesis slots may consume the same observed pair.
    assert(MatchObservedSourcePair(h, b, c, final));
    for (int i = 0; i != 7; ++i) {
        SourcePairHistory test = h; auto d = c;
        d.generation = 4; d.sequence = 7; d.qpc = 700; d.texture.object = b.texture.object;
        switch (i) {
        case 0: ++d.epoch; break;
        case 1: ++d.context; break;
        case 2: ++d.device; break;
        case 3: ++d.command_epoch; break;
        case 4: ++d.texture.width; break;
        case 5: ++d.generation; break;
        case 6: ++d.thread; break;
        }
        assert(AppendSourceWrite(test, d) && test.previous.generation == 0);
    }
    c.texture.samples = 4;
    assert(!AppendSourceWrite(h, c) && !h.current.generation);
    std::cout << "source update order, texture reuse and broken provenance passed\n";
}
