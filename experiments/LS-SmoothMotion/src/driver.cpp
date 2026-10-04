// SPDX-License-Identifier: MIT
#include "driver.h"
#include <nvs30/nvpresent.hpp>
#include <nvs30/config.hpp>
#include <nvs30/log.hpp>
#include <bcrypt.h>
#include <filesystem>
#include <fstream>
#include <array>
#include <vector>
#include <cstdarg>
#include <cstdio>
#include <mutex>
namespace {
std::function<void(const char*)> logCallback;
std::string status="Not initialized";
bool faulted=false,stoppedAfterInit=false;
std::mutex logMutex;
bool InitGuard(){__try{return nvs30::nvpresent::initialize();}__except(EXCEPTION_EXECUTE_HANDLER){faulted=true;return false;}}
void StopGuard(){__try{nvs30::nvpresent::shutdown();}__except(EXCEPTION_EXECUTE_HANDLER){faulted=true;}}
}
namespace nvs30 {
namespace {Config configuration;}
const Config& config(){return configuration;}
void set_config(const Config& c){configuration=c;}
void load_config(){}
void log_open(){}
void log_close(){}
void logf(const char* format,...){
    char text[2048]{};va_list args;va_start(args,format);std::vsnprintf(text,sizeof text,format,args);va_end(args);
    OutputDebugStringA(text);std::lock_guard<std::mutex> lock(logMutex);if(logCallback)logCallback(text);
}
}
namespace sm {
Profile CheckProfile(const std::wstring& path,std::string& sha){
    sha.clear();std::ifstream input(std::filesystem::path(path),std::ios::binary);if(!input)return Profile::Missing;
    BCRYPT_ALG_HANDLE algorithm=nullptr;BCRYPT_HASH_HANDLE hash=nullptr;
    if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)return Profile::Unsupported;
    DWORD length=0,written=0;
    if(BCryptGetProperty(algorithm,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&length),sizeof length,&written,0)<0){BCryptCloseAlgorithmProvider(algorithm,0);return Profile::Unsupported;}
    std::vector<UCHAR> object(length);
    if(BCryptCreateHash(algorithm,&hash,object.data(),length,nullptr,0,0)<0){BCryptCloseAlgorithmProvider(algorithm,0);return Profile::Unsupported;}
    std::array<char,65536> buffer;uint64_t bytes=0;bool good=true;
    while(input){input.read(buffer.data(),buffer.size());const auto count=input.gcount();if(count>0){bytes+=uint64_t(count);if(bytes>64*1024*1024 || BCryptHashData(hash,reinterpret_cast<PUCHAR>(buffer.data()),ULONG(count),0)<0){good=false;break;}}}
    std::array<UCHAR,32> digest{};
    good=good && input.eof() && BCryptFinishHash(hash,digest.data(),ULONG(digest.size()),0)>=0;
    BCryptDestroyHash(hash);BCryptCloseAlgorithmProvider(algorithm,0);
    if(!good)return Profile::Unsupported;
    constexpr char digits[]="0123456789abcdef";for(auto b:digest){sha.push_back(digits[b>>4]);sha.push_back(digits[b&15]);}
    // The upstream structural regression fingerprint is the only inspected
    // runtime profile currently available. No force-unknown checkbox or blind
    // offset patching. New profiles require source-level validation and tests.
    return sha=="cd395d58f41c6e393c31a9898f2be3da83f7bc109a2228935c23e8c89b944c15"?Profile::Known:Profile::Unsupported;
}
std::wstring ActiveDriverPath(){
    for(const wchar_t* name:{L"nvwgf2umx.dll",L"nvldumdx.dll"}){
        const HMODULE module=GetModuleHandleW(name);if(!module)continue;
        wchar_t path[32768]{};if(!GetModuleFileNameW(module,path,32768))continue;
        const auto candidate=std::filesystem::path(path).parent_path()/L"NvPresent64.dll";
        if(std::filesystem::is_regular_file(candidate))return candidate.wstring();
    }
    return {};
}
bool InitializeDriver(const std::wstring& configured,const Settings& settings,std::function<void(const char*)> log){
    if(stoppedAfterInit){status="Driver backend was stopped; restart LS before re-enabling Smooth Motion";return false;}
    if(faulted){status="NVIDIA driver exception; restart LS required";return false;}
    if(nvs30::nvpresent::initialized())return true;
    {std::lock_guard<std::mutex> lock(logMutex);logCallback=std::move(log);}
    const auto path=configured.empty()?ActiveDriverPath():configured;std::string sha;
    const auto profile=CheckProfile(path,sha);
    if(profile!=Profile::Known){
        status=profile==Profile::Missing?"NvPresent64.dll missing from active NVIDIA driver package":"Unsupported NvPresent64 profile; no driver patches applied";
        if(logCallback){logCallback(status.c_str());if(!sha.empty())logCallback(("Smooth Motion: NvPresent64 SHA256="+sha).c_str());}
        return false;
    }
    nvs30::Config cfg;cfg.nvpresent_path=path;cfg.diagnostics=settings.diagnostics;nvs30::set_config(cfg);
    if(GetModuleHandleW(L"NvPresent64.dll") && !nvs30::nvpresent::module()){
        status="NvPresent is already loaded outside this addon; restart LS with other Smooth Motion loaders disabled";if(logCallback)logCallback(status.c_str());return false;
    }
    if(!InitGuard()){StopGuard();status=faulted?"NVIDIA initialization exception; restart LS required":"Known driver profile did not initialize / inference hooks unavailable";return false;}
    status="Known profile initialized; awaiting wrapper and CUDA inference";return true;
}
void StopDriver(){if(nvs30::nvpresent::initialized())stoppedAfterInit=true;StopGuard();std::lock_guard<std::mutex> lock(logMutex);logCallback={};}
bool DriverFaulted(){return faulted;}
std::string DriverStatus(){return status;}
}
