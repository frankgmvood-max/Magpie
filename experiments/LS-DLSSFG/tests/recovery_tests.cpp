#include "recovery.h"
#include <cstdio>
#include <cstdlib>
void Check(bool b,const char* why){if(!b){std::fprintf(stderr,"%s\n",why);std::exit(1);}}
int main(){
    fg::Recovery r;
    using A=fg::RecoveryAction;
    Check(r.Observe(1,1,true,true)==A::Continue,"first live frame");
    for(unsigned i=1;i<500;++i)Check(r.Observe(1+i*0.016,1,true,true)==A::Continue,"regular (including duplicate) presentations keep history");
    Check(r.Observe(9,1,false,true)==A::Suspend,"minimized output suspends");
    Check(r.Observe(9.01,1,true,true)==A::Recreate,"resume rebuilds even after a short minimize");
    Check(r.Observe(9.5,1,true,true)==A::Recreate,"presentation gap rebuilds");
    Check(r.Observe(9.51,2,true,true)==A::Recreate,"new output window rebuilds");
    for(unsigned i=0;i<59;++i)Check(!r.RuntimeRejected(true),"do not churn on a few rejected frames");
    Check(r.RuntimeRejected(true),"sustained runtime refusal is detected");
    Check(!r.RuntimeRejected(false),"successful interpolation clears refusal streak");
    r.Reset();Check(r.Observe(1,3,true,true)==A::Continue,"explicit reset clears stale cadence");
    std::puts("continuous duplicates, minimize/resume, gap, new window and sustained runtime rejection passed");
}
