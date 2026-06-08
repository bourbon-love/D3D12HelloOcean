// ============================================================
// SkyCaptureCS.hlsl
// Captures the procedural sky radiance into ONE face of a small cubemap per
// dispatch. Because the sky (EvalSkyRadiance, see SkyCommon.hlsli) is a pure
// function of view direction + time + weather — no geometry, no rasterization —
// "capturing" it degenerates into per-texel function evaluation: work out the
// direction each texel represents, call the exact same function the rasterized
// sky dome uses, and write the result straight into the UAV.
//
// One dispatch writes ONE face (selected by faceIndex); the caller rotates
// faceIndex across frames in a 6-frame rolling cycle (see IBLSystem::Dispatch) —
// the sun/moon move slowly enough that a ~6-frame-stale face is imperceptible.
// ============================================================
#include "SkyCommon.hlsli"

// Bound as a single-face TEXTURE2DARRAY UAV view (FirstArraySlice = faceIndex,
// ArraySize = 1) — cubemaps have no native UAV view, so faces are addressed
// through the array-slice mechanism.
RWTexture2DArray<float4> g_captureFace : register(u0);

// CaptureCB mirrors SkyCB's flattened float3-plus-pad layout (see skyShaders.hlsl)
// instead of nesting SkyParams directly: HLSL cbuffer packing rules would force
// the same per-float3 padding either way, so flattening keeps this struct a
// direct mirror of the already-proven SkyCB layout rather than inventing a
// second packing scheme that could silently drift out of sync.
cbuffer CaptureCB : register(b0)
{
    float3 topColor;
    float  padTop;
    float3 middleColor;
    float  padMiddle;
    float3 bottomColor;
    float  padBottom;
    float3 sunPosition;
    float  time;
    float3 sunColor;
    float  lightningIntensity;
    float3 moonPosition;
    float  cameraY;
    float3 moonCrescentDir;
    float  moonBodyPow;
    float  moonOccludePow;
    float  crescentOffsetAmt;
    int    faceIndex;     // 0..5 — which cubemap face this dispatch writes
    int    faceSize;      // texel resolution of one face (e.g. 64)
    float4 _reserved[8];  // pad cbuffer to 256 bytes (CB_SLOT_SIZE convention, see CLAUDE.md)
};

// Direction <-> cubemap face/UV mapping, D3D convention (+X,-X,+Y,-Y,+Z,-Z).
// uv is texel-centered, in [-1, 1]. Getting a sign or axis wrong here produces
// a silently rotated/mirrored sky rather than a crash — verify against the
// debug preview (Task 7) before trusting this mapping.
float3 FaceUVToDirection(int face, float2 uv)
{
    switch (face)
    {
        case 0: return normalize(float3( 1.0, -uv.y, -uv.x));  // +X
        case 1: return normalize(float3(-1.0, -uv.y,  uv.x));  // -X
        case 2: return normalize(float3( uv.x,  1.0,  uv.y));  // +Y (zenith)
        case 3: return normalize(float3( uv.x, -1.0, -uv.y));  // -Y (nadir)
        case 4: return normalize(float3( uv.x, -uv.y,  1.0));  // +Z
        default:return normalize(float3(-uv.x, -uv.y, -1.0));  // -Z
    }
}

[numthreads(8, 8, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= (uint)faceSize || dtid.y >= (uint)faceSize)
        return;

    float2 uv  = (float2(dtid.xy) + 0.5) / faceSize * 2.0 - 1.0;
    float3 dir = FaceUVToDirection(faceIndex, uv);

    SkyParams p;
    p.topColor           = topColor;
    p.middleColor        = middleColor;
    p.bottomColor        = bottomColor;
    p.sunPosition        = sunPosition;
    p.sunColor           = sunColor;
    p.moonPosition       = moonPosition;
    p.moonCrescentDir    = moonCrescentDir;
    p.moonBodyPow        = moonBodyPow;
    p.moonOccludePow     = moonOccludePow;
    p.crescentOffsetAmt  = crescentOffsetAmt;
    p.time               = time;
    p.lightningIntensity = lightningIntensity;
    p.cameraY            = cameraY;

    g_captureFace[uint3(dtid.xy, 0)] = float4(EvalSkyRadiance(dir, p), 1.0);
}
