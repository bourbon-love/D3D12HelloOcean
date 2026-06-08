// ============================================================
// skyShaders.hlsl
// Sky sphere vertex and pixel shaders. Includes procedural sky gradient,
// sun/moon disc, starfield, and lightning effects.
// Clouds are rendered separately by VolumetricClouds (clouds.hlsl).
// ============================================================
#include "SkyCommon.hlsli"

cbuffer SkyCB : register(b0)
{
    float4x4 viewProj;
    float4 topColor;
    float4 middleColor;
    float4 bottomColor;
    float3 sunPosition;
    float time;
    float cloudDensity;
    float cloudScale;
    float cloudSharpness;
    float weatherIntensity;
    float3 sunColor;
    float padSunColor;
    float3 moonPosition;
    float padMoon;
    float3 moonCrescentDir; // Independent crescent direction: slowly rotated by the CPU
    float padCrescent;
    float moonBodyPow;
    float moonOccludePow;
    float crescentOffsetAmt;
    float padMoonParams;
    float lightningIntensity;
    float cloudDriftX;   // wind X * speed — applied as per-frame cloud position offset
    float cloudDriftY;   // wind Z * speed
    float padLightning;
    float cameraY;       // world Y of camera (negative = underwater)
    float padCam1;
    float padCam2;
    float padCam3;
};

struct VSInput
{
    float3 position : POSITION;
};
struct VSOutput
{
    float4 posH : SV_POSITION;
    float3 worldPos : TEXCOORD0;
};

VSOutput skyVS(VSInput vin)
{
    VSOutput vout;
    vout.posH = mul(float4(vin.position, 1.0f), viewProj);
    // Place sky sphere vertices at the far plane.
    // Setting z=w ensures the depth value after perspective division is 1.0 (far plane),
    // so the sky is always rendered behind all other geometry.
    vout.posH.z = vout.posH.w;
    vout.worldPos = vin.position;
    return vout;
}

// Pixel shader
// Sky radiance is computed by the shared EvalSkyRadiance() (see SkyCommon.hlsli)
// so the rasterized dome and the IBL sky-capture compute shader can never drift
// apart — both must always show/light with the exact same sky.
float4 skyPS(VSOutput input) : SV_Target
{
    SkyParams p;
    p.topColor         = topColor.rgb;
    p.middleColor      = middleColor.rgb;
    p.bottomColor      = bottomColor.rgb;
    p.sunPosition      = sunPosition;
    p.sunColor         = sunColor;
    p.moonPosition     = moonPosition;
    p.moonCrescentDir  = moonCrescentDir;
    p.moonBodyPow      = moonBodyPow;
    p.moonOccludePow   = moonOccludePow;
    p.crescentOffsetAmt = crescentOffsetAmt;
    p.time             = time;
    p.lightningIntensity = lightningIntensity;
    p.cameraY          = cameraY;

    return float4(EvalSkyRadiance(input.worldPos, p), 1.0);
}
