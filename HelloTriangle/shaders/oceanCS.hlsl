//oceanCS.hlsl

// Declare UAV: read-write Texture2D bound to register u0
RWTexture2D<float4> g_heightMap : register(u0);

// 8x8x1 threads per thread group.
// Combined with Dispatch(32,32,1): 256x256 total threads, each handling one pixel.
[numthreads(8, 8, 1)]

void CSMain(uint3 dispatchThreadID : SV_DispatchThreadID) // global thread ID = pixel coordinate
{

    uint2 pixel = dispatchThreadID.xy;

    // To be replaced with the actual IFFT height computation
    g_heightMap[pixel] = float4(0.0f, 0.3f, 0.8f, 1.0f);

}