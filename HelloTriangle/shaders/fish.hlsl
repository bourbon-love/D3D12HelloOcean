// fish.hlsl - Underwater fish school instancing shader (Boids CPU simulation + GPU batched rendering)

struct FishInstance
{
    float3 pos;     float phase;   // 16
    float3 forward; float speed;   // 16
    float3 up;      float pad;     // 16
    float4 color;                  // 16
};

StructuredBuffer<FishInstance> g_instances : register(t0);
TextureCube                    g_prefilter : register(t1);  // GGX prefiltered specular cubemap (IBL Phase C)
Texture2D                      g_brdfLUT   : register(t2);  // BRDF integration LUT (IBL Phase A)
SamplerState                   g_sampler   : register(s0);

#include "SkyIBL.hlsli"

static const uint PREFILTER_MIPS = 6;

cbuffer SceneCB : register(b0)
{
    float4x4 viewProj;
    float3   sunDir;      float sunIntensity;
    float3   sunColor;    float time;
    float3   cameraPos;   float pad_cb;
};

cbuffer SHCB : register(b1)
{
    float4 shCoeffs[9];
    float4 _shPad[7];
};

struct VSIn
{
    float3 pos    : POSITION;
    float3 normal : NORMAL;
    uint   instID : SV_InstanceID;
};

struct VSOut
{
    float4 svpos    : SV_POSITION;
    float3 worldPos : TEXCOORD0;
    float3 normal   : TEXCOORD1;
    float4 color    : TEXCOORD2;
};

VSOut FishVS(VSIn v)
{
    FishInstance inst = g_instances[v.instID];

    float3 fwd   = normalize(inst.forward);
    float3 up    = normalize(inst.up);
    float3 right = normalize(cross(up, fwd));

    // Tail fin sway (applied only to rear vertices; amplitude grows with t^2 weight)
    float3 lp = v.pos;
    if (lp.x < 0.0)
    {
        float t    = saturate(-lp.x * 2.0);
        float bend = sin(time * 6.5 + inst.phase) * 0.28 * t * t;
        lp.z += bend;
    }

    float3 wp = inst.pos
              + lp.x * fwd
              + lp.y * up
              + lp.z * right;

    float3 wn = normalize(v.normal.x * fwd + v.normal.y * up + v.normal.z * right);

    VSOut o;
    o.svpos    = mul(float4(wp, 1.0), viewProj);
    o.worldPos = wp;
    o.normal   = wn;
    o.color    = inst.color;
    return o;
}

float4 FishPS(VSOut p) : SV_TARGET
{
    float3 N = normalize(p.normal);
    float3 L = normalize(sunDir);

    float  diffuse = max(0.0, dot(N, L)) * sunIntensity;
    float3 irradiance = max(EvalSH(N, shCoeffs), 0.0);
    float3 ambCol  = irradiance / PI;
    float  rim     = max(0.0, dot(-N, L)) * sunIntensity * 0.20;

    // Beer-Lambert water absorption — matches tonemapping.hlsl clear-ocean coefficients.
    float  depth  = max(0.0, -p.worldPos.y);
    float3 absorb = float3(
        exp(-0.07  * depth),
        exp(-0.025 * depth),
        exp(-0.010 * depth));

    float3 col = p.color.rgb * (ambCol + (diffuse + rim) * sunColor) * absorb;

    // Specular IBL via Karis split-sum (fish scales: roughness=0.50, F0=0.03)
    {
        float3 V_fish   = normalize(cameraPos - p.worldPos);
        float3 R_fish   = reflect(-V_fish, N);
        float  NdotV_f  = max(dot(N, V_fish), 0.0001);
        float3 prefilt  = g_prefilter.SampleLevel(g_sampler, R_fish, 0.50 * (PREFILTER_MIPS - 1)).rgb;
        float2 envBRDF  = g_brdfLUT.Sample(g_sampler, float2(NdotV_f, 0.50)).rg;
        float3 F0_fish  = float3(0.03, 0.03, 0.03);
        col += (F0_fish * envBRDF.x + envBRDF.y) * prefilt * absorb;
    }

    // Bioluminescence: colorful glow when underwater at night.
    // The hue for each fish is stored in p.color.a (set at spawn from its random phase).
    // Emission is not absorbed by water — the fish itself is the light source.
    float underwaterBlend = saturate(-cameraPos.y / 2.0);       // 0 at surface, 1 at -2 m
    // Moon intensity = 0.15, so multiplier 3.0 gives nightBlend = 0.55 at night
    // (multiplier 5.0 only gave 0.25, which was too dim to see color variety).
    float nightBlend      = saturate(1.0 - sunIntensity * 3.0); // 0 in daylight, ~0.55 at night
    float biolumiStr      = underwaterBlend * nightBlend;

    [branch]
    if (biolumiStr > 0.001)
    {
        // Independent flicker per fish using its unique hue as a phase seed.
        float flicker = 0.80 + 0.20 * sin(time * 2.3 + p.color.a * 6.283);

        // Full-saturation rainbow: correct HSV(h,1,1)→RGB, offsets {0,4,2}.
        float3 bioColor = saturate(abs(fmod(p.color.a * 6.0 + float3(0.0, 4.0, 2.0), 6.0) - 3.0) - 1.0);

        // Pre-compensate for tonemapping.hlsl Beer-Lambert (same coefficients: 0.04/0.015).
        float camDepth = max(0.0, -cameraPos.y);
        float3 comp = min(float3(exp(0.04 * camDepth), exp(0.015 * camDepth), 1.0),
                          float3(3.0, 1.5, 1.0));

        // LERP the base silver-blue body color toward the bio emission target.
        // Adding (+= ) doesn't work: the fish's blue body channel bleeds into every
        // hue and collapses red/orange fish to purple after water absorption.
        // Replacing (lerp) clears the body color and lets each hue read cleanly.
        float3 glowTarget = bioColor * comp * 3.0 * flicker;
        col = lerp(col, glowTarget, biolumiStr);
    }

    return float4(col, 1.0);
}
