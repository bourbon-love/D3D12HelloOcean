// ============================================================
// tonemapping.hlsl
// ACES tone mapping + vignette + film grain + underwater camera effect
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
    float aoStrength;
    float pad;
    float cameraY;    // world Y of camera (negative = underwater)
    float pad2;
    float pad3;
    float pad4;
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
    // Underwater blend factor (0=above surface, 1=fully underwater, saturates at 2m)
    float uwBlend = saturate(-cameraY / 2.0);
    float uwDepth = max(0.0, -cameraY);

    // Underwater: apply surface wave refraction shimmer to HDR sample UV
    float2 sampleUV = i.uv;
    if (uwBlend > 0.001)
    {
        float2 warp = float2(
            sin(i.uv.y * 22.0 + time * 2.4) * 0.009,
            cos(i.uv.x * 18.0 + time * 1.8) * 0.007
        ) * uwBlend;
        sampleUV = clamp(i.uv + warp, 0.001, 0.999);
    }

    float3 hdr     = g_hdr.Sample(g_sampler, sampleUV).rgb;
    float3 bloom   = g_bloom.Sample(g_sampler, i.uv).rgb;
    float3 godrays = g_godrays.Sample(g_sampler, i.uv).rgb;
    float  ao      = lerp(1.0, g_ssao.Sample(g_sampler, i.uv).r, aoStrength);

    float3 ldr = ACESFilmic(hdr * ao * exposure + bloom * bloomStrength + godrays * godRayStrength);

    // Vignette
    float2 centered = i.uv - 0.5;
    float  vigDist  = dot(centered, centered);
    float  vignette = 1.0 - smoothstep(0.10, 0.65, vigDist) * vignetteStrength;
    ldr *= vignette;

    // Film grain
    float2 gUV  = i.uv * 800.0 + float2(time * 37.0, time * 53.0);
    float  grain = frac(sin(dot(floor(gUV), float2(127.1, 311.7))) * 43758.5453);
    grain = (grain - 0.5) * 2.0;
    float  lum = dot(ldr, float3(0.2126, 0.7152, 0.0722));
    ldr += grain * grainStrength * (1.0 - lum * 0.6);

    // ----------------------------------------------------------
    // Underwater camera effects
    // ----------------------------------------------------------
    if (uwBlend > 0.001)
    {
        // Beer-Lambert color absorption (clear tropical ocean coefficients).
        // Previous values (0.28/0.08/0.025) modelled turbid coastal water:
        // at 15 m, red was exp(-4.2)=0.015 — essentially black, destroying
        // any warm-coloured bioluminescence. Clear ocean values (0.04/0.015/0.006)
        // still produce a strong blue-green tint while keeping colours readable.
        float3 absorb = float3(
            exp(-0.04  * uwDepth),   // red:   at 15 m → 0.55 (was 0.015)
            exp(-0.015 * uwDepth),   // green: at 15 m → 0.80 (was 0.30)
            exp(-0.006 * uwDepth));  // blue:  at 15 m → 0.91 (was 0.69)
        ldr *= lerp(float3(1.0, 1.0, 1.0), absorb, uwBlend);

        // Deep ocean fog. Reduced density (0.020) so it doesn't replace 74% of
        // the image with flat blue at 15 m (old 0.09 gave fogAmt = 0.74 there).
        float3 fogColor = float3(0.01, 0.07, 0.18);
        float  fogAmt   = 1.0 - exp(-uwDepth * 0.020);
        ldr = lerp(ldr, fogColor, fogAmt * uwBlend);

        // Caustics (rippling light patterns from the water surface; stronger at shallow depths)
        float causticStr = uwBlend * saturate(1.0 - uwDepth * 0.18);
        if (causticStr > 0.001)
        {
            float2 cuv = i.uv * float2(9.0, 11.0);
            float  c   = sin(cuv.x * 2.1 + sin(cuv.y * 1.8 + time * 1.2) * 1.6 + time * 2.0)
                       * sin(cuv.y * 2.4 + sin(cuv.x * 1.6 + time * 0.9) * 1.4 + time * 1.6);
            c = pow(saturate(c * 0.5 + 0.5), 4.0);
            ldr += c * 0.28 * causticStr;
        }

        // Enhanced underwater vignette (darker periphery)
        float uwVig = smoothstep(0.15, 0.60, vigDist) * 0.5 * uwBlend;
        ldr *= (1.0 - uwVig);

        // Shift the overall color slightly toward blue-green
        float3 uwTint = float3(0.75, 0.92, 1.08);
        ldr *= lerp(float3(1.0, 1.0, 1.0), uwTint, uwBlend * 0.35);
    }

    return float4(saturate(ldr), 1.0);
}
