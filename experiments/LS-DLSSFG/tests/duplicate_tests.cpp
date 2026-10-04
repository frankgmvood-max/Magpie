#include "duplicates.h"
#include <vector>
#include <cstdio>
#include <cstdlib>
using namespace fg;
void Check(bool value,const char* message) {if(!value){std::fprintf(stderr,"%s\n",message);std::exit(1);}}
int main() {
    Ptr<ID3D11Device> device;Ptr<ID3D11DeviceContext> context;
    Check(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context)),"WARP device");
    for(const auto format:{DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_B8G8R8A8_UNORM,DXGI_FORMAT_R16G16B16A16_FLOAT}) {
        auto desc=TextureDesc(33,19,format,D3D11_BIND_SHADER_RESOURCE);Ptr<ID3D11Texture2D> input;
        const unsigned bpp=format==DXGI_FORMAT_R16G16B16A16_FLOAT?8u:4u;
        std::vector<unsigned char> pixels(33*19*bpp,0);
        Check(SUCCEEDED(device->CreateTexture2D(&desc,nullptr,&input)),"input texture");
        context->UpdateSubresource(input.Get(),0,nullptr,pixels.data(),33*bpp,0);
        DuplicateFilter filter;Check(filter.Init(device.Get(),desc),"duplicate shader/resources");
        Check(filter.Check(context.Get(),input.Get())==DuplicateResult::NewFrame,"first frame new");
        for(int i=0;i<20;++i) Check(filter.Check(context.Get(),input.Get())==DuplicateResult::Duplicate,"repeated exact frame");
        // Last pixel in a non-multiple-of-16 image: sampling/hashing cannot pass.
        pixels[(33*19-1)*bpp]=format==DXGI_FORMAT_R16G16B16A16_FLOAT?0x00:0x01;
        if(bpp==8) pixels[(33*19-1)*bpp+1]=0x3c; // half float 1.0, red only
        context->UpdateSubresource(input.Get(),0,nullptr,pixels.data(),33*bpp,0);
        Check(filter.Check(context.Get(),input.Get())==DuplicateResult::NewFrame,"single changed edge RGB pixel");
        Check(filter.Check(context.Get(),input.Get())==DuplicateResult::Duplicate,"changed frame becomes reference");
        pixels[bpp-1]=0x40;context->UpdateSubresource(input.Get(),0,nullptr,pixels.data(),33*bpp,0);
        Check(filter.Check(context.Get(),input.Get())==DuplicateResult::Duplicate,"alpha-only change ignored like Magpie RGB filter");
        filter.Reset();Check(filter.Check(context.Get(),input.Get())==DuplicateResult::NewFrame,"explicit reset primes fresh reference");
    }
    std::puts("exact GPU duplicate detection: RGBA8/BGRA8/scRGB, edges, alpha and history passed");
}
