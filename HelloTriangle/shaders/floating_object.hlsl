// ============================================================
// floating_object.hlsl
// Floating object (wooden crate) shader.
// Samples the height map to determine orientation along the wave surface,
// then renders with Phong + SH9 diffuse IBL ambient.
// ============================================================
#include "SkyIBL.hlsli"

Texture2D    g_heightMap : register(t0);
SamplerState g_sampler   : register(s0);

cbuffer ObjectCB : register(b0)
{
    matrix  viewProj;
    float3  worldPos;       // object center XZ coordinates (Y determined by height map)
    float   objectScale;
    float3  sunDir;
    float   sunIntensity;
    float3  sunColor;
    float   gridWorldSize;  // typically 400.0
    float3  cameraPos;
    float   dropOffset; // Y offset above wave surface during fall
};

cbuffer SHCB : register(b1)
{
    float4 shCoeffs[9];
    float4 _shPad[7];
};

struct VSIn  { float3 pos : POSITION; float3 normal : NORMAL; };
struct VSOut
{
    float4 clip     : SV_Position;
    float3 wNormal  : NORMAL;
    float3 wPos     : TEXCOORD0;
};

static const float FFT_HEIGHT_SCALE = 1.0 / 1000.0;

// Sample the height map at world XZ coordinates and return world-space displacement.
float SampleH(float2 xz)
{
    // Same UV calculation as the ocean shader: UV = xz / tileSize (WRAP sampler)
    float2 uv = xz / gridWorldSize;
    return -g_heightMap.SampleLevel(g_sampler, uv, 0).x * FFT_HEIGHT_SCALE;
}

VSOut FloatObjVS(VSIn v)
{
    float2 objXZ = worldPos.xz;
    float  step  = gridWorldSize / 128.0; // step size for finite differences

    // Estimate wave surface normal from 3 points
    float h0 = SampleH(objXZ);
    float hR = SampleH(objXZ + float2(step, 0));
    float hF = SampleH(objXZ + float2(0, step));

    // Upward normal (finite differences)
    float3 N = normalize(float3(h0 - hR, step, h0 - hF));

    // Build a TBN basis aligned to the surface normal
    float3 ref   = (abs(N.x) < 0.9) ? float3(1, 0, 0) : float3(0, 0, 1);
    float3 right = normalize(cross(ref, N));
    float3 fwd   = normalize(cross(right, N));

    // Scale local vertices, align to the surface, then transform to world coordinates
    float3 scaled = v.pos * objectScale;
    float3 wp = float3(objXZ.x, h0 + dropOffset, objXZ.y)
              + scaled.x * right
              + scaled.y * N
              + scaled.z * fwd;

    // Transform the normal in the same way
    float3 wn = normalize(v.normal.x * right + v.normal.y * N + v.normal.z * fwd);

    VSOut o;
    o.clip    = mul(float4(wp, 1.0), viewProj);
    o.wNormal = wn;
    o.wPos    = wp;
    return o;
}

float4 FloatObjPS(VSOut i) : SV_Target
{
    float3 N = normalize(i.wNormal);
    float3 L = normalize(sunDir);
    float3 V = normalize(cameraPos - i.wPos);
    float3 H = normalize(L + V);

    // Vary wood tone based on face orientation
    float upness = saturate(dot(N, float3(0, 1, 0)));
    float3 topColor  = float3(0.70, 0.57, 0.36); // top face of plank (bright wood)
    float3 sideColor = float3(0.52, 0.38, 0.22); // side face of plank (dark wood)
    float3 base = lerp(sideColor, topColor, upness * upness);

    float diff    = saturate(dot(N, L));
    float spec    = pow(saturate(dot(N, H)), 28.0) * 0.12;
    float3 irradiance = max(EvalSH(N, shCoeffs), 0.0);
    float3 ambient    = irradiance / PI;

    float3 color = base * (ambient + diff * sunIntensity * sunColor) + spec * sunColor;
    return float4(color, 1.0);
}
