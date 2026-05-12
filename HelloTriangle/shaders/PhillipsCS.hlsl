// ============================================================
// PhillipsCS.hlsl  (JONSWAPスペクトル)
// JONSWAP海洋パワースペクトル初期化コンピュートシェーダー。
// Phillipsスペクトルを置き換え、支配的な周期と方向性の高い
// 波浪場を生成する。毎フレーム実行（風速変化に追従）。
// ============================================================
RWTexture2D<float4> g_h0 : register(u0);

cbuffer PhillipsCB : register(b0)
{
    uint  N;          // テクスチャサイズ（256）
    float A;          // 振幅スケール（天気システムから渡される）
    float windSpeed;  // 風速 U10 (m/s)
    float windDirX;   // 正規化風向 X
    float windDirY;   // 正規化風向 Z
    float pad0, pad1, pad2;
};

static const float PI  = 3.14159265f;
static const float g   = 9.81f;

// Wangハッシュ：疑似乱数シード
uint wangHash(uint seed)
{
    seed = (seed ^ 61u) ^ (seed >> 16u);
    seed *= 9u;
    seed = seed ^ (seed >> 4u);
    seed *= 0x27d4eb2du;
    seed = seed ^ (seed >> 15u);
    return seed;
}

// Box-Muller変換：一様乱数 → ガウス乱数
float2 gaussianRandom(uint2 id)
{
    uint seed0 = wangHash(id.x + id.y * N);
    uint seed1 = wangHash(seed0 + 1u);
    float u0   = max(1e-6f, float(seed0) / 4294967295.0f);
    float u1   = max(1e-6f, float(seed1) / 4294967295.0f);
    float mag  = sqrt(-2.0f * log(u0));
    float ang  = 2.0f * PI * u1;
    return float2(mag * cos(ang), mag * sin(ang));
}

// ============================================================
// JONSWAPスペクトル（波数域、2D）
//
// 導出：S(ω) = α g²/ω⁵ · exp(-5/4·(ωp/ω)⁴) · γ^r  を
// 深水分散 ω=√(gk) で波数域に変換すると：
//   Ψ(k) = S(ω) · dω/dk · 1/k = (α/2) · k⁻⁴ · PM · ENH · D(θ)
// Phillipsと同じ k⁻⁴ 骨格を維持しつつ、支配周波数で鋭いピークを形成。
// ============================================================
float jonswap(float2 k)
{
    float kLen = length(k);
    if (kLen < 1e-6f) return 0.0f;

    // 深水角周波数
    float omega = sqrt(g * kLen);

    // フェッチ（吹送距離）：風速から比例推定
    // 穏やか U=10 → F=100km、嵐 U=75 → F=750km
    float fetch  = max(windSpeed * 10000.0f, 1000.0f);

    // JONSWAPピーク角周波数：ωp = 22·(g²/(U·F))^(1/3)
    float omegaP = 22.0f * pow(g * g / max(windSpeed * fetch, 1e-6f), 1.0f / 3.0f);
    float kp     = omegaP * omegaP / g;  // ピーク波数

    // Pierson-Moskowitz包絡：exp(-5/4·(kp/k)²)
    // Phillipsの exp(-1/(kL)²) を置き換え；支配波長より長い成分を強く抑制
    float pm = exp(-1.25f * (kp / kLen) * (kp / kLen));

    // JONSWAPピーク増強：γ^r（γ=3.3 標準値）
    // 支配周波数付近の波高を最大3.3倍増強し、規則的な波列を形成
    float sigma = (omega <= omegaP) ? 0.07f : 0.09f;
    float r     = exp(-(omega - omegaP) * (omega - omegaP)
                      / (2.0f * sigma * sigma * omegaP * omegaP));
    float enh   = pow(3.3f, r);

    // 方向拡散：追い風方向の波のみ（逆行波をゼロに）
    float2 kDir  = k / kLen;
    float2 wDir  = normalize(float2(windDirX, windDirY));
    float  kdotw = dot(kDir, wDir);
    float  spread = max(0.0f, kdotw) * max(0.0f, kdotw);

    // 極短波カット（数値安定性）
    float Lw = windSpeed * windSpeed / g;
    float lc = Lw * 0.001f;
    float cut = exp(-kLen * kLen * lc * lc);

    float kLen4 = kLen * kLen * kLen * kLen;
    return A * pm * enh * spread * cut / kLen4;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    // ピクセル座標 → 波数空間（中心: k=0）
    float2 k;
    static const float L = 400.0f;
    k.x = (float(id.x) - float(N) * 0.5f) * (2.0f * PI / L);
    k.y = (float(id.y) - float(N) * 0.5f) * (2.0f * PI / L);

    float2 xi  = gaussianRandom(id.xy);
    float  ph0 = sqrt(jonswap(k) * 0.5f);
    float2 h0  = xi * ph0;

    float2 xi2 = gaussianRandom(uint2((N - id.x) % N, (N - id.y) % N));
    float  ph1 = sqrt(jonswap(-k) * 0.5f);
    float2 h0c = xi2 * ph1;

    float sign = ((id.x + id.y) % 2 == 0) ? 1.0f : -1.0f;
    g_h0[id.xy] = float4(h0.x * sign, h0.y * sign, h0c.x * sign, h0c.y * sign);
}
