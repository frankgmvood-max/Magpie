#include "frame_trace.h"
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>

void Check(bool ok,const char* text){if(!ok){std::fprintf(stderr,"%s\n",text);std::exit(1);}}
int main(){
    try {
    const auto folder=std::filesystem::temp_directory_path()/L"LSFrameTraceTest"/std::to_wstring(GetCurrentProcessId());
    const auto path=folder/L"trace.csv";
    fg::FrameTrace trace;
    Check(trace.Start(path,1),"start bounded trace");
    for(unsigned i=0;i<100;++i){
        fg::TraceRow row{};row.timing.sequence=i+1;row.timing.beginQpc=1000+i*10;row.timing.endQpc=row.timing.beginQpc+2;row.timing.frequency=1000;row.timing.result=S_OK;
        row.base=i/2;row.chain=0x1234;row.kind=i%2;row.input=1.000;row.ready=1.005;row.due=1.007;
        trace.Record(row);
    }
    const auto deadline=GetTickCount64()+4000;
    while(trace.Active() && GetTickCount64()<deadline)Sleep(5);
    Check(!trace.Active(),"trace stops without requiring a new Present");
    trace.Stop();Check(!trace.Failed() && !trace.Dropped(),"writer drained without lost rows");
    std::ifstream file(path);std::string line;std::getline(file,line);
    Check(line.find("present_begin_qpc")!=std::string::npos && line.find("qpc_frequency")!=std::string::npos,"trace header identifies CPU timestamps");
    unsigned count=0;bool disclaimer=false;
    while(std::getline(file,line)){
        if(line.rfind("#",0)==0){disclaimer|=line.find("monitor_refresh_unmeasured")!=std::string::npos;continue;}
        Check(std::count(line.begin(),line.end(),',')==17,"row keeps full CSV schema");
        Check(std::stoull(line)==++count,"writer retains submission order");
    }
    Check(count==100 && disclaimer,"all rows and measurement limit are saved");
    // Windows won't delete a file still held by an ordinary ifstream; close
    // the reader before restarting the writer and removing the test directory.
    file.close();
    Check(!trace.Start(folder,1) && !trace.Active(),"unwritable target fails without starting worker");
    Check(trace.Start(path,60),"restart after an output failure");trace.Stop();
    std::filesystem::remove_all(folder);
    std::puts("bounded asynchronous trace, ordered rows, timed completion, shutdown drain and failure recovery passed");
    return 0;
    } catch(const std::exception& error){std::fprintf(stderr,"frame trace test: %s\n",error.what());return 1;}
}
