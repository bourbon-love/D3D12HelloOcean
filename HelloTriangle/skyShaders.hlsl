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
    float3 moonCrescentDir; // 独立月牙朝向，由 CPU 缓慢旋转
    float padCrescent;
    float moonBodyPow;
    float moonOccludePow;
    float crescentOffsetAmt;
    float padMoonParams;
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
    // set the skybox vertices to be at the far plane distance,
    //so that they will always be rendered behind all other geometry
    //set z to w to keep the depth value at 1.0 (far plane) after perspective division
    vout.posH.z = vout.posH.w;
    vout.worldPos = vin.position;
    return vout;
}

//哈希函数
float hash(float3 p)
{
    p = frac(p * 0.3183099 + 0.1);
    
    p *= 17.0;
    
    return frac(p.x * p.y * p.z * (p.x + p.y + p.z));
}

// Noise函数，产生更流畅的结果
float noise(float3 x)
{
    float3 i = floor(x);
    float3 f = frac(x);
    
    // 使用更平滑的插值曲线
    f = f * f * f * (f * (f * 6.0 - 15.0) + 10.0); // 更平滑的过渡
    
    float a = hash(i + float3(0, 0, 0));
    float b = hash(i + float3(1, 0, 0));
    float c = hash(i + float3(0, 1, 0));
    float d = hash(i + float3(1, 1, 0));
    float e = hash(i + float3(0, 0, 1));
    float f1 = hash(i + float3(1, 0, 1));
    float g = hash(i + float3(0, 1, 1));
    float h = hash(i + float3(1, 1, 1));
    
    return lerp(
            lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y),
            lerp(lerp(e, f1, f.x), lerp(g, h, f.x), f.y),
            f.z);
}

//FBM（分型布朗运动）-叠加不同频率的噪声
float improved_fbm(float3 p, int octaves)
{
    float value = 0.0;
    float amplitude = 0.5;
    float frequency = 1.0;
    
    // 使用更多的叠加层次
    for (int i = 0; i < octaves; i++)
    {
        value += amplitude * noise(p * frequency);
        amplitude *= 0.5;
        frequency *= 2.0;
    }
    
    return value;
}

// 用于形状的Worley噪声（元胞噪声）
float worley(float3 p)
{
    float3 id = floor(p);
    float3 fd = frac(p);
    
    float minDist = 1.0;
    
    // 检查周围3x3x3的单元格
    for (int z = -1; z <= 1; z++)
    {
        for (int y = -1; y <= 1; y++)
        {
            for (int x = -1; x <= 1; x++)
            {
                float3 offset = float3(x, y, z);
                
                // 为每个单元格生成一个伪随机点
                float3 cellPos = offset + hash(id + offset) * 0.9;
                
                // 计算到该点的距离
                float dist = length(fd - cellPos);
                minDist = min(minDist, dist);
            }
        }
    }
    
    return minDist;
}

// 逼真云函数
float realisticClouds(float3 p, float scale, float time)
{
    // 慢速移动
    float speed = 0.03;
    float3 movement = float3(time * speed, time * speed * 0.2, 0.0);
    
    // 添加轻微旋转
    float angle = time * 0.01;
    float sinAngle = sin(angle);
    float cosAngle = cos(angle);
    
    float3 rotatedP = float3(
        p.x * cosAngle - p.z * sinAngle,
        p.y,
        p.x * sinAngle + p.z * cosAngle
    );
    
    // 缩放并添加移动
    float3 q = rotatedP * scale + movement;
    
    // 使用Worley噪声创建云基础形状（花椰菜状）
    float shape = worley(q * 1.5);
    shape = 1.0 - shape; // 反转，使值在中心更高
    shape = pow(shape, 1.5); // 使边缘更加锐利
    
    // 使用FBM添加细节和纹理
    float3 detailCoord = q * 3.0 + float3(time * 0.01, 0.0, time * 0.015);
    float detail = improved_fbm(detailCoord, 5);
    
    // 混合形状和细节
    float finalCloud = shape * (0.7 + detail * 0.3);
    
    // 应用阈值，但使用平滑过渡
    float threshold = 0.1;
    finalCloud = smoothstep(threshold, threshold + 0.2, finalCloud);
    
    return finalCloud;
}

// 更详细的云生成函数
float detailedClouds(float3 p, float scale, float time)
{
    // 强化的横向移动 - 主要在x轴方向移动，但不依赖累积时间
    // 使用小范围的time值，避免过大导致的问题
    float baseTime = time;
    // 使用正弦函数创造循环运动，避免突变
    float moveSpeed = 0.15;
    float smoothTime = baseTime * moveSpeed;
    
    // 使用正弦函数创造平滑的往返运动
    float3 movement = float3(-smoothTime, 0.0, 0.0);
    
    // 添加明显的上下波动 - 使用平滑的正弦波组合，无突变
    movement.y += sin(baseTime * 0.15) * 0.07;
    
    // 将移动应用到位置向量
    float3 movedP = p + movement;
    
    // 使用正弦和余弦函数组合创建平滑的旋转，而不是直接使用时间
    float rotX = sin(baseTime * 0.21) * 0.15;
    float rotZ = cos(baseTime * 0.18) * 0.12;
    
    // 平滑旋转
    float3 rotatedP = movedP;
    rotatedP.x += sin(baseTime * 0.05) * 0.2;
    rotatedP.z += cos(baseTime * 0.06) * 0.15;
    
    // 添加平滑的扰动，基于正弦波而不是直接使用时间
    float distortFactor = sin(baseTime * 0.12) * 0.5 + 0.5; // 0到1的平滑循环
    float distort = noise(rotatedP * 1.3 + float3(distortFactor * 3.0, 0, 0)) * 0.25;
    rotatedP.x += distort;
    rotatedP.z += distort * 0.4;
    
    // 生成基本云形状 - 使用平滑变化的参数
    float detailTime = sin(baseTime * 0.08) * 0.5 + 0.5; // 0到1的平滑循环
    float base = improved_fbm(rotatedP * scale, 4);
    float detail = improved_fbm(rotatedP * scale * 1.7 + float3(detailTime * 2.0, 0, 0), 3);
    
    // 混合基础和细节
    float cloud = base * 0.7 + detail * 0.3;
    
    // 云朵形状参数 - 使用平滑变化的阈值
    float threshold = 0.35 - sin(baseTime * 0.1) * 0.05; // 平滑变化的阈值
    cloud = max(0.0, cloud - threshold);
    cloud *= 2.0;
    cloud = min(cloud, 1.0); // 限制最大值
    
    // 平滑的边缘变化
    float edgeFactor = 1.1 + sin(baseTime * 0.06) * 0.1; // 平滑变化的边缘因子
    cloud = pow(cloud, edgeFactor);
    
    return cloud;
}

// 像素着色器
float4 skyPS(VSOutput input) : SV_Target
{
    float3 normalizedPos = normalize(input.worldPos);
    
    float horizonAdjust = -0.15;
    float adjustedY = normalizedPos.y - horizonAdjust;
    float h = adjustedY * 0.5f + 0.5f;
    
    // 1. 天空渐变颜色
    float4 skyColor;
    if (h > 0.55f)
    {
        float t = smoothstep(0.55f, 1.0f, h);
        skyColor = lerp(middleColor, topColor, t);
    }
    else
    {
        float t = smoothstep(0.0f, 0.55f, h);
        skyColor = lerp(bottomColor, middleColor, t);
    }

    // 2. 夜晚天空变暗（先做，不影响后面的太阳月亮）
    float nightSky = saturate(-sunPosition.y * 3.0f);
    skyColor.rgb = lerp(skyColor.rgb, float3(0.02f, 0.02f, 0.08f), nightSky * 0.8f);

    // 3. 太阳光晕（在夜晚变暗之后叠加，不被压暗）
    float3 sunDir = sunPosition.xyz;
    float sunDot = max(0.0f, dot(normalizedPos, sunDir));
    // 太阳光盘核心（明亮暖白）
    float sunDisk = pow(sunDot, 2048.0f) * 8.0f;
    skyColor.rgb += float3(1.0f, 0.95f, 0.8f) * sunDisk;

    // 太阳光晕用 sunColor（日落时橙红）
    float sunHalo = pow(sunDot, 256.0f) * 2.0f;
    skyColor.rgb += sunColor * sunHalo;
    // 4. 月牙（同样不被夜晚压暗影响）
    float3 moonDir = moonPosition.xyz;
    float moonDot = max(0.0f, dot(normalizedPos, moonDir));

    // 使用 CPU 传入的缓慢旋转方向，与太阳实时位置解耦，避免月亮过地平线时月牙朝向突变
    float3 crescentDir = normalize(moonDir - moonCrescentDir * crescentOffsetAmt);
    float crescentDot = max(0.0f, dot(normalizedPos, crescentDir));

    float moonBody    = pow(moonDot,    moonBodyPow);
    float moonOcclude = pow(crescentDot, moonOccludePow);
    float moonCrescent = saturate(moonBody - moonOcclude * 2.0f);
    skyColor.rgb += float3(1.0f, 1.0f, 0.95f) * moonCrescent * 8.0f;
    
    
    // 光晕
    float moonHalo = pow(moonDot, 100.0f) * 0.5f;
    skyColor.rgb += float3(0.3f, 0.4f, 0.8f) * moonHalo;

    // 5. 云
    float baseHeight = normalizedPos.y;
    float baseTime = time;

    float heightMask = smoothstep(-0.1, 0.3, baseHeight)
        * (1.0 - smoothstep(0.5, 0.8, baseHeight));
    heightMask *= (0.7 + 0.3 * sin(baseTime * 0.03));

    float noiseParam1 = sin(baseTime * 0.05) * 0.5 + 0.5;
    float noiseParam2 = cos(baseTime * 0.04) * 0.5 + 0.5;

    float rotFactor = sin(baseTime * 0.02) * 3.14159;
    float3 rotatedNoisePos = float3(
        normalizedPos.x * cos(rotFactor) - normalizedPos.z * sin(rotFactor),
        normalizedPos.y,
        normalizedPos.x * sin(rotFactor) + normalizedPos.z * cos(rotFactor));

    float3 noiseCoord = rotatedNoisePos * 2.0
        + float3(noiseParam1, noiseParam2, noiseParam1 * noiseParam2);
    float distributionNoise = improved_fbm(noiseCoord, 3);

    float noiseThreshold = 0.3 + sin(baseTime * 0.02) * 0.05;
    distributionNoise = smoothstep(noiseThreshold, noiseThreshold + 0.3, distributionNoise);

    float hFreq = 1.5 + sin(baseTime * 0.01) * 0.3;
    float horizontalNoise =
        (sin(normalizedPos.x * hFreq + noiseParam1 * 6.28) * 0.5 + 0.5) *
        (sin(normalizedPos.z * hFreq + noiseParam2 * 6.28) * 0.5 + 0.5);
    horizontalNoise = pow(horizontalNoise, 1.1 + sin(baseTime * 0.015) * 0.1);

    float densityFactor = 2.8 + sin(baseTime * 0.01) * 0.3;
    float stormCloudDensity = cloudDensity * (1.0f + weatherIntensity * 2.0f);
    float cloudMask = heightMask
        * (distributionNoise * 0.7 + horizontalNoise * 0.3)
        * stormCloudDensity * densityFactor;

    if (cloudMask > 0.08)
    {
        float cloudLayer1 = detailedClouds(normalizedPos, cloudScale * 0.6, time);
        float cloudLayer2 = detailedClouds(normalizedPos * 1.1 + float3(4.0, -0.3, 1.0), cloudScale * 0.45, time * 0.75);
        float cloudLayer3 = detailedClouds(normalizedPos * 1.5 + float3(-6.0, -0.4, 1.5), cloudScale * 0.35, time * 1.2);

        float cloudFactor = cloudLayer1 * 0.5 + cloudLayer2 * 0.35 + cloudLayer3 * 0.25;
        cloudFactor *= cloudMask;
        cloudFactor = min(cloudFactor, 0.85);
        cloudFactor = pow(cloudFactor, 1.1);

        float heightFactor = smoothstep(-1.0, 0.0, baseHeight);
        float3 calmCloudBase = float3(0.75, 0.75, 0.82);
        float3 stormCloudBase = float3(0.25, 0.25, 0.30);
        float4 cloudColor;
        cloudColor.rgb = lerp(
            lerp(calmCloudBase, float3(1.0, 1.0, 1.0), heightFactor),
            stormCloudBase, weatherIntensity);
        cloudColor.a = 1.0;

        // 夜晚云也变暗
        cloudColor.rgb = lerp(cloudColor.rgb, cloudColor.rgb * 0.2f, nightSky);

        skyColor = lerp(skyColor, cloudColor, cloudFactor);

        float sunEdgeEffect = pow(sunDot, 8.0) * 0.5;
        skyColor.rgb += float3(1.0, 0.8, 0.6) * cloudFactor * sunEdgeEffect * (1.0 - nightSky);
    }

    return skyColor;
}