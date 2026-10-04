#include "settings.h"
#include "policy.h"
#include <cstdio>
#include <cstdlib>
#include <limits>
void Check(bool condition,const char* message) {if(!condition) {std::fprintf(stderr,"%s\n",message);std::exit(1);}}
int main() {
    Check(fg::ParseTargetFPS(L"bad")==136 && fg::ParseTargetFPS(L"")==136 && fg::ParseTargetFPS(L"136 FPS")==136 && fg::ParseTargetFPS(L"NaN")==136 && fg::ParseTargetFPS(L"500")==136,"bad INI FPS never enables adaptive pacing");
    Check(fg::ParseTargetFPS(L"0")==0 && fg::ParseTargetFPS(L" 68.5 \t")==68.5,"explicit adaptive and fractional INI FPS accepted");
    fg::Settings settings;settings.multiplier=99;settings.flowQuality=99;
    settings.flow=static_cast<fg::FlowMethod>(1);settings.maximumFrameLatency=99;
    settings.targetFPS=std::numeric_limits<double>::quiet_NaN();settings.presentApi=static_cast<fg::PresentApi>(99);settings.Validate();
    Check(settings.multiplier==4 && settings.flow==fg::FlowMethod::None && settings.flowQuality==5 && settings.maximumFrameLatency==1 && settings.targetFPS==136 && settings.presentApi==fg::PresentApi::Auto,"invalid settings normalized");
    settings.targetFPS=0;settings.maximumFrameLatency=0;settings.Validate();Check(settings.targetFPS==0 && settings.maximumFrameLatency==0,"adaptive/preserve LS accepted");
    for(unsigned m=2;m<=4;++m) {
        Check(fg::Plan(m,true).count==1 && fg::Plan(m,true).reset,"reset must be 1/1 at every multiplier");
        Check(fg::Plan(m,false).count==m-1,"MFG count is multiplier minus one");
        for(unsigned cap=1;cap<=3;++cap) Check(fg::SupportedMultiplier(m,cap)==std::min(m,cap+1),"runtime MFG cap honored");
        fg::Timeline timeline;timeline.Observe(1);timeline.Observe(1.02);timeline.Observe(1.04);timeline.Observe(1.06);
        Check(std::abs(timeline.Step(0,m)-.02/m)<1e-9,"adaptive spacing based on unique cadence and MFG");
        const double step=timeline.Step(136,m);
        for(unsigned i=1;i<m;++i) Check(std::abs(timeline.FrameDue(2,i,step)-timeline.FrameDue(2,i-1,step)-step)<1e-9,"all MFG frames evenly scheduled");
        Check(timeline.NeedsReset(2) && !timeline.NeedsReset(1.07),"unique frame gap resets history");
    }
    Check(fg::SupportedMultiplier(4,0)==2 && fg::SupportedMultiplier(4,100)==4,"invalid reported capability bounded");
    std::puts("settings, capability fallback, reset contract and x2-x4 pacing passed");
}
