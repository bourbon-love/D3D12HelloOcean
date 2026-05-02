// ============================================================
// ssao.hlsl
// Screen-Space Ambient Occlusion (half-resolution).
// Pass 0 SSAOPS  : 8-sample hemisphere occlusion from depth.
// Pass 1 SSAOBLURPS : 3x3 box blur to smooth the raw AO map.
// ============================================================

// ---- SSAO compute pass ----
Texture2D<float>  g_depth   : register(t0);
SamplerState      g_point   : register(s0);
SamplerState      g_linear  : register(s1);

cbuffer SSAOCB : register(b0)
{
    float4 kernel[8];   // hemisphere samples in tangent space (z >= 0)
    float  screenW;
    float  screenH;
    float  nearZ;
    float  farZ;
    float  radius;      // view-space sample radius
    float  bias;        // prevents self-intersection
    float  projX;       // proj[0][0] = cot(fov/2) / aspect
    float  projY;       // proj[1][1] = cot(fov/2)
    float  pad[24];     // padding to 256 bytes
};

struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD; };

VSOut SSAOVS(uint id : SV_VertexID)
{
    VSOut o;
    float2 uv = float2((id & 1) ? 2.0 : 0.0, (id & 2) ? 2.0 : 0.0);
    o.uv  = uv;
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

// NDC depth -> view-space Z (LH, Z increases away from camera)
float LinearZ(float d)
{
    return nearZ * farZ / max(farZ - d * (farZ - nearZ), 0.001);
}

// View-space position from UV + NDC depth
float3 ReconstructVS(float2 uv, float d)
{
    float z = LinearZ(d);
    return float3((uv.x * 2.0 - 1.0) * z / projX,
                  -(uv.y * 2.0 - 1.0) * z / projY,
                  z);
}

float hash21(float2 p) { return frac(sin(dot(p, float2(127.1, 311.7))) * 43758.5453); }

float4 SSAOPS(VSOut i) : SV_Target
{
    float2 uv     = i.uv;
    float2 texel  = float2(1.0 / screenW, 1.0 / screenH);

    float d = g_depth.SampleLevel(g_point, uv, 0);
    if (d >= 0.9999) return float4(1, 1, 1, 1); // sky pixel: no occlusion

    float3 vsPos = ReconstructVS(uv, d);

    // Reconstruct view-space normal from finite depth differences
    float dR = g_depth.SampleLevel(g_point, uv + float2(texel.x, 0), 0);
    float dD = g_depth.SampleLevel(g_point, uv + float2(0, texel.y), 0);
    float3 right = ReconstructVS(uv + float2(texel.x, 0), dR) - vsPos;
    float3 down  = ReconstructVS(uv + float2(0, texel.y), dD) - vsPos;
    float3 vsNormal = normalize(cross(right, down));
    if (vsNormal.z > 0) vsNormal = -vsNormal; // ensure it faces the camera

    // Random TBN frame using per-pixel hash
    float2 noiseUV = floor(uv * float2(screenW, screenH) * 0.5) * 0.25;
    float3 rndVec  = normalize(float3(hash21(noiseUV), hash21(noiseUV + 7.3), 0.5));
    float3 tangent   = normalize(rndVec - vsNormal * dot(rndVec, vsNormal));
    float3 bitangent = cross(vsNormal, tangent);
    float3x3 TBN = float3x3(tangent, bitangent, vsNormal);

    float occlusion = 0.0;
    [unroll] for (int k = 0; k < 8; k++)
    {
        // View-space sample position
        float3 sampleVS = mul(kernel[k].xyz, TBN) * radius + vsPos;
        if (sampleVS.z < nearZ) continue;

        // Project sample to screen UV
        float2 sampleNDC = float2(sampleVS.x * projX, sampleVS.y * projY) / sampleVS.z;
        float2 sampleUV  = saturate(sampleNDC * float2(0.5, -0.5) + 0.5);

        // Sample actual depth at that UV
        float  sampleD      = g_depth.SampleLevel(g_point, sampleUV, 0);
        float3 sampleActual = ReconstructVS(sampleUV, sampleD);

        // Occluded if actual geometry is closer (smaller Z) than our sample
        float rangeCheck = smoothstep(0.0, 1.0, radius / max(abs(vsPos.z - sampleActual.z), 0.001));
        occlusion += (sampleActual.z < sampleVS.z - bias) ? rangeCheck : 0.0;
    }

    float ao = 1.0 - occlusion / 8.0;
    return float4(ao, ao, ao, 1.0);
}

// ---- Blur pass ----
Texture2D g_ssaoRaw : register(t0);

cbuffer BlurCB : register(b0)
{
    float2 blurTexel; // 1/width, 1/height
    float2 blurPad;
};

float4 SSAOBLURPS(VSOut i) : SV_Target
{
    float2 uv  = i.uv;
    float  sum = 0.0;
    [unroll] for (int y = -1; y <= 1; y++)
    [unroll] for (int x = -1; x <= 1; x++)
        sum += g_ssaoRaw.SampleLevel(g_linear, uv + float2(x, y) * blurTexel, 0).r;
    return float4(sum / 9.0, 0, 0, 1);
}
