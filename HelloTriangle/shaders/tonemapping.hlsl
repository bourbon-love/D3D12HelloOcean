// ============================================================
// tonemapping.hlsl
// ACESフィルミックトーンマッピングシェーダー。
// HDR→LDR変換、ビネット、フィルムグレイン合成を担当する。
// ============================================================
Texture2D    g_hdr     : register(t0);
Texture2D    g_bloom   : register(t1);
Texture2D    g_godrays : register(t2);
Texture2D    g_ssao    : register(t3);
SamplerState g_sampler : register(s0);

cbuffer ToneCB : register(b0)
{
    float bloomStrength;
    float exposure;
    float godRayStrength;
    float vignetteStrength;
    float grainStrength;
    float time;
    float aoStrength;   // SSAO blend (0=off, 1=full)
    float pad;
};

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD;
};

VSOut ToneMapVS(uint id : SV_VertexID)
{
    VSOut o;
    float2 uv = float2((id & 1) ? 2.0 : 0.0, (id & 2) ? 2.0 : 0.0);
    o.uv  = uv;
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

float3 ACESFilmic(float3 x)
{
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

float4 ToneMapPS(VSOut i) : SV_Target
{
    float3 hdr     = g_hdr.Sample(g_sampler, i.uv).rgb;
    float3 bloom   = g_bloom.Sample(g_sampler, i.uv).rgb;
    float3 godrays = g_godrays.Sample(g_sampler, i.uv).rgb;
    float  ao      = lerp(1.0, g_ssao.Sample(g_sampler, i.uv).r, aoStrength);

    float3 ldr = ACESFilmic(hdr * ao * exposure + bloom * bloomStrength + godrays * godRayStrength);

    // ビネット：中心から滑らかな暗部フォールオフ
    float2 centered = i.uv - 0.5;
    float  vigDist  = dot(centered, centered); // 中心で0、角で0.5
    float  vignette = 1.0 - smoothstep(0.10, 0.65, vigDist) * vignetteStrength;
    ldr *= vignette;

    // フィルムグレイン：アニメーション付きハッシュノイズ、暗部で強め
    float2 gUV  = i.uv * 800.0 + float2(time * 37.0, time * 53.0);
    float  grain = frac(sin(dot(floor(gUV), float2(127.1, 311.7))) * 43758.5453);
    grain = (grain - 0.5) * 2.0;                          // [-1, 1]
    float  lum = dot(ldr, float3(0.2126, 0.7152, 0.0722));
    ldr += grain * grainStrength * (1.0 - lum * 0.6);     // 暗部でグレインを強くする

    return float4(saturate(ldr), 1.0);
}
