// ============================================================
// taa.hlsl
// 時域アンチエイリアシング（TAA）リゾルブシェーダー。
// 3×3近傍AABBクランプとフレーム間ブレンドを行う。
// ============================================================
Texture2D    g_current : register(t0);
Texture2D    g_history : register(t1);
SamplerState g_point   : register(s0);
SamplerState g_linear  : register(s1);

cbuffer TAACB : register(b0)
{
    float2 texelSize;   // 1/width, 1/height
    float  blendFactor; // 履歴ブレンド係数（0.9 = 90%履歴）
    float  pad;
};

struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD; };

VSOut TAAVS(uint id : SV_VertexID)
{
    VSOut o;
    float2 uv = float2((id & 1) ? 2.0 : 0.0, (id & 2) ? 2.0 : 0.0);
    o.uv  = uv;
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

// 履歴サンプルを近傍のAABBにクランプする（ゴーストアーティファクトを防ぐ）
float3 ClipAABB(float3 q, float3 aabbMin, float3 aabbMax)
{
    float3 p = 0.5 * (aabbMax + aabbMin);
    float3 e = 0.5 * (aabbMax - aabbMin) + 1e-5;
    float3 v = q - p;
    float3 a = abs(v / e);
    float  ma = max(a.x, max(a.y, a.z));
    return ma > 1.0 ? p + v / ma : q;
}

float4 TAAPS(VSOut i) : SV_Target
{
    float2 uv = i.uv;

    // 3×3近傍のAABBを計算する
    float3 nMin = float3( 1e10,  1e10,  1e10);
    float3 nMax = float3(-1e10, -1e10, -1e10);
    float3 center = 0;

    [unroll] for (int y = -1; y <= 1; y++)
    [unroll] for (int x = -1; x <= 1; x++)
    {
        float3 s = g_current.SampleLevel(g_point, uv + float2(x, y) * texelSize, 0).rgb;
        nMin = min(nMin, s);
        nMax = max(nMax, s);
        if (x == 0 && y == 0) center = s;
    }

    // 履歴をAABBにクランプしてゴーストを防ぐ
    float3 history = g_history.SampleLevel(g_linear, uv, 0).rgb;
    history = ClipAABB(history, nMin, nMax);

    return float4(lerp(center, history, blendFactor), 1.0);
}
