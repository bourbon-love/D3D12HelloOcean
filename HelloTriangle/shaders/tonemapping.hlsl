Texture2D    g_hdr     : register(t0); // HDR scene
Texture2D    g_bloom   : register(t1); // blurred bright areas
Texture2D    g_godrays : register(t2); // volumetric light shafts
SamplerState g_sampler : register(s0);

cbuffer ToneCB : register(b0)
{
    float bloomStrength;
    float exposure;
    float godRayStrength;
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

// ACES Filmic tone mapping approximation
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
    float3 ldr = ACESFilmic(hdr * exposure + bloom * bloomStrength + godrays * godRayStrength);
    return float4(ldr, 1.0);
}
