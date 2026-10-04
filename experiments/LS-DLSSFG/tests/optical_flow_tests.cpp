#include "optical_flow.h"
#include <nvOpticalFlowD3D11.h>
#include <vector>
#include <cstdio>
#include <cstdlib>
namespace fg {void SetOpticalFlowTestApi(const NV_OF_D3D11_API_FUNCTION_LIST*);}
using namespace fg;
namespace {
Ptr<ID3D11DeviceContext> context;
unsigned width=0,height=0,grid=0,executions=0,registered=0,destroyed=0;
NV_OF_PERF_LEVEL preset=NV_OF_PERF_LEVEL_UNDEFINED;
bool grid2=true,fail=false;
void Check(bool value,const char* why) {if(!value){std::fprintf(stderr,"%s\n",why);std::exit(1);}}
NV_OF_STATUS NVOFAPI Create(ID3D11Device* const device,ID3D11DeviceContext* const ctx,NvOFHandle* handle) {
    context=ctx;*handle=reinterpret_cast<NvOFHandle>(device);return NV_OF_SUCCESS;
}
NV_OF_STATUS NVOFAPI Init(NvOFHandle,const NV_OF_INIT_PARAMS* p) {width=p->width;height=p->height;grid=unsigned(p->outGridSize);preset=p->perfLevel;return NV_OF_SUCCESS;}
NV_OF_STATUS NVOFAPI FormatCount(NvOFHandle,NV_OF_BUFFER_USAGE,NV_OF_MODE,uint32_t* count) {*count=1;return NV_OF_SUCCESS;}
NV_OF_STATUS NVOFAPI Formats(NvOFHandle,NV_OF_BUFFER_USAGE usage,NV_OF_MODE,DXGI_FORMAT* format) {*format=usage==NV_OF_BUFFER_USAGE_INPUT?DXGI_FORMAT_B8G8R8A8_UNORM:DXGI_FORMAT_R16G16_SINT;return NV_OF_SUCCESS;}
NV_OF_STATUS NVOFAPI Register(NvOFHandle,ID3D11Resource* resource,NvOFGPUBufferHandle* handle) {++registered;*handle=reinterpret_cast<NvOFGPUBufferHandle>(resource);return NV_OF_SUCCESS;}
NV_OF_STATUS NVOFAPI Unregister(NvOFGPUBufferHandle) {--registered;return NV_OF_SUCCESS;}
NV_OF_STATUS NVOFAPI Destroy(NvOFHandle) {++destroyed;return NV_OF_SUCCESS;}
NV_OF_STATUS NVOFAPI Caps(NvOFHandle,NV_OF_CAPS,uint32_t* values,uint32_t* count) {if(!values) {*count=grid2?2u:1u;}else {values[0]=4;if(grid2) values[1]=2;}return NV_OF_SUCCESS;}
NV_OF_STATUS NVOFAPI Execute(NvOFHandle,const NV_OF_EXECUTE_INPUT_PARAMS* input,NV_OF_EXECUTE_OUTPUT_PARAMS* output) {
    if(fail) return NV_OF_ERR_GENERIC;
    Check(input->inputFrame!=input->referenceFrame && input->inputFrame && input->referenceFrame,"distinct current and previous inputs");
    const unsigned w=(width+grid-1)/grid,h=(height+grid-1)/grid;
    std::vector<short> flow(w*h*2);
    for(unsigned i=0;i<w*h;++i) {flow[i*2]=64;flow[i*2+1]=-96;}
    context->UpdateSubresource(reinterpret_cast<ID3D11Resource*>(output->outputBuffer),0,nullptr,flow.data(),w*4,0);++executions;return NV_OF_SUCCESS;
}
unsigned short Half(ID3D11Texture2D* texture,unsigned channel) {
    Ptr<ID3D11Device> device;texture->GetDevice(&device);D3D11_TEXTURE2D_DESC d{};texture->GetDesc(&d);
    d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;d.BindFlags=d.MiscFlags=0;
    Ptr<ID3D11Texture2D> staging;Check(SUCCEEDED(device->CreateTexture2D(&d,nullptr,&staging)),"motion staging");
    context->CopyResource(staging.Get(),texture);D3D11_MAPPED_SUBRESOURCE mapped{};
    Check(SUCCEEDED(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped)),"read dense motion");
    auto value=static_cast<const unsigned short*>(mapped.pData)[channel];context->Unmap(staging.Get(),0);return value;
}
}
int main() {
    Ptr<ID3D11Device> device;Ptr<ID3D11DeviceContext> own;
    Check(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&own)),"WARP device");
    NV_OF_D3D11_API_FUNCTION_LIST api{};api.nvCreateOpticalFlowD3D11=Create;api.nvOFInit=Init;api.nvOFGetCaps=Caps;
    api.nvOFGetSurfaceFormatCountD3D11=FormatCount;api.nvOFGetSurfaceFormatD3D11=Formats;api.nvOFRegisterResourceD3D11=Register;
    api.nvOFUnregisterResourceD3D11=Unregister;api.nvOFDestroy=Destroy;api.nvOFExecute=Execute;SetOpticalFlowTestApi(&api);
    for(const auto format:{DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_B8G8R8A8_UNORM,DXGI_FORMAT_R16G16B16A16_FLOAT}) {
        auto desc=TextureDesc(40,24,format,D3D11_BIND_SHADER_RESOURCE);Ptr<ID3D11Texture2D> input;
        Check(SUCCEEDED(device->CreateTexture2D(&desc,nullptr,&input)),"colour input");
        for(unsigned quality=1;quality<=5;++quality) {
            const unsigned before=executions;
            {
                OpticalFlow flow;Check(flow.Init(device.Get(),desc,quality,[](const char*){}),"initialize NVOF pipeline with simulated driver");
                Check(grid==(quality>=4?2u:4u),"quality grid selection");
                bool real=true;Check(flow.Process(input.Get(),true,real) && !real && Half(flow.Motion(),0)==0,"first/reset frame zero motion");
                Check(flow.Process(input.Get(),false,real) && real,"second frame estimated motion");
                Check(Half(flow.Motion(),0)==0x4000 && Half(flow.Motion(),1)==0xc200,"S10.5 decode preserves +2/-3 pixel signs");
                Check(executions==before+1,"one flow evaluation for one pair");
                Check(flow.Process(input.Get(),true,real) && !real && Half(flow.Motion(),1)==0,"history reset clears motion");
                fail=true;Check(!flow.Process(input.Get(),false,real),"driver failure propagated");fail=false;
            }
            Check(registered==0,"all NVOF resources unregistered on teardown");
        }
    }
    grid2=false;OpticalFlow fallback;auto desc=TextureDesc(40,24,DXGI_FORMAT_R8G8B8A8_UNORM,D3D11_BIND_SHADER_RESOURCE);
    Check(fallback.Init(device.Get(),desc,5,[](const char*){}) && fallback.Quality()==3 && grid==4 && preset==NV_OF_PERF_LEVEL_SLOW,"unsupported high-quality grid falls back to matching 4x4 preset");
    std::puts("OF conversion, five profiles, S10.5 decode, reset, failure and teardown passed with simulated driver / real WARP shaders");
}
