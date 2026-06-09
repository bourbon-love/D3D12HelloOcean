// PrefilterSpecularCS.hlsl
// GGX importance-sampled convolution that builds the prefiltered specular cubemap
// for split-sum IBL (Karis 2013).  Each mip level stores the environment convolved
// at a different roughness:  mip 0 = roughness 0 (mirror),  mip 5 = roughness 1.
//
// Dispatch: numthreads(8,8,1) → Dispatch(ceil(faceSize/8), ceil(faceSize/8), 6).
// SV_DispatchThreadID.z encodes the face index (0..5).
// A separate UAV view per mip is bound; the root constants carry roughness.
//
// Reference: Karis 2013, "Real Shading in Unreal Engine 4", Section 4.

#include "SkyIBL.hlsli"

TextureCube              g_srcCube : register(t0);  // captured sky (TextureCube SRV)
RWTexture2DArray<float4> g_output  : register(u0);  // one mip of the prefilter cubemap
SamplerState             g_sampler : register(s0);  // static linear-clamp

cbuffer PrefilterCB : register(b0)
{
    float roughness;    // mip / (PREFILTER_MIPS - 1), range [0,1]
    float invFaceSize;  // 1.0 / (faceSize >> mip)
    uint  numSamples;   // 512
    uint  _pad;
};

// Van der Corput radical inverse — forms the x component of the Hammersley sequence.
float RadicalInverse(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;  // / 0x100000000
}

float2 Hammersley(uint i, uint N)
{
    return float2(float(i) / float(N), RadicalInverse(i));
}

// GGX NDF importance sampling: given a uniform 2D sample Xi and a roughness,
// returns a half-vector H in the hemisphere around N (tangent-to-world space).
float3 ImportanceSampleGGX(float2 Xi, float rough, float3 N)
{
    float a     = rough * rough;
    float phi   = 2.0 * PI * Xi.x;
    float denom = max(1.0 + (a * a - 1.0) * Xi.y, 1e-7);
    float cosT  = sqrt((1.0 - Xi.y) / denom);
    float sinT  = sqrt(max(1.0 - cosT * cosT, 0.0));

    float3 H = float3(cos(phi) * sinT, sin(phi) * sinT, cosT);

    // Build TBN so H is in the same space as N
    float3 up = abs(N.z) < 0.999 ? float3(0, 0, 1) : float3(1, 0, 0);
    float3 T  = normalize(cross(up, N));
    float3 B  = cross(N, T);
    return normalize(T * H.x + B * H.y + N * H.z);
}

// Same face/UV → direction convention as IrradianceConvolveCS and SkyCaptureCS.
float3 FaceUVToDir(uint face, float2 uv)
{
    switch (face)
    {
        case 0:  return normalize(float3( 1.0, -uv.y, -uv.x));
        case 1:  return normalize(float3(-1.0, -uv.y,  uv.x));
        case 2:  return normalize(float3( uv.x,  1.0,  uv.y));
        case 3:  return normalize(float3( uv.x, -1.0, -uv.y));
        case 4:  return normalize(float3( uv.x, -uv.y,  1.0));
        default: return normalize(float3(-uv.x, -uv.y, -1.0));
    }
}

[numthreads(8, 8, 1)]
void CSMain(uint3 tid : SV_DispatchThreadID)
{
    uint face = tid.z;  // 0..5, mapped by Dispatch(x,y,6)

    // Texel-centred UV in [-1,1] for this mip face
    float2 uv = (float2(tid.xy) + 0.5) * invFaceSize * 2.0 - 1.0;

    // For split-sum: assume V = R = N (view equals reflection direction).
    // This loses some accuracy at grazing angles but is standard practice.
    float3 N = normalize(FaceUVToDir(face, uv));

    float3 totalWeight = 0;
    float3 result      = 0;

    for (uint i = 0; i < numSamples; ++i)
    {
        float2 Xi = Hammersley(i, numSamples);
        float3 H  = ImportanceSampleGGX(Xi, roughness, N);
        float3 L  = normalize(2.0 * dot(N, H) * H - N);
        float  NdotL = saturate(dot(N, L));
        if (NdotL > 0.0)
        {
            // Sample at mip 0 to avoid circular blurriness at high roughness.
            result      += g_srcCube.SampleLevel(g_sampler, L, 0).rgb * NdotL;
            totalWeight += NdotL;
        }
    }

    g_output[uint3(tid.xy, face)] = float4(result / max(totalWeight, 1e-5), 1.0);
}
