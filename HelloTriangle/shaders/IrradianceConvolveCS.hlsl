// ============================================================
// IrradianceConvolveCS.hlsl
// Convolves the captured sky cubemap into 9 SH9 RGB coefficients (L0+L1+L2).
// The cosine-lobe factor (A0=π, A1=2π/3, A2=π/4) is pre-multiplied here so
// that EvalSH in the pixel shader reduces to a plain dot-product polynomial of N,
// with no per-pixel branching or extra multiply.
//
// A single thread iterates all texels of the 64×64×6 cubemap serially.
// At GPU speeds a 24576-texel cubemap takes microseconds; no reduction pass needed.
// This dispatch fires every ~30 frames (after at least one full 6-face rolling cycle).
//
// Reference: Ramamoorthi & Hanrahan 2001, "An Efficient Representation for
// Irradiance Environment Maps." Section 4 (SH projection + cosine lobe factors).
// ============================================================

// Captured sky cubemap (RGBA16F, 6-layer Texture2DArray — cubemaps have no native
// SRV in DX12 compute; use TEXTURE2DARRAY with all 6 slices).
Texture2DArray<float4> g_cubemap : register(t0);

// Output: 9 RGBA16F slots (xyz = RGB SH coefficient, w unused).
RWBuffer<float4> g_shOut : register(u0);

static const float PI = 3.14159265f;

// Direction from face + UV — identical to SkyCaptureCS.hlsl's FaceUVToDirection
// so the two passes always agree on axis/sign conventions.
float3 FaceUVToDir(int face, float2 uv)
{
    switch (face)
    {
        case 0: return normalize(float3( 1.0,  -uv.y, -uv.x));
        case 1: return normalize(float3(-1.0,  -uv.y,  uv.x));
        case 2: return normalize(float3( uv.x,  1.0,   uv.y));
        case 3: return normalize(float3( uv.x, -1.0,  -uv.y));
        case 4: return normalize(float3( uv.x, -uv.y,  1.0 ));
        default:return normalize(float3(-uv.x, -uv.y, -1.0 ));
    }
}

[numthreads(1, 1, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    uint faceSize, dummy, numFaces;
    g_cubemap.GetDimensions(0, faceSize, faceSize, dummy, numFaces);
    float fs = float(faceSize);

    // Cosine-lobe factors for bands l=0,1,2 (Ramamoorthi & Hanrahan, eq. 13).
    // Pre-multiplying here means EvalSH just evaluates the SH basis polynomial.
    static const float A0 = PI;             // l=0
    static const float A1 = 2.0 * PI / 3;  // l=1
    static const float A2 = PI / 4;        // l=2

    float3 sh[9];
    [unroll] for (int k = 0; k < 9; k++) sh[k] = 0;

    for (uint face = 0; face < 6; face++)
    {
        for (uint py = 0; py < faceSize; py++)
        {
            for (uint px = 0; px < faceSize; px++)
            {
                // Texel-centred UV in [-1,1]
                float2 uv = (float2(px, py) + 0.5) / fs * 2.0 - 1.0;

                // Differential solid angle for a cubemap texel (Jaspe & Schuler 2012)
                float denom  = 1.0 + uv.x*uv.x + uv.y*uv.y;
                float dOmega = 4.0 / (denom * sqrt(denom) * fs * fs);

                float3 d   = FaceUVToDir(face, uv);
                float3 rad = g_cubemap[uint3(px, py, face)].rgb;
                float  x = d.x, y = d.y, z = d.z;

                // Real SH basis Y_l^m evaluated at direction d.
                // Order: Y00, Y1-1, Y10, Y11, Y2-2, Y2-1, Y20, Y21, Y22
                float basis[9] = {
                    0.282095f,                         // l=0, A0
                    0.488603f * y,                     // l=1, A1
                    0.488603f * z,
                    0.488603f * x,
                    1.092548f * x * y,                 // l=2, A2
                    1.092548f * y * z,
                    0.315392f * (3.0*z*z - 1.0),
                    1.092548f * x * z,
                    0.546274f * (x*x - y*y)
                };

                // Cosine-lobe factor per band
                float Al[9] = { A0, A1,A1,A1, A2,A2,A2,A2,A2 };

                [unroll]
                for (int i = 0; i < 9; i++)
                    sh[i] += rad * (basis[i] * Al[i] * dOmega);
            }
        }
    }

    // Write 9 coefficients; w channel unused (left 0 for padding).
    [unroll]
    for (int i = 0; i < 9; i++)
        g_shOut[i] = float4(sh[i], 0.0);
}
