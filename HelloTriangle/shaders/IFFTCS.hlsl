// IFFTCS.hlsl - Stockham FFT (2D, Ping-Pong)
RWTexture2D<float4> g_pingpong0 : register(u0); // hktMap
RWTexture2D<float4> g_pingpong1 : register(u1); // tempMap

cbuffer IFFTCB : register(b0)
{
    uint N;
    uint passIdx; // 0=水平, 1=垂直
    uint stepSize; // 当前步长 2,4,8,...,N
    uint pingpong; // 0=读pp0写pp1, 1=读pp1写pp0
};

static const float PI = 3.14159265358979f;

float2 complexMul(float2 a, float2 b)
{
    return float2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    uint x = id.x;
    uint y = id.y;

    // Stockham的核心：
    // 每个线程负责计算输出的一个元素
    // halfStep = stepSize/2
    // 每组stepSize个元素，分成两半
    // 前半段读srcA，后半段读srcB = srcA + halfStep

    uint halfStep = stepSize >> 1;

    if (passIdx == 0) // 水平Pass
    {
        // 当前线程在哪一组，组内位置是什么
        uint groupIdx = x / stepSize;
        uint idxInStep = x % stepSize;
        
        if (idxInStep >= halfStep)
            return;
    
        uint idxInHalf = idxInStep;
        // Stockham源地址
        uint srcA = groupIdx * halfStep + idxInHalf;
        uint srcB = srcA + (N >> 1);

        float4 va, vb;
        if (pingpong == 0)
        {
            va = g_pingpong0[uint2(srcA, y)];
            vb = g_pingpong0[uint2(srcB, y)];
        }
        else
        {
            va = g_pingpong1[uint2(srcA, y)];
            vb = g_pingpong1[uint2(srcB, y)];
        }

        // Twiddle factor (IFFT用正角)
        float angle = 2.0f * PI * float(idxInHalf) / float(stepSize);
        float2 W = float2(cos(angle), sin(angle));

        //高度蝶形计算
        float2 Wb_h = complexMul(W, vb.xy);
        float2 outA = va.xy + Wb_h;
        float2 outB = va.xy - Wb_h;

        // DX蝶形计算
        float2 Wb_dx = complexMul(W, vb.zw);
        float2 outA_dx = va.zw + Wb_dx;
        float2 outB_dx = va.zw - Wb_dx;

        uint dstA = groupIdx * stepSize + idxInHalf;
        uint dstB = dstA + halfStep;

        if (pingpong == 0)
        {
            g_pingpong1[uint2(dstA, y)] = float4(outA, outA_dx);
            g_pingpong1[uint2(dstB, y)] = float4(outB, outB_dx);
        }
        else
        {
            g_pingpong0[uint2(dstA, y)] = float4(outA, outA_dx);
            g_pingpong0[uint2(dstB, y)] = float4(outB, outB_dx);
        }
    }
    else // 垂直Pass，X/Y对称
    {
        uint groupIdx = y / stepSize;
        uint idxInStep = y % stepSize;
        
        if (idxInStep >= halfStep)
            return;
    
        uint idxInHalf = idxInStep;
        uint srcA = groupIdx * halfStep + idxInHalf;
        uint srcB = srcA + (N >> 1);

        float4 va, vb;
        if (pingpong == 0)
        {
            va = g_pingpong0[uint2(x, srcA)];
            vb = g_pingpong0[uint2(x, srcB)];
        }
        else
        {
            va = g_pingpong1[uint2(x, srcA)];
            vb = g_pingpong1[uint2(x, srcB)];
        }

        float angle = 2.0f * PI * float(idxInHalf) / float(stepSize);
        float2 W = float2(cos(angle), sin(angle));
        
        float2 Wb_h = complexMul(W, vb.xy);
        float2 outA_h = va.xy + Wb_h;
        float2 outB_h = va.xy - Wb_h;

        float2 Wb_dx = complexMul(W, vb.zw);
        float2 outA_dx = va.zw + Wb_dx;
        float2 outB_dx = va.zw - Wb_dx;
        
        uint dstA = groupIdx * stepSize + idxInHalf;
        uint dstB = dstA + halfStep;

        
        if (pingpong == 0)
        {
            g_pingpong1[uint2(x, dstA)] = float4(outA_h, outA_dx);
            g_pingpong1[uint2(x, dstB)] = float4(outB_h, outB_dx);
        }
        else
        {
            g_pingpong0[uint2(x, dstA)] = float4(outA_h, outA_dx);
            g_pingpong0[uint2(x, dstB)] = float4(outB_h, outB_dx);
        }
    }
}