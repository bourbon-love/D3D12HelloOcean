// Ship model VS/PS (PBR: GGX Cook-Torrance + sky ambient) + shadow depth VS.
// ShipVS samples the FFT heightmap to follow ocean surface.

Texture2D    g_diffuse   : register(t0);
Texture2D    g_normalMap : register(t1);
Texture2D    g_arm       : register(t2);
Texture2D    g_heightMap : register(t3);
SamplerState g_sampler   : register(s0);

cbuffer ShipCB : register(b0)
{
    matrix  viewProj;
    float3  worldPos;       float scale;
    float3  sunDir;         float sunIntensity;
    float3  sunColor;       float yaw;
    float3  cameraPos;      float pad0;
    float4  pad[8];
};

cbuffer ShadowCB : register(b0)
{
    matrix  lightViewProj;
    float3  s_worldPos;     float s_scale;
    float   s_yaw;          float s_pad[43];
};

static const float FFT_HEIGHT_SCALE = 1.0 / 1000.0;
static const float GRID_WORLD_SIZE  = 400.0;
static const float PI               = 3.14159265;

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

    float waveY = SampleWaveHeight(worldPos.xz);
    float3 wp   = lp + float3(worldPos.x, waveY, worldPos.z);

    VSOut o;
    o.clip  = mul(float4(wp, 1.0), viewProj);
    o.wPos  = wp;
    o.wNorm = normalize(ln);
    o.uv    = v.uv;
    return o;
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
    float  rough    = max(arm.g, 0.04);
    float  metal    = arm.b;

    float3x3 TBN = CotangentFrame(normalize(i.wNorm), i.wPos, i.uv);
    float3 N = normalize(mul(normSamp, TBN));
    float3 V = normalize(cameraPos - i.wPos);
    float3 L = normalize(-sunDir);
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

    float3 spec   = D * F * G;
    float3 kd     = (1.0 - F) * (1.0 - metal);
    float3 direct = (kd * albedo / PI + spec) * sunColor * sunIntensity * NdotL;

    // Sky-colored ambient: blend cool sky blue toward sun color by sun elevation
    float3 skyAmb     = lerp(float3(0.10, 0.15, 0.25), sunColor, saturate(sunDir.y));
    float3 diffAmb    = albedo * (1.0 - metal) * ao * skyAmb * 0.30;
    float3 specAmb    = F0 * ao * skyAmb * 0.15;  // specular ambient for metals
    float3 ambient    = diffAmb + specAmb;

    return float4(direct + ambient, 1.0);
}

float4 ShipShadowVS(VSIn v) : SV_Position
{
    float3 lp, ln;
    ApplyYawScale(v.pos, float3(0, 1, 0), s_scale, s_yaw, lp, ln);
    float3 wp = lp + s_worldPos;
    return mul(float4(wp, 1.0), lightViewProj);
}
