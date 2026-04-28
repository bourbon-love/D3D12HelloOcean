//shaders.hlsl
cbuffer SceneCB : register(b0)
{
    float4x4 view;
    float4x4 proj;
    float    time;
    float3 cameraPos;
    
    float3 sunDir; // 太阳方向（归一化）
    float sunIntensity; // 太阳强度，日出日落时降低
    float3 sunColor; // 太阳颜色，日出偏橙，正午偏白
    float padSun;
    float3 skyColor; // 天空主色，用于Fresnel反射
    float padSky;
    float fogStart; // 雾开始距离
    float fogEnd; // 雾结束距离
    float2 padFog;
    // Wave parameters for 4 waves, 
    //each wave has direction, amplitude, wavelength, speed, step and padding
    float2 waveDir0;   float waveAmp0; float waveLen0;
    float waveSpd0;    float waveStp0; float2 wavePad0;
    
    float2 waveDir1;   float waveAmp1; float waveLen1;
    float waveSpd1;    float waveStp1; float2 wavePad1;
    
    float2 waveDir2;   float waveAmp2; float waveLen2;
    float waveSpd2;    float waveStp2; float2 wavePad2;
    
    float2 waveDir3;   float waveAmp3; float waveLen3;
    float waveSpd3;    float waveStp3; float2 wavePad3;
    
    
};

struct RippleData
{
    float2 position;
    float radius;
    float strength;
};

cbuffer RippleCB : register(b1)
{
    RippleData ripples[200];
    uint rippleCount;
    float3 ripplePad;
};


Texture2D<float4> g_heightMap : register(t0);
Texture2D<float4> g_dztMap : register(t1);
SamplerState g_sampler : register(s0);


static const float FFT_HEIGHT_SCALE = 1.0f / 1000.0f;
static const float FFT_CHOP_SCALE = 1.0f / 1000.0f;
static const float FFT_TILE_SIZE = 400.0f;

struct VSInput
{
    float3 position : POSITION;
    float2 uv       : TEXCOORD0;
};

struct VSOutput
{
    float4 posH   : SV_POSITION;
    float3 posW   : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 uv     : TEXCOORD2;

};

// Gerstner wave function to modify vertex position and normal based on wave parameters
//input: position and normal of the vertex, wave direction, amplitude, wavelength, speed and step
//output: modified position and normal of the vertex
void GerstnerWave(
    float2 xz, float2 dir, float amp, float wavelen,
    float spd, float steep,
    inout float3 disp,
    inout float3 tangentX,
    inout float3 tangentZ)
{
    float k = 2.0f * 3.14159265f / wavelen;
    float f = k * dot(dir, xz) - spd * time;

    // Q直接用steep，范围控制在0到1之间
    
    float Q = steep;

    float sinF = sin(f);
    float cosF = cos(f);

    // XYZ位移
    disp.x += Q * amp * dir.x * cosF;
    disp.y += amp * sinF;
    disp.z += Q * amp * dir.y * cosF;

    // 切线X方向
    tangentX.x += 1.0f - Q * dir.x * dir.x * k * amp * sinF;
    tangentX.y += dir.x * k * amp * cosF;
    tangentX.z -= Q * dir.x * dir.y * k * amp * sinF;

    // 切线Z方向
    tangentZ.x -= Q * dir.x * dir.y * k * amp * sinF;
    tangentZ.y += dir.y * k * amp * cosF;
    tangentZ.z += 1.0f - Q * dir.y * dir.y * k * amp * sinF;
}
VSOutput VSMain(VSInput vin)
{
    VSOutput vout;
    
    float2 xz = vin.position.xz;
    float3 disp = float3(0.0f, 0.0f, 0.0f);
    float3 tangentX = float3(1.0f, 0.0f, 0.0f);
    float3 tangentZ = float3(0.0f, 0.0f, 1.0f);
    
    // 计算到边缘的距离，归一化到 0~1
    float halfSize = FFT_TILE_SIZE * 0.5f;
    float distToEdgeX = 1.0f - abs(vin.position.x) / halfSize;
    float distToEdgeZ = 1.0f - abs(vin.position.z) / halfSize;
    float distToEdge = min(distToEdgeX, distToEdgeZ);

    // 边缘衰减系数：边缘处为0，内部为1，过渡带宽度可调
    float heightFade = smoothstep(0.0f, 0.2f, distToEdge); // 0.05 = 5%的过渡带
    float chopFade = smoothstep(0.0f, 0.4f, distToEdge);
    // Gerstner 位移乘以衰减
    GerstnerWave(xz, waveDir0, waveAmp0, waveLen0, waveSpd0, waveStp0, disp, tangentX, tangentZ);
    GerstnerWave(xz, waveDir1, waveAmp1, waveLen1, waveSpd1, waveStp1, disp, tangentX, tangentZ);
    GerstnerWave(xz, waveDir2, waveAmp2, waveLen2, waveSpd2, waveStp2, disp, tangentX, tangentZ);
    GerstnerWave(xz, waveDir3, waveAmp3, waveLen3, waveSpd3, waveStp3, disp, tangentX, tangentZ);
    disp *= heightFade;

    float3 worldPos = vin.position + disp;
    float2 fftUV = vin.position.xz / FFT_TILE_SIZE;

    float4 fftSample = g_heightMap.SampleLevel(g_sampler, fftUV, 0);
    float fftHeight = fftSample.x * FFT_HEIGHT_SCALE * heightFade;
    float fftDx = fftSample.z * FFT_CHOP_SCALE * chopFade; // 用更强衰减
    float fftDz = g_dztMap.SampleLevel(g_sampler, fftUV, 0).x * FFT_CHOP_SCALE * chopFade;

    worldPos.y += fftHeight * -1.0f;
    worldPos.x += fftDx;
    worldPos.z += fftDz;

    float3 normal = normalize(cross(tangentZ, tangentX));
    //float3 normal = normalize(cross(tangentX, tangentZ));

    float4 posV = mul(float4(worldPos, 1.0f), view);
    vout.posH = mul(posV, proj);
    vout.posW = worldPos;
    vout.normal = normal;
    vout.uv = fftUV;
    return vout;

}
// Reconstruct sky color from a reflection direction, matching the dynamic sky palette
float3 SampleSkyReflection(float3 reflDir)
{
    // Elevation: 0 = horizon, 1 = zenith
    float h = saturate(reflDir.y);

    // Day/night factor from sun height
    float dayF  = saturate(sunDir.y * 4.0 + 0.4);
    // Sunset factor: peaks when sun is near horizon
    float sunsetF = saturate(1.0 - abs(sunDir.y) * 5.0);
    sunsetF = sunsetF * sunsetF;

    // Zenith / horizon for night and day
    float3 zenithDay  = float3(0.08, 0.25, 0.72);
    float3 horizDay   = skyColor * 1.5;
    float3 zenithNight = float3(0.01, 0.01, 0.06);
    float3 horizNight  = float3(0.03, 0.03, 0.12);

    float3 zenith = lerp(zenithNight, zenithDay,  dayF);
    float3 horiz  = lerp(horizNight,  horizDay,   dayF);
    float3 sky    = lerp(horiz, zenith, h);

    // Sunset overlay: orange-gold near horizon
    float3 sunsetHorizon = float3(1.6, 0.72, 0.12); // HDR gold
    float3 sunsetZenith  = float3(0.10, 0.16, 0.48); // blue-purple
    float3 sunsetCol = lerp(sunsetZenith, sunsetHorizon, saturate(1.2 - h * 3.0));
    sky = lerp(sky, sunsetCol, sunsetF * saturate(1.2 - h * 2.0));

    // Sun glow in the reflected direction
    float sunDotR = max(0.0, dot(reflDir, sunDir));
    sky += sunColor * pow(sunDotR, 6.0) * 4.0;

    // Below horizon: fade to deep water color
    float3 deepWater = float3(0.02, 0.06, 0.15);
    return lerp(deepWater, sky, smoothstep(-0.05, 0.1, reflDir.y));
}

float4 PSMain(VSOutput pin) : SV_TARGET
{
    // 逐像素从 heightMap 重新算法线（和 VS 用同一个 scale）
    const float texelSize = 1.0f / 256.0f;
    const float worldPerTexel = FFT_TILE_SIZE / 256.0f;

    // 采样周围高度
    float hL = -g_heightMap.SampleLevel(g_sampler, pin.uv + float2(-texelSize, 0), 0).x * FFT_HEIGHT_SCALE;
    float hR = -g_heightMap.SampleLevel(g_sampler, pin.uv + float2(texelSize, 0), 0).x * FFT_HEIGHT_SCALE;
    float hD = -g_heightMap.SampleLevel(g_sampler, pin.uv + float2(0, -texelSize), 0).x * FFT_HEIGHT_SCALE;
    float hU = -g_heightMap.SampleLevel(g_sampler, pin.uv + float2(0, texelSize), 0).x * FFT_HEIGHT_SCALE;

    // 采样周围 Dx
    float dxL = g_heightMap.SampleLevel(g_sampler, pin.uv + float2(-texelSize, 0), 0).z * FFT_CHOP_SCALE;
    float dxR = g_heightMap.SampleLevel(g_sampler, pin.uv + float2(texelSize, 0), 0).z * FFT_CHOP_SCALE;
    float dxD = g_heightMap.SampleLevel(g_sampler, pin.uv + float2(0, -texelSize), 0).z * FFT_CHOP_SCALE;
    float dxU = g_heightMap.SampleLevel(g_sampler, pin.uv + float2(0, texelSize), 0).z * FFT_CHOP_SCALE;

// 采样周围 Dz
    float dzL = g_dztMap.SampleLevel(g_sampler, pin.uv + float2(-texelSize, 0), 0).x * FFT_CHOP_SCALE;
    float dzR = g_dztMap.SampleLevel(g_sampler, pin.uv + float2(texelSize, 0), 0).x * FFT_CHOP_SCALE;
    float dzD = g_dztMap.SampleLevel(g_sampler, pin.uv + float2(0, -texelSize), 0).x * FFT_CHOP_SCALE;
    float dzU = g_dztMap.SampleLevel(g_sampler, pin.uv + float2(0, texelSize), 0).x * FFT_CHOP_SCALE;

// 偏导数
    float dHdx = (hR - hL) / (2.0f * worldPerTexel);
    float dHdz = (hU - hD) / (2.0f * worldPerTexel);
    float dDxdx = (dxR - dxL) / (2.0f * worldPerTexel);
    float dDzdz = (dzU - dzD) / (2.0f * worldPerTexel);
    float dDxdz = (dxU - dxD) / (2.0f * worldPerTexel);
    float dDzdx = (dzR - dzL) / (2.0f * worldPerTexel);

// 雅可比法线：tangentX = (1+dDxdx, dHdx, dDzdx), tangentZ = (dDxdz, dHdz, 1+dDzdz)
    float3 tangentX = float3(1.0f + dDxdx, dHdx, dDzdx);
    float3 tangentZ = float3(dDxdz, dHdz, 1.0f + dDzdz);
    float3 N = normalize(cross(tangentZ, tangentX));
    
    // 涟漪法线扰动
    for (uint i = 0; i < rippleCount; ++i)
    {
        float2 toPixel = pin.posW.xz - ripples[i].position;
        float dist = length(toPixel);
        float r = ripples[i].radius;

    // 只在涟漪圆环附近扰动
        float ringWidth = 1.5f;
        float inRing = saturate(1.0f - abs(dist - r) / ringWidth);

        if (inRing > 0.0f)
        {
            float2 dir = dist > 0.001f ? toPixel / dist : float2(1.0f, 0.0f);
            float wave = sin((dist - r) * 3.14159f / ringWidth);
            float strength = inRing * ripples[i].strength * wave * 0.3f;

            N.x += dir.x * strength;
            N.z += dir.y * strength;
            N = normalize(N);
        }
    }
    // 视线、光照方向
    float3 V = normalize(cameraPos - pin.posW);
    float3 L = sunDir;
    float3 H = normalize(V + L);

    // 基础水色
    float3 deepColor = float3(0.02f, 0.12f, 0.3f);
    float3 shallowColor = float3(0.08f, 0.3f, 0.6f);
    float heightFactor = saturate(pin.posW.y * 0.2f + 0.5f);
    float3 waterColor = lerp(deepColor, shallowColor, heightFactor);

    // Diffuse
    float NdotL = saturate(dot(N, L));
    float3 diffuse = waterColor * (NdotL * 0.5f * sunIntensity + 0.5f);
    diffuse *= sunColor;

    // Blinn-Phong specular (HDR — intentionally exceeds 1.0 for bloom)
    float NdotH = saturate(dot(N, H));
    float specular = pow(NdotH, 128.0f);
    float3 specularColor = sunColor * specular * sunIntensity * 15.0f;

    // Fresnel with full dynamic sky reflection
    float F0 = 0.02f;
    float NdotV = saturate(dot(N, V));
    float fresnel = F0 + (1.0f - F0) * pow(1.0f - NdotV, 5.0f);
    float3 reflectDir = reflect(-V, N);
    float3 reflectColor = SampleSkyReflection(reflectDir) * fresnel * 2.0;

    float3 color = diffuse + specularColor + reflectColor;

    // Fog
    float dist = length(cameraPos - pin.posW);
    float fogFactor = saturate((dist - fogStart) / (fogEnd - fogStart));
    color = lerp(color, skyColor, fogFactor);


    // 波峰Y值超过0.5时开始出现泡沫，超过1.0时完全是白色
    float J = (1.0f + dDxdx) * (1.0f + dDzdz) - dDxdz * dDzdx;
    float foam = 1.0f - saturate(J);
    foam = pow(foam, 2.0f);

    float3 foamColor = float3(2.5f, 2.5f, 2.5f); // HDR white — blooms on wave crests
    color = lerp(color, foamColor, foam * 0.8f);
    return float4(color, 1.0f);
}
