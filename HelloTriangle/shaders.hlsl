
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
    
    // Apply 4 Gerstner waves to the vertex
    GerstnerWave(xz, waveDir0, waveAmp0, waveLen0, waveSpd0, waveStp0, disp, tangentX, tangentZ);
    GerstnerWave(xz, waveDir1, waveAmp1, waveLen1, waveSpd1, waveStp1, disp, tangentX, tangentZ);
    GerstnerWave(xz, waveDir2, waveAmp2, waveLen2, waveSpd2, waveStp2, disp, tangentX, tangentZ);
    GerstnerWave(xz, waveDir3, waveAmp3, waveLen3, waveSpd3, waveStp3, disp, tangentX, tangentZ);

    //world position = original position + displacement
    float3 worldPos = vin.position + disp;
    
    //normal = cross product of tangent vectors
    float3 normal = normalize(cross(tangentZ, tangentX));
    
    //change to homogeneous clip space
    float4 posV = mul(float4(worldPos, 1.0f), view);
    vout.posH = mul(posV, proj);
    vout.posW = worldPos;
    vout.normal = normal;
    vout.uv = vin.uv;
    
    
    return vout;
}

// -----------------------------------------------
// Pixel Shader — 三个效果全在这里
// -----------------------------------------------
float4 PSMain(VSOutput pin) : SV_TARGET
{
    float3 N = normalize(pin.normal);

    // 视线方向（从片元指向摄像机，归一化）
    float3 V = normalize(cameraPos - pin.posW);

    // 太阳方向（从天空Shader同步过来）
    float3 L = sunDir;

    // 半程向量
    float3 H = normalize(V + L);

    // -----------------------------------------------
    // 1. 基础水色（深浅混合，和之前一样）
    // -----------------------------------------------
    float3 deepColor = float3(0.01f, 0.08f, 0.2f);
    float3 shallowColor = float3(0.05f, 0.3f, 0.5f);
    float heightFactor = saturate(pin.posW.y / 1.2f + 0.5f);
    float3 waterColor = lerp(deepColor, shallowColor, heightFactor);

    // -----------------------------------------------
    // 2. Diffuse漫反射
    // -----------------------------------------------
    float NdotL = saturate(dot(N, L));
    
    // 用sunColor和sunIntensity驱动漫反射，日落时偏橙变暗
    float3 diffuse = waterColor * (NdotL * 0.7f * sunIntensity + 0.15f);
    diffuse *= sunColor; //  太阳颜色染色
    // -----------------------------------------------
    // 3. Blinn-Phong高光
    // -----------------------------------------------
    float NdotH = saturate(dot(N, H));
    float specular = pow(NdotH, 128.0f); // 指数越大，高光点越小越亮
    
    // 高光颜色直接用太阳颜色，日落时高光变橙红
    float3 specularColor = sunColor * specular * sunIntensity * 2.0f;

    // -----------------------------------------------
    // 4. Fresnel反射
    // -----------------------------------------------
    // Schlick近似，F0=0.02是水的基础反射率
    float F0 = 0.02f;
    float NdotV = saturate(dot(N, V));
    float fresnel = F0 + (1.0f - F0) * pow(1.0f - NdotV, 5.0f);
    
    //反射颜色用天空颜色乘以Fresnel系数，日落时天空变橙，反射也变橙
    float3 reflectColor = skyColor * fresnel;

    // -----------------------------------------------
    // 5. 波峰泡沫
    // -----------------------------------------------
    // 波峰Y值超过0.5时开始出现泡沫，超过1.0时完全是白色
    float foam = smoothstep(1.2f, 2.0f, pin.posW.y);
    float3 foamColor = float3(1.0f, 1.0f, 1.0f);

    // -----------------------------------------------
    // 合并所有项
    // -----------------------------------------------
    float3 color = diffuse // 漫反射水色
                 + specularColor // 高光
                 + reflectColor; // Fresnel天空反射
    // 指数雾
    float dist = length(cameraPos - pin.posW); // 片元到摄像机的距离
    float fogStart = 80.0f; // 从80单位开始出雾
    float fogEnd = 180.0f; // 180单位完全是雾色
    float fogFactor = saturate((dist - fogStart) / (fogEnd - fogStart));

// 雾色和天空色一致，远处海面自然融入天空
    float3 fogColor = skyColor;
    color = lerp(color, fogColor, fogFactor);
    
    // 最后叠加泡沫（泡沫覆盖其他所有效果）
    color = lerp(color, foamColor, foam * 0.3f);

    return float4(color, 1.0f);
}