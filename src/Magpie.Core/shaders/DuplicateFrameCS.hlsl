// A group reduces its differences before publishing a monotonic atomic flag.
RWBuffer<uint> result : register(u0);
cbuffer CompareOptions : register(b0) {
	uint compareAlpha;
};
groupshared uint groupDifferent[64];

Texture2D tex1 : register(t0);
Texture2D tex2 : register(t1);

SamplerState sam : register(s0);

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_GroupThreadID, uint3 gid : SV_GroupID) {
	const int2 gxy = (gid.xy << 4) + (tid.xy << 1);
	
	// 不知为何这比通过 cbuffer 传入更快
	uint width, height;
	tex1.GetDimensions(width, height);
	const float2 pos = (gxy + 1) / float2(width, height);
	
	bool different = any(tex1.GatherRed(sam, pos) != tex2.GatherRed(sam, pos)) ||
		any(tex1.GatherGreen(sam, pos) != tex2.GatherGreen(sam, pos)) ||
		any(tex1.GatherBlue(sam, pos) != tex2.GatherBlue(sam, pos));
	if (compareAlpha != 0) different = different ||
		any(tex1.GatherAlpha(sam, pos) != tex2.GatherAlpha(sam, pos));
	// Each lane owns one flag. Reduce columns without groupshared atomics,
	// then publish one monotonic flag for the complete 16x16 tile. This also
	// works on the older WARP implementation used by Windows Server 2022.
	groupDifferent[tid.y * 8 + tid.x] = different ? 1u : 0u;
	GroupMemoryBarrierWithGroupSync();
	if (tid.y == 0) {
		uint columnDifferent = 0;
		[unroll] for (uint row = 0; row < 8; ++row)
			columnDifferent |= groupDifferent[row * 8 + tid.x];
		groupDifferent[tid.x] = columnDifferent;
	}
	GroupMemoryBarrierWithGroupSync();
	if (all(tid == 0)) {
		uint tileDifferent = 0;
		[unroll] for (uint column = 0; column < 8; ++column)
			tileDifferent |= groupDifferent[column];
		if (tileDifferent != 0) InterlockedOr(result[0], 1u);
	}
}
