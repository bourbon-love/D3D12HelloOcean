Texture2D    g_source  : register(t0);
SamplerState g_sampler : register(s0);

cbuffer BloomCB : register(b0)
{
    float threshold;  // bright-pass threshold
    float strength;   // composite multiply
    float blurDirX;   // 1=horizontal, 0=vertical
    float blurDirY;   // 0=horizontal, 1=vertical
};

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD;
};

// Fullscreen triangle (no vertex buffer required)
VSOut BloomVS(uint id : SV_VertexID)
{
    VSOut o;
    float2 uv = float2((id & 1) ? 2.0 : 0.0, (id & 2) ? 2.0 : 0.0);
    o.uv  = uv;
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

// Extract pixels brighter than threshold
float4 BrightPassPS(VSOut i) : SV_Target
{
    float4 c   = g_source.Sample(g_sampler, i.uv);
    float  lum = dot(c.rgb, float3(0.2126, 0.7152, 0.0722));
    float  t   = max(1.0 - threshold, 0.001);
    float  ext = saturate((lum - threshold) / t);
    return float4(c.rgb * ext, 1.0);
}

// Separable 9-tap Gaussian blur
float4 BlurPS(VSOut i) : SV_Target
{
    uint w, h;
    g_source.GetDimensions(w, h);
    float2 step = float2(blurDirX / (float)w, blurDirY / (float)h);

    // sigma ≈ 1.5, weights normalised to 1
    static const float k[9] = {
        0.0076, 0.0361, 0.1096, 0.2134, 0.2666,
        0.2134, 0.1096, 0.0361, 0.0076
    };
    float4 result = 0;
    [unroll]
    for (int j = -4; j <= 4; j++)
        result += g_source.Sample(g_sampler, i.uv + step * j) * k[j + 4];
    return result;
}

// Additive blend of bloom onto scene (SrcBlend=ONE, DestBlend=ONE)
float4 CompositePS(VSOut i) : SV_Target
{
    return g_source.Sample(g_sampler, i.uv) * strength;
}
