// ============================================================
// IFFTCS.hlsl
// Cooley-Tukey基数2 IFFTコンピュートシェーダー。
// 水平・垂直各軸でピンポンバッファを使用する。
// ============================================================
// IFFTCS.hlsl - Stockham FFT (2D, Ping-Pong)
RWTexture2D<float4> g_pingpong0 : register(u0); // hktMap
RWTexture2D<float4> g_pingpong1 : register(u1); // tempMap

cbuffer IFFTCB : register(b0)
{
    uint N;
    uint passIdx; // 0=水平パス、1=垂直パス
    uint stepSize; // 現在のステップサイズ 2,4,8,...,N
    uint pingpong; // 0=pp0読み取りpp1書き込み、1=pp1読み取りpp0書き込み
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

    // Stockhamのコア：
    // 各スレッドは出力の1要素を計算する
    // halfStep = stepSize/2
    // stepSize個の要素をグループに分け、それを2つに分割
    // 前半はsrcAを読み、後半はsrcB = srcA + halfStepを読む

    uint halfStep = stepSize >> 1;

    if (passIdx == 0) // 水平パス
    {
        // 現在のスレッドが属するグループとグループ内位置
        uint groupIdx = x / stepSize;
        uint idxInStep = x % stepSize;

        if (idxInStep >= halfStep)
            return;

        uint idxInHalf = idxInStep;
        // Stockhamのソースアドレス
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

        // 回転因子（IFFTでは正の角度を使用）
        float angle = 2.0f * PI * float(idxInHalf) / float(stepSize);
        float2 W = float2(cos(angle), sin(angle));

        // 高さバタフライ演算
        float2 Wb_h = complexMul(W, vb.xy);
        float2 outA = va.xy + Wb_h;
        float2 outB = va.xy - Wb_h;

        // DXバタフライ演算
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
    else // 垂直パス：X/Y対称
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
