// ============================================================
// waterbody.hlsl
// 水体バウンディングボックスの半透明描画シェーダー。
// ============================================================
cbuffer SceneCB : register(b0)
{
    float4x4 view;
    float4x4 proj;
    float time;
    float3 cameraPos;

    float3 sunDir;
    float sunIntensity;
    float3 sunColor;
    float padSun;
    float3 skyColor;
    float padSky;

    float2 waveDir0;
    float waveAmp0;
    float waveLen0;
    float waveSpd0;
    float waveStp0;
    float2 wavePad0;
    float2 waveDir1;
    float waveAmp1;
    float waveLen1;
    float waveSpd1;
    float waveStp1;
    float2 wavePad1;
    float2 waveDir2;
    float waveAmp2;
    float waveLen2;
    float waveSpd2;
    float waveStp2;
    float2 wavePad2;
    float2 waveDir3;
    float waveAmp3;
    float waveLen3;
    float waveSpd3;
    float waveStp3;
    float2 wavePad3;

    float fogStart;
    float fogEnd;
    float2 padFog;
};

struct VSInput
{
    float3 position : POSITION;
    float2 uv : TEXCOORD0;
};

struct VSOutput
{
    float4 posH : SV_POSITION;
    float3 posW : TEXCOORD0;
    float2 uv : TEXCOORD1;
};

VSOutput VSMain(VSInput vin)
{
    VSOutput vout;
    float4 posV = mul(float4(vin.position, 1.0f), view);
    vout.posH = mul(posV, proj);
    vout.posW = vin.position;
    vout.uv = vin.uv;
    return vout;
}

float4 PSMain(VSOutput pin) : SV_TARGET
{
    // 基本の水体色：深い青、半透明
    float3 deepColor = float3(0.02f, 0.12f, 0.3f);
    float3 surfaceColor = float3(0.08f, 0.3f, 0.6f);

    // 深さに応じて色を補間：深いほど暗くなる
    float depthFactor = saturate(-pin.posW.y / 200.0f);
    float3 waterColor = lerp(surfaceColor, deepColor, depthFactor);

    // Fresnel：視線が面と平行になるほど透明になる
    float3 V = normalize(cameraPos - pin.posW);
    float3 N = float3(0.0f, 0.0f, 1.0f); // デフォルト法線は前方向、実際は面が決める
    float NdotV = abs(dot(N, V));
    float fresnel = 1.0f - saturate(NdotV);
    fresnel = pow(fresnel, 2.0f);

    // 太陽光散乱：水体に光感を与える
    float3 L = sunDir;
    float sunScatter = saturate(dot(L, float3(0.0f, -1.0f, 0.0f)));
    waterColor += sunColor * sunScatter * sunIntensity * 0.3f;

    // アルファ：正面から見ると透明、斜めから見ると不透明
    float alpha = lerp(0.5f, 0.85f, fresnel);

    // 霧
    float dist = length(cameraPos - pin.posW);
    float fogFactor = saturate((dist - fogStart) / (fogEnd - fogStart));
    waterColor = lerp(waterColor, skyColor, fogFactor * 0.5f);

    return float4(waterColor, alpha);
}
