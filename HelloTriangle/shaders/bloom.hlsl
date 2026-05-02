// ============================================================
// bloom.hlsl
// ブルームポストプロセスシェーダー。
// 輝度抽出パスと分離ガウスブラーパス（H/V）を提供する。
// ============================================================
Texture2D    g_source  : register(t0);
SamplerState g_sampler : register(s0);

cbuffer BloomCB : register(b0)
{
    float threshold;  // 輝度抽出の閾値
    float strength;   // 合成時の乗算係数
    float blurDirX;   // 1=水平、0=垂直
    float blurDirY;   // 0=水平、1=垂直
};

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD;
};

// フルスクリーントライアングル（頂点バッファ不要）
VSOut BloomVS(uint id : SV_VertexID)
{
    VSOut o;
    float2 uv = float2((id & 1) ? 2.0 : 0.0, (id & 2) ? 2.0 : 0.0);
    o.uv  = uv;
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

// 閾値より明るいピクセルを抽出する
float4 BrightPassPS(VSOut i) : SV_Target
{
    float4 c   = g_source.Sample(g_sampler, i.uv);
    float  lum = dot(c.rgb, float3(0.2126, 0.7152, 0.0722));
    float  t   = max(1.0 - threshold, 0.001);
    float  ext = saturate((lum - threshold) / t);
    return float4(c.rgb * ext, 1.0);
}

// 分離9タップガウスブラー
float4 BlurPS(VSOut i) : SV_Target
{
    uint w, h;
    g_source.GetDimensions(w, h);
    float2 step = float2(blurDirX / (float)w, blurDirY / (float)h);

    // sigma ≈ 1.5、重みの合計を1に正規化
    static const float k[9] = {
        0.0076, 0.0361, 0.1096, 0.2134, 0.2666,
        0.2134, 0.1096, 0.0361, 0.0076
    };
    float4 result = 0;
    [unroll]
    for (int j = -4; j <= 4; j++)
        result += g_source.Sample(g_sampler, i.uv + step * j) * k[j + 4];
    return result;
}

// シーンへブルームを加算ブレンド（SrcBlend=ONE、DestBlend=ONE）
float4 CompositePS(VSOut i) : SV_Target
{
    return g_source.Sample(g_sampler, i.uv) * strength;
}
