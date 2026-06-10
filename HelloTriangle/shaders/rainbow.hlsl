// rainbow.hlsl
// Physically-based rainbow: primary bow at ~42°, secondary at ~51°.
// Additive-blended fullscreen pass rendered after volumetric clouds.

cbuffer RainbowCB : register(b0)
{
    float4x4 invViewProj;
    float3   cameraPos;   float pad0;
    float3   sunDir;      float intensity;
    float    rainFactor;
    float    sunIntensity;
    float    secondaryBow; // 0=off, 1=on
    float    bandWidth;    // colour-band width multiplier (default 2.0)
};

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VSOut RainbowVS(uint id : SV_VertexID)
{
    VSOut o;
    float2 uv = float2((id & 1) ? 2.0 : 0.0, (id & 2) ? 2.0 : 0.0);
    o.uv  = uv;
    o.pos = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0, 1);
    return o;
}

// Standard HSV -> RGB (hue in [0,1])
float3 hsv2rgb(float h, float s, float v)
{
    float3 rgb = clamp(abs(fmod(h * 6.0 + float3(0, 4, 2), 6.0) - 3.0) - 1.0, 0.0, 1.0);
    return v * lerp(float3(1, 1, 1), rgb, s);
}

// Map angle within rainbow band to spectral hue.
// t=0 → violet (hue 0.75), t=1 → red (hue 0).
// Traversing 0.75→0 in HSV passes through blue, cyan, green, yellow, orange, red — correct spectrum.
float3 SpectrumColor(float t)
{
    float hue = lerp(0.75, 0.0, saturate(t));
    return hsv2rgb(hue, 0.95, 1.0);
}

float4 RainbowPS(VSOut input) : SV_Target
{
    // Reconstruct world-space ray direction
    float2 ndc  = input.uv * float2(2.0, -2.0) + float2(-1.0, 1.0);
    float4 farH = mul(float4(ndc, 1.0, 1.0), invViewProj);
    farH.xyz /= farH.w;
    float3 rd = normalize(farH.xyz - cameraPos);

    // Only render when clearly looking into the sky.
    // Raising the threshold prevents the rainbow from painting onto the ocean surface,
    // which would make it appear incorrectly close to the camera.
    float horizonFade = smoothstep(0.04, 0.18, rd.y);
    if (horizonFade < 0.001) return float4(0, 0, 0, 0);

    // Fade out when looking nearly straight up — few droplets overhead
    float skyFade = smoothstep(0.82, 0.45, rd.y);

    // Rainbow window: sun above horizon (0°) up to ~46° elevation.
    // Lower end at 0.0 ensures rainbow vanishes exactly at sunset, preventing
    // overlap with the moon which appears at the anti-solar point after dark.
    // Upper end: peak at ~10° (golden hour), fully faded by ~46°.
    float sunFactor = smoothstep(0.0, 0.12, sunDir.y) * smoothstep(0.72, 0.18, sunDir.y);

    float totalIntensity = intensity * rainFactor * sunFactor * horizonFade * skyFade;
    if (totalIntensity < 0.001) return float4(0, 0, 0, 0);

    // Angle from anti-solar point (-sunDir)
    float3 antiSolar = -normalize(sunDir);
    float cosA = clamp(dot(rd, antiSolar), -1.0, 1.0);
    float angleDeg = acos(cosA) * 57.29577951; // rad → deg

    float3 color = float3(0, 0, 0);

    // Primary bow: asymmetric Gaussian centred at 41.55°.
    // bandWidth scales both the colour mapping range and the sigma values,
    // letting the user widen/narrow the visible colour bands in real time.
    {
        float bw       = max(bandWidth, 0.1);
        float halfBand = 0.95 * bw;                              // half colour-range in degrees
        float center   = 41.55;
        float t        = saturate((angleDeg - (center - halfBand)) / (2.0 * halfBand));
        float d        = angleDeg - center;
        float sigOuter = 2.5 * bw;                              // outer edge scales with width
        float sigInner = 10.0 + sigOuter;                       // inner fan stays wide
        float sigma    = (d < 0.0) ? sigInner : sigOuter;
        float env      = exp(-pow(d / sigma, 2.0));
        env           *= smoothstep(18.0, 28.0, angleDeg);
        color += SpectrumColor(t) * env * 0.42;
    }

    // Secondary bow: 51.0° (red/inner) … 53.5° (violet/outer), reversed, ~43% brightness
    if (secondaryBow > 0.5)
    {
        float t   = (angleDeg - 51.0) / 2.5;
        float env = exp(-pow((angleDeg - 52.25) / 1.40, 2.0));
        if (env > 0.001)
            color += SpectrumColor(1.0 - t) * env * 0.10;
    }

    // Additive output — alpha unused in ONE/ONE blend but set for clarity
    return float4(color * totalIntensity, 0.0);
}
