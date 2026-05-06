# DirectX 12 海洋渲染器 — 从零开始学习教程

> 本教程基于 D3D12HelloOcean 项目，由浅入深地讲解 DirectX 12 实时渲染的核心概念与实现技巧。项目涵盖 GPU 计算、物理模拟、后处理管线等多个主题，是学习现代图形编程的完整案例。

---

## 目录

1. [学习前提与环境配置](#1-学习前提与环境配置)
2. [DirectX 12 核心概念](#2-directx-12-核心概念)
3. [项目整体架构](#3-项目整体架构)
4. [FFT 海洋模拟](#4-fft-海洋模拟)
5. [海洋网格渲染](#5-海洋网格渲染)
6. [天空系统](#6-天空系统)
7. [天气与粒子系统](#7-天气与粒子系统)
8. [鱼群模拟（Boids 算法）](#8-鱼群模拟boids-算法)
9. [阴影映射](#9-阴影映射)
10. [屏幕空间反射（SSR）](#10-屏幕空间反射ssr)
11. [后处理管线](#11-后处理管线)
12. [水下摄像机效果](#12-水下摄像机效果)
13. [ImGui 调试界面](#13-imgui-调试界面)
14. [常见问题与陷阱](#14-常见问题与陷阱)
15. [扩展方向建议](#15-扩展方向建议)

---

## 1. 学习前提与环境配置

### 1.1 需要具备的基础知识

- **C++ 基础**：类、智能指针（`unique_ptr`、`ComPtr`）、模板
- **线性代数**：矩阵变换、向量运算、坐标系变换
- **图形学基础**：光栅化流程、深度缓冲、着色器概念

不需要提前了解 DirectX，本教程会从基础讲起。

### 1.2 开发环境

```
Visual Studio 2022（任意版本均可）
Windows 10/11 x64
DirectX 12 Agility SDK（项目自动通过 NuGet 获取）
```

### 1.3 构建与运行

```powershell
# 命令行构建
& "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\MSBuild\Current\Bin\MSBuild.exe" `
  D3D12HelloWorld.sln /p:Configuration=Debug /p:Platform=x64 /v:minimal

# 运行
Start-Process "HelloTriangle\bin\x64\Debug\D3D12HelloOcean.exe"
```

---

## 2. DirectX 12 核心概念

这是整个学习路径最重要的基础章节。DX12 比 DX11 复杂很多，但性能和灵活性更高。

### 2.1 为什么 DX12 更复杂？

DX11 帮你管理了很多事情（同步、内存、状态），DX12 把这些责任交给了开发者。好处是：

- **更低的 CPU 开销**：驱动不再猜测你要做什么
- **多线程友好**：可以在多个线程同时录制命令
- **显式控制**：你知道每一个字节在哪里

### 2.2 关键对象一览

```
ID3D12Device              → GPU 设备。所有资源都从这里创建。
ID3D12CommandQueue        → GPU 执行队列。把命令列表提交给它。
ID3D12CommandAllocator    → 命令内存池。命令列表从这里分配内存。
ID3D12GraphicsCommandList → 命令列表。你在这里"录制"GPU 操作。
ID3D12Resource            → 通用资源（纹理、缓冲区都是它）。
ID3D12DescriptorHeap      → 描述符堆。告诉 GPU 如何解释资源。
IDXGISwapChain3           → 交换链。管理前/后缓冲区的显示。
ID3D12Fence               → 围栏。CPU/GPU 同步的核心机制。
```

### 2.3 DX12 渲染一帧的最简流程

```cpp
// 1. 重置命令分配器（GPU 已经用完上一帧的命令）
m_commandAllocator->Reset();

// 2. 重置命令列表，开始录制
m_commandList->Reset(m_commandAllocator.Get(), nullptr);

// 3. 设置资源状态（Present → RenderTarget）
auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
    backBuffer,
    D3D12_RESOURCE_STATE_PRESENT,
    D3D12_RESOURCE_STATE_RENDER_TARGET);
m_commandList->ResourceBarrier(1, &barrier);

// 4. 录制绘制命令
m_commandList->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
m_commandList->DrawInstanced(3, 1, 0, 0);

// 5. 恢复状态（RenderTarget → Present）
// ... 再做一次 ResourceBarrier

// 6. 关闭命令列表
m_commandList->Close();

// 7. 提交给 GPU 执行
ID3D12CommandList* lists[] = { m_commandList.Get() };
m_commandQueue->ExecuteCommandLists(1, lists);

// 8. 显示
m_swapChain->Present(1, 0);

// 9. 等待 GPU 完成（本项目用简单围栏等待）
WaitForPreviousFrame();
```

### 2.4 资源屏障（Resource Barrier）——最容易出错的地方

GPU 资源有"状态"，不同操作需要不同状态。状态用错会导致 GPU 验证错误或画面异常。

```cpp
// 常见状态：
D3D12_RESOURCE_STATE_RENDER_TARGET          // 作为渲染目标写入
D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE  // 被像素着色器读取
D3D12_RESOURCE_STATE_DEPTH_WRITE            // 深度缓冲写入
D3D12_RESOURCE_STATE_COPY_SOURCE            // 作为 CopyResource 的来源
D3D12_RESOURCE_STATE_COPY_DEST             // 作为 CopyResource 的目标
D3D12_RESOURCE_STATE_UNORDERED_ACCESS       // 计算着色器读写
```

**本项目的实际例子**：HDR 渲染目标在每帧的状态变化

```
帧开始: RENDER_TARGET（场景渲染写入）
  ↓
天空渲染后: RENDER_TARGET → COPY_SOURCE（拷贝天空快照）
  ↓
  COPY_SOURCE → RENDER_TARGET（继续渲染水下物体）
  ↓
场景完成: RENDER_TARGET → PIXEL_SHADER_RESOURCE（后处理读取）
  ↓
后处理完成: PIXEL_SHADER_RESOURCE → RENDER_TARGET（恢复，下一帧用）
```

### 2.5 描述符堆（Descriptor Heap）

描述符是"告诉 GPU 如何看待一块内存"的小结构体。你不能直接把资源传给着色器，必须创建描述符。

```cpp
// 创建一个可着色器访问的 SRV 堆
D3D12_DESCRIPTOR_HEAP_DESC hd = {};
hd.Type  = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
hd.NumDescriptors = 5;
hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE; // 着色器可见！
device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap));

// 在堆的第 2 个槽写入一个 SRV
D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
sd.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
sd.Texture2D.MipLevels = 1;
device->CreateShaderResourceView(
    texture.Get(), &sd,
    CD3DX12_CPU_DESCRIPTOR_HANDLE(heap->GetCPUDescriptorHandleForHeapStart(), 2, incrSize));
```

### 2.6 根签名（Root Signature）

根签名定义"着色器从哪里拿数据"。就像函数签名定义参数一样。

```cpp
// 本项目的海洋渲染根签名：
// [0] b0: SceneCB（常量缓冲区视图，内联绑定）
// [1] t0-t4: 5 个纹理（描述符表）
// [2] b1: RippleCB（波纹常量缓冲区）
// [3] b2: ShadowSceneCB（阴影常量缓冲区）

CD3DX12_ROOT_PARAMETER params[4];
params[0].InitAsConstantBufferView(0);   // b0

CD3DX12_DESCRIPTOR_RANGE srvRange;
srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 5, 0); // t0~t4
params[1].InitAsDescriptorTable(1, &srvRange);

params[2].InitAsConstantBufferView(1);   // b1
params[3].InitAsConstantBufferView(2);   // b2
```

**重要教训**：根签名是全局状态。如果某个子系统（比如 FloatingObject）设置了自己的根签名，之后的代码必须重新绑定海洋的根签名，否则后续的 `SetGraphicsRootDescriptorTable` 调用会读到错误的槽位，导致 GPU 报错或画面损坏。

---

## 3. 项目整体架构

### 3.1 代码文件结构

```
HelloTriangle/
├── D3D12HelloTriangle.h / .cpp    ← 应用主类（设备、交换链、场景编排）
├── Main.cpp                        ← 程序入口
├── source/
│   ├── Renderer.h / .cpp          ← 海洋网格 + 相机
│   ├── SkyDome.h / .cpp           ← 天球系统
│   ├── OceanFFT.h / .cpp          ← GPU FFT 海洋模拟
│   ├── WeatherSystem.h / .cpp     ← 天气状态机
│   ├── RainSystem.h / .cpp        ← 雨粒子 + 波纹
│   ├── FloatingObject.h / .cpp    ← 浮箱 + 水下物体
│   ├── FishSchool.h / .cpp        ← 鱼群 Boids 模拟
│   ├── Camera.h / .cpp            ← 相机控制
│   └── PostProcessPipeline.h / Impl.cpp  ← 所有后处理效果
└── shaders/
    ├── shaders.hlsl               ← 海洋表面着色器
    ├── skyShaders.hlsl            ← 天空着色器
    ├── fish.hlsl                  ← 鱼群着色器
    ├── tonemapping.hlsl           ← 色调映射 + 水下效果
    ├── bloom_*.hlsl               ← 泛光
    ├── godrays_*.hlsl             ← 丁达尔光
    ├── ssao_*.hlsl                ← 环境光遮蔽
    ├── shadowmap_*.hlsl           ← 阴影深度
    ├── taa_*.hlsl                 ← 时间性抗锯齿
    ├── dof_*.hlsl                 ← 景深
    ├── lensflare_*.hlsl           ← 镜头光晕
    ├── lightning_*.hlsl           ← 闪电
    ├── PhillipsCS.hlsl            ← FFT 初始化计算着色器
    ├── TimeEvolutionCS.hlsl       ← FFT 时间演化计算着色器
    └── IFFTCS.hlsl                ← 逆 FFT 计算着色器
```

### 3.2 每帧调用链（概览）

```
OnUpdate()
  子系统 Update → ImGui 构建

OnRender()
  ┌─ OceanFFT 计算（GPU Compute）
  ├─ 阴影图渲染
  ├─ 天空渲染 → 天空快照（SSR 用）
  ├─ 水下物体 + 鱼群渲染 → 折射快照
  ├─ 海洋网格渲染
  ├─ 水体边界盒 / 浮箱 / 雨水
  ├─ 闪电 + SSAO
  └─ 后处理：TAA → Bloom → GodRays → DOF → ToneMap → LensFlare
```

### 3.3 职责分工原则

本项目遵循一个设计原则：**每个子系统管理自己的资源和 PSO，主类负责协调顺序**。

- 子系统持有自己的根签名、PSO、顶点缓冲区
- 主类（`D3D12HelloTriangle`）决定渲染顺序，传入 `RenderContext`
- 后处理统一由 `PostProcessPipeline` 管理，不需要主类关心细节

---

## 4. FFT 海洋模拟

这是项目中技术含量最高的部分，也是最值得深入理解的。

### 4.1 为什么用 FFT？

海洋表面是成千上万个正弦波叠加的结果。暴力方法（在 CPU 上叠加 N 个正弦波）的复杂度是 O(N×M)，其中 M 是网格顶点数，效率很低。

FFT 方法的关键洞察：**在频域定义波谱，再用逆 FFT 一次性转换到空域**。复杂度降至 O(M·logM)，而且整个过程在 GPU 上运行。

### 4.2 三个计算着色器阶段

**阶段 1：Phillips 谱（仅初始化一次）**

```hlsl
// PhillipsCS.hlsl 的核心逻辑
float phillipsSpectrum(float2 k) {
    float k_len = length(k);
    if (k_len < 0.0001) return 0.0;
    
    float k2 = k_len * k_len;
    float k4 = k2 * k2;
    
    // L = V²/g，V 是风速，g 是重力加速度
    float L = windSpeed * windSpeed / gravity;
    
    // Phillips 公式：振幅与波方向和风向的点积相关
    float dot_kw = dot(normalize(k), windDir);
    
    return amplitude * exp(-1.0 / (k2 * L * L)) / k4 * dot_kw * dot_kw;
}
```

Phillips 谱描述了：**在给定风速和风向下，各频率的波有多大振幅**。一次生成，永久有效。

**阶段 2：时间演化（每帧执行）**

```hlsl
// TimeEvolutionCS.hlsl 核心
// 色散关系：omega = sqrt(g * |k|)
// 不同频率的波以不同速度传播，这产生了真实的海浪外观

float omega = sqrt(gravity * k_len);
float cosOmegaT = cos(omega * time);
float sinOmegaT = sin(omega * time);

// 利用共轭对称性（厄米特对称）更新频域
h_kt = complex_mul(h0, complex(cosOmegaT, sinOmegaT))
     + complex_mul(conjugate(h0_conj), complex(cosOmegaT, -sinOmegaT));
```

**阶段 3：逆 FFT（每帧执行）**

Cooley-Tukey Radix-2 FFT 算法。先做行（X 轴），再做列（Y 轴），共 4 次 pass（高度图和 XZ 位移图各 2 次）。

```hlsl
// IFFTCS.hlsl 简化逻辑（蝶形运算）
for (int step = 1; step < N; step *= 2) {
    // 每个蝶形单元：
    // E = even_element, O = odd_element
    // W = twiddle factor = e^(2πi·k/N)
    float2 E = data[base];
    float2 O = complex_mul(twiddle, data[base + step]);
    data[base]        = E + O;  // 加法
    data[base + step] = E - O;  // 减法
}
```

### 4.3 UAV ↔ SRV 状态切换

FFT 的输出是 UAV（Unordered Access View，可读写），但海洋着色器需要 SRV（Shader Resource View，只读）。每帧都要做状态转换：

```cpp
// 计算完成后：UAV → 非像素着色器可读（SRV）
D3D12_RESOURCE_BARRIER toSRV[2] = {
    CD3DX12_RESOURCE_BARRIER::Transition(heightMap,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
    // ...dztMap 同理
};
m_commandList->ResourceBarrier(2, toSRV);

// 帧末尾：SRV → UAV（为下一帧的计算做准备）
D3D12_RESOURCE_BARRIER toUAV[2] = { /* 反过来 */ };
m_commandList->ResourceBarrier(2, toUAV);
```

注意是 `NON_PIXEL_SHADER_RESOURCE` 而不是 `PIXEL_SHADER_RESOURCE`，因为海洋顶点着色器（VS）也要采样这张图。

### 4.4 学习要点总结

| 概念 | 理解方式 |
|------|---------|
| Phillips 谱 | 频域中的"波浪食谱"，描述每种频率的波有多强 |
| 时间演化 | 让各频率的波以正确速度（色散关系）"跑动" |
| 逆 FFT | 把频域的"食谱"转换成实际的海面高度图 |
| 乒乓缓冲 | FFT 中间结果写入 temp，再写回 output，避免读写冲突 |

---

## 5. 海洋网格渲染

### 5.1 网格生成

```cpp
// GridMesh.cpp：512×512 的均匀网格，世界空间 400×400 单位
for (int z = 0; z < gridH; z++) {
    for (int x = 0; x < gridW; x++) {
        float wx = (x / (float)(gridW-1) - 0.5f) * worldSize;
        float wz = (z / (float)(gridH-1) - 0.5f) * worldSize;
        vertices[z * gridW + x] = { wx, 0.0f, wz };
    }
}
// 索引：三角形带，每个格子 2 个三角形
```

### 5.2 顶点着色器——高度采样

```hlsl
// shaders.hlsl VS 核心逻辑
float4 h = heightMap.Load(int3(texCoord, 0));
float height = h.x;     // 垂直位移
float dx     = h.z;     // X 方向水平位移（雅各比偏移）

// 应用 Gerstner 波（4 个叠加）
for (int i = 0; i < 4; i++) {
    // Gerstner 波公式：粒子沿圆形轨迹运动（比正弦波更真实）
    float phi = dot(waves[i].direction, pos.xz) * waves[i].frequency
                + time * waves[i].speed;
    pos.x += waves[i].amplitude * waves[i].direction.x * sin(phi);
    pos.y += waves[i].amplitude * cos(phi);
    pos.z += waves[i].amplitude * waves[i].direction.z * sin(phi);
}
```

**Gerstner 波 vs 正弦波**：正弦波让水面上下运动，Gerstner 波让水粒子做圆周运动（更接近真实），产生浪头尖锐、浪谷平缓的效果。

### 5.3 像素着色器——光照模型

```hlsl
// Phong 光照 + Fresnel 反射
float3 N = normalize(normal);
float3 V = normalize(cameraPos - worldPos);
float3 L = normalize(sunDir);
float3 R = reflect(-L, N);

// 漫反射
float diff = max(dot(N, L), 0.0);

// 镜面反射
float spec = pow(max(dot(R, V), 0.0), 64.0);

// Fresnel 效应：掠射角反射更强（水面边缘更亮）
float fresnel = pow(1.0 - max(dot(N, V), 0.0), 4.0);

// 水面颜色 = 折射色（透视水下） + 反射色（天空）
float3 waterColor = lerp(underwaterColor, skyColor, fresnel);
float3 finalColor = waterColor * diff + sunColor * spec;
```

### 5.4 线框模式切换

```cpp
// 两个 PSO：实体和线框，按 Tab 键切换
D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
// 实体 PSO：
pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
// 线框 PSO：
pd.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
```

---

## 6. 天空系统

### 6.1 天球几何体

天球是一个内翻（法线朝内）的球体，摄像机始终在它内部，因此：

```cpp
// PSO 设置：剔除正面（渲染内部面）
pd.RasterizerState.CullMode = D3D12_CULL_MODE_FRONT;
// 深度测试但不写入（天空永远在最远处）
pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
```

### 6.2 程序化云层

使用 3D FBM（分形布朗运动）噪声生成云：

```hlsl
// skyShaders.hlsl 云层算法
float fbm(float3 p) {
    float value = 0.0;
    float amplitude = 0.5;
    for (int i = 0; i < 5; i++) {
        value += amplitude * noise(p);  // Perlin 噪声
        p *= 2.0;                        // 频率翻倍（更细的细节）
        amplitude *= 0.5;               // 振幅减半（细节变弱）
    }
    return value;
}

float cloud = smoothstep(threshold, threshold + sharpness, fbm(samplePos + time));
```

FBM 的关键思想：**叠加不同频率的噪声**，频率越高的叠加越少，产生自然的分形外观。

### 6.3 太阳/月亮水平线裁剪

这是一个经常被忽视的细节：当太阳在海平面以下时，即使摄像机在水面以上，也不应该看到太阳（被地球遮挡）。

```hlsl
// 无论摄像机在水上还是水下，都基于太阳方向裁剪
float sunVis  = smoothstep(-0.04, 0.06, sunPosition.y);
float moonVis = smoothstep(-0.04, 0.06, moonPosition.y);

// 太阳的最终颜色乘以 sunVis，低于水平线时 sunVis→0
sunColor *= sunVis;
moonColor *= moonVis;
```

`smoothstep` 在 -0.04 到 +0.06 之间平滑过渡，避免突然消失的感觉。

---

## 7. 天气与粒子系统

### 7.1 天气状态机

```cpp
// WeatherSystem.cpp
enum class WeatherState { Calm, Windy, Storm };

void WeatherSystem::Update(float dt) {
    // 平滑过渡：目标参数 → 当前参数，速度 = lerpSpeed * dt
    m_currentWindSpeed = lerp(m_currentWindSpeed, m_targetWindSpeed, lerpSpeed * dt);
    m_currentCloudDensity = lerp(m_currentCloudDensity, m_targetCloudDensity, lerpSpeed * dt);
    m_currentRainIntensity = lerp(m_currentRainIntensity, m_targetRainIntensity, lerpSpeed * dt);
    
    // 自动切换（随机计时）
    m_stateTimer -= dt;
    if (m_stateTimer <= 0) {
        transitionToRandomState();
    }
}
```

lerp（线性插值）是游戏开发中最常用的平滑技巧：`lerp(a, b, t) = a + (b-a)*t`。每帧 lerp 一小步，就得到平滑过渡。

### 7.2 雨粒子系统

```cpp
// RainSystem.cpp：每帧更新雨滴位置
for (auto& drop : m_drops) {
    drop.pos += drop.velocity * dt;
    
    // 超出范围 → 重新生成在顶部
    if (drop.pos.y < -10.0f) {
        drop.pos.y = 50.0f;
        drop.pos.x = cameraPos.x + randomRange(-30, 30);
        drop.pos.z = cameraPos.z + randomRange(-30, 30);
    }
}
```

雨滴用**广告牌（Billboard）**渲染：总是朝向摄像机的平面。

```hlsl
// rain.hlsl VS：将顶点偏移到始终面朝相机的方向
float3 right = normalize(cross(float3(0,1,0), toCamera));
float3 up    = normalize(cross(toCamera, right));
pos += right * localPos.x + up * localPos.y;
```

### 7.3 水面波纹

雨滴落水时产生圆形扩散波纹。波纹用圆环几何体渲染，随时间扩大并淡出：

```cpp
// 波纹生命周期
ripple.radius += ripple.expandSpeed * dt;
ripple.alpha  -= dt / ripple.lifetime;  // 逐渐透明
if (ripple.alpha <= 0) removeRipple(ripple);
```

---

## 8. 鱼群模拟（Boids 算法）

Boids 是 1986 年 Craig Reynolds 提出的群体行为模型，用三条简单规则模拟鸟群/鱼群：

### 8.1 三条核心规则

```
1. 分离（Separation）：不要太靠近邻居
2. 对齐（Alignment）：朝向和邻居一致
3. 聚合（Cohesion）：向邻居的中心靠拢
```

```cpp
// FishSchool.cpp：Boids 实现
XMVECTOR sep = XMVectorZero(); // 分离力
XMVECTOR aln = XMVectorZero(); // 对齐力
XMVECTOR coh = XMVectorZero(); // 聚合力

for (int j = 0; j < N; j++) {
    XMVECTOR diff = pos_i - pos_j;
    float dist = length(diff);
    
    if (dist < SEP_R)            // 太近：推开
        sep += normalize(diff) / dist;
    if (dist < ALN_R)            // 中等距离：对齐速度方向
        aln += velocity_j;
    if (dist < COH_R)            // 较远：向群体中心靠拢
        coh += pos_j;
}

XMVECTOR steer = sep * SEP_W
               + normalize(aln / alnCount) * ALN_W
               + normalize(coh / cohCount - pos_i) * COH_W;
velocity_i += steer * dt * 3.0f;
```

### 8.2 边界约束

```cpp
// 水平范围约束：超出圆形区域 → 转向圆心
float xzDist = sqrt(pos.x*pos.x + pos.z*pos.z);
if (xzDist > ZONE_R)
    steer += normalize(-float3(pos.x, 0, pos.z)) * BOUND_W;

// 深度约束（重要！防止鱼露出水面）
if (pos.y > DMAX)  // DMAX = -5m（平均海平面以下 5 米）
    steer += float3(0, -BOUND_W, 0);  // 软推力向下

// 硬截断：绝对不允许超过上限（处理软推力来不及响应的情况）
if (np.y > DMAX) {
    np.y = DMAX;
    velocity.y = min(velocity.y, 0.0f); // 清零向上速度
}
```

**为什么需要硬截断？**  
软推力（施加向下的加速度）有响应延迟。如果鱼以较大速度向上冲，软推力需要好几帧才能使其减速。在此期间鱼可能已经冲出水面。硬截断是"保险丝"：位置绝对不超过上限，速度也立即清零。

### 8.3 GPU 实例化渲染

CPU 算好每条鱼的位置和朝向后，打包成 `StructuredBuffer` 传给 GPU：

```hlsl
// fish.hlsl VS
struct FishInstance {
    float3 pos;    float phase;   // 相位（用于尾部摆动动画）
    float3 fwd;    float speed;   // 朝向和速度
    float3 up;     float pad;
    float4 color;
};

StructuredBuffer<FishInstance> g_instances : register(t0);

// 每条鱼的每个顶点：
FishInstance inst = g_instances[SV_InstanceID];

// 构建局部→世界变换矩阵
float3 right = cross(inst.up, inst.fwd);
float3x3 rot = float3x3(right, inst.up, inst.fwd);

// 尾部摆动：X < 0 的顶点（尾部）随时间摆动
float tailWeight = saturate(-localPos.x);   // 头部=0，尾部=1
float wag = sin(inst.phase + time * 3.0) * tailWeight * tailWeight;
localPos.x += wag * 0.3;

float3 worldPos = inst.pos + mul(localPos, rot);
```

关键设计：`SV_InstanceID` 让 GPU 自动知道当前在画第几条鱼，一次 DrawCall 画完所有鱼。

---

## 9. 阴影映射

### 9.1 基本原理

阴影映射是两趟渲染：

1. **第一趟**：从光源视角渲染场景深度图（阴影图）
2. **第二趟**：正常渲染，每个像素检查"是否能被光源看到"

```cpp
// 阴影 PSO 特别设置
pd.RasterizerState.CullMode = D3D12_CULL_MODE_FRONT;  // 剔除正面（防止 Peter Pan 效应）
pd.RasterizerState.DepthBias = 8000;                   // 深度偏移（防止 Z-fighting）
pd.RasterizerState.SlopeScaledDepthBias = 3.0f;        // 斜面额外偏移
pd.NumRenderTargets = 0;                               // 只写深度，不写颜色
```

**Peter Pan 效应**：物体在地面上没有阴影，但脚下有一圈浮空的阴影。原因是深度偏移不够，用正面剔除可以缓解。

### 9.2 光源矩阵计算

```cpp
// 以相机为中心、光源方向为视点构建正交投影
XMVECTOR eye = camPos - sunDir * 150.0f;  // 光源位置
XMVECTOR at  = float3(camPos.x, 0, camPos.z);  // 看向水面
XMMATRIX lightView = XMMatrixLookAtLH(eye, at, up);
XMMATRIX lightProj = XMMatrixOrthographicLH(200.0f, 200.0f, 1.0f, 400.0f);
m_lightViewProj = lightView * lightProj;
```

### 9.3 着色器中的阴影检测

```hlsl
// shaders.hlsl：在世界空间采样阴影图
float4 lightClip = mul(float4(worldPos, 1.0), lightViewProj);
float2 shadowUV = lightClip.xy / lightClip.w * 0.5 + 0.5;
shadowUV.y = 1.0 - shadowUV.y;  // DX 的 UV 纵轴朝下

float shadowDepth = shadowMap.Sample(sampler, shadowUV).r;
float currentDepth = lightClip.z / lightClip.w;

float shadow = (currentDepth > shadowDepth + bias) ? shadowStrength : 0.0;
float3 finalColor = ambient + (1.0 - shadow) * directLight;
```

---

## 10. 屏幕空间反射（SSR）

SSR 是一种廉价近似海洋反射天空的技术。

### 10.1 策略：拍快照

```
每帧顺序：
1. 渲染天空 → 天空图像在 HDR RT 中
2. 把 HDR RT 拷贝到 skySnapshotRT（"拍照"）
3. 继续渲染其他物体
4. 海洋着色器采样 skySnapshotRT 作为反射源
```

```cpp
// 拍天空快照：HDR RT → skySnapshotRT
cmd->CopyResource(m_skySnapshotRT.Get(), m_hdrRT.Get());
```

这是最简单的 SSR 实现：反射图是当前帧之前渲染的天空，不包含场景中的动态物体，但对海洋来说已经足够真实。

### 10.2 折射快照

同样的技术用于折射（水下透视）：

```
渲染天空 + 水下物体 + 鱼群
        ↓
拷贝到 refractionRT
        ↓
海洋着色器用 refractionRT 模拟透过水面看到的物体
```

---

## 11. 后处理管线

所有后处理效果都在 `PostProcessPipeline` 类里，整体执行顺序如下：

```
场景 HDR RT → TAA → Bloom → GodRays → DOF → ToneMap → LensFlare → 屏幕
```

### 11.1 为什么用全屏三角形？

后处理不需要任何顶点缓冲区！直接用一个覆盖屏幕的大三角形：

```hlsl
// 全屏三角形 VS：不需要 VB，仅靠顶点 ID 生成
float2 uv = float2((vertexID << 1) & 2, vertexID & 2);
output.pos = float4(uv * 2.0 - 1.0, 0.0, 1.0);
output.uv  = float2(uv.x, 1.0 - uv.y);
```

三个顶点：`(-1,-1)`, `(-1, 3)`, `(3, -1)` —— 形成一个足够大的三角形覆盖整个屏幕。

### 11.2 TAA（时间性抗锯齿）

TAA 的核心思想：**把当前帧和历史帧混合，等效于每帧采样更多的位置**。

```hlsl
// taa.hlsl：混合当前帧和历史帧
float4 current = currentFrame.Sample(pointSampler, uv);   // 当前帧（点采样）
float4 history = historyFrame.Sample(bilinearSampler, uv); // 历史帧（双线性）

// 用当前帧的邻居像素来"夹住"历史帧，防止鬼影
float4 minNeighbor = min(min(min(n0, n1), min(n2, n3)), current);
float4 maxNeighbor = max(max(max(n0, n1), max(n2, n3)), current);
history = clamp(history, minNeighbor, maxNeighbor);

output = lerp(current, history, blendFactor); // 默认 0.9
```

为了让 TAA 有效，相机每帧的 VP 矩阵要加一个亚像素抖动：

```cpp
// Halton 序列（低差异序列，比随机更均匀）
auto Halton = [](int idx, int base) {
    float f = 1.0f, r = 0.0f;
    while (idx > 0) { f /= base; r += f * (idx % base); idx /= base; }
    return r;
};
jitter.x = (Halton(frameIndex, 2) - 0.5f) * 2.0f / width;
jitter.y = (Halton(frameIndex, 3) - 0.5f) * 2.0f / height;
```

**Halton 序列的意义**：比纯随机分布更均匀，16 帧循环后恰好覆盖了像素的各个位置。

### 11.3 Bloom（泛光）

Bloom 让明亮物体产生发光晕染效果。

```
第一步：提取高亮部分
  → 对每个像素：if (亮度 > threshold) 保留，否则置黑

第二步：高斯模糊（分离卷积）
  → 水平模糊 + 垂直模糊 × 2 次迭代

第三步：与原图叠加
  → finalColor = HDR + bloomStrength * bloomBlurred
```

分离卷积（先横后竖）比 2D 卷积快很多：N×N 核的 2D 卷积 = 2×N 次操作，而非 N² 次。

### 11.4 God Rays（丁达尔光）

```hlsl
// godrays.hlsl：径向模糊（Radial Blur）
float2 toSun = sunScreenPos - uv;      // 从像素指向太阳的方向
float2 step  = toSun * density / 64;  // 每步朝太阳走一小步

float3 color = 0;
for (int i = 0; i < 64; i++) {
    color += hdrRT.Sample(sampler, uv).rgb * weight;
    uv    += step;     // 向太阳方向走
    weight *= decay;   // 随距离衰减
}
color *= exposure;
```

这个算法的原理：从每个像素出发，沿着指向太阳的方向采样 64 次，把采样到的光叠加。越靠近太阳的采样贡献越大（因为衰减）。

### 11.5 SSAO（屏幕空间环境光遮蔽）

SSAO 让凹陷处（裂缝、角落）变暗，增加立体感。

```hlsl
// ssao.hlsl：在法线半球内随机采样
float occlusion = 0.0;
for (int i = 0; i < 8; i++) {
    // 将预计算的核样本变换到视空间
    float3 samplePos = viewPos + TBN * kernel[i].xyz * radius;
    
    // 把采样点投影到屏幕，读取深度图
    float4 projSample = mul(float4(samplePos, 1.0), proj);
    float2 sampleUV = projSample.xy / projSample.w * 0.5 + 0.5;
    float sampleDepth = depthBuffer.Sample(sampler, sampleUV).r;
    
    // 如果采样深度更靠近相机（说明被遮挡），贡献遮蔽
    float rangeCheck = smoothstep(0.0, 1.0, radius / abs(viewPos.z - sampleDepth));
    occlusion += (sampleDepth < samplePos.z - bias ? 1.0 : 0.0) * rangeCheck;
}
occlusion = 1.0 - occlusion / 8.0;
```

### 11.6 DOF（景深）

景深让焦点范围外的物体模糊，模拟真实镜头效果。

```hlsl
// dof.hlsl：基于深度计算弥散圆（CoC）半径
float depth = depthBuffer.Sample(sampler, uv).r;
float linearDepth = proj.z / (depth - proj.w); // 线性化深度

// CoC：偏离焦点越远，弥散圆越大
float coc = abs(linearDepth - focusDepth) / focusRange;
coc = clamp(coc, 0.0, maxRadius);

// 在弥散圆内随机采样 HDR 图（Bokeh 模糊）
float3 color = 0;
for (int i = 0; i < 16; i++) {
    float2 offset = poissonDisk[i] * coc;
    color += hdrRT.Sample(sampler, uv + offset).rgb;
}
output = color / 16.0;
```

### 11.7 ToneMap（色调映射）

HDR（高动态范围）图像不能直接显示在屏幕上（屏幕是 LDR）。色调映射把 HDR 压缩到 [0,1] 范围。

本项目使用 ACES 近似：

```hlsl
float3 ACESFilm(float3 x) {
    // 经验公式，接近 ACES 标准曲线
    float a = 2.51f, b = 0.03f, c = 2.43f, d = 0.59f, e = 0.14f;
    return saturate((x*(a*x+b)) / (x*(c*x+d)+e));
}

float3 hdr = hdrRT.Sample(sampler, uv).rgb;
hdr *= exposure;                    // 曝光控制
float3 ldr = ACESFilm(hdr);        // 色调映射
float3 final = pow(ldr, 1.0/2.2);  // Gamma 校正
```

---

## 12. 水下摄像机效果

当摄像机 Y 坐标低于 0（水面以下），逐渐叠加水下效果：

```hlsl
// tonemapping.hlsl 水中效果
float uwBlend = saturate(-cameraY / 2.0); // -2m 以下 = 完全水中

if (uwBlend > 0) {
    // 1. UV 歪曲（折射波纹感）
    float2 distortedUV = uv + float2(
        sin(uv.y * 8.0 + time * 1.5) * 0.005,
        cos(uv.x * 8.0 + time * 1.2) * 0.005
    ) * uwBlend;
    float3 color = hdrRT.Sample(sampler, distortedUV).rgb;
    
    // 2. Beer-Lambert 颜色吸收（红光衰减最快）
    float depth = -cameraY;
    float3 absorption = exp(-float3(0.18, 0.05, 0.02) * depth);
    color *= absorption;
    
    // 3. 深度雾（越深越暗越蓝）
    float3 fogColor = float3(0.01, 0.07, 0.18);
    float fogFactor = saturate(depth / 20.0);
    color = lerp(color, fogColor, fogFactor * uwBlend);
    
    // 4. 焦散（光线穿过水面形成的光斑）
    float caustic = pow(
        saturate(sin(worldPos.x * 3.0 + time) * sin(worldPos.z * 3.7 + time * 0.8)),
        4.0
    ) * uwBlend * 0.3;
    color += caustic * float3(0.8, 0.9, 1.0);
    
    output = lerp(originalColor, color, uwBlend);
}
```

**Beer-Lambert 定律**：光在介质中传播时，强度随深度指数衰减。红光的衰减系数（0.18）远大于蓝光（0.02），所以深水是蓝色的。

---

## 13. ImGui 调试界面

ImGui 是一个即时模式 GUI 库，代码极其简洁：

```cpp
// D3D12HelloTriangle.cpp BuildImGuiUI()

// 基本控件：
ImGui::SliderFloat("曝光", &m_pp->exposure, 0.1f, 5.0f);
ImGui::Checkbox("启用 Bloom", &m_pp->bloomEnabled);
ImGui::RadioButton("平静", &weatherChoice, 0);

// 条件显示（只有 Bloom 开启时才显示强度调节）
if (m_pp->bloomEnabled) {
    ImGui::SliderFloat("阈值", &m_pp->bloomThreshold, 0.5f, 5.0f);
    ImGui::SliderFloat("强度", &m_pp->bloomStrength,  0.1f, 3.0f);
}
```

ImGui 和 DX12 的集成需要：

1. 创建一个 shader visible 的 SRV 堆（ImGui 内部字体纹理用）
2. 每帧调用 `ImGui_ImplDX12_NewFrame()` / `ImGui::NewFrame()`
3. `ImGui::Render()` 后在命令列表中调用 `ImGui_ImplDX12_RenderDrawData()`
4. ImGui 渲染必须在最终的 RT → Present 屏障**之前**

---

## 14. 常见问题与陷阱

### 14.1 资源状态错误（最常见）

**症状**：DX12 调试层报 `D3D12 ERROR: ID3D12CommandList::ResourceBarrier: Before state must be...`

**原因**：对某个资源发出了错误的状态转换（当前状态与声明的"转换前"状态不符）

**解决**：仔细追踪每个资源在每个操作前后的状态。推荐画一张表格：

```
资源            | 操作前    | 操作后    | 操作
----------------|-----------|-----------|------
hdrRT           | RT        | PSR       | 进入后处理
bloomExtractRT  | RT        | PSR       | Bloom -> ToneMap
shadowMap       | DEPTH_WRITE | PSR     | 阴影图采样
```

### 14.2 根签名冲突（本项目曾遇到的 Bug）

**症状**：画面颜色异常，GPU 验证错误

**原因**：FloatingObject 设置了自己的 2 参数根签名，海洋渲染随后用 4 参数根签名的 slot[3] 绑定 ShadowSceneCB，但 GPU 认为只有 2 个 slot，越界读取。

**修复**：
```cpp
// 在 FloatingObject::RenderUnderwater 之后，立即恢复海洋根签名
m_commandList->SetGraphicsRootSignature(m_rootSignature.Get());
```

**教训**：任何子系统设置自己的根签名后，主循环必须重新绑定正确的根签名。

### 14.3 日文注释导致 MSVC 编译失败

**症状**：MSVC 在日文 Windows 环境下报 `C2065: 未声明的标识符`，错误行指向代码中完全无关的变量。

**原因**：日文 Windows 的代码页是 932（Shift-JIS）。UTF-8 编码的日文字符，某些字节序列恰好是有效的 Shift-JIS 多字节字符，导致解析器在注释中间读到了";""*"等特殊符号，破坏了后续代码的解析。

**解决方法**：
1. 在新文件中只使用 ASCII 字符（包括注释）
2. 或者给文件添加 UTF-8 BOM（`0xEF 0xBB 0xBF`）
3. 或者在项目设置中添加 `/utf-8` 编译选项

### 14.4 并行编译 PDB 冲突

**症状**：`C1041: 无法打开程序数据库 vc143.pdb`

**原因**：MSBuild 并行编译多个 .cpp 文件时，多个 cl.exe 同时写入同一个 .pdb 文件。

**解决**：在 vcxproj 的 ClCompile 节点添加 `/FS` 标志（File Synchronization）：
```xml
<AdditionalOptions>/FS %(AdditionalOptions)</AdditionalOptions>
```

### 14.5 Windows Defender 锁文件

**症状**：写入新文件后，立即被其他操作（编译、重命名）锁住。

**原因**：Defender 实时防护在新文件创建时扫描。大型二进制密集代码文件（如图形代码）特别容易触发深度扫描。

**解决**：将项目目录加入 Defender 排除列表：
```powershell
# 以管理员身份运行
Add-MpPreference -ExclusionPath "E:\Study\HelloDX12"
```

### 14.6 CBV 必须 256 字节对齐

DX12 的常量缓冲区（CBV）必须是 256 字节的整数倍大小：

```cpp
// 错误：sizeof(MyCB) = 68 字节，无效
struct MyCB { float4 data[4]; float extra; };  // 68 bytes

// 正确：手动补齐到 256 字节
struct alignas(256) MyCB {
    float4 data[4];  // 64 bytes
    float  extra;    //  4 bytes
    float  pad[47];  // 188 bytes → 总计 256 bytes
};
static_assert(sizeof(MyCB) == 256);
```

---

## 15. 扩展方向建议

学完本项目后，可以尝试以下扩展，从简单到复杂排列：

### 初级扩展

1. **调整 Gerstner 波参数**：修改 `SceneCB.waves` 中的振幅、频率、方向，观察海浪变化
2. **增加天气状态**：在 `WeatherSystem` 中添加"薄雾"状态，修改能见度参数
3. **修改鱼的颜色和大小**：在 `FishSchool::SpawnSchool` 中调整颜色范围和 `S` 缩放系数

### 中级扩展

4. **增加更多浮动物体类型**：参考 `FloatingObject`，实现木桶、救生圈等不同形状
5. **添加海鸥粒子**：类似鱼群，在水面以上用 Boids 模拟海鸟飞翔
6. **实现波浪泡沫**：在波峰（Jacobian 行列式接近 0 的位置）叠加白色泡沫纹理
7. **改进 SSAO**：增加采样数量（8→16），添加时间性累积降噪

### 高级扩展

8. **GPU Boids**：把鱼群模拟从 CPU 移到计算着色器，支持更多鱼（10000+）
9. **体积云**：用 Ray Marching 替代当前的 FBM 平面云，实现真实立体云层
10. **波浪粒子交互**：检测浮箱与水面的接触，在周围生成扰动波纹
11. **多光源阴影**：添加月光阴影（较暗），实现日月双光源系统
12. **屏幕空间水面折射**：基于法线图扭曲折射 UV，比当前的快照方法更精确

---

## 附录：关键数学公式

### Phillips 谱

$$P(k) = A \cdot \frac{e^{-1/(k^2 L^2)}}{k^4} \cdot |\hat{k} \cdot \hat{w}|^2$$

- $A$：振幅系数
- $L = V^2/g$：风力特征长度
- $\hat{k}$：波传播方向，$\hat{w}$：风向

### 色散关系

$$\omega(k) = \sqrt{g \cdot |k|}$$

不同频率的波以不同速度传播（频散），是海浪形态复杂多变的物理原因。

### Beer-Lambert 定律

$$I(d) = I_0 \cdot e^{-\mu d}$$

- $I_0$：初始光强
- $\mu$：吸收系数（RGB 各不同）
- $d$：传播深度

### Fresnel 近似（Schlick）

$$F(\theta) = F_0 + (1-F_0)(1-\cos\theta)^5$$

- $F_0$：法线入射时的反射率（水面约 0.02）
- $\theta$：视线与法线的夹角

---

*本教程对应项目版本：commit cd67baa*  
*作者：Claude Sonnet 4.6 / 2026-05-06*
