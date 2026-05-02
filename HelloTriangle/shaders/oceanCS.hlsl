//oceanCS.hlsl

// 声明UAV：可读写的Texture2D，对应u0寄存器
RWTexture2D<float4> g_heightMap : register(u0);

// 每个线程组8×8×1个线程
// 和Dispatch(32,32,1)配合，总共256×256个线程，每个线程处理一个像素
[numthreads(8, 8, 1)]

void CSMain(uint3 dispatchThreadID : SV_DispatchThreadID)//全局线程ID，即像素坐标
{
    
    uint2 pixel = dispatchThreadID.xy;
    
    // 后续换成真正的IFFT高度计算
    g_heightMap[pixel] = float4(0.0f, 0.3f, 0.8f, 1.0f);
    
}