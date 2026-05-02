// ============================================================
// dof.hlsl
// 被写界深度（DOF）シェーダー。
// Vogelディスクサンプリング（20点）によるボケ効果を実現する。
// ============================================================
Texture2D<float4> g_scene : register(t0); // HDRシーン色
Texture2D<float>  g_depth : register(t1); // 線形NDC深度 [0,1]
SamplerState g_sampler : register(s0);

cbuffer DOFCB : register(b0)
{
    float focusDepth;   // フォーカス平面のNDC深度（0=近、1=遠）
    float focusRange;   // ピントが合う領域のNDC単位での半幅
    float maxRadius;    // UV空間での最大CoC半径
    float aspectRatio;  // 幅 / 高さ
};

struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD; };

VSOut DOFVS(uint id : SV_VertexID)
{
    VSOut o;
    float2 uv = float2((id & 1) ? 2.0 : 0.0, (id & 2) ? 2.0 : 0.0);
    o.uv  = uv;
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

float4 DOFPS(VSOut i) : SV_Target
{
    float2 uv = i.uv;

    float depth = g_depth.SampleLevel(g_sampler, uv, 0).r;
    float coc   = saturate(abs(depth - focusDepth) / focusRange) * maxRadius;

    // 完全にピントが合っているピクセルを早期リターン
    if (coc < 0.0005)
        return g_scene.SampleLevel(g_sampler, uv, 0);

    // Vogelディスクサンプリング（黄金角スパイラル）— 20サンプル
    static const int N = 20;
    float3 color = 0;

    [unroll]
    for (int k = 0; k < N; k++)
    {
        float angle = k * 2.3999632; // 黄金角 ≈ 2.4 rad
        float r     = sqrt((k + 0.5) / N);
        float2 off  = float2(cos(angle), sin(angle)) * (r * coc);
        off.x      /= aspectRatio;           // 非正方形スクリーンの補正

        color += g_scene.SampleLevel(g_sampler, saturate(uv + off), 0).rgb;
    }
    color /= N;

    // CoC量に基づいてセンターサンプルとブレンド（シャープな中心を保持）
    float3 center = g_scene.SampleLevel(g_sampler, uv, 0).rgb;
    float  blend  = saturate(coc / (maxRadius * 0.5));
    return float4(lerp(center, color, blend), 1.0);
}
