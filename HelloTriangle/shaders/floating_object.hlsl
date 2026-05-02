// ============================================================
// floating_object.hlsl
// 浮遊オブジェクト（木箱）シェーダー。
// 高さマップをサンプリングして波面に沿った姿勢を決定し、
// Phong照明で描画する。
// ============================================================
Texture2D    g_heightMap : register(t0);
SamplerState g_sampler   : register(s0);

cbuffer ObjectCB : register(b0)
{
    matrix  viewProj;
    float3  worldPos;       // オブジェクト中心のXZ座標（Yは高さマップで決定）
    float   objectScale;
    float3  sunDir;
    float   sunIntensity;
    float3  sunColor;
    float   gridWorldSize;  // 通常400.0
    float3  cameraPos;
    float   dropOffset; // Y offset above wave surface during fall
};

struct VSIn  { float3 pos : POSITION; float3 normal : NORMAL; };
struct VSOut
{
    float4 clip     : SV_Position;
    float3 wNormal  : NORMAL;
    float3 wPos     : TEXCOORD0;
};

static const float FFT_HEIGHT_SCALE = 1.0 / 1000.0;

// ワールドXZ座標で高さマップをサンプリングし、ワールド空間の変位を返す
float SampleH(float2 xz)
{
    // 海洋シェーダーと同じUV計算：UV = xz / tileSize (WRAPサンプラー)
    float2 uv = xz / gridWorldSize;
    return -g_heightMap.SampleLevel(g_sampler, uv, 0).x * FFT_HEIGHT_SCALE;
}

VSOut FloatObjVS(VSIn v)
{
    float2 objXZ = worldPos.xz;
    float  step  = gridWorldSize / 128.0; // 有限差分のステップ幅

    // 3点から波面法線を推定する
    float h0 = SampleH(objXZ);
    float hR = SampleH(objXZ + float2(step, 0));
    float hF = SampleH(objXZ + float2(0, step));

    // 上向き法線（有限差分）
    float3 N = normalize(float3(h0 - hR, step, h0 - hF));

    // 表面法線に整合するTBN基底を構築する
    float3 ref   = (abs(N.x) < 0.9) ? float3(1, 0, 0) : float3(0, 0, 1);
    float3 right = normalize(cross(ref, N));
    float3 fwd   = normalize(cross(right, N));

    // ローカル頂点をスケールして表面に整列させた後、ワールド座標に変換する
    float3 scaled = v.pos * objectScale;
    float3 wp = float3(objXZ.x, h0 + dropOffset, objXZ.y)
              + scaled.x * right
              + scaled.y * N
              + scaled.z * fwd;

    // 法線も同様に変換する
    float3 wn = normalize(v.normal.x * right + v.normal.y * N + v.normal.z * fwd);

    VSOut o;
    o.clip    = mul(float4(wp, 1.0), viewProj);
    o.wNormal = wn;
    o.wPos    = wp;
    return o;
}

float4 FloatObjPS(VSOut i) : SV_Target
{
    float3 N = normalize(i.wNormal);
    float3 L = normalize(-sunDir);
    float3 V = normalize(cameraPos - i.wPos);
    float3 H = normalize(L + V);

    // 面の向きで木材のトーンを変化させる
    float upness = saturate(dot(N, float3(0, 1, 0)));
    float3 topColor  = float3(0.70, 0.57, 0.36); // 板の上面（明るい木）
    float3 sideColor = float3(0.52, 0.38, 0.22); // 板の側面（暗い木）
    float3 base = lerp(sideColor, topColor, upness * upness);

    float diff    = saturate(dot(N, L));
    float spec    = pow(saturate(dot(N, H)), 28.0) * 0.12;
    float ambient = 0.28;

    float3 color = base * (ambient + diff * sunIntensity) * sunColor + spec * sunColor;
    return float4(color, 1.0);
}
