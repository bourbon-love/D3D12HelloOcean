# Ship PBR Rendering Design

## Overview

Upgrade the ship model from Lambert diffuse to physically-based rendering (PBR) using the Cook-Torrance GGX BRDF. All three texture sets (hull / rigging / sails) already ship with diffuse, normal (`_nor_gl_`), and ARM maps. No new assets are needed.

## Scope

- Shader: GGX Cook-Torrance BRDF, cotangent-frame TBN, sky-colored ambient
- C++ data: 2 extra textures per MeshGroup, SRV heap 2 → 4 slots
- Root signature: descriptor table 2 → 4 SRVs
- No vertex format change (tangents derived in PS via ddx/ddy)
- No CB changes (cameraPos / sunDir / sunColor / sunIntensity already present)
- Shadow pass unchanged

## Texture Convention

Poly Haven asset: `dutch_ship_large_02_{hull|rigging|sails}_{diff|nor_gl|arm}_1k.jpg`

| Map | Channel layout | Notes |
|---|---|---|
| `_diff_` | RGB = albedo | existing |
| `_nor_gl_` | RGB = tangent-space normal (OpenGL convention) | G channel must be flipped in shader for DirectX |
| `_arm_` | R=AO, G=roughness, B=metallic | standard Poly Haven convention |

## Data Structure Changes

### `MeshGroup` (ShipModel.h)

```cpp
struct MeshGroup {
    ComPtr<ID3D12Resource> vb, vbUpload, ib, ibUpload;
    D3D12_VERTEX_BUFFER_VIEW vbView{};
    D3D12_INDEX_BUFFER_VIEW  ibView{};
    UINT indexCount = 0;

    ComPtr<ID3D12Resource> diffuseTex,  diffuseUpload;
    ComPtr<ID3D12Resource> normalTex,   normalUpload;   // NEW
    ComPtr<ID3D12Resource> armTex,      armUpload;      // NEW

    ComPtr<ID3D12DescriptorHeap> srvHeap; // 4 slots: diff/norm/arm/heightmap
};
```

### SRV Heap Layout (per group)

| Slot | Register | Resource | Format |
|---|---|---|---|
| 0 | t0 | diffuse | R8G8B8A8_UNORM |
| 1 | t1 | normal map | R8G8B8A8_UNORM |
| 2 | t2 | ARM | R8G8B8A8_UNORM |
| 3 | t3 | heightMap | R32G32B32A32_FLOAT |

Root signature descriptor table: `SRV, 4, t0`.  
`g_heightMap` register moves from `t1` → `t3`.

## Texture Path Inference

In `LoadGLTF`, diffuse path is already stored in `m_texPaths[g]`.  
Normal and ARM paths are derived by string substitution:

```cpp
// diffuse: "...dutch_ship_large_02_hull_diff_1k.jpg"
// replace "_diff_" with "_nor_gl_" / "_arm_"
m_normalPaths[g] = replace(m_texPaths[g], "_diff_", "_nor_gl_");
m_armPaths[g]    = replace(m_texPaths[g], "_diff_", "_arm_");
```

Add `std::string m_normalPaths[3]` and `m_armPaths[3]` to ShipModel private members.

## Shader Design (`shipShader.hlsl`)

### Registers

```hlsl
Texture2D    g_diffuse   : register(t0);
Texture2D    g_normalMap : register(t1);
Texture2D    g_arm       : register(t2);
Texture2D    g_heightMap : register(t3);
SamplerState g_sampler   : register(s0);
```

### Cotangent TBN (Mikkelsen, no vertex tangent needed)

```hlsl
float3x3 CotangentFrame(float3 N, float3 p, float2 uv)
{
    float3 dp1  = ddx(p),  dp2  = ddy(p);
    float2 duv1 = ddx(uv), duv2 = ddy(uv);
    float3 dp2perp = cross(dp2, N);
    float3 dp1perp = cross(N,   dp1);
    float3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    float3 B = dp2perp * duv1.y + dp1perp * duv2.y;
    float  invmax = rsqrt(max(dot(T,T), dot(B,B)));
    return float3x3(T * invmax, B * invmax, N);
}
```

### GGX BRDF helpers

```hlsl
float  DistGGX(float NdotH, float a)   // D term
float3 FresnelSchlick(float cosT, float3 F0)  // F term
float  GeomSmith(float NdotV, float NdotL, float a) // G term (Smith-GGX)
```

### Pixel Shader main

```hlsl
float4 ShipPS(VSOut i) : SV_Target
{
    float3 albedo    = g_diffuse.Sample(g_sampler, i.uv).rgb;
    float3 normSamp  = g_normalMap.Sample(g_sampler, i.uv).rgb * 2.0 - 1.0;
    normSamp.g       = -normSamp.g;  // OpenGL -> DX normal Y flip
    float3 arm       = g_arm.Sample(g_sampler, i.uv).rgb;
    float  ao        = arm.r;
    float  roughness = max(arm.g, 0.04);
    float  metallic  = arm.b;

    float3x3 TBN = CotangentFrame(normalize(i.wNorm), i.wPos, i.uv);
    float3 N = normalize(mul(normSamp, TBN));
    float3 V = normalize(cameraPos - i.wPos);
    float3 L = normalize(-sunDir);
    float3 H = normalize(V + L);

    float NdotL = saturate(dot(N, L));
    float NdotV = saturate(dot(N, V));
    float NdotH = saturate(dot(N, H));
    float VdotH = saturate(dot(V, H));

    float3 F0   = lerp(float3(0.04,0.04,0.04), albedo, metallic);
    float  D    = DistGGX(NdotH, roughness * roughness);
    float3 F    = FresnelSchlick(VdotH, F0);
    float  G    = GeomSmith(NdotV, NdotL, roughness * roughness);

    float3 spec   = (D * F * G) / max(4.0 * NdotV * NdotL, 0.001);
    float3 kd     = (1.0 - F) * (1.0 - metallic);
    float3 direct = (kd * albedo / 3.14159 + spec) * sunColor * sunIntensity * NdotL;

    // Sky-colored ambient: interpolate cool sky blue -> sun color by sun height
    float3 skyAmb  = lerp(float3(0.10, 0.15, 0.25), sunColor, saturate(sunDir.y));
    float3 ambient = albedo * (1.0 - metallic) * ao * skyAmb * 0.30;

    return float4(direct + ambient, 1.0);
}
```

## Files Changed

| File | Change |
|---|---|
| `HelloTriangle/source/ShipModel.h` | Add `normalTex/normalUpload/armTex/armUpload` to MeshGroup; add `m_normalPaths[3]`, `m_armPaths[3]` private members |
| `HelloTriangle/source/ShipModel.cpp` | `LoadGLTF`: derive normal/arm paths; `InitBuffers`: load 2 new textures, SRV heap 2→4 slots, heightmap moves to slot 3; root sig table 2→4 |
| `HelloTriangle/shaders/shipShader.hlsl` | Full rewrite of PS; heightmap register t1→t3; add PBR helpers |
| `HelloTriangle/D3D12HelloTriangle.vcxproj` | No change |

## Out of Scope

- IBL / environment cube map
- Pre-filtered specular / irradiance map
- Emissive map
- Clearcoat or subsurface scattering
- Mip generation for textures
