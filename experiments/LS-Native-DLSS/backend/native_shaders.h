#pragma once
namespace ls_native::shaders {
constexpr char input[] = R"(
Texture2D<float4> image : register(t0); SamplerState linearClamp : register(s0);
cbuffer Size : register(b0) {uint width;uint height;uint2 unused;float4 ignored;};
float4 vs(uint vertex:SV_VertexID):SV_Position {
 float2 p=float2((vertex<<1)&2,vertex&2);return float4(p*float2(2,-2)+float2(-1,1),0,1);
}
float4 ps(float4 position:SV_Position):SV_Target {
 return float4(saturate(image.SampleLevel(linearClamp,position.xy/float2(width,height),0).rgb),1);
})";
constexpr char densify[] = R"(
Texture2D<int2> coarse : register(t0); RWTexture2D<float2> motion : register(u0);
cbuffer Layout : register(b0) {uint width;uint height;uint grid;uint padding;float2 toPixels;float2 reserved;};
float2 at(int2 p) {uint w,h;coarse.GetDimensions(w,h);return float2(coarse.Load(int3(clamp(p,0,int2(w,h)-1),0)))/32.0;}
[numthreads(8,8,1)] void main(uint3 p:SV_DispatchThreadID) {
 if(p.x>=width||p.y>=height)return;float2 loc=(float2(p.xy)+0.5)/toPixels/grid-0.5;
 int2 cell=int2(floor(loc));float2 f=frac(loc);
 motion[p.xy]=lerp(lerp(at(cell),at(cell+int2(1,0)),f.x),lerp(at(cell+int2(0,1)),at(cell+int2(1,1)),f.x),f.y)*toPixels;
})";
constexpr char flag[] = R"(
ByteAddressBuffer disabled : register(t0); RWTexture2D<uint> result : register(u0);
[numthreads(1,1,1)] void main(uint3 p:SV_DispatchThreadID) {result[uint2(0,0)]=disabled.Load(0);}
)";
constexpr char composite[] = R"(
Texture2D<float4> generated : register(t0); Texture2D<uint> disabled : register(t1);
RWTexture2D<float4> nativeOutput : register(u0);
[numthreads(16,16,1)] void main(uint3 p:SV_DispatchThreadID) {
 uint w,h;nativeOutput.GetDimensions(w,h);
 if(p.x>=w||p.y>=h||disabled.Load(int3(0,0,0))!=0)return;
 nativeOutput[p.xy]=generated.Load(int3(p.xy,0));
})";
#ifdef LS_NATIVE_TEST
// Only the WARP regression executable can compile/use this non-NVIDIA provider.
constexpr char synthetic[] = R"(
Texture2D<float4> previous : register(t0);Texture2D<float4> current : register(t1);
RWTexture2D<float4> result : register(u0);
[numthreads(8,8,1)] void main(uint3 p:SV_DispatchThreadID) {
 uint w,h;result.GetDimensions(w,h);if(p.x>=w||p.y>=h)return;
 result[p.xy]=(previous.Load(int3(p.xy,0))+current.Load(int3(p.xy,0)))*0.5;
})";
#endif
} // namespace ls_native::shaders
