// ============================================================
// lensflare.hlsl
// レンズフレアシェーダー。
// ゴーストフレア7本、アナモルフィックストリーク、太陽ハローを手続き的に生成する。
// ============================================================
cbuffer LensFlareCB : register(b0)
{
    float2 sunScreenPos;
    float  sunVisibility;
    float  strength;
    float  aspectRatio;
    float  time;
};

struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD; };

VSOut LensFlareVS(uint id : SV_VertexID)
{
    VSOut o;
    float2 uv = float2((id & 1) ? 2.0 : 0.0, (id & 2) ? 2.0 : 0.0);
    o.uv  = uv;
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

float4 LensFlarePS(VSOut i) : SV_Target
{
    if (sunVisibility <= 0.001) return float4(0, 0, 0, 0);

    float2 uv     = i.uv;
    float2 sunPos = sunScreenPos;
    float2 axis   = float2(0.5, 0.5) - sunPos; // 太陽→画面中心

    float3 color = 0;

    // フレア軸に沿ったゴーストフレア
    static const int NUM_GHOSTS = 7;
    static const float gScale[NUM_GHOSTS]  = { 0.40, 0.62, 0.88, 1.12, 1.45, 1.85, 2.45 };
    static const float gRadius[NUM_GHOSTS] = { 0.040, 0.090, 0.025, 0.060, 0.035, 0.080, 0.050 };

    [unroll]
    for (int g = 0; g < NUM_GHOSTS; g++)
    {
        float2 ghostPos = sunPos + axis * gScale[g];
        float2 d = (uv - ghostPos) * float2(aspectRatio, 1.0);
        float  dist = length(d) / gRadius[g];
        float  glow = saturate(1.0 - dist);
        glow = glow * glow * glow;

        // 各ゴーストに固有の色相を割り当て
        float hue = (float)g / (float)NUM_GHOSTS;
        float3 gc;
        if      (g == 0) gc = float3(1.0, 0.55, 0.10); // オレンジ
        else if (g == 1) gc = float3(0.20, 0.50, 1.00); // ブルー
        else if (g == 2) gc = float3(0.90, 0.90, 0.20); // イエロー
        else if (g == 3) gc = float3(0.30, 0.90, 0.30); // グリーン
        else if (g == 4) gc = float3(0.90, 0.20, 0.80); // ピンク
        else if (g == 5) gc = float3(0.30, 0.80, 1.00); // シアン
        else             gc = float3(1.00, 0.45, 0.10); // オレンジ

        color += gc * glow * 0.35;
    }

    // 太陽を通る水平アナモルフィックストリーク
    {
        float dy      = uv.y - sunPos.y;
        float streak  = exp(-dy * dy * 7000.0) * 0.28;
        float shimmer = 0.8 + 0.2 * sin(uv.x * 50.0 + time * 2.5);
        color += float3(0.40, 0.65, 1.00) * streak * shimmer;
    }

    // 太陽周辺の柔らかいハロー
    {
        float2 d    = (uv - sunPos) * float2(aspectRatio, 1.0);
        float  dist = length(d);
        float  halo = saturate(1.0 - dist / 0.10);
        halo = halo * halo;
        color += float3(1.00, 0.95, 0.80) * halo * 0.22;
    }

    return float4(color * sunVisibility * strength, 1.0);
}
