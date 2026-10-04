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
    fg::OutputPacer pacer;
    pacer.Submitted(1);
    Check(std::abs(pacer.Due(1.001,step)-(1+step))<1e-10,"early deadline beyond 2ms is retained");
    // The previous DXGI call can block for 5ms. Its return must not become
    // the anchor and charge those same 5ms again to the output interval.
    Check(std::abs(pacer.Due(1.005,step)-(1+step))<1e-10,"blocking Present is included in the same output interval");
    Check(pacer.Due(20,step)==20,"late input has no accumulating scheduling debt");
    pacer.Submitted(20);
    Check(std::abs(pacer.Due(20.001,step)-(20+step))<1e-10,"late input cannot trigger catch-up bursts");
    for(unsigned multiplier=2;multiplier<=4;++multiplier){
        pacer.Reset();double now=2,previous=0;
        for(unsigned group=0;group<300;++group){
            // A group alternates with a single duplicate/history pass-through.
            const unsigned outputs=group%9==0?1:multiplier;
            for(unsigned index=0;index<outputs;++index){
                const double begin=pacer.Due(now,step);
                if(previous)Check(begin-previous>=step-1e-10,"all real/intermediate/duplicate outputs share the same minimum spacing");
                pacer.Submitted(begin);previous=begin;
                now=begin+(index%2?0.005:0.0002);
            }
            now+=0.0005;
        }
    }
    fg::SubmissionStats stats;stats.Observe(1,1.003,1);stats.Observe(1.01,1.011,1.009);stats.Observe(1.025,1.026,1.025);
    Check(stats.count==3 && std::abs(stats.MeanGap()-.0125)<1e-10 && std::abs(stats.maxGap-.015)<1e-10 && std::abs(stats.maxPresent-.003)<1e-10 && std::abs(stats.maxLate-.001)<1e-10,"CPU telemetry records actual submissions and deadline misses");
    stats.ClearWindow();stats.Observe(1.035,1.036,1.035);
    Check(stats.count==1 && stats.gaps==1 && std::abs(stats.MeanGap()-.01)<1e-10,"telemetry keeps boundary interval across reporting windows");
    Check(!t.Observe(30), "pause must reset history");
    Check(fg::SafePresent(0, 0x200), "tearing preserved");
    Check(fg::SafePresent(1, 0), "vsync allowed");
    Check(!fg::SafePresent(0, 1), "test present rejected");
    Check(!fg::SafePresent(0, 8), "nonblocking present rejected");
    Check(!fg::SafePresent(2, 0), "multi-vblank rejected");
    auto mode=fg::ChoosePresent(1,0,true,true,true);
    Check(mode.vrrRequested && mode.sync==0 && mode.flags==0x200,"VRR replaces vblank sync on eligible output");
    mode=fg::ChoosePresent(0,0x200,true,true,true);
    Check(mode.sync==0 && mode.flags==0x200,"existing tearing remains legal");
    for(const auto& m:{fg::ChoosePresent(1,0,true,false,true),
                       fg::ChoosePresent(1,0,true,true,false),
                       fg::ChoosePresent(1,0,false,true,true)})
        Check(!m.vrrRequested && m.sync==1 && m.flags==0,"ineligible or disabled VRR preserves LS");
    mode=fg::ChoosePresent(0,1,true,true,true);
    Check(!mode.vrrRequested && mode.flags==1,"TEST never converted to a displaying present");
    mode=fg::ChoosePresent(0,8,true,true,true);
    Check(!mode.vrrRequested && mode.flags==8,"DO_NOT_WAIT contract preserved");
    std::puts("presentation policy passed");
}
