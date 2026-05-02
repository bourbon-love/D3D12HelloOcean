// ============================================================
// spray.hlsl
// 飛沫パーティクルのビルボードシェーダー。
// ノイズで縁を崩した不規則な白色破片として描画する。
// ============================================================
cbuffer SprayCB : register(b0)
{
    float4x4 viewProj;
    float3   cameraRight;
    float    pad0;
    float3   cameraUp;
    float    pad1;
};

struct VSInput
{
    float3 center  : POSITION;
    float  size    : TEXCOORD0;
    float  alpha   : TEXCOORD1;
    float  cornerU : TEXCOORD2;
    float  cornerV : TEXCOORD3;
};

struct VSOutput
{
    float4 posH  : SV_POSITION;
    float2 uv    : TEXCOORD0;
    float  alpha : TEXCOORD1;
};

VSOutput SprayVS(VSInput vin)
{
    // 縦長の楕円ビルボード（水しぶきの縦方向の伸びを表現）
    float3 wp = vin.center
              + cameraRight * vin.cornerU * vin.size * 0.55
              + cameraUp    * vin.cornerV * vin.size;
    VSOutput o;
    o.posH  = mul(float4(wp, 1.0), viewProj);
    o.uv    = float2(vin.cornerU * 0.5 + 0.5, -vin.cornerV * 0.5 + 0.5);
    o.alpha = vin.alpha;
    return o;
}

float hash21(float2 p)
{
    p = frac(p * float2(127.1, 311.7));
    return frac(sin(dot(p, p + 45.32)) * 43758.5);
}

float4 SprayPS(VSOutput pin) : SV_TARGET
{
    float2 c = pin.uv * 2.0 - 1.0;

    // 縁をノイズで崩して不規則な飛沫の形に
    float2 noiseUV = floor(pin.uv * 7.0);
    float  edge    = 0.72 + hash21(noiseUV) * 0.22;
    float  d       = dot(c, c);
    if (d > edge * edge) discard;

    // 中心ほど不透明、縁ほど透明
    float soft = saturate(1.0 - sqrt(d) / edge);
    soft = soft * soft;   // 2乗で縁をシャープに

    // 白色（HDR乗算なし）、わずかに青みがかった水しぶき色
    float3 color = lerp(float3(0.82, 0.92, 1.0), float3(1.0, 1.0, 1.0), soft);
    return float4(color, pin.alpha * soft);
}
