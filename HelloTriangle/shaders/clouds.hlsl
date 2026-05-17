// clouds.hlsl
// Volumetric cloud ray-marching. Renders as alpha-blended fullscreen pass
// on top of the sky dome, before scene geometry writes depth.

cbuffer CloudCB : register(b0)
{
    float4x4 invViewProj;
    float3   cameraPos;
    float    time;
    float3   sunDir;
    float    cloudCoverage;
    float3   sunColor;
    float    sunIntensity;
    float    cloudBase;
    float    cloudTop;
    float    windX;
    float    windZ;
    float    weatherIntensity;
    float    cloudScale;
    float    densityMult;
    float    nightFactor;
};

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VSOut CloudVS(uint id : SV_VertexID)
{
    VSOut o;
    float2 uv = float2((id & 1) ? 2.0 : 0.0, (id & 2) ? 2.0 : 0.0);
    o.uv  = uv;
    o.pos = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0, 1);
    return o;
}

// --- Gradient noise (Perlin-like) ---
// Stores a random gradient vector at each lattice point and computes dot(gradient, offset).
// Isosurfaces are organic/flowing curves — unlike value noise whose isosurfaces are
// grid-aligned bumpy spheres that produce fingerprint and grid-edge artifacts.

float3 gradHash(float3 p)
{
    // Cheap integer-based hash → pseudo-random gradient in [-1, 1]^3
    // Uses only frac/dot/multiply — no sin(), fast on GPU.
    p = frac(p * float3(0.1031, 0.1030, 0.0973));
    p += dot(p, p.yxz + 33.33);
    return -1.0 + 2.0 * frac((p.xxy + p.yxx) * p.zyx);
}

float gradNoise(float3 p)
{
    float3 i = floor(p), f = frac(p);
    float3 u = f * f * (3.0 - 2.0 * f);  // smoothstep blend weights

    float n = lerp(
        lerp(lerp(dot(gradHash(i),               f),
                  dot(gradHash(i+float3(1,0,0)), f-float3(1,0,0)), u.x),
             lerp(dot(gradHash(i+float3(0,1,0)), f-float3(0,1,0)),
                  dot(gradHash(i+float3(1,1,0)), f-float3(1,1,0)), u.x), u.y),
        lerp(lerp(dot(gradHash(i+float3(0,0,1)), f-float3(0,0,1)),
                  dot(gradHash(i+float3(1,0,1)), f-float3(1,0,1)), u.x),
             lerp(dot(gradHash(i+float3(0,1,1)), f-float3(0,1,1)),
                  dot(gradHash(i+float3(1,1,1)), f-float3(1,1,1)), u.x), u.y),
        u.z);
    // Remap from ~[-1.5, 1.5] to [0, 1]; saturate handles out-of-range corners
    return saturate(n * 0.45 + 0.5);
}

// 5-octave FBM built on gradient noise — no domain warp needed
float fbm5(float3 p)
{
    return gradNoise(p)          * 0.500
         + gradNoise(p * 2.031)  * 0.250
         + gradNoise(p * 4.073)  * 0.125
         + gradNoise(p * 8.157)  * 0.0625
         + gradNoise(p * 16.314) * 0.03125;
}

// --- Cloud density at world position ---

float SampleDensity(float3 pos)
{
    // Wind drift in world space
    float3 drift = float3(windX, 0.0, windZ) * time * 30.0;
    float3 q = (pos + drift) * cloudScale * 0.00030;

    float base = fbm5(q);

    float threshold = 0.56 - cloudCoverage * 0.44;
    float raw = base - threshold;
    // Wide smoothstep gives a gradual density ramp at cloud edges,
    // preventing hard binary on/off transitions visible as grid pixels at half-resolution
    float density = smoothstep(0.0, 0.55, raw) * densityMult;

    // Altitude gradient: wider base fade (0.15 vs 0.10) for softer cloud bottoms
    float h = saturate((pos.y - cloudBase) / (cloudTop - cloudBase));
    float altGrad = smoothstep(0.0, 0.15, h) * smoothstep(1.0, 0.50, h);

    return density * altGrad;
}

// Short ray march toward the sun for self-shadowing
float LightTransmittance(float3 pos)
{
    float3 step = sunDir * 220.0;
    float sigma = 0.0;
    [unroll]
    for (int i = 0; i < 5; i++)
    {
        pos += step;
        sigma += max(0.0, SampleDensity(pos));
    }
    return exp(-sigma * 0.50);
}

float4 CloudPS(VSOut input) : SV_Target
{
    // Reconstruct world-space ray direction from UV
    float2 ndc = input.uv * float2(2.0, -2.0) + float2(-1.0, 1.0);
    float4 farH = mul(float4(ndc, 1.0, 1.0), invViewProj);
    farH.xyz /= farH.w;
    float3 rd = normalize(farH.xyz - cameraPos);

    // Horizon fade: smooth fade to transparent instead of hard cutoff line
    float horizonFade = smoothstep(0.02, 0.10, rd.y);
    if (horizonFade < 0.001) return float4(0, 0, 0, 0);

    // Camera below cloud layer: ray-slab intersection
    if (cameraPos.y >= cloudTop) return float4(0, 0, 0, 0);

    float tStart, tEnd;
    if (cameraPos.y < cloudBase)
    {
        tStart = (cloudBase - cameraPos.y) / rd.y;
        tEnd   = (cloudTop  - cameraPos.y) / rd.y;
    }
    else
    {
        // Camera inside cloud layer
        tStart = 0.0;
        tEnd   = (cloudTop - cameraPos.y) / max(rd.y, 0.001);
    }

    if (tEnd <= tStart || tStart < 0.0) return float4(0, 0, 0, 0);

    const int   STEPS    = 32;
    float       stepSize = min((tEnd - tStart) / float(STEPS), 90.0);
    // Temporal dither: golden ratio per-frame offset lets TAA accumulate and wash out banding.
    // frac(time * 23.7) changes every frame; the golden ratio (0.618) keeps the sequence
    // quasi-random so no short-period repetition is visible.
    float dither = frac(dot(input.pos.xy, float2(0.75488, 0.56984))
                      + frac(time * 23.7) * 0.61803398875);
    float3      pos      = cameraPos + rd * (tStart + stepSize * dither);

    float3 cloudColor = float3(0, 0, 0);
    float  transmit   = 1.0;

    // Henyey-Greenstein phase function (forward scattering)
    float cosT  = dot(rd, normalize(sunDir));
    float g     = 0.28;
    float phase = (1.0 - g * g) / (4.0 * 3.14159265 * pow(abs(1.0 + g*g - 2.0*g*cosT), 1.5));

    for (int i = 0; i < STEPS; i++)
    {
        float d = SampleDensity(pos);
        if (d > 0.001)
        {
            // Skip the 5-step light march for very thin cloud regions — saves ~70% of
            // light-march cost since most visible cloud area is near the density threshold.
            float lightT = (d > 0.04) ? LightTransmittance(pos) : 0.85;

            // Albedo: near-white for calm cumulus, dark grey for storm clouds
            float3 albedo = lerp(float3(0.93, 0.94, 0.97), float3(0.22, 0.24, 0.30), weatherIntensity);

            // Direct sun contribution
            float3 directLight = sunColor * sunIntensity * lightT * phase * 3.0;

            // Ambient: top (sky-lit) to bottom (self-shadowed)
            // Day tops: sky-blue influence; day bottoms: neutral grey (not over-blue)
            // Night values simulate moonlight/starlight (raised from old 0.04-0.24 that caused black holes)
            float hFrac = saturate((pos.y - cloudBase) / (cloudTop - cloudBase));
            float3 ambUp = lerp(float3(0.52, 0.60, 0.80), float3(0.22, 0.28, 0.52), nightFactor);
            float3 ambDn = lerp(float3(0.28, 0.30, 0.38), float3(0.07, 0.10, 0.22), nightFactor);
            float  ambMult = lerp(0.32, 0.40, nightFactor);
            float3 ambient = lerp(ambDn, ambUp, hFrac) * ambMult;

            float3 sc = albedo * (directLight + ambient);

            // Beer-Lambert extinction
            float absorption = d * stepSize * 0.052;
            float dT = exp(-absorption);
            cloudColor += sc * (1.0 - dT) * transmit;
            transmit   *= dT;

            if (transmit < 0.005) break;
        }
        pos += rd * stepSize;
    }

    // Sunrise/sunset warm tint (only during day transitions, not at full night)
    float sunsetT = saturate(1.0 - abs(sunDir.y) * 5.0) * (1.0 - nightFactor);
    // Softer gold-orange (was 1.55/0.72/0.28 which was too saturated)
    cloudColor = lerp(cloudColor, cloudColor * float3(1.38, 0.82, 0.48), sunsetT * 0.60);
    // Add a faint pink-purple tint to shadowed (bottom) cloud parts at sunset
    float shadowFace = saturate(0.5 - dot(rd, normalize(sunDir)) * 0.5);
    cloudColor += float3(0.14, 0.05, 0.10) * shadowFace * sunsetT * saturate(1.0 - transmit) * 0.4;

    float alpha = saturate(1.0 - transmit) * horizonFade;
    return float4(cloudColor, alpha);
}

// ---------- Composite pass: 4-tap tent filter upsample ----------
// A pure bilinear sample from half-res can leave visible step-pattern edges.
// Averaging 4 neighbours with a tent weighting softens the transition without
// blurring cloud interiors (which are already low-frequency).
Texture2D<float4> g_cloudTex : register(t0);
SamplerState      g_sampler  : register(s0);

float4 CompositePS(VSOut input) : SV_Target
{
    // Half-texel offset in UV space (texel size of the half-res RT)
    float2 texelSize = float2(ddx(input.uv.x), ddy(input.uv.y)) * 0.5;

    float4 c0 = g_cloudTex.Sample(g_sampler, input.uv + float2(-texelSize.x, -texelSize.y));
    float4 c1 = g_cloudTex.Sample(g_sampler, input.uv + float2( texelSize.x, -texelSize.y));
    float4 c2 = g_cloudTex.Sample(g_sampler, input.uv + float2(-texelSize.x,  texelSize.y));
    float4 c3 = g_cloudTex.Sample(g_sampler, input.uv + float2( texelSize.x,  texelSize.y));

    return (c0 + c1 + c2 + c3) * 0.25;
}
