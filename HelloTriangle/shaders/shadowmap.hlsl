// ============================================================
// shadowmap.hlsl
// Depth-pass shader for floating objects.
// Transforms to light space and writes depth only to the shadow map.
// ============================================================
cbuffer ShadowInstCB : register(b0)
{
    float4x4 lightViewProj;
    float3   worldPos;
    float    objectScale;
    float    dropOffset;
    float3   pad;
};

float4 ShadowVS(float3 pos : POSITION) : SV_Position
{
    float3 wp = worldPos + pos * objectScale;
    wp.y += dropOffset;
    return mul(float4(wp, 1.0), lightViewProj);
}
