// Ship model VS/PS (PBR: GGX Cook-Torrance + SH9 diffuse + split-sum specular IBL) + shadow depth VS.
// ShipVS samples the FFT heightmap to follow ocean surface.

#include "SkyIBL.hlsli"

Texture2D    g_diffuse   : register(t0);
Texture2D    g_normalMap : register(t1);
Texture2D    g_arm       : register(t2);
Texture2D    g_heightMap : register(t3);
TextureCube  g_prefilter : register(t4);  // GGX prefiltered specular cubemap (IBL Phase C)
Texture2D    g_brdfLUT   : register(t5);  // BRDF integration LUT (IBL Phase A)
SamplerState g_sampler   : register(s0);

static const uint  PREFILTER_MIPS = 6;
// Sky radiance values are artistic HDR (midD.b = 1.05) — not physically calibrated.
// Without scaling, SH irradiance (~3.0 blue channel) makes additive diffAmb/specAmb
// sum to >1.0 before the direct light, washing every surface toward white after ACES.
// 0.40 brings ambient to ~30% of direct at noon — proper fill-light ratio.
static const float IBL_INTENSITY  = 0.40;

cbuffer ShipCB : register(b0)
{
    matrix  viewProj;
    float3  worldPos;       float scale;
    // lightDir/lightColor/lightIntensity: single light source already blended between
    // sun and moon on the CPU (dayBlend), matching the ocean's lighting model —
    // avoids two simultaneously-active lights causing conflicting highlights at dusk/dawn.
    float3  lightDir;       float lightIntensity;
    float3  lightColor;     float yaw;
    float3  cameraPos;      float lightningIntensity;
    float   time;           float cloudCoverage;
    float   cloudScale;     float cloudBase;
    float   cloudTop;       float cloudWindX;
    float   cloudWindZ;     float cloudDensityMult;
    float   cloudEnabled;   float pitch;
    float   roll;           float _pad0;
    float4  _cp1[5];
};

cbuffer SHCB : register(b1)
{
    float4 shCoeffs[9];
    float4 _shPad[7];
};

cbuffer ShadowCB : register(b0)
{
    matrix  lightViewProj;
    float3  s_worldPos;     float s_scale;
    float   s_yaw;          float s_pad[43];
};

static const float FFT_HEIGHT_SCALE = 1.0 / 1000.0;
static const float GRID_WORLD_SIZE  = 400.0;

float SampleWaveHeight(float2 xz)
{
    float2 uv = xz / GRID_WORLD_SIZE;
    return -g_heightMap.SampleLevel(g_sampler, uv, 0).x * FFT_HEIGHT_SCALE;
}

void ApplyYawScale(float3 inPos, float3 inNorm, float s, float r,
                   out float3 outPos, out float3 outNorm)
{
    float c = cos(r), sn = sin(r);
    outPos  = float3(inPos.x*c - inPos.z*sn,  inPos.y,  inPos.x*sn + inPos.z*c) * s;
    outNorm = float3(inNorm.x*c - inNorm.z*sn, inNorm.y, inNorm.x*sn + inNorm.z*c);
}

struct VSIn  { float3 pos : POSITION; float3 normal : NORMAL; float2 uv : TEXCOORD0; };
struct VSOut { float4 clip : SV_Position; float3 wPos : TEXCOORD0;
               float3 wNorm : TEXCOORD1;  float2 uv  : TEXCOORD2; };

VSOut ShipVS(VSIn v)
{
    float3 lp, ln;
    ApplyYawScale(v.pos, v.normal, scale, yaw, lp, ln);

    // Wave Y position: sample heightmap at ship center
    float h0 = SampleWaveHeight(worldPos.xz);

    // Pitch/roll supplied by CPU spring-damper (smooth, inertia-delayed)
    float cp = cos(pitch), sp = sin(pitch);
    float3 p1 = float3(lp.x, cp * lp.y - sp * lp.z, sp * lp.y + cp * lp.z);
    float3 n1 = float3(ln.x, cp * ln.y - sp * ln.z, sp * ln.y + cp * ln.z);

    float cr = cos(roll), sr = sin(roll);
    float3 p2 = float3(cr * p1.x - sr * p1.y, sr * p1.x + cr * p1.y, p1.z);
    float3 n2 = float3(cr * n1.x - sr * n1.y, sr * n1.x + cr * n1.y, n1.z);

    float3 wp = p2 + float3(worldPos.x, h0, worldPos.z);

    VSOut o;
    o.clip  = mul(float4(wp, 1.0), viewProj);
    o.wPos  = wp;
    o.wNorm = normalize(n2);
    o.uv    = v.uv;
    return o;
}

// ---- Cloud shadow (same algorithm as ocean surface) ----
float cs_hash(float3 p)
{
    p = frac(p * float3(443.897, 441.423, 437.195));
    p += dot(p, p.yxz + 19.19);
    return frac((p.x + p.y) * p.z);
}
float cs_vnoise(float3 p)
{
    float3 i = floor(p), f = frac(p);
    f = f * f * (3.0 - 2.0 * f);
    return lerp(
        lerp(lerp(cs_hash(i),               cs_hash(i+float3(1,0,0)), f.x),
             lerp(cs_hash(i+float3(0,1,0)), cs_hash(i+float3(1,1,0)), f.x), f.y),
        lerp(lerp(cs_hash(i+float3(0,0,1)), cs_hash(i+float3(1,0,1)), f.x),
             lerp(cs_hash(i+float3(0,1,1)), cs_hash(i+float3(1,1,1)), f.x), f.y),
        f.z);
}
float cs_fbm3(float3 p)
{
    return cs_vnoise(p) * 0.500
         + cs_vnoise(p * 2.031) * 0.250
         + cs_vnoise(p * 4.073) * 0.125;
}
float CloudShadow(float3 worldPos)
{
    if (cloudEnabled < 0.5 || lightDir.y <= 0.02) return 1.0;
    float3 sd     = normalize(lightDir);
    float  tStart = (cloudBase - worldPos.y) / max(sd.y, 0.02);
    float  tEnd   = (cloudTop  - worldPos.y) / max(sd.y, 0.02);
    if (tEnd <= tStart) return 1.0;
    const int STEPS = 6;
    float stepSize = (tEnd - tStart) / float(STEPS);
    float sigma    = 0.0;
    float3 drift   = float3(cloudWindX, 0.0, cloudWindZ) * time * 30.0;
    [unroll]
    for (int i = 0; i < STEPS; i++)
    {
        float  t       = tStart + (i + 0.5) * stepSize;
        float3 p       = worldPos + sd * t;
        float3 q       = (p + drift) * cloudScale * 0.00030;
        float  base    = cs_fbm3(q);
        float  thresh  = 0.56 - cloudCoverage * 0.44;
        float  density = smoothstep(0.0, 0.35, base - thresh) * cloudDensityMult;
        sigma += density * stepSize * 0.052;
    }
    return exp(-sigma * 2.0);
}

// Cotangent-frame TBN (Mikkelsen method — no vertex tangents needed)
float3x3 CotangentFrame(float3 N, float3 p, float2 uv)
{
    float3 dp1  = ddx(p),  dp2  = ddy(p);
    float2 duv1 = ddx(uv), duv2 = ddy(uv);
    float3 dp2perp = cross(dp2, N);
    float3 dp1perp = cross(N, dp1);
    float3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    float3 B = dp2perp * duv1.y + dp1perp * duv2.y;
    float  invmax = rsqrt(max(dot(T, T), dot(B, B)));
    return float3x3(T * invmax, B * invmax, N);
}

// GGX normal distribution (D term)
float DistGGX(float NdotH, float a2)
{
    float d = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}

// Schlick Fresnel (F term)
float3 FresnelSchlick(float cosT, float3 F0)
{
    return F0 + (1.0 - F0) * pow(1.0 - cosT, 5.0);
}

// Smith-GGX visibility (G term, combined numerator and denominator)
float GeomSmith(float NdotV, float NdotL, float a2)
{
    float gv = NdotV + sqrt(a2 + (1.0 - a2) * NdotV * NdotV);
    float gl = NdotL + sqrt(a2 + (1.0 - a2) * NdotL * NdotL);
    return 1.0 / (gv * gl);
}

float4 ShipPS(VSOut i) : SV_Target
{
    float3 albedo   = g_diffuse.Sample(g_sampler, i.uv).rgb;
    float3 normSamp = g_normalMap.Sample(g_sampler, i.uv).rgb * 2.0 - 1.0;
    normSamp.g      = -normSamp.g;  // OpenGL normal map Y-flip for DirectX
    float3 arm      = g_arm.Sample(g_sampler, i.uv).rgb;
    float  ao       = arm.r;
    float  rough    = clamp(arm.g, 0.04, 0.65);  // cap to keep GGX lobe visible on wood
    float  metal    = arm.b;

    float3x3 TBN = CotangentFrame(normalize(i.wNorm), i.wPos, i.uv);
    float3 N = normalize(mul(normSamp, TBN));
    float3 V = normalize(cameraPos - i.wPos);
    float3 L = normalize(lightDir);
    float3 H = normalize(V + L);

    float NdotL = saturate(dot(N, L));
    float NdotV = max(dot(N, V), 0.0001);
    float NdotH = saturate(dot(N, H));
    float VdotH = saturate(dot(V, H));

    // Disney roughness remapping: alpha = roughness^2, a2 = alpha^2
    float  a2  = rough * rough * rough * rough;
    float3 F0  = lerp(float3(0.04, 0.04, 0.04), albedo, metal);
    float  D   = DistGGX(NdotH, a2);
    float3 F   = FresnelSchlick(VdotH, F0);
    float  G   = GeomSmith(NdotV, NdotL, a2);

    float3 spec   = D * F * G * 2.0;  // x2 boost: wood F0=0.04 otherwise invisible
    float3 kd     = (1.0 - F) * (1.0 - metal);

    // cloudShad: per-pixel ray-march shadow from clouds above (only meaningful while the
    // blended light sits above the horizon; CloudShadow() early-outs otherwise)
    float  cloudShad = CloudShadow(i.wPos);
    // cloudOcc: coverage-based uniform occlusion applied to ambient (day+night)
    float  cloudOcc  = cloudEnabled > 0.5 ? lerp(1.0, 0.20, cloudCoverage) : 1.0;

    // Single light source, already blended between sun and moon by the CPU (dayBlend) —
    // mirrors the ocean's lighting model so the ship never receives two simultaneously
    // active lights from opposite directions (which used to leak "moonlight" onto the
    // sunlit side of the hull at full daylight, since the moon's intensity never faded
    // with its elevation).
    float3 direct = (kd * albedo / PI + spec) * lightColor * lightIntensity * NdotL * cloudShad * 4.0;

    // Diffuse irradiance from SH9 convolution of the captured sky cubemap.
    // Coefficients are pre-multiplied by cosine-lobe factors in IrradianceConvolveCS.
    float3 irradiance = max(EvalSH(N, shCoeffs), 0.0) * IBL_INTENSITY;

    // Specular IBL via Karis split-sum (Phase C):
    // prefiltered env at reflection direction (mip = roughness) × BRDF LUT(NdotV, roughness).
    float3 R           = reflect(-V, N);
    float3 prefiltered = g_prefilter.SampleLevel(g_sampler, R, rough * (PREFILTER_MIPS - 1)).rgb * IBL_INTENSITY;
    float2 envBRDF     = g_brdfLUT.Sample(g_sampler, float2(NdotV, rough)).rg;

    // Energy-conserving IBL ambient.
    // envBRDF.y (rough-surface bias) is gated by metal: for dielectrics (metal≈0) it
    // represents diffuse-like energy already captured by SH, so including it doubles
    // the ambient. For metals (metal≈1) it correctly adds the conductor specular bias.
    float3 specBRDF = F0 * envBRDF.x + envBRDF.y * metal;
    float3 kd_ibl   = (1.0 - specBRDF) * (1.0 - metal);
    float3 diffAmb  = albedo * kd_ibl * irradiance / PI * ao * cloudOcc;
    float3 specAmb  = specBRDF * prefiltered * ao * cloudOcc * 0.60;
    float3 ambient  = diffAmb + specAmb;

    // Lightning: brief white flash illuminating the whole ship
    float3 lightning = albedo * lightningIntensity * float3(0.85, 0.90, 1.0) * 0.6;

    return float4(direct + ambient + lightning, 1.0);
}

float4 ShipShadowVS(VSIn v) : SV_Position
{
    float3 lp, ln;
    ApplyYawScale(v.pos, float3(0, 1, 0), s_scale, s_yaw, lp, ln);
    float3 wp = lp + s_worldPos;
    return mul(float4(wp, 1.0), lightViewProj);
}
