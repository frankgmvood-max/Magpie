// SPDX-License-Identifier: MIT
#pragma once
#include <ls_output_bridge.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>
#include <mutex>
#include <thread>

namespace fg {
struct TraceRow {
    LsBridgePresentTiming timing{};
    uintptr_t chain=0;
    uint64_t base=0;
    unsigned kind=0,index=0,multiplier=2; // real=0, generated=1, duplicate=2, history=3
    double input=0,ready=0,due=0,generationMs=0,preprocessMs=0,queueWaitMs=0;
};
// All file writes happen on a worker. The presenting thread only copies one
// bounded row; if storage falls behind it drops rows and reports that loss.
// Capture is opt-in, lasts 60 seconds and never captures images or game input.
class FrameTrace {
public:
    ~FrameTrace(){Stop();}
    bool Active() const{return active_.load(std::memory_order_acquire);}
    uint64_t Dropped() const{return dropped_.load();}
    bool Failed() const{return failed_.load();}
    bool Start(const std::filesystem::path& path,unsigned seconds=60){
        if(Active())return false;
        Stop();
        try {
            std::filesystem::create_directories(path.parent_path());
            std::ofstream file(path,std::ios::binary|std::ios::trunc);
            if(!file)return false;
            file.imbue(std::locale::classic());
            file<<"sequence,base_id,swapchain,kind,index,multiplier,qpc_frequency,present_begin_qpc,present_end_qpc,input_seconds,ready_seconds,deadline_seconds,generation_ms,preprocess_cpu_ms,queue_wait_ms,sync,flags,hresult\n";
            head_=count_=0;stopping_=false;dropped_=0;failed_=false;
            active_.store(true,std::memory_order_release);
            worker_=std::thread([this,file=std::move(file),seconds]() mutable {
                try {
                    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(seconds);
                    std::array<TraceRow,128> batch{};
                    for(;;){
                        size_t count=0;bool finish=false;
                        {
                            std::unique_lock<std::mutex> lock(mutex_);
                            wake_.wait_until(lock,std::min(end,std::chrono::steady_clock::now()+std::chrono::milliseconds(250)),[&]{return stopping_ || count_>=64;});
                            finish=stopping_ || std::chrono::steady_clock::now()>=end;
                            if(finish)active_.store(false,std::memory_order_release);
                            count=std::min(count_,batch.size());
                            for(size_t i=0;i<count;++i){batch[i]=rows_[head_];head_=(head_+1)%rows_.size();}
                            count_-=count;
                            finish=finish && !count_;
                        }
                        for(size_t i=0;i<count;++i)Write(file,batch[i]);
                        if(!file){failed_=true;break;}
                        if(finish)break;
                    }
                    file<<"# dropped_rows="<<Dropped()<<"\n# timing=CPU_DXGI_submission; monitor_refresh_unmeasured\n";
                    file.flush();if(!file)failed_=true;
                } catch(...){failed_=true;}
                active_.store(false,std::memory_order_release);
            });
            return true;
        } catch(...){failed_=true;active_=false;return false;}
    }
    void Record(const TraceRow& row){
        if(!Active())return;
        std::lock_guard<std::mutex> lock(mutex_);
        if(!Active())return;
        if(count_==rows_.size()){++dropped_;return;}
        rows_[(head_+count_)%rows_.size()]=row;++count_;
        if(count_>=64)wake_.notify_one();
    }
    void Stop(){
        {std::lock_guard<std::mutex> lock(mutex_);stopping_=true;active_=false;}
        wake_.notify_one();if(worker_.joinable())worker_.join();
    }
private:
    static void Write(std::ostream& file,const TraceRow& row){
        const auto& t=row.timing;
        file<<t.sequence<<','<<row.base<<",0x"<<std::hex<<row.chain<<std::dec<<','<<row.kind<<','<<row.index<<','<<row.multiplier<<','
            <<t.frequency<<','<<t.beginQpc<<','<<t.endQpc<<','<<std::fixed<<std::setprecision(9)
            <<row.input<<','<<row.ready<<','<<row.due<<','<<row.generationMs<<','<<row.preprocessMs<<','<<row.queueWaitMs<<','
            <<t.sync<<",0x"<<std::hex<<t.flags<<",0x"<<uint32_t(t.result)<<std::dec<<'\n';
    }
    std::array<TraceRow,2048> rows_{};
    size_t head_=0,count_=0;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::thread worker_;
    bool stopping_=false;
    std::atomic<bool> active_{false},failed_{false};
    std::atomic<uint64_t> dropped_{0};
};
}
