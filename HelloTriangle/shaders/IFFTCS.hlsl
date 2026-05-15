// ============================================================
// IFFTCS.hlsl
// Cooley-Tukey radix-2 IFFT compute shader.
// Uses ping-pong buffers for each horizontal and vertical axis pass.
// ============================================================
// IFFTCS.hlsl - Stockham FFT (2D, Ping-Pong)
RWTexture2D<float4> g_pingpong0 : register(u0); // hktMap
RWTexture2D<float4> g_pingpong1 : register(u1); // tempMap

cbuffer IFFTCB : register(b0)
{
    uint N;
    uint passIdx; // 0=horizontal pass, 1=vertical pass
    uint stepSize; // current step size 2,4,8,...,N
    uint pingpong; // 0=read pp0 write pp1, 1=read pp1 write pp0
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

    // Stockham core:
    // Each thread computes one output element.
    // halfStep = stepSize/2
    // Divide elements into groups of stepSize, then split each group in two.
    // The first half reads srcA; the second half reads srcB = srcA + halfStep.

    uint halfStep = stepSize >> 1;

    if (passIdx == 0) // horizontal pass
    {
        // Group index and position within the group for the current thread
        uint groupIdx = x / stepSize;
        uint idxInStep = x % stepSize;

        if (idxInStep >= halfStep)
            return;

        uint idxInHalf = idxInStep;
        // Stockham source addresses
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

        // Twiddle factor (positive angle for IFFT)
        float angle = 2.0f * PI * float(idxInHalf) / float(stepSize);
        float2 W = float2(cos(angle), sin(angle));

        // Height butterfly operation
        float2 Wb_h = complexMul(W, vb.xy);
        float2 outA = va.xy + Wb_h;
        float2 outB = va.xy - Wb_h;

        // DX butterfly operation
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
    else // vertical pass: symmetric with X/Y swapped
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
