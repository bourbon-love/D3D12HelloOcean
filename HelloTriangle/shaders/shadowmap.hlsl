// ============================================================
// shadowmap.hlsl
// 浮遊物体の深度パスシェーダー。
// ライト空間へ変換してシャドウマップに深度のみを書き込む。
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
