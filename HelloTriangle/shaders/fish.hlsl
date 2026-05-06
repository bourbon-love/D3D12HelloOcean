// fish.hlsl - 水中魚群インスタンシングシェーダー（Boids CPU シミュレーション + GPU 一括描画）

struct FishInstance
{
    float3 pos;     float phase;   // 16
    float3 forward; float speed;   // 16
    float3 up;      float pad;     // 16
    float4 color;                  // 16
};

StructuredBuffer<FishInstance> g_instances : register(t0);

cbuffer SceneCB : register(b0)
{
    float4x4 viewProj;
    float3   sunDir;      float sunIntensity;
    float3   sunColor;    float time;
    float3   cameraPos;   float pad_cb;
};

struct VSIn
{
    float3 pos    : POSITION;
    float3 normal : NORMAL;
    uint   instID : SV_InstanceID;
};

struct VSOut
{
    float4 svpos    : SV_POSITION;
    float3 worldPos : TEXCOORD0;
    float3 normal   : TEXCOORD1;
    float4 color    : TEXCOORD2;
};

VSOut FishVS(VSIn v)
{
    FishInstance inst = g_instances[v.instID];

    float3 fwd   = normalize(inst.forward);
    float3 up    = normalize(inst.up);
    float3 right = normalize(cross(up, fwd));

    // 尾びれの揺れ（後方頂点のみ適用、t^2 ウェイトで振幅増大）
    float3 lp = v.pos;
    if (lp.x < 0.0)
    {
        float t    = saturate(-lp.x * 2.0);
        float bend = sin(time * 6.5 + inst.phase) * 0.28 * t * t;
        lp.z += bend;
    }

    float3 wp = inst.pos
              + lp.x * fwd
              + lp.y * up
              + lp.z * right;

    float3 wn = normalize(v.normal.x * fwd + v.normal.y * up + v.normal.z * right);

    VSOut o;
    o.svpos    = mul(float4(wp, 1.0), viewProj);
    o.worldPos = wp;
    o.normal   = wn;
    o.color    = inst.color;
    return o;
}

float4 FishPS(VSOut p) : SV_TARGET
{
    float3 N = normalize(p.normal);
    float3 L = normalize(sunDir);

    float  diffuse = max(0.0, dot(N, L)) * sunIntensity;
    float  ambient = 0.35;
    float  rim     = max(0.0, dot(-N, L)) * sunIntensity * 0.20;

    // Beer-Lambert water absorption (gentle, fish visible at -3 to -10m)
    float  depth  = max(0.0, -p.worldPos.y);
    float3 absorb = float3(
        exp(-0.18 * depth),
        exp(-0.05 * depth),
        exp(-0.02 * depth));

    float3 col = p.color.rgb * (ambient + diffuse + rim) * sunColor * absorb;
    return float4(col, 1.0);
}
