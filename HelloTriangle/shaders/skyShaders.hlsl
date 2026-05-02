// ============================================================
// skyShaders.hlsl
// 天空球の頂点・ピクセルシェーダー。手続き的空グラデーション、
// 太陽・月ディスク、FBMクラウド、星空、稲妻エフェクトを含む。
// ============================================================
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
    float3 moonCrescentDir; // 独立した月牙の向き：CPUが緩やかに回転させる
    float padCrescent;
    float moonBodyPow;
    float moonOccludePow;
    float crescentOffsetAmt;
    float padMoonParams;
    float lightningIntensity;
    float cloudDriftX;   // wind X * speed — applied as per-frame cloud position offset
    float cloudDriftY;   // wind Z * speed
    float padLightning;
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
    // 天空球頂点を遠平面に置く
    // 他のジオメトリの後ろに常にレンダリングされるよう
    // z=wとすることで、透視除算後の深度値が1.0（遠平面）になる
    vout.posH.z = vout.posH.w;
    vout.worldPos = vin.position;
    return vout;
}

// ハッシュ関数
float hash(float3 p)
{
    p = frac(p * 0.3183099 + 0.1);

    p *= 17.0;

    return frac(p.x * p.y * p.z * (p.x + p.y + p.z));
}

// ノイズ関数：より滑らかな結果を生成する
float noise(float3 x)
{
    float3 i = floor(x);
    float3 f = frac(x);

    // より滑らかな補間曲線を使用
    f = f * f * f * (f * (f * 6.0 - 15.0) + 10.0); // 滑らかなトランジション

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

// FBM（フラクタルブラウン運動）：異なる周波数のノイズを重畳する
float improved_fbm(float3 p, int octaves)
{
    float value = 0.0;
    float amplitude = 0.5;
    float frequency = 1.0;

    // より多くの重畳レイヤーを使用
    for (int i = 0; i < octaves; i++)
    {
        value += amplitude * noise(p * frequency);
        amplitude *= 0.5;
        frequency *= 2.0;
    }

    return value;
}

// 形状用のWorleyノイズ（セルノイズ）
float worley(float3 p)
{
    float3 id = floor(p);
    float3 fd = frac(p);

    float minDist = 1.0;

    // 周囲3x3x3のセルを検索
    for (int z = -1; z <= 1; z++)
    {
        for (int y = -1; y <= 1; y++)
        {
            for (int x = -1; x <= 1; x++)
            {
                float3 offset = float3(x, y, z);

                // 各セルに疑似乱数点を生成
                float3 cellPos = offset + hash(id + offset) * 0.9;

                // その点までの距離を計算
                float dist = length(fd - cellPos);
                minDist = min(minDist, dist);
            }
        }
    }

    return minDist;
}

// リアルな雲生成関数
float realisticClouds(float3 p, float scale, float time)
{
    // 緩やかな移動
    float speed = 0.03;
    float3 movement = float3(time * speed, time * speed * 0.2, 0.0);

    // わずかな回転を加える
    float angle = time * 0.01;
    float sinAngle = sin(angle);
    float cosAngle = cos(angle);

    float3 rotatedP = float3(
        p.x * cosAngle - p.z * sinAngle,
        p.y,
        p.x * sinAngle + p.z * cosAngle
    );

    // スケールと移動を適用
    float3 q = rotatedP * scale + movement;

    // Worleyノイズで雲の基本形状を生成（ブロッコリー状）
    float shape = worley(q * 1.5);
    shape = 1.0 - shape; // 反転して中心の値を高くする
    shape = pow(shape, 1.5); // 端をよりシャープにする

    // FBMで細部とテクスチャを追加
    float3 detailCoord = q * 3.0 + float3(time * 0.01, 0.0, time * 0.015);
    float detail = improved_fbm(detailCoord, 5);

    // 形状と細部を混合
    float finalCloud = shape * (0.7 + detail * 0.3);

    // 閾値を適用し、平滑化トランジションを使用
    float threshold = 0.1;
    finalCloud = smoothstep(threshold, threshold + 0.2, finalCloud);

    return finalCloud;
}

// より詳細な雲生成関数
float detailedClouds(float3 p, float scale, float time)
{
    float baseTime = time;
    float moveSpeed = 0.15;
    float smoothTime = baseTime * moveSpeed;

    // Wind-direction-based primary drift (cloudDriftX/Y set per-frame by CPU)
    float3 movement = float3(cloudDriftX * baseTime, 0.0, cloudDriftY * baseTime);

    // 明確な上下振動を追加：平滑な正弦波の組み合わせ、突変なし
    movement.y += sin(baseTime * 0.15) * 0.07;

    // 位置ベクトルに移動を適用
    float3 movedP = p + movement;

    // 正弦と余弦の組み合わせで平滑な回転を作る（直接時間を使わない）
    float rotX = sin(baseTime * 0.21) * 0.15;
    float rotZ = cos(baseTime * 0.18) * 0.12;

    // 平滑な回転
    float3 rotatedP = movedP;
    rotatedP.x += sin(baseTime * 0.05) * 0.2;
    rotatedP.z += cos(baseTime * 0.06) * 0.15;

    // 正弦波による平滑な歪み（直接時間を使わない）
    float distortFactor = sin(baseTime * 0.12) * 0.5 + 0.5; // 0〜1の平滑サイクル
    float distort = noise(rotatedP * 1.3 + float3(distortFactor * 3.0, 0, 0)) * 0.25;
    rotatedP.x += distort;
    rotatedP.z += distort * 0.4;

    // 基本雲形状の生成：平滑に変化するパラメータを使用
    float detailTime = sin(baseTime * 0.08) * 0.5 + 0.5; // 0〜1の平滑サイクル
    float base = improved_fbm(rotatedP * scale, 4);
    float detail = improved_fbm(rotatedP * scale * 1.7 + float3(detailTime * 2.0, 0, 0), 3);

    // 基本形状と細部の混合
    float cloud = base * 0.7 + detail * 0.3;

    // 雲形状パラメータ：平滑に変化する閾値
    float threshold = 0.35 - sin(baseTime * 0.1) * 0.05; // 平滑に変化する閾値
    cloud = max(0.0, cloud - threshold);
    cloud *= 2.0;
    cloud = min(cloud, 1.0); // 最大値を制限

    // 平滑なエッジ変化
    float edgeFactor = 1.1 + sin(baseTime * 0.06) * 0.1; // 平滑に変化するエッジ係数
    cloud = pow(cloud, edgeFactor);

    return cloud;
}

// 巻雲（Cirrus）：高高度の薄い白いすじ雲。風向きで速く流れる
float cirrusClouds(float3 dir)
{
    if (dir.y < 0.15) return 0.0;

    // 高高度への投影UV（遠近法的に引き延ばす）
    float2 uv = dir.xz / max(dir.y, 0.15) * 0.6;

    // 巻雲はメイン雲より1.5倍速く流れる
    uv += float2(cloudDriftX, cloudDriftY) * time * 1.5;

    float3 p = float3(uv * 4.0, time * 0.005);

    // 細長い繊維状テクスチャ：2軸のFBMを非対称に重ねる
    float n1 = improved_fbm(p,                          4);
    float n2 = improved_fbm(p * float3(2.0, 0.5, 2.0) + float3(7.3, 0.0, 3.1), 3);
    float n3 = improved_fbm(p * float3(4.0, 0.2, 4.0) + float3(1.7, 0.0, 8.9), 2);

    float c = n1 * 0.5 + n2 * 0.3 + n3 * 0.2;
    c = saturate(c - 0.42) * 3.5;
    c = pow(c, 2.2);

    // 地平線に近いほど薄くなる
    float heightFade = smoothstep(0.15, 0.50, dir.y);
    // 嵐の時は巻雲が消える（低層雲が覆う）
    float stormFade  = 1.0 - saturate(weatherIntensity * 1.5);

    return saturate(c) * heightFade * stormFade;
}

float3 renderStars(float3 dir, float time, float nightFactor)
{
    // 地平線付近および以下でフェードアウト
    float horizonMask = smoothstep(0.0, 0.2, dir.y);

    // 空を小セルに分割。各セルに1つの星が含まれる場合がある
    float3 p = dir * 150.0;
    float3 cellId = floor(p);
    float3 cellFrac = frac(p);

    float h = hash(cellId);
    if (h < 0.94) return float3(0, 0, 0); // 約6%のセルに星がある

    // セル内の星の位置
    float3 starOffset = float3(
        hash(cellId + float3(1.3, 7.7, 2.5)),
        hash(cellId + float3(9.1, 4.3, 6.7)),
        hash(cellId + float3(3.7, 11.1, 8.3))
    );

    float dist = length(cellFrac - starOffset);
    float core = 1.0 - smoothstep(0.0, 0.12, dist);

    // 等級：hが1に近いセルほど明るい星になる
    float mag = (h - 0.94) / 0.06;

    // またたき：各星が固有の位相と周波数を持つ
    float twinkle = 0.7 + 0.3 * sin(time * (1.5 + h * 6.0) + h * 57.3);

    // 色：高温星は青白、低温星は暖白
    float colorVar = hash(cellId + float3(5.5, 3.3, 9.9));
    float3 starColor = lerp(float3(0.7, 0.85, 1.0), float3(1.0, 0.95, 0.8), colorVar);

    return starColor * core * mag * twinkle * nightFactor * horizonMask * 5.0;
}

// ピクセルシェーダー
float4 skyPS(VSOutput input) : SV_Target
{
    float3 normalizedPos = normalize(input.worldPos);

    float horizonAdjust = -0.15;
    float adjustedY = normalizedPos.y - horizonAdjust;
    float h = adjustedY * 0.5f + 0.5f;

    // 1. 空のグラデーション色
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

    // 2. 夜間の空を暗くする（太陽・月に影響しないよう先に処理）
    float nightSky = saturate(-sunPosition.y * 3.0f);
    skyColor.rgb = lerp(skyColor.rgb, float3(0.02f, 0.02f, 0.08f), nightSky * 0.8f);

    // 2b. 星空（夜間に徐々に現れ、雲は後続ステップで自然に覆う）
    skyColor.rgb += renderStars(normalizedPos, time, nightSky);

    // 2c. 稲妻：空が瞬間的に青白になり、高周波のちらつきを伴う
    float flicker = lightningIntensity * (0.8 + 0.2 * sin(time * 50.0));
    skyColor.rgb += float3(0.85, 0.92, 1.0) * flicker * 3.0;

    // 3. 太陽光輝（夜間暗化の後に重ねる）
    float3 sunDir = sunPosition.xyz;
    float sunDot = max(0.0f, dot(normalizedPos, sunDir));
    // 太陽ディスクコア（HDR高輝度。ACESで圧縮される）
    float sunDisk = pow(sunDot, 2048.0f) * 20.0f;
    skyColor.rgb += float3(1.0f, 0.95f, 0.8f) * sunDisk;

    // 太陽ハロー
    float sunHalo = pow(sunDot, 256.0f) * 5.0f;
    skyColor.rgb += sunColor * sunHalo;

    // 大気散乱：太陽が地平線近くにあるとき、広いオレンジレッドグローを追加
    float sunNearHorizon = saturate(1.0f - abs(sunPosition.y) * 5.0f);
    float atmScatter = pow(sunDot, 4.0f) * sunNearHorizon * 2.5f;
    skyColor.rgb += float3(1.1f, 0.42f, 0.04f) * atmScatter;
    // 4. 月牙（夜間暗化の影響を受けない）
    float3 moonDir = moonPosition.xyz;
    float moonDot = max(0.0f, dot(normalizedPos, moonDir));

    // CPUから渡された緩やかに回転する方向を使用。太陽の実時間位置から独立させ、
    // 月が地平線を越えるときの月牙の向きの急変を防ぐ
    float3 crescentDir = normalize(moonDir - moonCrescentDir * crescentOffsetAmt);
    float crescentDot = max(0.0f, dot(normalizedPos, crescentDir));

    float moonBody    = pow(moonDot,    moonBodyPow);
    float moonOcclude = pow(crescentDot, moonOccludePow);
    float moonCrescent = saturate(moonBody - moonOcclude * 2.0f);
    skyColor.rgb += float3(1.0f, 1.0f, 0.95f) * moonCrescent * 18.0f;

    // ハロー
    float moonHalo = pow(moonDot, 100.0f) * 1.2f;
    skyColor.rgb += float3(0.3f, 0.4f, 0.8f) * moonHalo;

    // 5. 雲
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

        // 夜間は雲も暗くする
        cloudColor.rgb = lerp(cloudColor.rgb, cloudColor.rgb * 0.2f, nightSky);

        // 日の出・日の入り時に雲をオレンジピンクに染める
        float sunsetCloud = saturate(1.0f - abs(sunPosition.y) * 5.0f);
        cloudColor.rgb = lerp(cloudColor.rgb, float3(1.3f, 0.55f, 0.22f), sunsetCloud * 0.55f);

        // 稲妻で雲を内側から照らす
        cloudColor.rgb = lerp(cloudColor.rgb, float3(0.88, 0.92, 1.0), lightningIntensity * 0.7);

        skyColor = lerp(skyColor, cloudColor, cloudFactor);

        float sunEdgeEffect = pow(sunDot, 8.0) * 0.5;
        skyColor.rgb += float3(1.0, 0.8, 0.6) * cloudFactor * sunEdgeEffect * (1.0 - nightSky);
    }

    // 6. 巻雲（Cirrus）— 昼夜両方で薄く重ねる
    float cirrus = cirrusClouds(normalizedPos);
    if (cirrus > 0.005)
    {
        float3 cirrusCol = float3(0.88, 0.90, 1.00); // 薄い青白
        // 日の出・日の入りで淡いオレンジに染める
        float sunsetCloud = saturate(1.0f - abs(sunPosition.y) * 5.0f);
        cirrusCol = lerp(cirrusCol, float3(1.3, 0.68, 0.30), sunsetCloud * 0.45);
        // 夜間は暗くする
        cirrusCol = lerp(cirrusCol, cirrusCol * 0.15, nightSky * 0.8);
        skyColor.rgb = lerp(skyColor.rgb, cirrusCol, cirrus * 0.55);
    }

    return skyColor;
}
