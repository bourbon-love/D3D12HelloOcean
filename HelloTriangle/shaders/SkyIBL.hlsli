// SkyIBL.hlsli
// Evaluates SH9 diffuse irradiance for a surface normal N.
// The 9 coefficients are pre-multiplied by cosine-lobe factors (A0=π, A1=2π/3, A2=π/4)
// during IrradianceConvolveCS, so this function is a pure polynomial evaluation —
// no branching, no extra multiply per pixel.
//
// Usage: #include "SkyIBL.hlsli" in each surface shader, then declare:
//   cbuffer SHCB : register(b1) { float4 shCoeffs[9]; float4 _pad[7]; }
// and call: float3 irr = EvalSH(N, shCoeffs);
//
// Coefficient order matches IrradianceConvolveCS.hlsl:
//   0=Y00, 1=Y1-1, 2=Y10, 3=Y11, 4=Y2-2, 5=Y2-1, 6=Y20, 7=Y21, 8=Y22

float3 EvalSH(float3 N, float4 c[9])
{
    float x = N.x, y = N.y, z = N.z;
    return c[0].xyz
         + c[1].xyz * y
         + c[2].xyz * z
         + c[3].xyz * x
         + c[4].xyz * (x * y)
         + c[5].xyz * (y * z)
         + c[6].xyz * (3.0 * z * z - 1.0)
         + c[7].xyz * (x * z)
         + c[8].xyz * (x * x - y * y);
}
