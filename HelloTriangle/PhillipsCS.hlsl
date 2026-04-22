// Phillips spectrum initialization
// Runs once at startup, outputs h0(k) for all frequency components
RWTexture2D<float4> g_h0 : register(u0); // R=Re(h0(k)), G=Im(h0(k)), B=Re(h0(-k)), A=Im(h0(-k))

cbuffer PhillipsCB : register(b0)
{
    uint N; // texture size (256)
    float A; // global amplitude scale
    float windSpeed; // wind speed (m/s)
    float windDirX; // normalized wind direction X
    float windDirY; // normalized wind direction Z
    float pad0, pad1, pad2;
};

static const float PI = 3.14159265f;
static const float g = 9.81f;

// Hash function for pseudo-random number generation
// Wang hash，比简单取模分布更均匀
uint wangHash(uint seed)
{
    seed = (seed ^ 61u) ^ (seed >> 16u);
    seed *= 9u;
    seed = seed ^ (seed >> 4u);
    seed *= 0x27d4eb2du;
    seed = seed ^ (seed >> 15u);
    return seed;
}

// Converts uniform random to Gaussian using Box-Muller transform
// 两个均匀分布随机数→两个独立高斯随机数
float2 gaussianRandom(uint2 id)
{
    uint seed0 = wangHash(id.x + id.y * N);
    uint seed1 = wangHash(seed0 + 1u);

    // 映射到(0,1)的均匀分布
    float u0 = max(1e-6f, float(seed0) / 4294967295.0f);
    float u1 = max(1e-6f, float(seed1) / 4294967295.0f);

    // Box-Muller变换
    float mag = sqrt(-2.0f * log(u0));
    float angle = 2.0f * PI * u1;

    return float2(mag * cos(angle), mag * sin(angle));
}

// Phillips spectrum value for wave vector k
float phillips(float2 k)
{
    float kLen = length(k);
    if (kLen < 1e-6f)
        return 0.0f; // 避免除零

    float kLen2 = kLen * kLen;
    float kLen4 = kLen2 * kLen2;

    // L = V²/g，风速相关的特征波长
    float L = windSpeed * windSpeed / g;
    float L2 = L * L;

    // Phillips谱主体
    float ph = A * exp(-1.0f / (kLen2 * L2)) / kLen4;

    // 风向项：只保留顺风方向的波
    float2 kDir = k / kLen;
    float2 windDir = float2(windDirX, windDirY);
    float kdotw = dot(kDir, windDir);
    float kdotw2 = kdotw * kdotw;
    float kdotwAbs = abs(kdotw);
    ph *= kdotwAbs * kdotwAbs /** (0.7f + 0.3f * kdotw2)*/;
    // 消除极短波（数值稳定性）
    float l = L * 0.001f;
    ph *= exp(-kLen2 * l * l);

    return ph;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    // 把像素坐标映射到频率空间
    // 中心化：让(N/2, N/2)对应k=(0,0)
    float2 k;
    static const float L = 400.0f; // 和GRID_WORLD_SIZE保持一致
    k.x = (float(id.x) - float(N) / 2.0f) * (2.0f * PI / L);
    k.y = (float(id.y) - float(N) / 2.0f) * (2.0f * PI / L);

    // 生成高斯随机数
    float2 xi = gaussianRandom(id.xy);

    // h0(k)
    float ph0 = sqrt(phillips(k) * 0.5f);
    float2 h0 = xi * ph0;

    // h0(-k)，共轭对称性
    float2 xi2 = gaussianRandom(uint2((N - id.x) % N, (N - id.y) % N));
    float ph1 = sqrt(phillips(-k) * 0.5f);
    float2 h0c = xi2 * ph1;
    float sign = ((id.x + id.y) % 2 == 0) ? 1.0f : -1.0f;
    g_h0[id.xy] = float4(h0.x * sign, h0.y * sign, h0c.x * sign, h0c.y * sign);
    //g_h0[id.xy] = float4(h0.x, h0.y, h0c.x, h0c.y);
}