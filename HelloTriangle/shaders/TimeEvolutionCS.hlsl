// ============================================================
// TimeEvolutionCS.hlsl
// 海洋スペクトルの時間発展コンピュートシェーダー。
// 分散関係 ω(k)=√(g|k|) を用いて毎フレーム周波数域を更新する。
// ============================================================
// 周波数領域データの時間発展
// 毎フレームh0(k)からh(k,t)を更新する
Texture2D<float4> g_h0 : register(t0); // Phillips初期化結果（読み取り専用）
RWTexture2D<float4> g_hkt : register(u0); // .xy = h(k,t), .zw = Dx(k,t)
RWTexture2D<float4> g_dztMap : register(u1); // .xy = Dz(k,t)


cbuffer TimeCB : register(b0)
{
    uint N;
    float time;
    float pad0, pad1;
};

static const float PI = 3.14159265f;
static const float g = 9.81f;

// 複素数乗算
float2 complexMul(float2 a, float2 b)
{
    return float2(a.x * b.x - a.y * b.y,
                  a.x * b.y + a.y * b.x);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    float2 k;
    static const float L = 400.0f;
    k.x = (float(id.x) - float(N) * 0.5f) * (2.0f * PI / L);
    k.y = (float(id.y) - float(N) * 0.5f) * (2.0f * PI / L);

    float kLen = length(k);
    float omega = sqrt(g * max(kLen, 0.0001f));

    float4 h0data = g_h0[id.xy];
    float2 h0 = h0data.xy;
    float2 h0c = h0data.zw;

    float cosOmega = cos(omega * time);
    float sinOmega = sin(omega * time);
    float2 expPos = float2(cosOmega, sinOmega);
    float2 expNeg = float2(cosOmega, -sinOmega);

    float2 hkt = complexMul(h0, expPos) +
                 complexMul(float2(h0c.x, -h0c.y), expNeg);

    // Dx(k,t) = -i * (kx/|k|) * h(k,t)
    // -iの乗算：(a+bi)*(-i) = (b, -a)
    float2 dxFreq = float2(0.0f, 0.0f);
    float2 dzFreq = float2(0.0f, 0.0f);

    if (kLen > 1e-6f)
    {

        float2 iHkt = float2(hkt.y, -hkt.x); // -i * hkt
        // k.x/kLenとk.y/kLenは単位方向ベクトル。-i*hktを乗算してDxとDzの周波数成分を得る
        dxFreq = (k.x / kLen) * iHkt; // dx = -i * (kx/|k|) * h(k,t)
        dzFreq = (k.y / kLen) * iHkt; // dz = -i * (ky/|k|) * h(k,t)
    }

    uint2 dst = uint2((id.x + (N >> 1)) % N, (id.y + (N >> 1)) % N);
    g_hkt[dst] = float4(hkt.x, hkt.y, dxFreq.x, dxFreq.y);
    g_dztMap[dst] = float4(dzFreq.x, dzFreq.y, 0.0f, 0.0f);

}
