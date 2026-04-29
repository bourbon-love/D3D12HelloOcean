Texture2D<float4> g_scene : register(t0); // HDR scene color
Texture2D<float>  g_depth : register(t1); // linear NDC depth [0,1]
SamplerState g_sampler : register(s0);

cbuffer DOFCB : register(b0)
{
    float focusDepth;   // NDC depth of focus plane (0=near, 1=far)
    float focusRange;   // half-width of in-focus zone in NDC units
    float maxRadius;    // max CoC radius in UV space
    float aspectRatio;  // width / height
};

struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD; };

VSOut DOFVS(uint id : SV_VertexID)
{
    VSOut o;
    float2 uv = float2((id & 1) ? 2.0 : 0.0, (id & 2) ? 2.0 : 0.0);
    o.uv  = uv;
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

float4 DOFPS(VSOut i) : SV_Target
{
    float2 uv = i.uv;

    float depth = g_depth.SampleLevel(g_sampler, uv, 0).r;
    float coc   = saturate(abs(depth - focusDepth) / focusRange) * maxRadius;

    // Early-out for fully in-focus pixels
    if (coc < 0.0005)
        return g_scene.SampleLevel(g_sampler, uv, 0);

    // Vogel-disc sampling (golden-angle spiral) — 20 samples
    static const int N = 20;
    float3 color = 0;

    [unroll]
    for (int k = 0; k < N; k++)
    {
        float angle = k * 2.3999632; // golden angle ≈ 2.4 rad
        float r     = sqrt((k + 0.5) / N);
        float2 off  = float2(cos(angle), sin(angle)) * (r * coc);
        off.x      /= aspectRatio;           // correct for non-square screen

        color += g_scene.SampleLevel(g_sampler, saturate(uv + off), 0).rgb;
    }
    color /= N;

    // Blend with center sample based on CoC magnitude (preserve sharp center)
    float3 center = g_scene.SampleLevel(g_sampler, uv, 0).rgb;
    float  blend  = saturate(coc / (maxRadius * 0.5));
    return float4(lerp(center, color, blend), 1.0);
}
