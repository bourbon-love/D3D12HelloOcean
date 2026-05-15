// ============================================================
// PhillipsCS.hlsl  (JONSWAP spectrum)
// JONSWAP ocean power spectrum initialization compute shader.
// Replaces the Phillips spectrum to generate a dominant-period,
// highly directional wave field. Executed every frame (tracks wind speed changes).
// ============================================================
RWTexture2D<float4> g_h0 : register(u0);

cbuffer PhillipsCB : register(b0)
{
    uint  N;          // texture size (256)
    float A;          // amplitude scale (passed from the weather system)
    float windSpeed;  // wind speed U10 (m/s)
    float windDirX;   // normalized wind direction X
    float windDirY;   // normalized wind direction Z
    float pad0, pad1, pad2;
};

static const float PI  = 3.14159265f;
static const float g   = 9.81f;

// Wang hash: pseudo-random seed
uint wangHash(uint seed)
{
    seed = (seed ^ 61u) ^ (seed >> 16u);
    seed *= 9u;
    seed = seed ^ (seed >> 4u);
    seed *= 0x27d4eb2du;
    seed = seed ^ (seed >> 15u);
    return seed;
}

// Box-Muller transform: uniform random → Gaussian random
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
// JONSWAP spectrum (wavenumber domain, 2D)
//
// Derivation: convert S(ω) = α g²/ω⁵ · exp(-5/4·(ωp/ω)⁴) · γ^r
// to the wavenumber domain using the deep-water dispersion relation ω=√(gk):
//   Ψ(k) = S(ω) · dω/dk · 1/k = (α/2) · k⁻⁴ · PM · ENH · D(θ)
// Retains the same k⁻⁴ skeleton as Phillips while forming a sharp peak
// at the dominant frequency.
// ============================================================
float jonswap(float2 k)
{
    float kLen = length(k);
    if (kLen < 1e-6f) return 0.0f;

    // Deep-water angular frequency
    float omega = sqrt(g * kLen);

    // Fetch (wind fetch distance): estimated proportionally from wind speed
    // Calm U=10 → F=100km, storm U=75 → F=750km
    float fetch  = max(windSpeed * 10000.0f, 1000.0f);

    // JONSWAP peak angular frequency: ωp = 22·(g²/(U·F))^(1/3)
    float omegaP = 22.0f * pow(g * g / max(windSpeed * fetch, 1e-6f), 1.0f / 3.0f);
    float kp     = omegaP * omegaP / g;  // peak wavenumber

    // Pierson-Moskowitz envelope: exp(-5/4·(kp/k)²)
    // Replaces Phillips' exp(-1/(kL)²); strongly suppresses components longer than the dominant wavelength
    float pm = exp(-1.25f * (kp / kLen) * (kp / kLen));

    // JONSWAP peak enhancement: γ^r (γ=3.3 standard value)
    // Amplifies wave height near the dominant frequency by up to 3.3x, forming a regular wave train
    float sigma = (omega <= omegaP) ? 0.07f : 0.09f;
    float r     = exp(-(omega - omegaP) * (omega - omegaP)
                      / (2.0f * sigma * sigma * omegaP * omegaP));
    float enh   = pow(3.3f, r);

    // Directional spreading: only downwind waves (zero out counter-propagating waves)
    float2 kDir  = k / kLen;
    float2 wDir  = normalize(float2(windDirX, windDirY));
    float  kdotw = dot(kDir, wDir);
    float  spread = max(0.0f, kdotw) * max(0.0f, kdotw);

    // Very short wave cut (numerical stability)
    float Lw = windSpeed * windSpeed / g;
    float lc = Lw * 0.001f;
    float cut = exp(-kLen * kLen * lc * lc);

    float kLen4 = kLen * kLen * kLen * kLen;
    return A * pm * enh * spread * cut / kLen4;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    // Pixel coordinates → wavenumber space (center: k=0)
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
