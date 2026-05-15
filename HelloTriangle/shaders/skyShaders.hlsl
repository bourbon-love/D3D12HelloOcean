// ============================================================
// skyShaders.hlsl
// Sky sphere vertex and pixel shaders. Includes procedural sky gradient,
// sun/moon disc, starfield, and lightning effects.
// Clouds are rendered separately by VolumetricClouds (clouds.hlsl).
// ============================================================
cbuffer SkyCB : register(b0)
{
    float4x4 viewProj;
    float4 topColor;
    float4 middleColor;
    float4 bottomColor;
    float3 sunPosition;
    float time;
    float cloudDensity;
    float cloudScale;
    float cloudSharpness;
    float weatherIntensity;
    float3 sunColor;
    float padSunColor;
    float3 moonPosition;
    float padMoon;
    float3 moonCrescentDir; // Independent crescent direction: slowly rotated by the CPU
    float padCrescent;
    float moonBodyPow;
    float moonOccludePow;
    float crescentOffsetAmt;
    float padMoonParams;
    float lightningIntensity;
    float cloudDriftX;   // wind X * speed — applied as per-frame cloud position offset
    float cloudDriftY;   // wind Z * speed
    float padLightning;
    float cameraY;       // world Y of camera (negative = underwater)
    float padCam1;
    float padCam2;
    float padCam3;
};

struct VSInput
{
    float3 position : POSITION;
};
struct VSOutput
{
    float4 posH : SV_POSITION;
    float3 worldPos : TEXCOORD0;
};

VSOutput skyVS(VSInput vin)
{
    VSOutput vout;
    vout.posH = mul(float4(vin.position, 1.0f), viewProj);
    // Place sky sphere vertices at the far plane.
    // Setting z=w ensures the depth value after perspective division is 1.0 (far plane),
    // so the sky is always rendered behind all other geometry.
    vout.posH.z = vout.posH.w;
    vout.worldPos = vin.position;
    return vout;
}

// Hash function
float hash(float3 p)
{
    p = frac(p * 0.3183099 + 0.1);

    p *= 17.0;

    return frac(p.x * p.y * p.z * (p.x + p.y + p.z));
}


float3 renderStars(float3 dir, float time, float nightFactor)
{
    // Fade out near and below the horizon
    float horizonMask = smoothstep(0.0, 0.2, dir.y);

    // Divide the sky into small cells; each cell may contain one star
    float3 p = dir * 150.0;
    float3 cellId = floor(p);
    float3 cellFrac = frac(p);

    float h = hash(cellId);
    if (h < 0.94) return float3(0, 0, 0); // ~6% of cells have a star

    // Star position within the cell
    float3 starOffset = float3(
        hash(cellId + float3(1.3, 7.7, 2.5)),
        hash(cellId + float3(9.1, 4.3, 6.7)),
        hash(cellId + float3(3.7, 11.1, 8.3))
    );

    float dist = length(cellFrac - starOffset);
    float core = 1.0 - smoothstep(0.0, 0.12, dist);

    // Magnitude: cells with h closer to 1 produce brighter stars
    float mag = (h - 0.94) / 0.06;

    // Twinkle: each star has a unique phase and frequency
    float twinkle = 0.7 + 0.3 * sin(time * (1.5 + h * 6.0) + h * 57.3);

    // Color: hot stars are blue-white, cool stars are warm white
    float colorVar = hash(cellId + float3(5.5, 3.3, 9.9));
    float3 starColor = lerp(float3(0.7, 0.85, 1.0), float3(1.0, 0.95, 0.8), colorVar);

    return starColor * core * mag * twinkle * nightFactor * horizonMask * 5.0;
}

// Pixel shader
float4 skyPS(VSOutput input) : SV_Target
{
    float3 normalizedPos = normalize(input.worldPos);

    float horizonAdjust = -0.15;
    float adjustedY = normalizedPos.y - horizonAdjust;
    float h = adjustedY * 0.5f + 0.5f;

    // 1. Sky gradient color
    float4 skyColor;
    if (h > 0.55f)
    {
        float t = smoothstep(0.55f, 1.0f, h);
        skyColor = lerp(middleColor, topColor, t);
    }
    else
    {
        float t = smoothstep(0.0f, 0.55f, h);
        skyColor = lerp(bottomColor, middleColor, t);
    }

    // 2. Darken the nighttime sky (process before sun/moon so they are unaffected)
    float nightSky = saturate(-sunPosition.y * 3.0f);
    skyColor.rgb = lerp(skyColor.rgb, float3(0.02f, 0.02f, 0.08f), nightSky * 0.8f);

    // 2b. Stars (gradually appear at night; clouds will naturally cover them in subsequent steps)
    skyColor.rgb += renderStars(normalizedPos, time, nightSky);

    // 2c. Lightning: sky flashes blue-white with high-frequency flicker
    float flicker = lightningIntensity * (0.8 + 0.2 * sin(time * 50.0));
    skyColor.rgb += float3(0.85, 0.92, 1.0) * flicker * 3.0;

    // Fully suppress sun/moon below the horizon (invisible when y<0, regardless of camera position)
    // Use smoothstep for natural transition near the horizon
    float sunVis  = smoothstep(-0.04, 0.06, sunPosition.y);
    float moonVis = smoothstep(-0.04, 0.06, moonPosition.y);

    // Additional suppression when camera is underwater
    float uwCamSky = saturate(-cameraY / 2.0);

    // 3. Sun radiance (composited after nighttime darkening)
    float3 sunDir = sunPosition.xyz;
    float sunDot = max(0.0f, dot(normalizedPos, sunDir));
    // Sun disc core (HDR high intensity; compressed by ACES)
    float sunDisk = pow(sunDot, 2048.0f) * 20.0f;
    skyColor.rgb += float3(1.0f, 0.95f, 0.8f) * sunDisk * sunVis;

    // Sun halo
    float sunHalo = pow(sunDot, 256.0f) * 5.0f;
    skyColor.rgb += sunColor * sunHalo * sunVis;

    // Atmospheric scattering: only afterglow below the horizon (attenuated by sunVis, preserves sunset glow)
    float sunNearHorizon = saturate(1.0f - abs(sunPosition.y) * 5.0f);
    float atmScatter = pow(sunDot, 4.0f) * sunNearHorizon * 2.5f;
    skyColor.rgb += float3(1.1f, 0.42f, 0.04f) * atmScatter * sunVis;

    // 4. Moon crescent (unaffected by nighttime darkening)
    float3 moonDir = moonPosition.xyz;
    float moonDot = max(0.0f, dot(normalizedPos, moonDir));

    // Use the slowly rotating direction passed from the CPU. Keep it independent from
    // the real-time sun position to prevent abrupt crescent direction changes as the moon crosses the horizon.
    float3 crescentDir = normalize(moonDir - moonCrescentDir * crescentOffsetAmt);
    float crescentDot = max(0.0f, dot(normalizedPos, crescentDir));

    float moonBody    = pow(moonDot,    moonBodyPow);
    float moonOcclude = pow(crescentDot, moonOccludePow);
    float moonCrescent = saturate(moonBody - moonOcclude * 2.0f);
    skyColor.rgb += float3(1.0f, 1.0f, 0.95f) * moonCrescent * 18.0f * moonVis;

    // Halo
    float moonHalo = pow(moonDot, 100.0f) * 1.2f;
    skyColor.rgb += float3(0.3f, 0.4f, 0.8f) * moonHalo * moonVis;

    // 5. Underwater camera correction
    if (uwCamSky > 0.001)
    {
        // Replace the lower hemisphere (below horizontal) with dark deep ocean color
        float lookDown = saturate((-normalizedPos.y + 0.05) / 0.20);
        float3 deepColor = float3(0.01, 0.06, 0.14);
        skyColor.rgb = lerp(skyColor.rgb, deepColor, lookDown * uwCamSky);

        // Tint the upper hemisphere (toward water surface) blue-green (sky as seen through water)
        float lookUp = saturate(normalizedPos.y / 0.25);
        skyColor.rgb = lerp(skyColor.rgb,
            skyColor.rgb * float3(0.35, 0.70, 1.00),
            lookUp * uwCamSky * 0.55);

        // Add a bright band near the horizon from total internal reflection
        float horizon = exp(-abs(normalizedPos.y) * 10.0) * 0.4 * uwCamSky;
        skyColor.rgb += float3(0.04, 0.18, 0.38) * horizon;
    }

    return skyColor;
}
