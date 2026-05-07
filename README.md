# D3D12 Hello Ocean

A real-time ocean renderer built from scratch with DirectX 12 and C++20. The ocean surface is simulated entirely on the GPU using Fast Fourier Transform (FFT) based on the Phillips spectrum, combined with Gerstner waves, a full post-processing pipeline, and dynamic weather.

> **Demo video:** <-- https://youtu.be/7AlJAtE4OF4 -->  
> **Build:** Visual Studio 2022 · DirectX 12 · C++20 · Windows 10/11

---

## Technical Highlights

| Feature | Detail |
|---|---|
| Ocean simulation | GPU FFT (Phillips spectrum + Cooley-Tukey Radix-2 IFFT) |
| Ocean mesh | 512 × 512 vertices, 400 × 400 world units |
| Wave model | FFT displacement map + 4 Gerstner waves |
| Sky | Procedural sky with sun/moon cycle, 3D Perlin noise clouds |
| Post-processing | TAA · Bloom · God Rays · DOF · SSAO · Tone mapping |
| Water shading | Beer-Lambert absorption · caustics · refraction · Fresnel |
| Render target | HDR (R16G16B16A16_FLOAT) → LDR (R8G8B8A8_UNORM) |
| Shadow map | 2048 × 2048 orthographic |

---

## Ocean Simulation Pipeline

The simulation runs as three consecutive compute shader passes each frame:

```
[Startup]
PhillipsCS.hlsl      — Generate initial spectrum H₀(k) from Phillips model

[Per Frame]
TimeEvolutionCS.hlsl — Evolve spectrum using dispersion relation ω(k) = √(g|k|)
IFFTCS.hlsl          — Ping-pong Radix-2 IFFT on X then Y axis → spatial height map
```

The resulting displacement texture is sampled in the vertex shader. Surface normals are computed via finite differences for accurate Phong lighting and Fresnel reflection.

---

## Scene Systems

### Weather
Three presets with smooth parameter interpolation:

| Key | Mode | Effect |
|---|---|---|
| `1` | Calm | Low waves, clear sky |
| `2` | Windy | Larger FFT amplitude, moving clouds |
| `3` | Storm | Maximum wave height, heavy rain |
| `4` | Auto | Cycles between states automatically |

### Rain & Ripples
Up to **2000 billboard rain particles** with up to **200 water surface ripples** rendered as expanding rings.

### Fish School
CPU **Boids flocking** (separation, alignment, cohesion) driving GPU instanced rendering of up to ~500 fish.

### Floating Objects
Boxes sampled directly from the FFT displacement map — they ride the waves with correct height and tilt. Underwater objects use Beer-Lambert fog.

### Showcase Mode (`V`)
Automated camera demo combining three sine frequencies for natural vertical oscillation (−6 m to +16 m), useful for recording footage.

---

## Post-Processing Pipeline

```
HDR scene
  └─ SSAO
  └─ Screen-Space Reflections (SSR)
  └─ God Rays (light shaft scattering)
  └─ Bloom
  └─ Depth of Field
  └─ TAA (Temporal Anti-Aliasing)
  └─ Tone mapping → LDR output
```

---

## Controls

| Input | Action |
|---|---|
| `W / A / S / D` | Move camera |
| Mouse drag | Rotate camera |
| `V` | Toggle showcase (auto-fly) mode |
| `Tab` | Toggle wireframe |
| `1 / 2 / 3` | Switch weather preset |
| `4` | Auto weather cycling |
| ImGui panel | Adjust wave params, lighting, effects in real time |

---

## Build Instructions

**Requirements**
- Windows 10 / 11 (x64)
- Visual Studio 2022 with "Desktop development with C++" workload
- NuGet packages are restored automatically on first build:
  - `Microsoft.Direct3D.D3D12` 1.618.3 (Agility SDK)
  - `Microsoft.Direct3D.DXC` 1.8.2505.32

**Steps**
1. Clone the repository
2. Open the `.sln` file in Visual Studio 2022
3. Build → Build Solution (`Ctrl+Shift+B`) — NuGet packages restore automatically
4. Run the executable from the project root

---

## Project Structure

```
HelloTriangle/
├── Main.cpp                    Entry point (WinMain)
├── D3D12HelloTriangle.h/cpp    Application class, DX12 device setup
└── source/
    ├── OceanFFT.h/cpp          FFT simulation (Phillips, time evolution, IFFT)
    ├── Renderer.h/cpp          Ocean mesh, draw calls
    ├── SkyDome.h/cpp           Procedural sky + clouds
    ├── WeatherSystem.h/cpp     Weather state machine
    ├── RainSystem.h/cpp        Rain particles + ripples
    ├── FishSchool.h/cpp        Boids + GPU instancing
    ├── FloatingObject.h/cpp    Wave-riding objects
    └── PostProcessPipeline.h/cpp  Full post-process chain

shaders/
├── PhillipsCS.hlsl             Spectrum initialisation (compute)
├── TimeEvolutionCS.hlsl        Per-frame spectrum update (compute)
├── IFFTCS.hlsl                 Inverse FFT (compute)
├── shaders.hlsl                Ocean surface vertex/pixel shader
├── skyShaders.hlsl             Sky dome shaders
└── ...                         Post-process shaders
```

---

## Documentation

Detailed technical write-ups are included in the repository:

- [`TechGuide_CN.md`](TechGuide_CN.md) — Technical guide (Chinese)
- [`TechnicalDocument_JP.md`](TechnicalDocument_JP.md) — Technical document (Japanese)
- [`UserGuide_CN.md`](UserGuide_CN.md) — User guide (Chinese)
- [`UserGuide_JP.md`](UserGuide_JP.md) — User guide (Japanese)
- [`TECHSPEC.md`](TECHSPEC.md) — Full technical specification (Japanese)
