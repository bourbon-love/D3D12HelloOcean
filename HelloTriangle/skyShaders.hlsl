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
    float pad;
    float3 sunColor;
    float padSunColor;
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
    float threshold = 0.4 - sin(baseTime * 0.1) * 0.05; // 平滑变化的阈值
    cloud = max(0.0, cloud - threshold);
    cloud *= 1.6;
    cloud = min(cloud, 1.0); // 限制最大值
    
    // 平滑的边缘变化
    float edgeFactor = 1.1 + sin(baseTime * 0.06) * 0.1; // 平滑变化的边缘因子
    cloud = pow(cloud, edgeFactor);
    
    return cloud;
}

// 像素着色器
float4 skyPS(VSOutput input) : SV_Target
{
    // 归一化方向向量
    float3 normalizedPos = normalize(input.worldPos);
    
    //调整地平线位置，将地平线降低
    float horizonAdjust = -0.15;
    float adjustedY = normalizedPos.y - horizonAdjust;
    // 基于高度计算渐变，使用非线性插值
    float h = adjustedY * 0.5f + 0.5f;
    
    // 使用平滑阶梯函数，不是线性插值
    float4 skyColor;
    if (h > 0.55f)
    {
        // 顶部到中间渐变
        float t = smoothstep(0.55f, 1.0f, h);
        skyColor = lerp(middleColor, topColor, t);
    }
    else
    {
        // 中间到底部渐变
        float t = smoothstep(0.0f, 0.55f, h);
        skyColor = lerp(bottomColor, middleColor, t);
    }
    
    // 添加太阳光晕效果
    float3 sunDir = sunPosition.xyz; //已经是归一化方向
    float sunDot = max(0.0f, dot(normalizedPos, sunDir));
    float sunIntensity = pow(sunDot, 256.0f) * 2.0f;
    
    // 向最终颜色添加太阳光晕
    skyColor.rgb += sunColor * sunIntensity;
    
    // ===== 修改云分布逻辑，使用平滑变化避免突变 =====
    
    // 修改高度计算 - 专注于下半球区域
    float baseHeight = normalizedPos.y;
    
    // 新的高度掩码 - 在下半球分布云，扩大范围
    float heightMask = 1.0 - smoothstep(-0.9, 0.0, baseHeight);
    
    // 使用原始时间值，但应用正弦函数创造平滑循环，避免突变
    float baseTime = time;
    
    // 创建平滑变化的噪声参数 - 通过正弦函数而不是直接使用时间
    float noiseParam1 = sin(baseTime * 0.05) * 0.5 + 0.5; // 0-1循环
    float noiseParam2 = cos(baseTime * 0.04) * 0.5 + 0.5; // 0-1循环
    
    // 添加变化，增强云层分布的动态性，但使用平滑的正弦变化
    heightMask = heightMask * (0.7 + 0.3 * sin(baseTime * 0.03));
    
    // 使用平滑变化的旋转参数
    float rotFactor = sin(baseTime * 0.02) * 3.14159; // 平滑循环的旋转角度
    float3 rotatedNoisePos = float3(
        normalizedPos.x * cos(rotFactor) - normalizedPos.z * sin(rotFactor),
        normalizedPos.y,
        normalizedPos.x * sin(rotFactor) + normalizedPos.z * cos(rotFactor)
    );
    
    // 降低噪声频率，创造更大的云区域，使用平滑变化的参数
    float3 noiseCoord = rotatedNoisePos * 2.0 + float3(noiseParam1, noiseParam2, noiseParam1 * noiseParam2);
    float distributionNoise = improved_fbm(noiseCoord, 3);
    
    // 使用平滑变化的阈值
    float noiseThreshold = 0.3 + sin(baseTime * 0.02) * 0.05;
    distributionNoise = smoothstep(noiseThreshold, noiseThreshold + 0.3, distributionNoise);
    
    // 添加水平方向的额外噪声，但使用平滑变化参数
    float hFreq = 1.5 + sin(baseTime * 0.01) * 0.3; // 平滑变化的频率
    float horizontalNoise = (sin(normalizedPos.x * hFreq + noiseParam1 * 6.28) * 0.5 + 0.5) *
                           (sin(normalizedPos.z * hFreq + noiseParam2 * 6.28) * 0.5 + 0.5);
    
    // 平滑变化的指数效果
    float powerFactor = 1.1 + sin(baseTime * 0.015) * 0.1;
    horizontalNoise = pow(horizontalNoise, powerFactor);
    
    // 平滑变化的云密度系数
    float densityFactor = 2.8 + sin(baseTime * 0.01) * 0.3;
    
    // 结合高度掩码和噪声，创建平滑变化的云分布
    float cloudMask = heightMask * (distributionNoise * 0.7 + horizontalNoise * 0.3) * cloudDensity * densityFactor;
    
    // 创建多个云层 - 降低阈值，增加云的区域
    if (cloudMask > 0.08) // 降低阈值，增加云的区域
    {
        // 使用不同参数创建多层云 - 优化下半球的云层参数
        // 第一层云 - 主要左右移动，减小缩放因子使云更大
        float cloudLayer1 = detailedClouds(normalizedPos, cloudScale * 0.6, time);
        
        // 第二层云 - 不同的偏移和移动速度，使用更小的缩放使云更大
        float cloudLayer2 = detailedClouds(normalizedPos * 1.1 + float3(4.0, -0.3, 1.0), cloudScale * 0.45, time * 0.75);
        
        // 第三层云 - 使用较小的偏移，增加覆盖度
        float cloudLayer3 = detailedClouds(normalizedPos * 1.5 + float3(-6.0, -0.4, 1.5), cloudScale * 0.35, time * 1.2);
        
        // 混合云层 - 增加各层的影响
        float cloudFactor = cloudLayer1 * 0.5 + cloudLayer2 * 0.35 + cloudLayer3 * 0.25;
        cloudFactor *= cloudMask;
        
        // 使用更大的最大不透明度
        cloudFactor = min(cloudFactor, 0.85);
        
        // 应用较弱的幂函数，减少对比度，使云更连续
        cloudFactor = pow(cloudFactor, 1.1);
        
        // 云颜色 - 使用更自然的云颜色，底部稍微偏暗
        float4 cloudColor = float4(1.0, 1.0, 1.0, 1.0);
        
        // 根据高度调整云颜色 - 下方云层略微偏灰
        float heightFactor = smoothstep(-1.0, 0.0, baseHeight);
        cloudColor.rgb = lerp(float3(0.9, 0.9, 0.95), float3(1.0, 1.0, 1.0), heightFactor);
        
        // 根据太阳方向调整云颜色
        float sunInfluence = pow(max(0.0, dot(normalizedPos, sunDir) * 0.5 + 0.5), 2.0);
        
        // 混合天空颜色和云颜色
        skyColor = lerp(skyColor, cloudColor, cloudFactor);
        
        // 添加太阳照射效果 - 使边缘发光
        float sunEdgeEffect = pow(sunDot, 8.0) * 0.5;
        skyColor.rgb += float3(1.0, 0.8, 0.6) * cloudFactor * sunEdgeEffect;
    }
    
    return skyColor;
    
}