// ============================================================
// rain.hlsl
// Rain particle billboard vertex and pixel shaders.
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
    float4 posH   : SV_POSITION;
    float  alpha  : TEXCOORD0;
    float  worldY : TEXCOORD1;
};

VSOutput VSMain(VSInput vin)
{
    VSOutput vout;
    vout.posH   = mul(float4(vin.position, 1.0f), viewProj);
    vout.alpha  = vin.alpha;
    vout.worldY = vin.position.y;
    return vout;
}

float4 PSMain(VSOutput pin) : SV_TARGET
{
    // Discard raindrops below the water surface (y=0)
    clip(pin.worldY);

    // Desaturated grey-blue: real rain streaks are nearly transparent and cool-tinted
    float3 rainColor = float3(0.55f, 0.60f, 0.72f);
    return float4(rainColor, pin.alpha);
}
