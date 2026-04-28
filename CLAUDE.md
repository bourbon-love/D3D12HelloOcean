# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Git 提交规范

提交时**不要**在 commit message 中添加 `Co-Authored-By: Claude` 字样，直接用简洁的中文描述提交内容。

## 构建方法

用 Visual Studio 2022 打开 `D3D12HelloWorld.sln`，选择 `HelloTriangle` 项目，配置 x64 Debug 或 Release 后构建。

命令行构建（使用本机 VS2022 Enterprise MSBuild）：

```powershell
& "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\MSBuild\Current\Bin\MSBuild.exe" D3D12HelloWorld.sln /p:Configuration=Debug /p:Platform=x64 /v:minimal
```

运行：

```powershell
Start-Process "HelloTriangle\bin\x64\Debug\D3D12HelloOcean.exe"
```

输出目录为 `HelloTriangle/bin/x64/Debug/` 或 `Release/`。项目无自动化测试。

编译要求：C++20、平台工具集 v143、Windows SDK 10.0+。

## 项目概览

一个基于 DirectX 12 的海洋模拟渲染器，包含动态天空。入口为 `HelloTriangle/Main.cpp` → `Win32Application` → `D3D12HelloTriangle`（主应用类）。

## 架构

主应用类 `D3D12HelloTriangle` 以 `unique_ptr` 持有所有子系统，驱动标准 DX12 帧循环：`OnInit → OnUpdate → OnRender → OnDestroy`。

**子系统职责：**

| 类 | 文件 | 职责 |
|---|---|---|
| `Renderer` | `source/Renderer.cpp` | 海洋网格（512×512 格、400×400 世界单位），Gerstner 波，持有 `Camera` |
| `SkyDome` | `source/SkyDome.cpp` | 程序化天空球，Perlin 噪声云层，太阳/月亮追踪 |
| `OceanFFT` | `source/OceanFFT.cpp` | GPU 计算 FFT 管线（Phillips 谱 → 时间演化 → IFFT），生成高度/位移贴图 |
| `WeatherSystem` | `source/WeatherSystem.cpp` | 天气状态机（平静 / 多风 / 风暴），平滑插值风速、云量、雨量参数 |
| `RainSystem` | `source/RainSystem.cpp` | 雨滴粒子系统（最多 2000 个）及水面涟漪（最多 200 个） |
| `Camera` | `source/Camera.cpp` | 内嵌于 `Renderer`；WASD + 鼠标控制，支持自动展示环绕模式 |

**帧循环数据流：**

```
OnUpdate:
  Renderer::Update        → 推进时间，更新相机
  OceanFFT::Dispatch      → 执行计算着色器，写入高度贴图
  WeatherSystem::Update   → 插值参数，推送给 OceanFFT / SkyDome / RainSystem
  RainSystem::Update      → 生成/移动粒子和涟漪
  SkyDome::Update         → 更新太阳/月亮位置和云层参数

OnRender → PopulateCommandList:
  清除 RT + DSV
  更新常量缓冲区
  Renderer::Render        → 绘制海洋网格（采样 OceanFFT 高度贴图）
  SkyDome::Render         → 绘制天空球（渲染到远平面）
  RainSystem::Render      → 绘制雨滴和涟漪
```

## 着色器结构

| 着色器 | 类型 | 用途 |
|---|---|---|
| `shaders.hlsl` | VS + PS | 海洋表面：采样 FFT 高度贴图 + Gerstner 波，Phong + Fresnel 光照 |
| `skyShaders.hlsl` | VS + PS | 天空球：3D Perlin/FBM 云层、渐变天空色、太阳/月亮光晕 |
| `shaders/PhillipsCS.hlsl` | CS | 生成初始 Phillips 谱（仅初始化时执行一次） |
| `shaders/TimeEvolutionCS.hlsl` | CS | 按帧演化频谱，使用色散关系 ω(k)=√(g|k|) |
| `shaders/IFFTCS.hlsl` | CS | Cooley-Tukey Radix-2 IFFT，每轴两次（乒乓缓冲） |
| `shaders/rain.hlsl` | VS + PS | 雨滴广告牌粒子 |
| `shaders/waterbody.hlsl` | VS + PS | 半透明水体边界盒 |

## 关键常量缓冲区

**`SceneCB`**（Renderer，每帧更新）——包含 view/proj 矩阵、相机位置、`time`、`sunDir`、`sunIntensity`、`sunColor`、`skyColor`、雾参数，以及四组 Gerstner 波参数 `WaveParam waves[4]`。

**`SkyCB`**（SkyDome，256 字节对齐）——`viewProj`、`topColor`、`middleColor`、`bottomColor`、`sunPosition`、`moonPosition`、`time`、`cloudDensity`、`cloudScale`、`cloudSharpness`、`weatherIntensity`。

## 渲染管线状态对象（PSO）

启动时创建五类 PSO：
1. **海洋实体** — 背面剔除，深度读写
2. **海洋线框** — 相同拓扑，线框填充（按 `W` 键切换）
3. **水体边界盒** — 不剔除，Alpha 混合，深度测试不写入
4. **天空球** — 正面剔除（内外翻转球体），深度测试不写入
5. **计算 PSO** — Phillips、TimeEvolution、IFFT×2

## OceanFFT 贴图管线

```
m_h0Map（初始化一次）  →  Phillips 频谱
         ↓ 每帧
m_hktMap               →  h(k,t) + Dx(k,t) 频域
m_dztMap               →  Dz(k,t) 频域
         ↓ IFFT（通过 m_tempMap / m_dztTempMap 乒乓）
m_heightMap            →  .x = 高度，.z = Dx 位移（由 shaders.hlsl 采样）
```

## 其他说明

- DX12 资源屏障、描述符堆管理和围栏同步均在 `D3D12HelloTriangle.cpp` 中内联处理。`Renderer` 使用 1024 帧环形缓冲区管理每帧常量缓冲槽。
- ImGUI 后端位于 `HelloTriangle/ImGUI/`，基础设施已就绪，但尚未绑定任何 UI 控件。
- 代码注释主要为中文。
- `Camera::UpdateShowcase()` 提供自动环绕展示模式，通过 `D3D12HelloTriangle::OnKeyDown` 中的按键切换。
