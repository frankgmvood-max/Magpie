#pragma once

namespace Magpie {

inline constexpr char DENSIFY_FLOW_HLSL[] = R"(
Texture2D<int2> ForwardFlow : register(t0);
Texture2D<int2> BackwardFlow : register(t1);
Texture2D<uint> ForwardCost : register(t2);
Texture2D<uint> BackwardCost : register(t3);
RWTexture2D<float2> DenseMotion : register(u0);
RWTexture2D<float> DenseConfidence : register(u1);

cbuffer Params : register(b0) {
    uint2 SourceExtent;
    uint2 AnalysisExtent;
    uint2 FlowExtent;
    uint GridSize;
    uint HasForwardCost;
    uint HasBackward;
    uint HasBackwardCost;
    uint2 Reserved;
};

float2 LoadFlow(Texture2D<int2> field, int2 p) {
    p = clamp(p, int2(0, 0), int2(FlowExtent) - 1);
    return float2(field.Load(int3(p, 0))) / 32.0;
}

float2 SampleFlow(Texture2D<int2> field, float2 sourcePixel) {
    float2 gridPos = sourcePixel / float(GridSize) - 0.5;
    int2 p0 = int2(floor(gridPos));
    float2 f = frac(gridPos);
    return lerp(
        lerp(LoadFlow(field, p0), LoadFlow(field, p0 + int2(1, 0)), f.x),
        lerp(LoadFlow(field, p0 + int2(0, 1)),
             LoadFlow(field, p0 + int2(1, 1)), f.x),
        f.y);
}

float LoadCost(Texture2D<uint> field, int2 p) {
    p = clamp(p, int2(0, 0), int2(FlowExtent) - 1);
    return float(field.Load(int3(p, 0))) / 255.0;
}

float SampleCost(Texture2D<uint> field, float2 sourcePixel) {
    float2 gridPos = sourcePixel / float(GridSize) - 0.5;
    int2 p0 = int2(floor(gridPos));
    float2 f = frac(gridPos);
    return lerp(
        lerp(LoadCost(field, p0), LoadCost(field, p0 + int2(1, 0)), f.x),
        lerp(LoadCost(field, p0 + int2(0, 1)),
             LoadCost(field, p0 + int2(1, 1)), f.x),
        f.y);
}

[numthreads(8, 8, 1)]
void Densify(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= SourceExtent)) return;

    float2 toSourcePixels = float2(SourceExtent) / float2(AnalysisExtent);
    float2 p = (float2(tid.xy) + 0.5) / toSourcePixels;
    float2 forward = SampleFlow(ForwardFlow, p);
    // Turing has no hardware cost output. A conservative non-zero baseline
    // still lets downstream consumers use coherent motion while reset frames
    // remain explicitly zero-confidence.
    float confidence = HasForwardCost != 0 ?
        1.0 - SampleCost(ForwardCost, p) : 0.65;

    if (HasBackward != 0) {
        float2 referencePixel = p + forward;
        bool inside = all(referencePixel >= 0.0) &&
            all(referencePixel < float2(AnalysisExtent));
        float2 backward = SampleFlow(BackwardFlow, referencePixel);
        float fbError = length(forward + backward);
        float threshold = 0.75 + 0.05 * length(forward);
        confidence *= inside ? saturate(1.0 - fbError / threshold) : 0.0;
        if (HasBackwardCost != 0) {
            confidence *= 1.0 - SampleCost(BackwardCost, referencePixel);
        }
    }

    DenseMotion[tid.xy] = forward * toSourcePixels;
    DenseConfidence[tid.xy] = saturate(confidence);
}
)";


inline constexpr char NVOF_INPUT_HLSL[] = R"(
Texture2D<float4> Source : register(t0);
SamplerState LinearClamp : register(s0);
cbuffer InputParams : register(b0) { uint2 AnalysisExtent; uint IsHdr; uint Reserved; };
float4 InputVS(uint vertex : SV_VertexID) : SV_Position {
    float2 p = float2((vertex << 1) & 2, vertex & 2);
    return float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 InputPS(float4 position : SV_Position) : SV_Target {
    float4 value = Source.SampleLevel(LinearClamp, position.xy / float2(AnalysisExtent), 0);
    if (IsHdr != 0) value.rgb = max(value.rgb, 0.0) / 4.5;
    return float4(saturate(value.rgb), 1);
}
)";

}
