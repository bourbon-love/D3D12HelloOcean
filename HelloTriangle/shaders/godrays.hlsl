// ============================================================
// godrays.hlsl
// 体積光（ゴッドレイ）シェーダー。
// 太陽スクリーン位置に向けた64サンプルの放射状ブラーで光条を生成する。
// ============================================================
Texture2D    g_hdr     : register(t0);
SamplerState g_sampler : register(s0);

cbuffer GodRayCB : register(b0)
{
    float2 sunScreenPos;  // スクリーン空間[0,1]での太陽UV
    float  density;       // 太陽方向へのステップスケール
    float  decay;         // サンプルごとの減衰量
    float  weight;        // サンプルごとの輝度
    float  sunVisibility; // 0=夜/画外、1=完全に見える
};

struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD; };

VSOut GodRayVS(uint id : SV_VertexID)
{
    VSOut o;
    float2 uv = float2((id & 1) ? 2.0 : 0.0, (id & 2) ? 2.0 : 0.0);
    o.uv  = uv;
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

float4 GodRayPS(VSOut i) : SV_Target
{
    if (sunVisibility <= 0.001) return float4(0, 0, 0, 0);

    static const int NUM_SAMPLES = 64;
    float2 delta = (i.uv - sunScreenPos) * (density / NUM_SAMPLES);
    float2 uv    = i.uv;
    float3 color = 0;
    float  decay_acc = 1.0;

    [unroll(64)]
    for (int j = 0; j < NUM_SAMPLES; j++)
    {
        uv -= delta;
        float3 s = g_hdr.SampleLevel(g_sampler, saturate(uv), 0).rgb;
        // 明るい空の領域のみが寄与する（暗い海洋を閾値でカット）
        float lum = dot(s, float3(0.2126, 0.7152, 0.0722));
        s *= saturate((lum - 0.4) / 0.6);
        color     += s * decay_acc * weight;
        decay_acc *= decay;
    }

    return float4(color * sunVisibility, 1.0);
}
