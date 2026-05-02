// ============================================================
// PhillipsCS.hlsl
// Phillipsパワースペクトル初期化コンピュートシェーダー。
// 起動時に一度だけ実行され、全周波数成分のh0(k)を出力する。
// ============================================================
// Phillipsスペクトル初期化
// 起動時に一度実行し、全周波数成分のh0(k)を出力する
RWTexture2D<float4> g_h0 : register(u0); // R=Re(h0(k)), G=Im(h0(k)), B=Re(h0(-k)), A=Im(h0(-k))

cbuffer PhillipsCB : register(b0)
{
    uint N; // テクスチャサイズ（256）
    float A; // グローバル振幅スケール
    float windSpeed; // 風速（m/s）
    float windDirX; // 正規化風向 X
    float windDirY; // 正規化風向 Z
    float pad0, pad1, pad2;
};

static const float PI = 3.14159265f;
static const float g = 9.81f;

// 疑似乱数生成のためのハッシュ関数
// Wangハッシュ：単純な剰余より均一な分布を持つ
uint wangHash(uint seed)
{
    seed = (seed ^ 61u) ^ (seed >> 16u);
    seed *= 9u;
    seed = seed ^ (seed >> 4u);
    seed *= 0x27d4eb2du;
    seed = seed ^ (seed >> 15u);
    return seed;
}

// Box-Muller変換で一様乱数をガウス分布に変換する
// 2つの一様乱数 → 2つの独立したガウス乱数
float2 gaussianRandom(uint2 id)
{
    uint seed0 = wangHash(id.x + id.y * N);
    uint seed1 = wangHash(seed0 + 1u);

    // (0,1)の一様分布にマッピング
    float u0 = max(1e-6f, float(seed0) / 4294967295.0f);
    float u1 = max(1e-6f, float(seed1) / 4294967295.0f);

    // Box-Muller変換
    float mag = sqrt(-2.0f * log(u0));
    float angle = 2.0f * PI * u1;

    return float2(mag * cos(angle), mag * sin(angle));
}

// 波ベクトルkに対するPhillipsスペクトル値
float phillips(float2 k)
{
    float kLen = length(k);
    if (kLen < 1e-6f)
        return 0.0f; // ゼロ除算を回避

    float kLen2 = kLen * kLen;
    float kLen4 = kLen2 * kLen2;

    // L = V²/g：風速に依存する特性波長
    float L = windSpeed * windSpeed / g;
    float L2 = L * L;

    // Phillipsスペクトルの主要項
    float ph = A * exp(-1.0f / (kLen2 * L2)) / kLen4;

    // 風向項：追い風方向の波のみを保持
    float2 kDir = k / kLen;
    float2 windDir = float2(windDirX, windDirY);
    float kdotw = dot(kDir, windDir);
    float kdotw2 = kdotw * kdotw;
    float kdotwAbs = abs(kdotw);
    ph *= kdotwAbs * kdotwAbs /** (0.7f + 0.3f * kdotw2)*/;
    // 極短波を除去（数値安定性のため）
    float l = L * 0.001f;
    ph *= exp(-kLen2 * l * l);

    return ph;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    // ピクセル座標を周波数空間にマッピング
    // 中心化：(N/2, N/2)がk=(0,0)に対応するよう
    float2 k;
    static const float L = 400.0f; // GRID_WORLD_SIZEと一致させる
    k.x = (float(id.x) - float(N) / 2.0f) * (2.0f * PI / L);
    k.y = (float(id.y) - float(N) / 2.0f) * (2.0f * PI / L);

    // ガウス乱数を生成
    float2 xi = gaussianRandom(id.xy);

    // h0(k)
    float ph0 = sqrt(phillips(k) * 0.5f);
    float2 h0 = xi * ph0;

    // h0(-k)：共役対称性
    float2 xi2 = gaussianRandom(uint2((N - id.x) % N, (N - id.y) % N));
    float ph1 = sqrt(phillips(-k) * 0.5f);
    float2 h0c = xi2 * ph1;
    float sign = ((id.x + id.y) % 2 == 0) ? 1.0f : -1.0f;
    g_h0[id.xy] = float4(h0.x * sign, h0.y * sign, h0c.x * sign, h0c.y * sign);
    //g_h0[id.xy] = float4(h0.x, h0.y, h0c.x, h0c.y);
}
