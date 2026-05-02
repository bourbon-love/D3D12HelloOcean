// ============================================================
// shaders.hlsl
// 海洋表面の頂点・ピクセルシェーダー。
// FFT高さマップとGerstnerウェーブによる変位、Phong+Fresnelライティング、
// ヤコビアン泡沫生成、SSRを担当する。
// ============================================================
//shaders.hlsl
cbuffer SceneCB : register(b0)
{
    float4x4 view;
    float4x4 proj;
    float    time;
    float3 cameraPos;

    float3 sunDir; // 太陽方向（正規化済み）
    float sunIntensity; // 太陽強度：日の出・日の入り時に低下
    float3 sunColor; // 太陽色：日の出はオレンジ寄り、正午は白寄り
    float padSun;
    float3 skyColor; // 天空主色：Fresnel反射に使用
    float padSky;
    float fogStart;
    float fogEnd;
    float foamIntensity;
    float ssrMix;
    // 4つの波のパラメータ
    // 各波は方向、振幅、波長、速度、ステップ、パディングを持つ
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


Texture2D<float4> g_heightMap   : register(t0);
Texture2D<float4> g_dztMap      : register(t1);
Texture2D<float4> g_skySnapshot : register(t2);
Texture2D<float>  g_shadowMap   : register(t3); // 浮遊物体のシャドウマップ
SamplerState g_sampler : register(s0);

cbuffer ShadowCB : register(b2)
{
    float4x4 lightViewProj;
    float    shadowBias;
    float    shadowStrength;
    float    shadowEnabled;
    float    pad_shadow;
};


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

// Gerstnerウェーブ関数：波パラメータに基づいて頂点位置と法線を変形する
// 入力：頂点のXZ位置、波方向・振幅・波長・速度・ステップ
// 出力：変形後の変位量と接線ベクトル
void GerstnerWave(
    float2 xz, float2 dir, float amp, float wavelen,
    float spd, float steep,
    inout float3 disp,
    inout float3 tangentX,
    inout float3 tangentZ)
{
    float k = 2.0f * 3.14159265f / wavelen;
    float f = k * dot(dir, xz) - spd * time;

    // Qはsteepをそのまま使用（0〜1の範囲で制御）

    float Q = steep;

    float sinF = sin(f);
    float cosF = cos(f);

    // XYZ変位
    disp.x += Q * amp * dir.x * cosF;
    disp.y += amp * sinF;
    disp.z += Q * amp * dir.y * cosF;

    // 接線X方向
    tangentX.x += 1.0f - Q * dir.x * dir.x * k * amp * sinF;
    tangentX.y += dir.x * k * amp * cosF;
    tangentX.z -= Q * dir.x * dir.y * k * amp * sinF;

    // 接線Z方向
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

    // 端までの距離を0〜1に正規化
    float halfSize = FFT_TILE_SIZE * 0.5f;
    float distToEdgeX = 1.0f - abs(vin.position.x) / halfSize;
    float distToEdgeZ = 1.0f - abs(vin.position.z) / halfSize;
    float distToEdge = min(distToEdgeX, distToEdgeZ);

    // エッジ減衰係数：端で0、内部で1、トランジション幅は調整可
    float heightFade = smoothstep(0.0f, 0.2f, distToEdge); // 0.05 = 5%のトランジション帯
    float chopFade = smoothstep(0.0f, 0.4f, distToEdge);
    // Gerstner変位に減衰を乗算
    GerstnerWave(xz, waveDir0, waveAmp0, waveLen0, waveSpd0, waveStp0, disp, tangentX, tangentZ);
    GerstnerWave(xz, waveDir1, waveAmp1, waveLen1, waveSpd1, waveStp1, disp, tangentX, tangentZ);
    GerstnerWave(xz, waveDir2, waveAmp2, waveLen2, waveSpd2, waveStp2, disp, tangentX, tangentZ);
    GerstnerWave(xz, waveDir3, waveAmp3, waveLen3, waveSpd3, waveStp3, disp, tangentX, tangentZ);
    disp *= heightFade;

    float3 worldPos = vin.position + disp;
    float2 fftUV = vin.position.xz / FFT_TILE_SIZE;

    float4 fftSample = g_heightMap.SampleLevel(g_sampler, fftUV, 0);
    float fftHeight = fftSample.x * FFT_HEIGHT_SCALE * heightFade;
    float fftDx = fftSample.z * FFT_CHOP_SCALE * chopFade; // より強い減衰を使用
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
// --- 手続き的泡沫ノイズヘルパー ---
float hash21(float2 p)
{
    p = frac(p * float2(127.1, 311.7));
    p += dot(p, p + 45.32);
    return frac(p.x * p.y);
}

float valueNoise(float2 p)
{
    float2 i = floor(p);
    float2 f = frac(p);
    float2 u = f * f * (3.0 - 2.0 * f);
    float a = hash21(i);
    float b = hash21(i + float2(1, 0));
    float c = hash21(i + float2(0, 1));
    float d = hash21(i + float2(1, 1));
    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}

// 反射方向から空の色を再構成。動的空パレットと一致させる
float3 SampleSkyReflection(float3 reflDir)
{
    // 高度：0=地平線、1=天頂
    float h = saturate(reflDir.y);

    // 太陽の高さから昼夜係数を計算
    float dayF  = saturate(sunDir.y * 4.0 + 0.4);
    // 夕焼け係数：太陽が地平線近くにあるときにピーク
    float sunsetF = saturate(1.0 - abs(sunDir.y) * 5.0);
    sunsetF = sunsetF * sunsetF;

    // 夜と昼の天頂・地平線カラー
    float3 zenithDay  = float3(0.08, 0.25, 0.72);
    float3 horizDay   = skyColor * 1.5;
    // 夜空：ほぼ黒に近い暗いニュートラルグレー（青色バイアスを抑制）
    float3 zenithNight = float3(0.004, 0.005, 0.010);
    float3 horizNight  = float3(0.007, 0.008, 0.014);

    float3 zenith = lerp(zenithNight, zenithDay,  dayF);
    float3 horiz  = lerp(horizNight,  horizDay,   dayF);
    float3 sky    = lerp(horiz, zenith, h);

    // 夕焼けオーバーレイ：地平線付近にオレンジゴールドを加算
    float3 sunsetHorizon = float3(1.6, 0.72, 0.12); // HDR ゴールド
    float3 sunsetZenith  = float3(0.10, 0.16, 0.48); // 青紫
    float3 sunsetCol = lerp(sunsetZenith, sunsetHorizon, saturate(1.2 - h * 3.0));
    sky = lerp(sky, sunsetCol, sunsetF * saturate(1.2 - h * 2.0));

    // 反射方向の太陽グロー
    float sunDotR = max(0.0, dot(reflDir, sunDir));
    sky += sunColor * pow(sunDotR, 6.0) * 4.0;

    // 地平線以下：深海色へフェード（夜は暗く）
    float3 deepWater = float3(0.01, 0.02, 0.04) * (0.3 + dayF * 0.7);
    return lerp(deepWater, sky, smoothstep(-0.05, 0.1, reflDir.y));
}

float4 PSMain(VSOutput pin) : SV_TARGET
{
    // ピクセル単位でheightMapから法線を再計算（VSと同じスケールを使用）
    const float texelSize = 1.0f / 256.0f;
    const float worldPerTexel = FFT_TILE_SIZE / 256.0f;

    // 周辺高さをサンプリング
    float hL = -g_heightMap.SampleLevel(g_sampler, pin.uv + float2(-texelSize, 0), 0).x * FFT_HEIGHT_SCALE;
    float hR = -g_heightMap.SampleLevel(g_sampler, pin.uv + float2(texelSize, 0), 0).x * FFT_HEIGHT_SCALE;
    float hD = -g_heightMap.SampleLevel(g_sampler, pin.uv + float2(0, -texelSize), 0).x * FFT_HEIGHT_SCALE;
    float hU = -g_heightMap.SampleLevel(g_sampler, pin.uv + float2(0, texelSize), 0).x * FFT_HEIGHT_SCALE;

    // 周辺Dxをサンプリング
    float dxL = g_heightMap.SampleLevel(g_sampler, pin.uv + float2(-texelSize, 0), 0).z * FFT_CHOP_SCALE;
    float dxR = g_heightMap.SampleLevel(g_sampler, pin.uv + float2(texelSize, 0), 0).z * FFT_CHOP_SCALE;
    float dxD = g_heightMap.SampleLevel(g_sampler, pin.uv + float2(0, -texelSize), 0).z * FFT_CHOP_SCALE;
    float dxU = g_heightMap.SampleLevel(g_sampler, pin.uv + float2(0, texelSize), 0).z * FFT_CHOP_SCALE;

// 周辺Dzをサンプリング
    float dzL = g_dztMap.SampleLevel(g_sampler, pin.uv + float2(-texelSize, 0), 0).x * FFT_CHOP_SCALE;
    float dzR = g_dztMap.SampleLevel(g_sampler, pin.uv + float2(texelSize, 0), 0).x * FFT_CHOP_SCALE;
    float dzD = g_dztMap.SampleLevel(g_sampler, pin.uv + float2(0, -texelSize), 0).x * FFT_CHOP_SCALE;
    float dzU = g_dztMap.SampleLevel(g_sampler, pin.uv + float2(0, texelSize), 0).x * FFT_CHOP_SCALE;

// 偏微分
    float dHdx = (hR - hL) / (2.0f * worldPerTexel);
    float dHdz = (hU - hD) / (2.0f * worldPerTexel);
    float dDxdx = (dxR - dxL) / (2.0f * worldPerTexel);
    float dDzdz = (dzU - dzD) / (2.0f * worldPerTexel);
    float dDxdz = (dxU - dxD) / (2.0f * worldPerTexel);
    float dDzdx = (dzR - dzL) / (2.0f * worldPerTexel);

// ヤコビアン法線：tangentX = (1+dDxdx, dHdx, dDzdx), tangentZ = (dDxdz, dHdz, 1+dDzdz)
    float3 tangentX = float3(1.0f + dDxdx, dHdx, dDzdx);
    float3 tangentZ = float3(dDxdz, dHdz, 1.0f + dDzdz);
    float3 N = normalize(cross(tangentZ, tangentX));

    // 波紋による法線の乱れ
    for (uint i = 0; i < rippleCount; ++i)
    {
        float2 toPixel = pin.posW.xz - ripples[i].position;
        float dist = length(toPixel);
        float r = ripples[i].radius;

    // 波紋リング付近のみ乱れを適用
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
    // 視線・光源方向
    float3 V = normalize(cameraPos - pin.posW);
    float3 L = sunDir;
    float3 H = normalize(V + L);

    // 水体固有色：深海はほぼ黒に近い暗色。
    // 可視光の「青」はFresnelによる空の映り込みで生まれるため、
    // 水体自体に鮮やかな青を持たせない。
    float3 deepColor    = float3(0.004, 0.014, 0.030);
    float3 shallowColor = float3(0.007, 0.022, 0.048);
    // 高さによる補間は極めて控えめに（縞模様防止）
    float heightFactor = saturate(pin.posW.y * 0.08f + 0.5f);
    float3 waterColor = lerp(deepColor, shallowColor, heightFactor);

    // Diffuse：夜間はニュートラルな暗色（sunColorの青色バイアスを除去）
    float NdotL = saturate(dot(N, L));
    float nightT = saturate(1.0 - sunIntensity * 3.0);   // 0=昼, 1=深夜
    float3 nightAmbient = float3(0.022, 0.024, 0.028);   // ほぼ黒、ごく僅かに青みがかる
    float3 ambLight = lerp(sunColor, nightAmbient, nightT);
    float3 diffuse = waterColor * (NdotL * 0.5f * sunIntensity + 0.5f);
    diffuse *= ambLight;

    // スペキュラー：タイトな鏡面ハイライト + 広散乱ローブの2層構造
    float NdotH = saturate(dot(N, H));
    float specTight = pow(NdotH, 128.0f) * 14.0f;
    // 夜間（月光）は広散乱ローブを無効：月色{0.6,0.7,1.0}が緑かぶりを起こすため
    float daySpec   = saturate((sunIntensity - 0.35) * 10.0);
    float specBroad = pow(NdotH,  18.0f) *  0.6f * daySpec;
    float3 specularColor = sunColor * (specTight + specBroad) * sunIntensity;

    // Fresnel
    float F0 = 0.02f;
    float NdotV = saturate(dot(N, V));
    float fresnel = F0 + (1.0f - F0) * pow(1.0f - NdotV, 5.0f);
    float3 reflectDir = reflect(-V, N);

    // SSR：法線で反射UVをわずかに揺らして粗さを模倣
    float3 reflectColor;
    {
        float4x4 vp = mul(view, proj);
        float4 reflClip = mul(float4(pin.posW + reflectDir * 300.0, 1.0), vp);
        float2 reflUV = reflClip.xy / reflClip.w * float2(0.5, -0.5) + 0.5;
        reflUV += N.xz * 0.018;   // 法線による微小揺らぎ（粗面反射）
        reflUV = saturate(reflUV);

        float2 edgeFade = saturate(min(reflUV, 1.0 - reflUV) * 6.0);
        float fade = min(edgeFade.x, edgeFade.y);
        fade *= saturate(reflectDir.y * 4.0 + 0.3);
        fade *= ssrMix;

        float3 ssrSample  = g_skySnapshot.SampleLevel(g_sampler, reflUV, 0).rgb;
        float3 procSample = SampleSkyReflection(reflectDir);
        // 夜間は反射輝度を大幅に下げる（sunIntensity=0で約5%の輝度）
        float reflBright = lerp(0.10, 2.0, saturate(sunIntensity * 1.8));
        reflectColor = lerp(procSample, ssrSample, fade) * fresnel * reflBright;
    }

    float3 color = diffuse + specularColor + reflectColor;

    // --- 次表面散乱（SSS）：波頂部の透過光 ---
    // 太陽光が薄い波峰を透過するときの青緑色の輝き
    // 太陽に向かって見るとき（逆光）に最も顕著に現れる
    {
        float3 sssDir   = normalize(-sunDir + N * 0.5);
        float  sssView  = pow(saturate(dot(V, sssDir)), 4.0);
        // 波高が高いほど（波頂部）薄い水層を光が通り抜ける
        float  sssCrest = saturate(pin.posW.y * 0.18 + 0.2);
        // 夜間はSSSをゼロにする。
        // sunDirは夜間に月の方向へブレンドされるためsunDir.yは使えない。
        // moonIntensity=0.15固定、日中のsunIntensityは0.5以上なので
        // 0.35をしきい値として昼夜を判定する。
        float  sssDaylight = saturate((sunIntensity - 0.35) * 10.0);
        float3 sssColor = float3(0.0, 0.55, 0.40) * sssView * sssCrest
                          * min(sunIntensity, 1.5) * 1.4 * sssDaylight;
        color += sssColor;
    }

    // 霧
    float dist = length(cameraPos - pin.posW);
    float fogFactor = saturate((dist - fogStart) / (fogEnd - fogStart));
    color = lerp(color, skyColor, fogFactor);


    // --- 波頂部泡沫 ---
    float J = (1.0f + dDxdx) * (1.0f + dDzdz) - dDxdz * dDzdx;

    // ヤコビアンが1を下回るほど（波が折り畳まれるほど）泡沫が強くなる
    float sharpness = lerp(5.0, 2.5, foamIntensity);
    float rawFoam   = pow(saturate(1.0 - J), sharpness);
    rawFoam *= lerp(0.15, 1.0, foamIntensity);

    // 上向き面のみに泡沫を制限（波の側面に貼り付かないよう）
    float topFace = saturate((N.y - 0.45) / 0.55);

    // FBMノイズ（3オクターブ）：高周波 + smoothstepで大まかな迷彩ブロブを除去
    float2 fuv  = pin.uv * 95.0 + float2(time * 0.10, time * 0.06);
    float  fn1  = valueNoise(fuv);
    float  fn2  = valueNoise(fuv * 3.2 + float2(5.1, 1.9));
    float  fn3  = valueNoise(fuv * 7.5 + float2(2.3, 4.7));
    float  fbm  = fn1 * 0.50 + fn2 * 0.32 + fn3 * 0.18;
    // smoothstepで閾値化：滑らかな塊をシャープなストリークに変換
    float  foamNoise = smoothstep(0.30, 0.68, fbm);

    float foamMask = saturate(rawFoam * topFace * (0.15 + foamNoise * 1.6));

    // 嵐時の微細飛沫レイヤー
    float spray = 0.0;
    [branch]
    if (foamIntensity > 0.35)
    {
        float2 suv = pin.uv * 120.0 + float2(time * 0.20, time * 0.14);
        float  sn  = valueNoise(suv);
        spray = smoothstep(0.55, 0.80, sn) * saturate((foamIntensity - 0.35) * 3.0);
        spray *= saturate(rawFoam * topFace * 5.0);
    }

    // HDR泡沫色（ブルームを誘発）
    // 実際の海洋泡沫は白色：青緑のバイアスを除去
    float3 foamWhite = float3(2.2, 2.2, 2.2);
    float3 foamEdge  = float3(1.9, 1.9, 1.95);
    float3 foamColor = lerp(foamEdge, foamWhite, foamNoise);
    color = lerp(color, foamColor, foamMask * 0.90);
    color = lerp(color, foamWhite * 0.75, spray * 0.45);

    // --- 影（浮遊物体から海面へのシャドウ） ---
    [branch]
    if (shadowEnabled > 0.5 && shadowStrength > 0.0)
    {
        float4 posLS  = mul(float4(pin.posW, 1.0), lightViewProj);
        float2 suv    = posLS.xy / posLS.w * float2(0.5, -0.5) + 0.5;
        float  sdepth = posLS.z / posLS.w;

        [branch]
        if (suv.x > 0.01 && suv.x < 0.99 && suv.y > 0.01 && suv.y < 0.99 && sdepth < 1.0)
        {
            float  shadow = 0.0;
            float  dx     = 1.0 / 2048.0;
            int2   tc     = (int2)(suv * 2048.0);
            // 3×3 PCF（整数座標Loadで正確な深度比較）
            [unroll] for (int sy = -1; sy <= 1; sy++)
            [unroll] for (int sx = -1; sx <= 1; sx++)
            {
                float sm = g_shadowMap.Load(int3(tc + int2(sx, sy), 0));
                shadow += (sm + shadowBias < sdepth) ? 1.0 : 0.0;
            }
            shadow /= 9.0;
            color.rgb *= (1.0 - shadow * shadowStrength);
        }
    }

    return float4(color, 1.0f);
}
