// ============================================================
// shaders.hlsl
// Ocean surface vertex and pixel shaders.
// Handles FFT height map + Gerstner wave displacement, Phong+Fresnel lighting,
// Jacobian foam generation, and SSR.
// ============================================================
//shaders.hlsl
cbuffer SceneCB : register(b0)
{
    float4x4 view;
    float4x4 proj;
    float    time;
    float3 cameraPos;

    float3 sunDir; // sun direction (normalized)
    float sunIntensity; // sun intensity: decreases at sunrise/sunset
    float3 sunColor; // sun color: orange at sunrise, white at noon
    float padSun;
    float3 skyColor; // primary sky color: used for Fresnel reflection
    float padSky;
    float fogStart;
    float fogEnd;
    float foamIntensity;
    float ssrMix;
    // Parameters for 4 waves.
    // Each wave has direction, amplitude, wavelength, speed, steepness, and padding.
    float2 waveDir0;   float waveAmp0; float waveLen0;
    float waveSpd0;    float waveStp0; float2 wavePad0;

    float2 waveDir1;   float waveAmp1; float waveLen1;
    float waveSpd1;    float waveStp1; float2 wavePad1;

    float2 waveDir2;   float waveAmp2; float waveLen2;
    float waveSpd2;    float waveStp2; float2 wavePad2;

    float2 waveDir3;   float waveAmp3; float waveLen3;
    float waveSpd3;    float waveStp3; float2 wavePad3;

    float2 tileOffset;  // tile XZ offset (world space)
    float2 tilePad;

    // Cloud shadow
    float cloudCoverage;
    float cloudScale;
    float cloudBase;
    float cloudTop;
    float cloudWindX;
    float cloudWindZ;
    float cloudDensityMult;
    float cloudEnabled;
};

// ---- Cloud shadow: 6-step ray march toward sun through cloud slab ----
float cs_hash(float3 p)
{
    p = frac(p * float3(443.897, 441.423, 437.195));
    p += dot(p, p.yxz + 19.19);
    return frac((p.x + p.y) * p.z);
}
float cs_vnoise(float3 p)
{
    float3 i = floor(p), f = frac(p);
    f = f * f * (3.0 - 2.0 * f);
    return lerp(
        lerp(lerp(cs_hash(i),               cs_hash(i+float3(1,0,0)), f.x),
             lerp(cs_hash(i+float3(0,1,0)), cs_hash(i+float3(1,1,0)), f.x), f.y),
        lerp(lerp(cs_hash(i+float3(0,0,1)), cs_hash(i+float3(1,0,1)), f.x),
             lerp(cs_hash(i+float3(0,1,1)), cs_hash(i+float3(1,1,1)), f.x), f.y),
        f.z);
}
float cs_fbm3(float3 p)
{
    return cs_vnoise(p) * 0.500
         + cs_vnoise(p * 2.031) * 0.250
         + cs_vnoise(p * 4.073) * 0.125;
}
float CloudShadow(float3 worldPos)
{
    if (cloudEnabled < 0.5 || sunDir.y <= 0.02) return 1.0;
    float3 sd     = normalize(sunDir);
    float  tStart = (cloudBase - worldPos.y) / max(sd.y, 0.02);
    float  tEnd   = (cloudTop  - worldPos.y) / max(sd.y, 0.02);
    if (tEnd <= tStart) return 1.0;

    const int STEPS = 6;
    float stepSize = (tEnd - tStart) / float(STEPS);
    float sigma    = 0.0;
    float3 drift   = float3(cloudWindX, 0.0, cloudWindZ) * time * 30.0;

    [unroll]
    for (int i = 0; i < STEPS; i++)
    {
        float  t       = tStart + (i + 0.5) * stepSize;
        float3 p       = worldPos + sd * t;
        float3 q       = (p + drift) * cloudScale * 0.00030;
        float  base    = cs_fbm3(q);
        float  thresh  = 0.56 - cloudCoverage * 0.44;
        float  density = smoothstep(0.0, 0.35, base - thresh) * cloudDensityMult;
        sigma += density * stepSize * 0.052;
    }
    // Amplify slightly so thin cloud still casts a perceptible shadow
    return exp(-sigma * 2.0);
}

struct RippleData
{
    float2 position;
    float radius;
    float strength;
};

cbuffer RippleCB : register(b1)
{
    RippleData ripples[200];
    uint rippleCount;
    float3 ripplePad;
};


Texture2D<float4> g_heightMap   : register(t0);
Texture2D<float4> g_dztMap      : register(t1);
Texture2D<float4> g_skySnapshot : register(t2);
Texture2D<float>  g_shadowMap   : register(t3);
Texture2D<float4> g_refraction  : register(t4); // sky + underwater objects (for refraction)
SamplerState g_sampler : register(s0);

cbuffer ShadowCB : register(b2)
{
    float4x4 lightViewProj;
    float    shadowBias;
    float    shadowStrength;
    float    shadowEnabled;
    float    pad_shadow;
    float    screenW;
    float    screenH;
    float    waterBodyStr;   // water body color multiplier
    float    waterRefract;   // refraction UV distortion
    float    waterMinTrans;  // min transmission at grazing
};


static const float FFT_HEIGHT_SCALE = 1.0f / 1000.0f;
static const float FFT_CHOP_SCALE = 1.0f / 1000.0f;
static const float FFT_TILE_SIZE = 400.0f;

struct VSInput
{
    float3 position : POSITION;
    float2 uv       : TEXCOORD0;
};

struct VSOutput
{
    float4 posH   : SV_POSITION;
    float3 posW   : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 uv     : TEXCOORD2;

};

// Gerstner wave function: deforms vertex position and normal based on wave parameters.
// Input:  vertex XZ position, wave direction, amplitude, wavelength, speed, steepness
// Output: displacement and tangent vectors after deformation
void GerstnerWave(
    float2 xz, float2 dir, float amp, float wavelen,
    float spd, float steep,
    inout float3 disp,
    inout float3 tangentX,
    inout float3 tangentZ)
{
    float k = 2.0f * 3.14159265f / wavelen;
    float f = k * dot(dir, xz) - spd * time;

    // Q uses steep directly (controlled in the 0-1 range)

    float Q = steep;

    float sinF = sin(f);
    float cosF = cos(f);

    // XYZ displacement
    disp.x += Q * amp * dir.x * cosF;
    disp.y += amp * sinF;
    disp.z += Q * amp * dir.y * cosF;

    // Tangent X direction
    tangentX.x += 1.0f - Q * dir.x * dir.x * k * amp * sinF;
    tangentX.y += dir.x * k * amp * cosF;
    tangentX.z -= Q * dir.x * dir.y * k * amp * sinF;

    // Tangent Z direction
    tangentZ.x -= Q * dir.x * dir.y * k * amp * sinF;
    tangentZ.y += dir.y * k * amp * cosF;
    tangentZ.z += 1.0f - Q * dir.y * dir.y * k * amp * sinF;
}
VSOutput VSMain(VSInput vin)
{
    VSOutput vout;

    // World XZ including tile offset: keeps Gerstner waves phase-continuous with adjacent tiles
    float2 xz = vin.position.xz + tileOffset;
    float3 disp = float3(0.0f, 0.0f, 0.0f);
    float3 tangentX = float3(1.0f, 0.0f, 0.0f);
    float3 tangentZ = float3(0.0f, 0.0f, 1.0f);

    GerstnerWave(xz, waveDir0, waveAmp0, waveLen0, waveSpd0, waveStp0, disp, tangentX, tangentZ);
    GerstnerWave(xz, waveDir1, waveAmp1, waveLen1, waveSpd1, waveStp1, disp, tangentX, tangentZ);
    GerstnerWave(xz, waveDir2, waveAmp2, waveLen2, waveSpd2, waveStp2, disp, tangentX, tangentZ);
    GerstnerWave(xz, waveDir3, waveAmp3, waveLen3, waveSpd3, waveStp3, disp, tangentX, tangentZ);

    float3 worldPos = vin.position + disp;
    worldPos.xz += tileOffset;  // place the tile in world space

    // FFT UV stays in [0,1] within the tile: the FFT texture tiles periodically in WRAP mode
    float2 fftUV = vin.position.xz / FFT_TILE_SIZE;

    float4 fftSample = g_heightMap.SampleLevel(g_sampler, fftUV, 0);
    float fftHeight = fftSample.x * FFT_HEIGHT_SCALE;
    float fftDx = fftSample.z * FFT_CHOP_SCALE;
    float fftDz = g_dztMap.SampleLevel(g_sampler, fftUV, 0).x * FFT_CHOP_SCALE;

    worldPos.y += fftHeight * -1.0f;
    worldPos.x += fftDx;
    worldPos.z += fftDz;

    float3 normal = normalize(cross(tangentZ, tangentX));
    //float3 normal = normalize(cross(tangentX, tangentZ));

    float4 posV = mul(float4(worldPos, 1.0f), view);
    vout.posH = mul(posV, proj);
    vout.posW = worldPos;
    vout.normal = normal;
    vout.uv = fftUV;
    return vout;

}
// --- Procedural foam noise helpers ---
float hash21(float2 p)
{
    p = frac(p * float2(127.1, 311.7));
    p += dot(p, p + 45.32);
    return frac(p.x * p.y);
}

float valueNoise(float2 p)
{
    float2 i = floor(p);
    float2 f = frac(p);
    float2 u = f * f * (3.0 - 2.0 * f);
    float a = hash21(i);
    float b = hash21(i + float2(1, 0));
    float c = hash21(i + float2(0, 1));
    float d = hash21(i + float2(1, 1));
    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}

// Reconstruct sky color from reflection direction, matching the dynamic sky palette.
float3 SampleSkyReflection(float3 reflDir)
{
    // Elevation: 0=horizon, 1=zenith
    float h = saturate(reflDir.y);

    // Compute day/night factor from sun elevation
    float dayF  = saturate(sunDir.y * 4.0 + 0.4);
    // Sunset factor: peaks when sun is near the horizon
    float sunsetF = saturate(1.0 - abs(sunDir.y) * 5.0);
    sunsetF = sunsetF * sunsetF;

    // Zenith and horizon colors for night and day
    float3 zenithDay  = float3(0.08, 0.25, 0.72);
    float3 horizDay   = skyColor * 0.9;
    // Night sky: near-black dark neutral gray (suppress blue bias)
    float3 zenithNight = float3(0.004, 0.005, 0.010);
    float3 horizNight  = float3(0.007, 0.008, 0.014);

    float3 zenith = lerp(zenithNight, zenithDay,  dayF);
    float3 horiz  = lerp(horizNight,  horizDay,   dayF);
    float3 sky    = lerp(horiz, zenith, h);

    // Sunset overlay: add orange-gold near the horizon
    float3 sunsetHorizon = float3(1.6, 0.72, 0.12); // HDR gold
    float3 sunsetZenith  = float3(0.10, 0.16, 0.48); // blue-purple
    float3 sunsetCol = lerp(sunsetZenith, sunsetHorizon, saturate(1.2 - h * 3.0));
    sky = lerp(sky, sunsetCol, sunsetF * saturate(1.2 - h * 2.0));

    // Sun glow in reflection direction
    float sunDotR = max(0.0, dot(reflDir, sunDir));
    sky += sunColor * pow(sunDotR, 6.0) * 4.0;

    // Below horizon: fade to deep ocean color (darker at night)
    float3 deepWater = float3(0.01, 0.02, 0.04) * (0.3 + dayF * 0.7);
    return lerp(deepWater, sky, smoothstep(-0.05, 0.1, reflDir.y));
}

float4 PSMain(VSOutput pin) : SV_TARGET
{
    // Recompute normals from heightMap per pixel (using the same scale as VS)
    const float texelSize = 1.0f / 256.0f;
    const float worldPerTexel = FFT_TILE_SIZE / 256.0f;

    // Sample neighboring heights
    float hL = -g_heightMap.SampleLevel(g_sampler, pin.uv + float2(-texelSize, 0), 0).x * FFT_HEIGHT_SCALE;
    float hR = -g_heightMap.SampleLevel(g_sampler, pin.uv + float2(texelSize, 0), 0).x * FFT_HEIGHT_SCALE;
    float hD = -g_heightMap.SampleLevel(g_sampler, pin.uv + float2(0, -texelSize), 0).x * FFT_HEIGHT_SCALE;
    float hU = -g_heightMap.SampleLevel(g_sampler, pin.uv + float2(0, texelSize), 0).x * FFT_HEIGHT_SCALE;

    // Sample neighboring Dx
    float dxL = g_heightMap.SampleLevel(g_sampler, pin.uv + float2(-texelSize, 0), 0).z * FFT_CHOP_SCALE;
    float dxR = g_heightMap.SampleLevel(g_sampler, pin.uv + float2(texelSize, 0), 0).z * FFT_CHOP_SCALE;
    float dxD = g_heightMap.SampleLevel(g_sampler, pin.uv + float2(0, -texelSize), 0).z * FFT_CHOP_SCALE;
    float dxU = g_heightMap.SampleLevel(g_sampler, pin.uv + float2(0, texelSize), 0).z * FFT_CHOP_SCALE;

    // Sample neighboring Dz
    float dzL = g_dztMap.SampleLevel(g_sampler, pin.uv + float2(-texelSize, 0), 0).x * FFT_CHOP_SCALE;
    float dzR = g_dztMap.SampleLevel(g_sampler, pin.uv + float2(texelSize, 0), 0).x * FFT_CHOP_SCALE;
    float dzD = g_dztMap.SampleLevel(g_sampler, pin.uv + float2(0, -texelSize), 0).x * FFT_CHOP_SCALE;
    float dzU = g_dztMap.SampleLevel(g_sampler, pin.uv + float2(0, texelSize), 0).x * FFT_CHOP_SCALE;

    // Partial derivatives
    float dHdx = (hR - hL) / (2.0f * worldPerTexel);
    float dHdz = (hU - hD) / (2.0f * worldPerTexel);
    float dDxdx = (dxR - dxL) / (2.0f * worldPerTexel);
    float dDzdz = (dzU - dzD) / (2.0f * worldPerTexel);
    float dDxdz = (dxU - dxD) / (2.0f * worldPerTexel);
    float dDzdx = (dzR - dzL) / (2.0f * worldPerTexel);

    // Jacobian normals: tangentX = (1+dDxdx, dHdx, dDzdx), tangentZ = (dDxdz, dHdz, 1+dDzdz)
    float3 tangentX = float3(1.0f + dDxdx, dHdx, dDzdx);
    float3 tangentZ = float3(dDxdz, dHdz, 1.0f + dDzdz);
    float3 N = normalize(cross(tangentZ, tangentX));

    // Normal perturbation by ripples
    for (uint i = 0; i < rippleCount; ++i)
    {
        float2 toPixel = pin.posW.xz - ripples[i].position;
        float dist = length(toPixel);
        float r = ripples[i].radius;

        // Apply perturbation only near the ripple ring
        float ringWidth = 1.5f;
        float inRing = saturate(1.0f - abs(dist - r) / ringWidth);

        if (inRing > 0.0f)
        {
            float2 dir = dist > 0.001f ? toPixel / dist : float2(1.0f, 0.0f);
            float wave = sin((dist - r) * 3.14159f / ringWidth);
            float strength = inRing * ripples[i].strength * wave * 0.3f;

            N.x += dir.x * strength;
            N.z += dir.y * strength;
            N = normalize(N);
        }
    }
    // View and light directions
    float3 V = normalize(cameraPos - pin.posW);
    // Double-sided normal: when viewed from underwater, N and V point in opposite directions; flip to unify.
    if (dot(N, V) < 0.0f) N = -N;
    float3 L = sunDir;
    float3 H = normalize(V + L);

    // Intrinsic water body color: deep ocean is near-black.
    // The visible "blue" of water arises from Fresnel sky reflection, so
    // the water body itself should not carry a vivid blue.
    float3 deepColor    = float3(0.004, 0.014, 0.030);
    float3 shallowColor = float3(0.007, 0.022, 0.048);
    // Height-based interpolation is very subtle (to prevent banding)
    float heightFactor = saturate(pin.posW.y * 0.08f + 0.5f);
    float3 waterColor = lerp(deepColor, shallowColor, heightFactor);

    // Diffuse: neutral dark at night (removes blue bias from sunColor)
    float NdotL = saturate(dot(N, L));
    float nightT = saturate(1.0 - sunIntensity * 3.0);   // 0=day, 1=deep night
    float3 nightAmbient = float3(0.022, 0.024, 0.028);   // near-black with very slight blue tint
    float3 ambLight = lerp(sunColor, nightAmbient, nightT);
    // Cloud shadow: attenuates direct (NdotL) light; ambient (0.5f) stays unaffected
    float cloudShad = CloudShadow(pin.posW);
    float3 diffuse = waterColor * (NdotL * 0.5f * sunIntensity * cloudShad + 0.5f);
    diffuse *= ambLight;

    // Specular: two-layer structure — tight specular highlight + broad scatter lobe
    float NdotH = saturate(dot(N, H));
    float specTight = pow(NdotH, 128.0f) * 14.0f;
    // Disable the broad scatter lobe at night (moonlight): moon color {0.6,0.7,1.0} causes green cast
    float daySpec   = saturate((sunIntensity - 0.35) * 10.0);
    float specBroad = pow(NdotH,  18.0f) *  0.6f * daySpec;
    float3 specularColor = sunColor * (specTight + specBroad) * sunIntensity * cloudShad;

    // Fresnel
    float F0 = 0.02f;
    float NdotV = saturate(dot(N, V));  // always positive after flip

    // ====== Underwater camera: Snell's window + total internal reflection ======
    if (cameraPos.y < -0.3f)
    {
        // Critical angle arcsin(1/1.333) ≈ 48.6° → cos ≈ 0.664
        // NdotV > critCos → Snell's window (sky visible); below → total internal reflection
        float critCos    = 0.664f;
        float snellT     = smoothstep(critCos - 0.12f, critCos + 0.08f, NdotV);

        // Inside window: sample skySnapshot in refraction direction
        float3 refractDir = refract(-V, N, 1.0f / 1.333f);
        float4x4 vp_uw    = mul(view, proj);
        float4 rc         = mul(float4(pin.posW + refractDir * 200.0f, 1.0f), vp_uw);
        float2 snellUV    = saturate(rc.xy / rc.w * float2(0.5f, -0.5f) + 0.5f);
        snellUV          += N.xz * 0.025f;  // wave surface distortion
        float3 snellSky   = g_skySnapshot.SampleLevel(g_sampler, snellUV, 0).rgb;
        // Slightly brighter at the window edge (caustic effect)
        float rimBright   = 1.0f + smoothstep(critCos - 0.15f, critCos, NdotV) * 0.4f;
        snellSky         *= rimBright;

        // Total internal reflection side: dark underwater scatter color
        float3 tirColor   = float3(0.003f, 0.018f, 0.055f);

        float3 surfaceColor = lerp(tirColor, snellSky, snellT);

        // Light absorption by camera depth
        float camDepth    = max(0.1f, -cameraPos.y);
        float3 absorbUW   = float3(0.45f, 0.06f, 0.025f);
        float3 absorption = exp(-absorbUW * camDepth * 0.4f);
        surfaceColor     *= absorption;

        // Underwater specular (shallow areas only)
        float specUW = pow(saturate(dot(N, H)), 64.0f) * 0.2f * sunIntensity;
        surfaceColor += sunColor * specUW * saturate(1.0f - camDepth * 0.1f);

        // Distance fog (underwater visibility)
        float dist_uw = length(cameraPos - pin.posW);
        float uwFog   = saturate((dist_uw - 15.0f) / 60.0f);
        float3 deepFog = float3(0.004f, 0.018f, 0.05f) * absorption;
        surfaceColor   = lerp(surfaceColor, deepFog, uwFog);

        return float4(surfaceColor, 1.0f);
    }
    // ====== Below: normal (above-water) rendering ======

    float fresnel = min(F0 + (1.0f - F0) * pow(1.0f - NdotV, 5.0f), 0.65f);
    float3 reflectDir = reflect(-V, N);

    // ---- Reflection sample (Fresnel not yet applied; composited later) ----
    float3 reflectSample;
    {
        float4x4 vp = mul(view, proj);
        float4 reflClip = mul(float4(pin.posW + reflectDir * 300.0, 1.0), vp);
        float2 reflUV = reflClip.xy / reflClip.w * float2(0.5, -0.5) + 0.5;
        reflUV += N.xz * 0.018;
        reflUV = saturate(reflUV);

        float2 edgeFade = saturate(min(reflUV, 1.0 - reflUV) * 6.0);
        float fade = min(edgeFade.x, edgeFade.y);
        fade *= saturate(reflectDir.y * 4.0 + 0.3);
        fade *= ssrMix;

        float3 ssrSample  = g_skySnapshot.SampleLevel(g_sampler, reflUV, 0).rgb;
        float3 procSample = SampleSkyReflection(reflectDir);
        float  reflBright = lerp(0.08, 1.0, saturate(sunIntensity * 1.8));
        reflectSample = lerp(procSample, ssrSample, fade) * reflBright;
    }

    // ---- Transmitted color (Beer-Lambert water absorption + refraction shimmer) ----
    float3 transmitted;
    {
        // Open ocean: optical path length based on NdotV (longer at grazing angles), clamped to 12m
        float  waterDepth  = clamp(4.0 / max(NdotV, 0.15), 4.0, 12.0);
        // Open ocean blue-green: G/B ratio ≈ 0.47 (measured open-ocean value); lower G absorption to retain green tint
        float3 absorbCoeff = float3(0.45, 0.06, 0.025);
        float3 deepOcean   = float3(0.010, 0.15, 0.32) * waterBodyStr;
        float  sunLit      = sunIntensity * 0.55 + 0.12;
        float3 transBody   = deepOcean * exp(-absorbCoeff * waterDepth) * sunLit;

        // Refraction shimmer: slightly displace screen UV by the normal
        // Refraction UV: sample refractionRT (sky + underwater objects) disturbed by normal
        // Phase1: refractionRT contains only sky color, so not used here
        // Phase2 (underwater camera): will reuse as ScreenUV + normal displacement for Snell's window
        transmitted = transBody;
    }

    // ---- Fresnel composite: reflection + transmission ----
    float transWeight = waterMinTrans + (1.0 - waterMinTrans) * (1.0 - fresnel);
    float3 color = fresnel * reflectSample
                 + transWeight * transmitted
                 + specularColor
                 + diffuse * 0.30;

    // --- Sub-surface scattering (SSS): transmitted light at wave crests (glows blue-green against backlight) ---
    {
        float3 sssDir      = normalize(-sunDir + N * 0.5);
        float  sssView     = pow(saturate(dot(V, sssDir)), 4.0);
        float  sssCrest    = saturate(pin.posW.y * 0.18 + 0.2);
        float  sssDaylight = saturate((sunIntensity - 0.35) * 10.0);
        // Wave crest transmitted light: real waves are blue-green (keep green subtle)
        float3 sssColor    = float3(0.0, 0.38, 0.42) * sssView * sssCrest
                             * min(sunIntensity, 1.5) * 1.4 * sssDaylight * cloudShad;
        color += sssColor;
    }

    // Fog
    float dist = length(cameraPos - pin.posW);
    float fogFactor = saturate((dist - fogStart) / (fogEnd - fogStart));
    color = lerp(color, skyColor, fogFactor);


    // --- Wave crest foam ---
    float J = (1.0f + dDxdx) * (1.0f + dDzdz) - dDxdz * dDzdx;

    // The more the Jacobian falls below 1 (more wave folding), the stronger the foam
    float sharpness = lerp(5.0, 2.5, foamIntensity);
    float rawFoam   = pow(saturate(1.0 - J), sharpness);
    rawFoam *= lerp(0.15, 1.0, foamIntensity);

    // Restrict foam to upward-facing surfaces (prevents sticking to wave sides)
    float topFace = saturate((N.y - 0.45) / 0.55);

    // Foam streak UV along wind direction:
    // Stretch along wave direction (4:1 ratio), generate streaks perpendicular to it
    float2 waveDir2D = normalize(waveDir0);
    float2 perpDir2D = float2(-waveDir2D.y, waveDir2D.x);
    float2 worldXZ   = pin.uv * FFT_TILE_SIZE;
    float  alongAxis = dot(worldXZ, waveDir2D);
    float  perpAxis  = dot(worldXZ, perpDir2D);
    // Streak coordinate system stretched along wave direction (along-axis: 1/4 scale → long streaks)
    float2 foamUVBase = float2(perpAxis * 0.25f, alongAxis * 0.065f);
    float2 fuv  = foamUVBase + float2(time * 0.10f, time * 0.06f);
    float  fn1  = valueNoise(fuv);
    float  fn2  = valueNoise(fuv * 3.2 + float2(5.1, 1.9));
    float  fn3  = valueNoise(fuv * 7.5 + float2(2.3, 4.7));
    float  fbm  = fn1 * 0.50 + fn2 * 0.32 + fn3 * 0.18;
    float  foamNoise = smoothstep(0.30, 0.68, fbm);

    float foamMask = saturate(rawFoam * topFace * (0.15 + foamNoise * 1.6));

    // Fine spray layer during storms
    float spray = 0.0;
    [branch]
    if (foamIntensity > 0.35)
    {
        float2 suv = pin.uv * 120.0 + float2(time * 0.20, time * 0.14);
        float  sn  = valueNoise(suv);
        spray = smoothstep(0.55, 0.80, sn) * saturate((foamIntensity - 0.35) * 3.0);
        spray *= saturate(rawFoam * topFace * 5.0);
    }

    // HDR foam color (triggers bloom)
    // Real ocean foam is white: remove blue-green bias
    float3 foamWhite = float3(2.2, 2.2, 2.2);
    float3 foamEdge  = float3(1.9, 1.9, 1.95);
    float3 foamColor = lerp(foamEdge, foamWhite, foamNoise);
    color = lerp(color, foamColor, foamMask * 0.90);
    color = lerp(color, foamWhite * 0.75, spray * 0.45);

    // --- Shadow (from floating objects onto the ocean surface) ---
    [branch]
    if (shadowEnabled > 0.5 && shadowStrength > 0.0)
    {
        float4 posLS  = mul(float4(pin.posW, 1.0), lightViewProj);
        float2 suv    = posLS.xy / posLS.w * float2(0.5, -0.5) + 0.5;
        float  sdepth = posLS.z / posLS.w;

        [branch]
        if (suv.x > 0.01 && suv.x < 0.99 && suv.y > 0.01 && suv.y < 0.99 && sdepth < 1.0)
        {
            float  shadow = 0.0;
            float  dx     = 1.0 / 2048.0;
            int2   tc     = (int2)(suv * 2048.0);
            // 3×3 PCF (accurate depth comparison using integer-coordinate Load)
            [unroll] for (int sy = -1; sy <= 1; sy++)
            [unroll] for (int sx = -1; sx <= 1; sx++)
            {
                float sm = g_shadowMap.Load(int3(tc + int2(sx, sy), 0));
                shadow += (sm + shadowBias < sdepth) ? 1.0 : 0.0;
            }
            shadow /= 9.0;
            color.rgb *= (1.0 - shadow * shadowStrength);
        }
    }

    // ============================================================
    // Debug output: identify the cause of white-out
    // Change DEBUG_MODE and rebuild; if the white area turns red, that source is the cause.
    //   0 = normal rendering
    //   1 = Fresnel value (bright = high Fresnel = grazing angle)
    //   2 = reflection sample reflectSample
    //   3 = specular color specularColor
    //   4 = transmitted color
    //   5 = normal N (red=X, green=Y, blue=Z)
    //   6 = diffuse only
    #define DEBUG_MODE 0

    #if DEBUG_MODE == 1
        return float4(fresnel, fresnel, fresnel, 1.0f);
    #elif DEBUG_MODE == 2
        return float4(saturate(reflectSample), 1.0f);
    #elif DEBUG_MODE == 3
        return float4(saturate(specularColor), 1.0f);
    #elif DEBUG_MODE == 4
        return float4(saturate(transmitted), 1.0f);
    #elif DEBUG_MODE == 5
        return float4(N * 0.5f + 0.5f, 1.0f);
    #elif DEBUG_MODE == 6
        return float4(saturate(diffuse), 1.0f);
    #else
        return float4(color, 1.0f);
    #endif
    // ============================================================
}
