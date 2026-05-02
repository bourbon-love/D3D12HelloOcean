// ============================================================
// rain.hlsl
// 雨粒子ビルボードの頂点・ピクセルシェーダー。
// ============================================================
cbuffer RainCB : register(b0)
{
    float4x4 viewProj;
    float alpha;
    float3 pad;
};

struct VSInput
{
    float3 position : POSITION;
    float alpha : TEXCOORD0;
};

struct VSOutput
{
    float4 posH : SV_POSITION;
    float alpha : TEXCOORD0;
};

VSOutput VSMain(VSInput vin)
{
    VSOutput vout;
    vout.posH = mul(float4(vin.position, 1.0f), viewProj);
    vout.alpha = vin.alpha;
    return vout;
}

float4 PSMain(VSOutput pin) : SV_TARGET
{
    // 雨線の色：わずかに青みがかった白、半透明
    float3 rainColor = float3(0.7f, 0.8f, 1.0f);
    return float4(rainColor, pin.alpha);

}
