#include "optical_flow.h"
#include <nvOpticalFlowD3D11.h>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <array>
#include <cmath>
namespace fg {void SetOpticalFlowTestApi(const NV_OF_D3D11_API_FUNCTION_LIST*);}
using namespace fg;
namespace {
Ptr<ID3D11DeviceContext> context;
unsigned width=0,height=0,grid=0,executions=0,registered=0,destroyed=0;
unsigned minimumWidth=1,minimumHeight=1,fullWidth=0,fullHeight=0;
NV_OF_PERF_LEVEL preset=NV_OF_PERF_LEVEL_UNDEFINED;
bool grid2=true,fail=false,bgra=true,expectedTemporalReset=true;
bool gradient=false,varyingFlow=false;
std::array<unsigned char,4> previousColour{},currentColour{};
void Check(bool value,const char* why) {if(!value){std::fprintf(stderr,"%s\n",why);std::exit(1);}}
NV_OF_STATUS NVOFAPI Create(ID3D11Device* const device,ID3D11DeviceContext* const ctx,NvOFHandle* handle) {
    context=ctx;*handle=reinterpret_cast<NvOFHandle>(device);return NV_OF_SUCCESS;
}
NV_OF_STATUS NVOFAPI Init(NvOFHandle,const NV_OF_INIT_PARAMS* p) {width=p->width;height=p->height;grid=unsigned(p->outGridSize);preset=p->perfLevel;expectedTemporalReset=true;return NV_OF_SUCCESS;}
NV_OF_STATUS NVOFAPI FormatCount(NvOFHandle,NV_OF_BUFFER_USAGE,NV_OF_MODE,uint32_t* count) {*count=1;return NV_OF_SUCCESS;}
NV_OF_STATUS NVOFAPI Formats(NvOFHandle,NV_OF_BUFFER_USAGE usage,NV_OF_MODE,DXGI_FORMAT* format) {*format=usage==NV_OF_BUFFER_USAGE_INPUT?(bgra?DXGI_FORMAT_B8G8R8A8_UNORM:DXGI_FORMAT_R8G8B8A8_UNORM):DXGI_FORMAT_R16G16_SINT;return NV_OF_SUCCESS;}
NV_OF_STATUS NVOFAPI Register(NvOFHandle,ID3D11Resource* resource,NvOFGPUBufferHandle* handle) {++registered;*handle=reinterpret_cast<NvOFGPUBufferHandle>(resource);return NV_OF_SUCCESS;}
NV_OF_STATUS NVOFAPI Unregister(NvOFGPUBufferHandle) {--registered;return NV_OF_SUCCESS;}
NV_OF_STATUS NVOFAPI Destroy(NvOFHandle) {++destroyed;return NV_OF_SUCCESS;}
NV_OF_STATUS NVOFAPI Caps(NvOFHandle,NV_OF_CAPS cap,uint32_t* values,uint32_t* count) {
    if(cap!=NV_OF_CAPS_SUPPORTED_OUTPUT_GRID_SIZES) {
        if(values) {*values=cap==NV_OF_CAPS_WIDTH_MIN?minimumWidth:minimumHeight;}
        *count=1;
    } else if(!values) {*count=grid2?2u:1u;}
    else {Check(*count>=(grid2?2u:1u),"capability buffer bounds");values[0]=4;if(grid2) values[1]=2;}
    return NV_OF_SUCCESS;
}
void Colour(ID3D11Resource* resource,const std::array<unsigned char,4>& expected) {
    Ptr<ID3D11Texture2D> texture;Check(SUCCEEDED(resource->QueryInterface(IID_PPV_ARGS(&texture))),"registered OF colour texture");
    Ptr<ID3D11Device> device;texture->GetDevice(&device);D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);
    const bool isBGRA=desc.Format==DXGI_FORMAT_B8G8R8A8_UNORM;
    Check(desc.Width==width && desc.Height==height,"NVOF input texture matches analysis extent");
    desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;desc.BindFlags=desc.MiscFlags=0;
    Ptr<ID3D11Texture2D> readback;Check(SUCCEEDED(device->CreateTexture2D(&desc,nullptr,&readback)),"OF colour staging");
    context->CopyResource(readback.Get(),texture.Get());D3D11_MAPPED_SUBRESOURCE mapped{};
    Check(SUCCEEDED(context->Map(readback.Get(),0,D3D11_MAP_READ,0,&mapped)),"read converted OF colour");
    for(unsigned y:{0u,height/2,height-1}) for(unsigned x:{0u,width/2,width-1}) {
        const auto* pixel=static_cast<const unsigned char*>(mapped.pData)+y*mapped.RowPitch+x*4;
        auto colour=expected;
        if(gradient) {
            const double sourceX=std::clamp((x+.5)*fullWidth/width-.5,0.0,double(fullWidth-1));
            const double sourceY=std::clamp((y+.5)*fullHeight/height-.5,0.0,double(fullHeight-1));
            colour[0]=static_cast<unsigned char>(std::lround(16+4*sourceX));
            colour[1]=static_cast<unsigned char>(std::lround(32+5*sourceY));
        }
        for(unsigned channel=0;channel<4;++channel) {
            const unsigned order=isBGRA && channel!=1 && channel!=3?2-channel:channel;
            Check(std::abs(int(pixel[order])-int(colour[channel]))<=1,"OF colour channels, HDR tone map, resampling coordinates and edge coverage");
        }
    }
    context->Unmap(readback.Get(),0);
}
NV_OF_STATUS NVOFAPI Execute(NvOFHandle,const NV_OF_EXECUTE_INPUT_PARAMS* input,NV_OF_EXECUTE_OUTPUT_PARAMS* output) {
    if(fail) return NV_OF_ERR_GENERIC;
    Check(input->inputFrame!=input->referenceFrame && input->inputFrame && input->referenceFrame,"distinct current and previous inputs");
    Check(input->disableTemporalHints==(expectedTemporalReset?NV_OF_TRUE:NV_OF_FALSE),"temporal hints disabled for first pair after reset, reused only on continuous pairs");expectedTemporalReset=false;
    Colour(reinterpret_cast<ID3D11Resource*>(input->inputFrame),currentColour);
    Colour(reinterpret_cast<ID3D11Resource*>(input->referenceFrame),previousColour);
    const unsigned w=(width+grid-1)/grid,h=(height+grid-1)/grid;
    std::vector<short> flow(w*h*2);
    for(unsigned y=0;y<h;++y) for(unsigned x=0;x<w;++x) {
        flow[(y*w+x)*2]=short(64+(varyingFlow?x*32:0));flow[(y*w+x)*2+1]=short(-96-(varyingFlow?int(y)*32:0));
    }
    context->UpdateSubresource(reinterpret_cast<ID3D11Resource*>(output->outputBuffer),0,nullptr,flow.data(),w*4,0);++executions;return NV_OF_SUCCESS;
}
void Fill(ID3D11Texture2D* texture,DXGI_FORMAT format,bool second) {
    D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);const unsigned w=desc.Width,h=desc.Height;
    fullWidth=w;fullHeight=h;
    if(format==DXGI_FORMAT_R16G16B16A16_FLOAT) {
        // 3, 1, -1 map to .75, .5, 0 only in the analysis image.
        const std::array<unsigned short,4> colour=second?std::array<unsigned short,4>{0,0x4200,0x3c00,0x3c00}:std::array<unsigned short,4>{0x4200,0x3c00,0xbc00,0x3c00};
        std::vector<unsigned short> bytes(w*h*4);
        for(unsigned pixel=0;pixel<w*h;++pixel) for(unsigned c=0;c<4;++c) bytes[pixel*4+c]=colour[c];
        context->UpdateSubresource(texture,0,nullptr,bytes.data(),w*8,0);
        (second?currentColour:previousColour)=second?std::array<unsigned char,4>{0,191,128,255}:std::array<unsigned char,4>{191,128,0,255};
    } else {
        const std::array<unsigned char,4> colour=gradient?std::array<unsigned char,4>{16,32,static_cast<unsigned char>(second?139:64),255}:
            second?std::array<unsigned char,4>{201,34,11,220}:std::array<unsigned char,4>{64,128,192,21};
        std::vector<unsigned char> bytes(w*h*4);
        for(unsigned y=0;y<h;++y) for(unsigned x=0;x<w;++x) {
            auto pixel=colour;if(gradient) {pixel[0]=static_cast<unsigned char>(16+x*4);pixel[1]=static_cast<unsigned char>(32+y*5);}
            for(unsigned c=0;c<4;++c) bytes[(y*w+x)*4+c]=pixel[format==DXGI_FORMAT_B8G8R8A8_UNORM && c!=1 && c!=3?2-c:c];
        }
        context->UpdateSubresource(texture,0,nullptr,bytes.data(),w*4,0);
        (second?currentColour:previousColour)=colour;(second?currentColour:previousColour)[3]=255;
    }
}
void Motion(ID3D11Texture2D* texture,bool zero=false) {
    Ptr<ID3D11Device> device;texture->GetDevice(&device);D3D11_TEXTURE2D_DESC d{};texture->GetDesc(&d);
    Check(d.Width==fullWidth && d.Height==fullHeight && d.Format==DXGI_FORMAT_R16G16_FLOAT,"dense motion remains at full output resolution");
    d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;d.BindFlags=d.MiscFlags=0;
    Ptr<ID3D11Texture2D> staging;Check(SUCCEEDED(device->CreateTexture2D(&d,nullptr,&staging)),"motion staging");
    context->CopyResource(staging.Get(),texture);D3D11_MAPPED_SUBRESOURCE mapped{};
    Check(SUCCEEDED(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped)),"read dense motion");
    auto decode=[](unsigned short h) {
        const unsigned exponent=(h>>10)&31,mantissa=h&1023;
        const float value=exponent?std::ldexp(1.f+mantissa/1024.f,int(exponent)-15):std::ldexp(float(mantissa),-24);
        return (h&0x8000)?-value:value;
    };
    const double scaleX=double(fullWidth)/width,scaleY=double(fullHeight)/height;
    for(unsigned y:{0u,fullHeight/2,fullHeight-1}) for(unsigned x:{0u,fullWidth/2,fullWidth-1}) {
        const auto* pixel=reinterpret_cast<const unsigned short*>(static_cast<const unsigned char*>(mapped.pData)+y*mapped.RowPitch)+x*2;
        const double cellX=varyingFlow?std::clamp((x+.5)/scaleX/grid-.5,0.0,double((width+grid-1)/grid-1)):0;
        const double cellY=varyingFlow?std::clamp((y+.5)/scaleY/grid-.5,0.0,double((height+grid-1)/grid-1)):0;
        Check(std::abs(decode(pixel[0])-(zero?0:(2+cellX)*scaleX))<.03 &&
            std::abs(decode(pixel[1])-(zero?0:(-3-cellY)*scaleY))<.03,"S10.5 signs, dense coordinates and vectors in full output pixels");
    }
    context->Unmap(staging.Get(),0);
}
}
int main() {
    Ptr<ID3D11Device> device;Ptr<ID3D11DeviceContext> own;
    Check(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&own)),"WARP device");
    NV_OF_D3D11_API_FUNCTION_LIST api{};api.nvCreateOpticalFlowD3D11=Create;api.nvOFInit=Init;api.nvOFGetCaps=Caps;
    api.nvOFGetSurfaceFormatCountD3D11=FormatCount;api.nvOFGetSurfaceFormatD3D11=Formats;api.nvOFRegisterResourceD3D11=Register;
    api.nvOFUnregisterResourceD3D11=Unregister;api.nvOFDestroy=Destroy;api.nvOFExecute=Execute;SetOpticalFlowTestApi(&api);
    for(const auto driverBGRA:{true,false}) for(const auto format:{DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_B8G8R8A8_UNORM,DXGI_FORMAT_R16G16B16A16_FLOAT}) {
        bgra=driverBGRA;
        auto desc=TextureDesc(40,24,format,D3D11_BIND_SHADER_RESOURCE);Ptr<ID3D11Texture2D> input;
        Check(SUCCEEDED(device->CreateTexture2D(&desc,nullptr,&input)),"colour input");
        for(const unsigned scale:{25u,50u,75u,100u}) for(unsigned quality=1;quality<=5;++quality) {
            const unsigned before=executions;
            {
                OpticalFlow flow;Check(flow.Init(device.Get(),desc,quality,[](const char*){},scale),"initialize NVOF pipeline with simulated driver");
                Check(width==AnalysisDimension(desc.Width,scale) && height==AnalysisDimension(desc.Height,scale),"requested analysis scale controls NVOF extent");
                Check(grid==(quality>=4?2u:4u),"quality grid selection");
                Fill(input.Get(),format,false);
                bool real=true;Check(flow.Process(input.Get(),true,real) && !real,"first/reset frame zero motion");Motion(flow.Motion(),true);
                Fill(input.Get(),format,true);
                Check(flow.Process(input.Get(),false,real) && real,"second frame estimated motion");
                Motion(flow.Motion());
                Check(executions==before+1,"one flow evaluation for one pair");
                previousColour=currentColour;
                Check(flow.Process(input.Get(),false,real) && real && executions==before+2,"continuous pair reuses temporal hints");
                Fill(input.Get(),format,false);
                Check(flow.Process(input.Get(),true,real) && !real,"history reset clears motion");Motion(flow.Motion(),true);
                expectedTemporalReset=true;
                fail=true;Check(!flow.Process(input.Get(),false,real),"driver failure propagated");fail=false;
                Check(flow.Process(input.Get(),false,real) && !real,"failed pair reprimes history");
                Fill(input.Get(),format,true);
                Check(flow.Process(input.Get(),false,real) && real,"first pair after failure resets temporal hints");
            }
            Check(registered==0,"all NVOF resources unregistered on teardown");
        }
    }
    // Odd extents require independent X/Y vector scaling. A spatial colour ramp
    // and nonuniform coarse vectors also catch incorrect sampling coordinates.
    gradient=varyingFlow=true;
    for(const bool driverBGRA:{true,false}) for(const unsigned scale:{25u,50u,75u,100u}) {
        bgra=driverBGRA;
        auto desc=TextureDesc(41,25,DXGI_FORMAT_R8G8B8A8_UNORM,D3D11_BIND_SHADER_RESOURCE);Ptr<ID3D11Texture2D> input;
        Check(SUCCEEDED(device->CreateTexture2D(&desc,nullptr,&input)),"odd colour-ramp input");
        OpticalFlow flow;Check(flow.Init(device.Get(),desc,2,[](const char*){},scale),"scaled odd-extent pipeline");
        Fill(input.Get(),desc.Format,false);bool real=false;Check(flow.Process(input.Get(),true,real) && !real,"ramp history");
        Fill(input.Get(),desc.Format,true);Check(flow.Process(input.Get(),false,real) && real,"ramp pair");Motion(flow.Motion());
    }
    gradient=varyingFlow=false;
    minimumWidth=32;minimumHeight=16;
    {
        auto desc=TextureDesc(41,25,DXGI_FORMAT_R8G8B8A8_UNORM,D3D11_BIND_SHADER_RESOURCE);
        OpticalFlow minimum;Check(minimum.Init(device.Get(),desc,2,[](const char*){},25) && width==32 && height==16,"analysis extent respects reported driver minimums");
    }
    minimumWidth=minimumHeight=1;
    grid2=false;OpticalFlow fallback;auto desc=TextureDesc(40,24,DXGI_FORMAT_R8G8B8A8_UNORM,D3D11_BIND_SHADER_RESOURCE);
    Check(fallback.Init(device.Get(),desc,5,[](const char*){}) && fallback.Quality()==3 && grid==4 && preset==NV_OF_PERF_LEVEL_SLOW,"unsupported high-quality grid falls back to matching 4x4 preset");
    std::puts("Verified 25/50/75/100% OF analysis, driver minimums, odd extents, colour sampling, full-resolution vector scaling, RGB/BGR/HDR, five profiles and reset/failure with simulated driver / real WARP shaders");
}
