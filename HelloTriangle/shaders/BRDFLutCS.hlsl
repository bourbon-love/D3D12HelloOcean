// ============================================================
// BRDFLutCS.hlsl
// Generates the split-sum BRDF integration LUT (Karis/Epic "Real Shading in
// Unreal Engine 4"). Unlike SkyCaptureCS, this depends on nothing but
// (NdotV, roughness) — it has no relationship to the sky, the weather, or the
// time of day, so it is generated exactly ONCE at init and never touched again.
//
// Split-sum idea: the IBL specular integral factors into an "environment-only"
// half (this LUT — precomputable because it only depends on viewing geometry
// and roughness) and a "material-only" half (F0 * scale + bias, evaluated per
// pixel at runtime). See the tutorial's section 3 for the full derivation.
//
// Reference: Karis 2013 split-sum approximation / GGX importance sampling,
// canonical implementation: learnopengl.com's IBL specular chapter.
// ============================================================
RWTexture2D<float2> g_lut : register(u0);

static const float PI = 3.14159265f;
static const uint  SAMPLE_COUNT = 256u;

// Van der Corput radical inverse via bit reversal — produces a low-discrepancy
// 1D sequence that, paired with i/N, forms a Hammersley point set.
float RadicalInverseVdC(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10; // / 2^32
}

float2 Hammersley(uint i, uint n)
{
    return float2(float(i) / float(n), RadicalInverseVdC(i));
}

// Importance-sample a half-vector H around N from the GGX distribution —
// concentrates samples where the BRDF actually has energy instead of wasting
// them on directions that contribute almost nothing to the integral.
float3 ImportanceSampleGGX(float2 xi, float3 N, float roughness)
{
    float a = roughness * roughness;

    float phi      = 2.0f * PI * xi.x;
    float cosTheta = sqrt((1.0f - xi.y) / (1.0f + (a * a - 1.0f) * xi.y));
    float sinTheta = sqrt(1.0f - cosTheta * cosTheta);

    // Spherical -> tangent-space cartesian (halfway vector)
    float3 H = float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);

    // Tangent-space -> world-space (basis aligned to N)
    float3 up        = (abs(N.z) < 0.999f) ? float3(0.0f, 0.0f, 1.0f) : float3(1.0f, 0.0f, 0.0f);
    float3 tangent   = normalize(cross(up, N));
    float3 bitangent = cross(N, tangent);

    return normalize(tangent * H.x + bitangent * H.y + N * H.z);
}

// Schlick-GGX geometry term with the IBL remapping k = roughness^2 / 2
// (distinct from the direct-lighting remapping k = (roughness+1)^2 / 8).
float GeometrySchlickGGX_IBL(float NdotV, float roughness)
{
    float a = roughness;
    float k = (a * a) / 2.0f;
    return NdotV / (NdotV * (1.0f - k) + k);
}

float GeometrySmith_IBL(float3 N, float3 V, float3 L, float roughness)
{
    float NdotV = max(dot(N, V), 0.0f);
    float NdotL = max(dot(N, L), 0.0f);
    return GeometrySchlickGGX_IBL(NdotV, roughness) * GeometrySchlickGGX_IBL(NdotL, roughness);
}

// Monte-Carlo integrate the split-sum's environment-only half for a given
// (NdotV, roughness) pair, returning (scale, bias) such that, at runtime,
// specular_ambient ~= prefilteredColor * (F0 * scale + bias).
float2 IntegrateBRDF(float NdotV, float roughness)
{
    // Place V in the local frame so N = (0,0,1); only the angle to N matters.
    float3 V = float3(sqrt(1.0f - NdotV * NdotV), 0.0f, NdotV);
    float3 N = float3(0.0f, 0.0f, 1.0f);

    float A = 0.0f;
    float B = 0.0f;

    [loop]
    for (uint i = 0u; i < SAMPLE_COUNT; ++i)
    {
        float2 xi = Hammersley(i, SAMPLE_COUNT);
        float3 H  = ImportanceSampleGGX(xi, N, roughness);
        float3 L  = normalize(2.0f * dot(V, H) * H - V);

        float NdotL = max(L.z, 0.0f);
        float NdotH = max(H.z, 0.0f);
        float VdotH = max(dot(V, H), 0.0f);

        if (NdotL > 0.0f)
        {
            float G    = GeometrySmith_IBL(N, V, L, roughness);
            float Gvis = (G * VdotH) / (NdotH * NdotV);
            float Fc   = pow(1.0f - VdotH, 5.0f);

            A += (1.0f - Fc) * Gvis;
            B += Fc * Gvis;
        }
    }

    return float2(A, B) / float(SAMPLE_COUNT);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    uint w, h;
    g_lut.GetDimensions(w, h);
    if (dtid.x >= w || dtid.y >= h)
        return;

    // Texel (x,y) <-> (NdotV, roughness), texel-centered in [0,1]^2 — the same
    // domain the runtime sampler indexes with float2(NdotV, roughness).
    float NdotV     = (float(dtid.x) + 0.5f) / float(w);
    float roughness = (float(dtid.y) + 0.5f) / float(h);

    g_lut[dtid.xy] = IntegrateBRDF(NdotV, roughness);
}
