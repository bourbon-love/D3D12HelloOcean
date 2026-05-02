// ============================================================
// lightning.hlsl
// Lightning bolt overlay rendered additively into the HDR buffer.
// Computes per-pixel distance to each bolt segment and applies
// a bright core + soft glow falloff. Bloom handles the halo.
// ============================================================

cbuffer LightningCB : register(b0)
{
    float4 pts[13];   // [0..numBolt-1]=main, [numBolt..numBolt+numBranch-1]=branch
    float  intensity; // 0..1, fades over ~0.2s
    float  aspect;    // width / height (for circular glow)
    int    numBolt;
    int    numBranch;
    float4 pad[2];    // padding to 256 bytes
};

struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD; };

VSOut LightningVS(uint id : SV_VertexID)
{
    VSOut o;
    float2 uv = float2((id & 1) ? 2.0 : 0.0, (id & 2) ? 2.0 : 0.0);
    o.uv  = uv;
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

// Squared distance from p to segment a-b (aspect-corrected so glow is circular)
float DistSqToSeg(float2 p, float2 a, float2 b)
{
    float2 ab = (b - a) * float2(aspect, 1.0);
    float2 ap = (p - a) * float2(aspect, 1.0);
    float  t  = saturate(dot(ap, ab) / max(dot(ab, ab), 1e-8));
    float2 d  = ap - t * ab;
    return dot(d, d);
}

float4 LightningPS(VSOut i) : SV_Target
{
    if (intensity <= 0.001) return float4(0, 0, 0, 0);

    float2 uv = i.uv;
    float  minDSq = 1e10;

    // Main bolt segments
    for (int k = 0; k < numBolt - 1; k++)
        minDSq = min(minDSq, DistSqToSeg(uv, pts[k].xy, pts[k + 1].xy));

    // Branch segments
    for (int k = numBolt; k < numBolt + numBranch - 1; k++)
        minDSq = min(minDSq, DistSqToSeg(uv, pts[k].xy, pts[k + 1].xy));

    // Bright white-blue core (triggers bloom strongly)
    float core = exp(-minDSq / 8e-7) * 30.0;
    // Soft glow
    float glow = exp(-minDSq / 3e-5) * 5.0;
    // Wide atmospheric haze
    float haze = exp(-minDSq / 1e-3) * 0.6;

    float3 boltColor = float3(0.75, 0.88, 1.0);
    float3 color = boltColor * (core + glow + haze) * intensity;
    return float4(color, 0.0);
}
