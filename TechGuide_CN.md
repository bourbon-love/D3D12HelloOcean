# DirectX 12 海洋渲染引擎 — 技术原理详解

> 本文档面向希望深入理解项目设计的读者，逐一解释每个系统的工作原理。
> 阅读建议：先读第 1-2 章了解整体架构，再根据兴趣选读具体功能章节。

---

## 目录

1. [项目整体架构](#1-项目整体架构)
2. [DirectX 12 核心概念](#2-directx-12-核心概念)
3. [FFT 海洋物理模拟](#3-fft-海洋物理模拟)
4. [海洋表面渲染](#4-海洋表面渲染)
5. [HDR 渲染管线](#5-hdr-渲染管线)
6. [Bloom 泛光效果](#6-bloom-泛光效果)
7. [体积光（God Rays）](#7-体积光god-rays)
8. [屏幕空间反射（SSR）](#8-屏幕空间反射ssr)
9. [景深效果（DOF）](#9-景深效果dof)
10. [色调映射（ACES）](#10-色调映射aces)
11. [镜头光晕（Lens Flare）](#11-镜头光晕lens-flare)
12. [自动曝光与后期滤镜](#12-自动曝光与后期滤镜)
13. [天空系统](#13-天空系统)
14. [天气系统](#14-天气系统)
15. [雨水与水面涟漪](#15-雨水与水面涟漪)
16. [帧循环与渲染顺序](#16-帧循环与渲染顺序)

---

## 1. 项目整体架构

### 1.1 为什么选 DirectX 12？

DirectX 12 与 DX11 最大的区别在于**显式控制**。DX11 的驱动层会自动做很多事情（同步、资源状态管理、内存分配），开发者感觉方便，但无法精确掌控 GPU 的行为。DX12 把这些控制权完全交给开发者：

- 你必须**手动告诉 GPU** 每个资源从什么状态变成什么状态（资源屏障）
- 你必须**手动管理**描述符堆和内存
- 你必须**手动处理**CPU/GPU 同步（围栏/Fence）

这样做的代价是代码更复杂，但收益是对 GPU 行为有完全的掌控力，可以做出更激进的优化。本项目用 DX12 的目的就是学习和实践这种低层次的控制。

### 1.2 子系统组织

```
主类 D3D12HelloTriangle
│
├── Renderer        —— 管理海洋网格的绘制和场景常量缓冲区
├── OceanFFT        —— 纯计算子系统，每帧在 GPU 上跑 FFT
├── SkyDome         —— 天空球渲染和大气参数
├── WeatherSystem   —— 天气状态机，向其他子系统广播参数
├── RainSystem      —— 雨粒子和水面涟漪
└── Camera          —— 视图矩阵和输入处理
```

**数据流向**（单向，避免循环依赖）：

```
WeatherSystem
    ↓ 推送风速/云量/雨量参数
    ↓
OceanFFT ← 风速影响海浪高度
SkyDome  ← 云量影响天空外观
RainSystem ← 雨量影响粒子密度
```

### 1.3 帧的生命周期

每一帧分为两个阶段：

**OnUpdate（CPU 阶段）：**
- 推进物理时间，更新所有子系统的 CPU 侧状态
- 向各子系统写入常量缓冲区数据（矩阵、颜色、参数等）
- 构建 ImGui UI 数据（但不提交到 GPU）

**OnRender（GPU 记录+提交阶段）：**
- 重置命令分配器，开始记录命令
- 按照固定顺序记录所有渲染指令（不是立即执行，只是录制）
- 关闭命令列表，一次性提交给 GPU 执行
- 调用 Present 显示结果，等待上一帧完成（Fence 同步）

---

## 2. DirectX 12 核心概念

理解本项目必须先理解以下 DX12 概念。

### 2.1 资源屏障（Resource Barrier）

这是 DX12 最重要的概念之一。GPU 内部有不同的硬件单元：渲染单元（RTV）、着色器读取单元（SRV）、计算单元（UAV）等。一个纹理资源同一时刻只能处于一种"状态"，对应一种用途。

当你想换一种方式使用同一个纹理时，必须显式地告诉 GPU：

```cpp
// 例子：把 HDR 渲染目标从"可写入"状态切换到"着色器可读"状态
auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
    m_hdrRT.Get(),
    D3D12_RESOURCE_STATE_RENDER_TARGET,    // 之前的状态
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE  // 目标状态
);
m_commandList->ResourceBarrier(1, &barrier);
```

**为什么需要这个？**  
因为 GPU 是高度并行的流水线架构。如果不加屏障，渲染单元可能还没写完，着色器单元就开始读了，导致读取到错误数据（竞争条件）。屏障的作用就是在两者之间插入一个同步点。

**本项目中的状态流转（以 HDR RT 为例）：**
```
每帧开始: RENDER_TARGET（天空、海洋向它写入）
   ↓ 天空渲染完毕，准备快照
COPY_SOURCE → CopyResource 到 SkySnapshot → RENDER_TARGET（继续让海洋写入）
   ↓ 所有场景渲染完毕
PIXEL_SHADER_RESOURCE（Bloom 读取它）
   ↓ DOF 也读取它
PIXEL_SHADER_RESOURCE
   ↓ ToneMap 读取完毕，清理
RENDER_TARGET（为下一帧做好准备）
```

### 2.2 描述符堆（Descriptor Heap）

GPU 不直接通过指针访问纹理，而是通过"描述符"——一个告诉 GPU"这个纹理在哪里、格式是什么、怎么读取"的数据结构。描述符存放在描述符堆里。

描述符堆有一个**关键限制**：在绘制调用时，只能有**一个** CBV/SRV/UAV 堆绑定到管线上。

这意味着，如果你的着色器需要访问两个来自不同堆的纹理，你必须把它们放到同一个堆里。

**本项目中的解决方案：**

```
m_oceanSRVHeap（3个槽）：
  槽0: FFT heightMap（SRV）
  槽1: FFT dztMap（SRV）
  槽2: SkySnapshot（SRV）
→ 海洋着色器只需绑定这一个堆，就能访问所有需要的纹理

m_bloomSRVHeap（4个槽）：
  槽0: HDR RT（SRV）
  槽1: Bloom ExtractRT（SRV）
  槽2: Bloom BlurRT（SRV）
  槽3: GodRay RT（SRV）
→ ToneMap 着色器绑定这个堆
```

同一个物理纹理资源可以在多个堆里各有一个描述符，这是合法的。所以 HDR RT 既出现在 bloomSRVHeap 里，也出现在 dofSRVHeap 里。

### 2.3 根签名（Root Signature）

根签名是着色器和 CPU 之间的"合同"：声明着色器需要什么类型的输入（常量缓冲区在哪里、SRV 表在哪里）。

```cpp
// 例子：Bloom 的根签名
// 参数0: 一个 SRV 描述符表（对应着色器里的 t0）
// 参数1: 4个根常量（对应着色器里的 b0 cbuffer）
CD3DX12_ROOT_PARAMETER params[2];
params[0].InitAsDescriptorTable(1, &srvRange, D3D12_SHADER_VISIBILITY_PIXEL);
params[1].InitAsConstants(4, 0);  // 4个float，寄存器b0
```

每个渲染通道都有自己独立的根签名，这样可以精确地只绑定该通道需要的资源，避免浪费。

### 2.4 流水线状态对象（PSO）

PSO 包含了渲染管线的全部状态：使用哪个着色器、如何进行光栅化、是否开启深度测试、混合模式是什么等。

创建 PSO 很慢（需要编译着色器、初始化驱动状态），所以必须在启动时全部创建完毕，运行时只是切换使用不同的 PSO。

```cpp
// 例如镜头光晕的 PSO 使用加法混合（而不是普通的覆盖混合）
blendDesc.RenderTarget[0].SrcBlend  = D3D12_BLEND_ONE;
blendDesc.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;   // src + dest（加法）
blendDesc.RenderTarget[0].BlendOp   = D3D12_BLEND_OP_ADD;
```

### 2.5 Fence 同步

CPU 记录命令、提交命令、然后等待 GPU 执行完毕——这三步之间需要同步。Fence 是一个计数器，GPU 执行完一批命令后会把计数器推进，CPU 可以等待特定的计数值。

```
每帧：
  CPU 记录命令 → 提交命令 → Signal(fence, value+1) → Present
  → WaitForPreviousFrame:
    if GPU 还没到 value → CPU 在此等待
  → CPU 开始记录下一帧
```

本项目采用最简单的"等待上一帧完成"策略（没有多帧飞行），确保 CPU 和 GPU 不会同时访问同一资源。

---

## 3. FFT 海洋物理模拟

这是本项目技术含量最高的部分，完全在 GPU 上运行。

### 3.1 海洋模拟的数学基础

真实的海浪是无数小波的叠加。在频率域（波数域）中，每个波用一个复数 $\hat{h}(k)$ 表示，k 是波的方向和频率。海洋表面的高度是这些频率分量的叠加，正好可以用**逆傅里叶变换（IFFT）**来计算。

整个流程：
1. **初始化阶段**：用 Phillips 谱定义各个频率分量的初始振幅（基于风速、风向）
2. **每帧**：让每个频率分量按物理规律演化（不同频率的波跑得快慢不同）
3. **每帧**：做一次 IFFT，把频率域数据转换回空间域，得到实际的高度/位移

### 3.2 Phillips 谱

Phillips 谱描述了在特定风况下，各个方向和频率的波浪各有多大能量。公式：

$$P(k) = A \cdot \frac{e^{-1/(kL)^2}}{k^4} \cdot |\hat{k} \cdot \hat{w}|^2$$

- $k$ = 波数（频率）
- $L = v^2/g$ = 由风速 $v$ 和重力加速度 $g$ 决定的最大波长
- $\hat{w}$ = 风向（单位向量）
- $A$ = 振幅系数（可调）

**直觉理解**：
- 与风向一致的波（$\hat{k} \cdot \hat{w}$ 接近1）能量最大
- 太短的波（k 很大）能量被抑制（$e^{-1/(kL)^2}$）
- 中等频率的波获得最多能量

在代码中（PhillipsCS.hlsl），每个线程负责计算一个 $k$ 值对应的频谱分量，写入 256×256 的复数纹理 `m_h0Map`。

### 3.3 时间演化

每帧，我们让每个频率分量随时间演化。根据深水波的**色散关系**：

$$\omega(k) = \sqrt{g \cdot |k|}$$

（波速和频率的平方根成正比，所以高频波传播更快。这也是为什么远处的海浪总是比近处的更细碎。）

对初始频谱做相位旋转：

$$\hat{h}(k, t) = \hat{h}_0(k) \cdot e^{i\omega(k)t} + \hat{h}_0^*(-k) \cdot e^{-i\omega(k)t}$$

这步在 TimeEvolutionCS.hlsl 里完成，每帧都在 GPU 上跑，输出结果写入 `m_hktMap`。

### 3.4 逆 FFT（IFFT）

IFFT 把频率域的复数数组转换成空间域的实数高度场。算法使用经典的 **Cooley-Tukey Radix-2**：

```
输入: 256个复数 [H(0), H(1), ..., H(255)]（频率域）
输出: 256个实数 [h(0), h(1), ..., h(255)]（高度值）

过程（蝴蝶运算）：
  第1轮: 把256个元素分成128对，每对做一次加减
  第2轮: 把结果分成64组，每组4个元素
  ...
  第8轮: 最终结果（log2(256) = 8 轮）
```

2D IFFT 就是先对所有行做 1D IFFT，再对所有列做 1D IFFT。使用**ピンポンバッファ**（乒乓缓冲）：水平方向 IFFT 的输出作为垂直方向的输入，通过交替读写 `m_heightMap` 和 `m_tempMap` 实现。

最终 `m_heightMap` 的每个像素：
- `.x` = 该点的垂直高度
- `.z` = 该点的水平位移 Dx（Gerstner 效果，让波峰前倾）

`m_dztMap` 的 `.x` = Dz 方向的水平位移。

### 3.5 为什么在 GPU 上做 FFT？

256×256 的 FFT 包含约 40 万次复数乘加运算。CPU 上做需要几毫秒，而 GPU 并行处理只需要几十微秒（100 倍以上的加速）。

---

## 4. 海洋表面渲染

### 4.1 网格结构

海洋是一个 512×512 的平坦网格，世界空间尺寸 400×400 单位。每个顶点在 CPU 侧固定不动，真正的波浪形变全部在**顶点着色器**（GPU 上）完成。

### 4.2 Gerstner 波

FFT 给了我们海浪的整体形状，但 Gerstner 波提供了额外的细节：波浪的**水平位移**。

真实的水面波浪，水体不仅上下运动，还会水平循环运动（椭圆轨迹）。Gerstner 波模拟了这个：

```
对于每个波（方向 d，振幅 A，波长 λ，速度 v，陡度 Q）：
  k = 2π/λ
  f = k*(d·xz) - v*time   （相位）

  水平位移: Δx = Q * A * d.x * cos(f)
  垂直位移: Δy = A * sin(f)
  Δz = Q * A * d.z * cos(f)
```

陡度 Q 控制水平位移的强度。Q=0 是普通的正弦波，Q=1 是极度前倾的波（接近翻卷）。本项目叠加了 4 个不同方向的 Gerstner 波。

### 4.3 法线计算

正确的光照需要知道每个点的法线方向。法线通过**切线向量的叉积**计算：

在顶点着色器中，每次计算 Gerstner 位移的同时，也会计算出该波对切线向量的贡献：

```
切线X方向的变化：
  tangentX.x += 1 - Q*dir.x²*k*A*sin(f)
  tangentX.y += dir.x*k*A*cos(f)
  ...

法线 = normalize(cross(tangentZ, tangentX))
```

在像素着色器中，还会从 FFT 高度图的相邻像素采样，用有限差分法重新计算更精确的法线（考虑了 FFT 的贡献）：

```hlsl
float hL = heightMap.Sample(uv + float2(-dx, 0)).x;
float hR = heightMap.Sample(uv + float2( dx, 0)).x;
float dHdx = (hR - hL) / (2 * worldPerTexel);
// 同理计算 dHdz...
normal = normalize(cross(tangentZ, tangentX));
```

### 4.4 雅可比泡沫

这是区分"游戏感"海洋和"真实感"海洋的关键细节。

**原理**：当波浪足够陡峭时，浪尖会"翻卷"——在数学上表现为位移场的**雅可比行列式** J < 1：

$$J = \left(1 + \frac{\partial D_x}{\partial x}\right)\left(1 + \frac{\partial D_z}{\partial z}\right) - \frac{\partial D_x}{\partial z} \cdot \frac{\partial D_z}{\partial x}$$

- J = 1：无变形，正常海面
- J < 1：波浪开始压缩折叠（波峰开始翻卷）
- J < 0：严格来说是数学翻转（在物理上不可能，说明模拟过强了）

泡沫强度 = `saturate(1 - J)`，即 J 越小，泡沫越多。

**噪声细化**：纯 J 值产生的泡沫是锋利的边缘。用两层 Value Noise 让泡沫边界变成不规则的有机形状：

```hlsl
float2 foamUV = pin.uv * 60.0 + drift; // 随时间漂移
float n1 = valueNoise(foamUV);
float n2 = valueNoise(foamUV * 2.8 + offset);
float noiseBlend = n1 * 0.65 + n2 * 0.35;
float foamMask = saturate(rawFoam * (0.4 + noiseBlend * 1.2));
```

**天气联动**：暴风天 foamIntensity 接近 1，泡沫更浓更广；平静天 foamIntensity 约 0.15，只有最高的浪尖有泡沫。

### 4.5 光照模型

采用简化的 Phong 光照 + Fresnel 反射：

```hlsl
// 漫反射：受法线和光方向的夹角控制
float NdotL = saturate(dot(N, sunDir));
float3 diffuse = waterColor * (NdotL * 0.5 + 0.5) * sunColor;

// 镜面反射：Blinn-Phong，指数128（非常光亮的水面）
float NdotH = saturate(dot(N, H));
float specular = pow(NdotH, 128.0) * sunIntensity * 15.0; // ×15 让它在 HDR 管线中突出

// Fresnel：掠射角时反射率增加
float fresnel = F0 + (1-F0) * pow(1 - NdotV, 5.0); // Schlick 近似
float3 reflectColor = SampleSkyReflection(reflectDir) * fresnel * 2.0;
```

镜面反射乘以 15 是刻意的 HDR 过曝——这样太阳反射在水面的高光会触发 Bloom 效果，产生真实的闪光感。

---

## 5. HDR 渲染管线

### 5.1 为什么用 HDR？

普通显示器只能显示 0-255 的亮度（LDR）。但真实世界的光照范围远超这个：

- 正午阳光直射：约 100,000 勒克斯
- 室内：约 400 勒克斯
- 月光：约 0.1 勒克斯

如果我们在 LDR（0-1 范围）中渲染，所有"太亮"的地方都会被截断为纯白，失去细节。

**HDR 管线的思路**：
1. 用 16 位浮点格式（R16G16B16A16_FLOAT）渲染，值可以超过 1.0
2. 让太阳光、闪电等高亮物体输出 > 1.0 的值
3. 最后用**色调映射**把整个高动态范围压缩到显示器能显示的 0-1 范围

### 5.2 为什么 R16G16B16A16_FLOAT？

每个通道 16 位，能表示约 ±65504 的范围，精度约 0.001（在 0-1 范围内）。相比 R8G8B8A8_UNORM（8位整数，精度约 0.004），16 位浮点的精度更高，且能存储超出 1.0 的值。

代价是内存占用翻倍（每像素 8 字节 vs 4 字节）。对 1920×1080 的画面，每个 RT 约 16MB。

---

## 6. Bloom 泛光效果

### 6.1 原理

真实相机的镜头会把极亮的光源"晕开"。Bloom 就是模拟这个效果：

1. **提取高亮部分**：只保留亮度超过阈值的像素
2. **模糊高亮区域**：用高斯模糊让高亮区域"扩散"开
3. **叠加到原图**：把模糊后的高亮叠加到原始画面上

### 6.2 亮度提取

```hlsl
// BrightPassPS：只输出超过阈值的部分
float lum = dot(color.rgb, float3(0.2126, 0.7152, 0.0722));
float bright = saturate((lum - threshold) / max(threshold, 0.001));
output = color * bright;
```

阈值（默认 1.0）意味着只有 HDR 中超过 1.0 的像素会进入 Bloom，这正是我们把太阳反射设置为 15.0 的原因——确保它一定触发 Bloom。

### 6.3 高斯模糊

高斯模糊用正态分布权重对周围像素求加权平均，产生自然的"扩散"效果。

**分离实现**：2D 高斯可以分解为两次 1D 高斯（先水平，再垂直）。这将计算量从 O(n²) 降到 O(2n)。

```hlsl
// BlurPS：9个采样点，sigma ≈ 1.5
// 水平方向：偏移 (offset, 0)
// 垂直方向：偏移 (0, offset)
// 权重：[0.0625, 0.125, 0.25, 0.125, ...]（正态分布）
```

**两次迭代**：做两轮水平+垂直模糊，让扩散范围更大、过渡更平滑，而不用采样更多点。

### 6.4 最终合成

在色调映射着色器中：

```hlsl
float3 ldr = ACESFilmic(hdr * exposure + bloom * bloomStrength + ...);
```

Bloom 在色调映射**之前**叠加到 HDR 场景上，这样 Bloom 本身也经过了 ACES 压缩，不会产生不自然的过曝。

---

## 7. 体积光（God Rays）

### 7.1 原理

体积光（God Rays，光轴）是光线穿过介质（云、雾、尘埃）时散射形成的放射状光条。实现方式有很多，本项目用最高效的**屏幕空间径向模糊**。

### 7.2 径向模糊算法

核心思路：对每个像素，沿着"当前像素→太阳位置"方向，在路径上采样多个点，累加亮度。越靠近太阳的采样权重越高（因为离光源近）。

```hlsl
float2 delta = (uv - sunScreenPos) * (density / NUM_SAMPLES);
float2 sampleUV = uv;
float decay = 1.0;

for (int i = 0; i < 64; i++) {
    sampleUV -= delta;  // 每次向太阳方向移动一步
    float3 s = sceneColor.SampleLevel(sampler, saturate(sampleUV), 0).rgb;

    // 只让亮的天空区域贡献（过滤掉暗色的海洋）
    float lum = dot(s, float3(0.2126, 0.7152, 0.0722));
    s *= saturate((lum - 0.4) / 0.6);  // 亮度阈值

    color += s * decay * weight;
    decay *= decayFactor;  // 每步衰减（离太阳越近的采样权重越低...不对，是越远的步越少）
}
```

**视觉效果**：从屏幕上任意一点向太阳方向"望"过去，把路径上收集到的光叠加起来，就形成了由太阳放射出来的光条感。

### 7.3 太阳可见性计算

只有当太阳在屏幕内、且在地平线以上时，God Rays 才有意义：

```cpp
// 把太阳方向投影到屏幕
XMVECTOR clip = XMVector4Transform(sunWorld, viewProjMatrix);
float sunScreenX = x / w * 0.5 + 0.5;
float sunScreenY = -y / w * 0.5 + 0.5;

// 太阳高度淡出（低于地平线就没有 God Rays）
float sunVis = clamp(sunDir.y * 3.0 + 0.3, 0.0, 1.0);

// 太阳离屏幕边缘越近，效果越弱
float offScreen = max(abs(sunScreenX-0.5), abs(sunScreenY-0.5));
sunVis *= clamp(1.0 - (offScreen - 0.5) * 3.0, 0.0, 1.0);
```

### 7.4 为什么用半分辨率？

God Ray RT 只有主渲染分辨率的一半（960×540 for 1080p）。这样：
- 内存占用减少 75%
- 64 次采样的计算量减少 75%
- 视觉上几乎没有区别（God Rays 本身就是模糊的低频效果）

---

## 8. 屏幕空间反射（SSR）

### 8.1 原理

传统的水面反射用"反射相机"（把相机关于水平面做镜像，重新渲染一遍场景）。这很精确但性能开销大。

SSR 是一种廉价的近似：**只反射当前帧已经渲染在屏幕上的内容**。

### 8.2 天空快照方案

本项目的 SSR 专门为海洋反射天空设计：

**问题**：如果直接读取已渲染好的场景，海洋像素会把自身的颜色（蓝色海水）作为反射来源，而不是天空。

**解决**：在天空渲染完毕、海洋渲染开始之前，拍一张"天空快照"：

```cpp
// 天空渲染到 hdrRT 后
ResourceBarrier: hdrRT  RT → COPY_SOURCE
ResourceBarrier: skySnapshot  COPY_DEST（初始状态）
CopyResource(skySnapshot, hdrRT);   // 拷贝纯天空画面
ResourceBarrier: hdrRT  COPY_SOURCE → RT（继续让海洋写入）
ResourceBarrier: skySnapshot  COPY_DEST → PSR（供海洋着色器读取）
```

### 8.3 反射 UV 计算

对于海洋表面的每个像素，我们知道它的世界坐标（posW）和法线（N）。反射方向：

```hlsl
float3 reflectDir = reflect(-V, N);  // V 是指向摄像机的方向
```

把反射方向上的一个远处点投影到屏幕坐标：

```hlsl
float3 reflPt = posW + reflectDir * 300.0;  // 沿反射方向走 300 单位
float4 reflClip = mul(float4(reflPt, 1.0), viewProj);  // 投影到裁剪空间
float2 reflUV = reflClip.xy / reflClip.w * float2(0.5, -0.5) + 0.5;  // 转 UV
```

然后采样天空快照：

```hlsl
float3 ssrColor = skySnapshot.SampleLevel(sampler, saturate(reflUV), 0).rgb;
```

### 8.4 边缘淡出

当反射方向指向屏幕外时，屏幕空间 SSR 就失效了（快照里没有那部分内容）。用程序化天空函数兜底：

```hlsl
// 边缘淡出
float2 edgeFade = saturate(min(reflUV, 1.0 - reflUV) * 6.0);
float fade = min(edgeFade.x, edgeFade.y) * ssrMix;
fade *= saturate(reflectDir.y * 4.0 + 0.3);  // 向下的反射也淡出

float3 procFallback = SampleSkyReflection(reflectDir);  // 程序化天空
reflectColor = lerp(procFallback, ssrColor, fade) * fresnel * 2.0;
```

---

## 9. 景深效果（DOF）

### 9.1 原理

真实相机有焦平面——在焦平面上的物体清晰，离焦平面越远的物体越模糊（虚化）。这种模糊的圆形光晕叫做**焦外成像（Bokeh）**。

景深的核心参数：
- **焦点深度（Focus Depth）**：焦平面在 NDC 深度空间的位置（0=近，1=远）
- **焦点范围（Focus Range）**：焦平面的清晰区域宽度
- **最大模糊半径（Max Radius）**：焦外最大模糊程度（以 UV 为单位）

### 9.2 深度缓冲 SRV 化

要实现景深，需要在着色器中读取深度缓冲区。但 DX12 中，深度缓冲的格式 `D32_FLOAT` 默认不能创建 SRV（着色器资源视图）。

解决方法：使用**无类型格式（Typeless Format）**：

```cpp
// 资源本身：无类型（可以解释为多种格式）
desc.Format = DXGI_FORMAT_R32_TYPELESS;

// 创建 DSV（深度测试用）：解释为 D32_FLOAT
dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;

// 创建 SRV（着色器读取用）：解释为 R32_FLOAT
srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
```

同一块 GPU 内存，既能用于深度测试，也能被着色器读取，只是解释方式不同。

### 9.3 Vogel 圆盘采样

对于每个像素，根据其深度计算**弥散圆（CoC，Circle of Confusion）**大小：

```hlsl
float coc = saturate(abs(depth - focusDepth) / focusRange) * maxRadius;
```

然后在 coc 大小的圆盘范围内采样 20 个点。采样点的分布用 **Vogel 黄金角螺旋**，确保均匀分布：

```hlsl
// 黄金角约 2.4 弧度 ≈ 137.5°
// 螺旋分布：内圈少、外圈多，整体均匀
for (int k = 0; k < 20; k++) {
    float angle = k * 2.3999632;           // 黄金角
    float r = sqrt((k + 0.5) / 20.0);      // 半径随 k 增大
    float2 offset = float2(cos(angle), sin(angle)) * r * coc;
    offset.x /= aspectRatio;               // 纠正非正方形屏幕
    color += scene.SampleLevel(sampler, saturate(uv + offset), 0).rgb;
}
```

**为什么用黄金角？** 黄金角（137.5°）是数论中最"无理"的角度，用它生成的螺旋点分布最均匀，避免采样点聚集或出现规律性图案。

### 9.4 过渡混合

完全在焦点处的像素不需要模糊，直接用原始颜色。模糊强度随 CoC 大小线性增加：

```hlsl
float3 center = scene.SampleLevel(sampler, uv, 0).rgb;  // 原始清晰像素
float blend = saturate(coc / (maxRadius * 0.5));
return float4(lerp(center, blurredColor, blend), 1.0);
```

---

## 10. 色调映射（ACES）

### 10.1 为什么需要色调映射？

HDR 渲染产生的值可以从 0 到 10 甚至更高，但显示器只能显示 0-1。色调映射就是把这个大范围"合理地"压缩到 0-1，同时尽量保留视觉细节。

### 10.2 ACES Filmic 曲线

ACES（Academy Color Encoding System）是好莱坞电影工业开发的标准色彩空间。ACES Filmic 曲线有这些特性：

- **暗部压缩少**：低亮度区域几乎线性，保留细节
- **亮部软压缩（Shoulder）**：高亮区域渐渐趋向 1.0，而不是硬截断
- **胶片感**：比线性映射更有"电影感"的对比度

公式（近似版）：

```hlsl
float3 ACESFilmic(float3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return saturate((x*(a*x+b)) / (x*(c*x+d)+e));
}
```

这是一个有理函数（分子/分母都是关于 x 的二次多项式），形状类似 S 曲线：

```
输出
1.0 |          ___________
    |       __/
    |    __/
    |  _/
0.0 |_/__________________ 输入
    0.0         1.0   2.0+
```

### 10.3 曝光控制

在送入 ACES 之前，先乘以曝光值：

```hlsl
float3 ldr = ACESFilmic(hdr * exposure + bloom * bloomStrength + godray * godRayStrength);
```

曝光 = 1.0 是正常，> 1.0 让画面变亮（例如夜晚调高曝光看清暗处），< 1.0 让画面变暗。

### 10.4 暗角（Vignette）

物理相机的镜头边缘会比中心暗（光学渐晕）。用数学来模拟：

```hlsl
float2 centered = uv - 0.5;        // 以画面中心为原点
float vigDist = dot(centered, centered);  // 距中心距离²（最大值 0.5）
// smoothstep 产生平滑的从中心亮到边缘暗的过渡
float vignette = 1.0 - smoothstep(0.10, 0.65, vigDist) * vignetteStrength;
ldr *= vignette;
```

### 10.5 胶片颗粒（Film Grain）

真实电影胶片有随机噪点，给画面增加"有机感"。用动画哈希噪声模拟：

```hlsl
// 随时间变化的随机噪声（用 floor 让它是像素级而不是亚像素级）
float2 gUV = floor(uv * 800.0 + float2(time*37, time*53));
float grain = frac(sin(dot(gUV, float2(127.1, 311.7))) * 43758.5453);
grain = (grain - 0.5) * 2.0;   // 范围 [-1, 1]

// 在暗部噪点更明显（人眼对暗部噪点更敏感）
float lum = dot(ldr, float3(0.2126, 0.7152, 0.0722));
ldr += grain * grainStrength * (1.0 - lum * 0.6);
```

---

## 11. 镜头光晕（Lens Flare）

### 11.1 原理

镜头光晕是光线在相机镜头内部多次反射和折射形成的。它包含几个元素：

1. **光鬼（Ghosts）**：太阳的虚像，沿"太阳→画面中心"轴线排列
2. **变形条纹（Anamorphic Streak）**：水平方向的蓝色光条（模仿变形镜头的特性）
3. **光晕（Halo）**：太阳位置的柔和圆形光晕

### 11.2 光鬼的位置

每个光鬼都位于太阳通过画面中心的反射轴上：

```hlsl
float2 axis = float2(0.5, 0.5) - sunScreenPos;  // 从太阳指向画面中心的方向
float2 ghostPos = sunScreenPos + axis * scale;   // scale 是每个光鬼的比例系数
```

scale 为 0 时在太阳位置，scale 为 1 时在画面中心，scale 为 2 时在太阳关于中心的对称位置。

### 11.3 圆形光鬼渲染

每个光鬼是一个软边圆，用到当前像素到光鬼中心距离的距离函数：

```hlsl
float2 d = (uv - ghostPos) * float2(aspectRatio, 1.0);  // 纠正宽高比
float dist = length(d) / radius;
float glow = saturate(1.0 - dist);
glow = glow * glow * glow;  // 三次方使边缘更平滑
color += ghostColor * glow * 0.35;
```

### 11.4 完全手续化——无需纹理

整个镜头光晕不用任何纹理，全部由数学公式生成。这样做的优点：
- 零内存占用
- 可以动态改变参数
- 根据太阳位置实时计算，总是正确对齐

---

## 12. 自动曝光与后期滤镜

### 12.1 自动曝光原理

真实相机（和人眼）会根据环境亮度自动调节曝光：白天明亮，自动降低曝光；夜晚黑暗，自动提高曝光。

本项目的自动曝光是基于太阳高度的 **CPU 侧估算**：

```
目标曝光值：
  白天（sunH > 0.25）：1.0（正常曝光）
  日落（sunH 0 → 0.25）：1.0 → 1.6（天空暗了，提高曝光）
  黄昏（sunH -0.15 → 0）：1.6 → 2.2
  夜晚（sunH < -0.15）：2.5（让星光和月光可见）
  暴风：+0.4（乌云遮天，场景偏暗）
```

然后让实际曝光值**平滑地趋向**目标值，模拟眼睛适应光线的过程：

```cpp
// 不对称速度：明亮环境→快速适应（1.0/秒），黑暗环境→慢慢适应（0.35/秒）
float speed = (target < m_exposure) ? 1.0f : 0.35f;
m_exposure += (target - m_exposure) * speed * deltaTime;
```

为什么不对称？人眼从黑暗走到明亮需要几秒（瞳孔收缩快），从明亮走到黑暗需要几分钟（暗适应慢）。这里用 1.0 vs 0.35 做了简化模拟。

### 12.2 更专业的做法（未实现）

本项目用太阳高度代替实际测量亮度。更完整的实现是：
1. 每帧计算 HDR 场景的平均亮度（可用 compute shader + 降采样）
2. 根据平均亮度计算 EV（曝光值）
3. 平滑调整 exposure

---

## 13. 天空系统

### 13.1 天空球结构

天空球是一个以摄像机为中心的大球，从内部渲染（所以要剔除正面，保留背面）。渲染时把深度值强制设为 w（即最远处），确保天空总是在所有物体的后面。

### 13.2 日周运动

太阳位置按时间旋转：

```cpp
float sunAngle = m_time * 0.3f;  // 0.3 rad/s 控制日夜循环速度
m_sunDir.x = cos(sunAngle);
m_sunDir.y = sin(sunAngle) * tilt;  // tilt 让轨道倾斜，否则太阳会从头顶垂直穿过
m_sunDir.z = sin(sunAngle) * sqrt(1-tilt²);
```

月亮是太阳的对面（y 分量略有偏移，保证月亮在地平线附近的平滑过渡）。

### 13.3 动态天空渐变

天空颜色由三套调色板（夜晚/日落/白天）通过太阳高度插值：

```cpp
float dayT    = smoothstep(-0.15, 0.20, sunH);   // 0=夜，1=白天
float sunsetT = clamp(1.0 - abs(sunH)/0.22, 0, 1)²;  // 日落时段峰值

// 颜色 = 基础色（夜晚→白天插值）+ 日落叠加
color = lerp(nightColor, dayColor, dayT);
color = lerp(color, sunsetColor, sunsetT);
```

用 `smoothstep` 使过渡更平滑（而不是线性的生硬切换）。

### 13.4 积云渲染

积云使用组合的噪声技术：

1. **Worley 噪声**：产生"棉花糖"形状的基础云形（内部高、外部低）
2. **FBM（分形布朗运动）**：叠加多个频率的 Perlin 噪声，产生自然的随机细节
3. **三层合成**：用三套不同参数的噪声合成，产生不同尺寸的云

```hlsl
float cloud1 = detailedClouds(pos, scale * 0.6, time);         // 大云
float cloud2 = detailedClouds(pos*1.1 + offset2, scale*0.45);  // 中云
float cloud3 = detailedClouds(pos*1.5 + offset3, scale*0.35);  // 小云
float combined = cloud1*0.5 + cloud2*0.35 + cloud3*0.25;
```

### 13.5 风向联动雲移

WeatherSystem 会设置风向（windDirX, windDirY）。每帧这个值被传入天空着色器：

```hlsl
// 云的采样坐标随时间偏移（方向由风向决定）
float3 movement = float3(cloudDriftX * time, 0.0, cloudDriftY * time);
// cloudDriftX = windDirX * windStrength * 0.08（暴风时3.5倍速）
```

### 13.6 巻雲（Cirrus）

高空的卷云（薄薄的白色丝状）：

```hlsl
// 高空投影：把球面方向投影到高空平面
float2 uv = dir.xz / max(dir.y, 0.15) * 0.6;
uv += float2(windX, windY) * time * 1.5;  // 比积云快 1.5 倍

// 非对称 FBM：X 方向尺度大（拉伸），Y 方向尺度小（细条）
float n = improved_fbm(float3(uv * 4.0, time*0.005), 4)
        + improved_fbm(float3(uv * float3(2.0,0.5,2.0), t), 3) * 0.5;
```

只在 dir.y > 0.15（高于地平线）时渲染，暴风时消失。

### 13.7 稲妻ステートマシン

```
状态：
  正常状态 → 天气强度 > 0.7 且冷却时间 ≤ 0 → 触发闪电
  闪电状态 → intensity 从随机值快速衰减到 0 → 回到正常
  冷却 → 随机 2-8 秒后才能再次触发

效果：
  skyColor += float3(0.85, 0.92, 1.0) * flicker * 3.0;
  （整个天空变成蓝白色，模拟闪电照亮云层）
```

---

## 14. 天气系统

### 14.1 设计目标

天气系统的目标是让视觉效果和物理效果同步变化，而不是突兀地切换。状态机有 3 个状态：平静（Calm）、有风（Windy）、暴风（Storm）。

### 14.2 状态间的平滑插值

每个状态有一套预设参数：

```
Calm:  windSpeed=5,  phillipsA=0.1, cloudDensity=0.4, weatherIntensity=0.0
Windy: windSpeed=15, phillipsA=0.2, cloudDensity=0.6, weatherIntensity=0.5
Storm: windSpeed=30, phillipsA=0.5, cloudDensity=0.9, weatherIntensity=1.0
```

切换时，在指定时间（默认 5 秒）内线性插值：

```cpp
// 插值进度
m_transitionElapsed += deltaTime;
float t = clamp(m_transitionElapsed / m_transitionTime, 0, 1);

// 线性插值所有参数
m_currentParams.windSpeed = lerp(m_fromParams.windSpeed, m_targetParams.windSpeed, t);
// ... 其他参数同理
```

### 14.3 参数广播

每帧更新后，天气参数广播给各子系统：

```cpp
m_ocean->windSpeed = params.windSpeed;    // 影响 FFT 海浪高度
m_ocean->phillipsA = params.phillipsA;   // 影响波谱振幅
m_sky->SetCloudParams(density, scale, sharpness);  // 影响云量
m_sky->SetWeatherIntensity(intensity);   // 影响多个效果
```

---

## 15. 雨水与水面涟漪

### 15.1 雨滴粒子

雨滴是简单的线段粒子（每滴 2 个顶点，用 TriangleStrip 渲染成细线）。

每帧在 CPU 侧更新所有活跃粒子：

```cpp
// 每个粒子向下移动（速度受重力和风向影响）
drop.position.y -= drop.speed * deltaTime;
drop.position.x += windDirX * drop.speed * 0.3f * deltaTime;

// 超出底部就"重生"到上方随机位置
if (drop.position.y < -50.0f) {
    drop.position = randomPosition near camera;
}
```

用动态顶点缓冲区（`D3D12_HEAP_TYPE_UPLOAD`，每帧直接写入 GPU 可读内存）更新所有粒子位置。

### 15.2 水面涟漪

雨滴落水点产生圆形涟漪，通过**常量缓冲区**传给海洋着色器：

```cpp
// 涟漪数据结构（最多 200 个）
struct RippleData {
    XMFLOAT2 position;  // 水面 XZ 坐标
    float radius;       // 当前半径
    float strength;     // 强度（随时间衰减）
};
```

在海洋像素着色器中，遍历所有涟漪，对每个处于涟漪环形区域内的像素扰动法线：

```hlsl
for (uint i = 0; i < rippleCount; ++i) {
    float dist = length(worldPos.xz - ripples[i].position);
    float inRing = saturate(1.0 - abs(dist - ripples[i].radius) / 1.5);

    float2 dir = normalize(worldPos.xz - ripples[i].position);
    float wave = sin((dist - radius) * PI / ringWidth);
    N.xz += dir * inRing * strength * wave * 0.3;
    N = normalize(N);
}
```

---

## 16. 帧循环与渲染顺序

### 16.1 完整的一帧

```
[CPU - OnUpdate]
  1. 计算 deltaTime
  2. 自动曝光更新（根据太阳高度）
  3. SkyDome::Update（太阳/月亮位置、稲妻）
  4. Renderer::Update（相机、写入 SceneCB）
  5. WeatherSystem::Update（推送参数）
  6. RainSystem::Update（粒子物理）
  7. 构建 ImGui 数据

[GPU - OnRender]
  8. 重置命令分配器
  9. OceanFFT::Dispatch（计算：PhillipsCS/TimeEvoCS/IFFTCS）
  10. 资源屏障：FFT UAV → NON_PIXEL_SHADER_RESOURCE
  11. 清空 HDR RT（深蓝色）+ 清空深度缓冲
  12. SkyDome::Render（天空球 → HDR RT）
  13. [SSR] 快照：HDR RT COPY_SOURCE → CopyResource → skySnapshot → PSR
  14. 绑定 oceanSRVHeap（FFT 纹理 + 天空快照）
  15. Renderer::Render（海洋网格 → HDR RT）
  16. RainSystem::Render（雨粒子 + 涟漪法线更新 → HDR RT）
  17. RenderBloom（HDR RT → Extract RT → Blur RT）
  18. RenderGodRays（HDR RT → GodRay RT，半分辨率）
  19. RenderDOF（HDR RT + 深度 → DOF RT）
  20. RenderToneMap（DOF RT + Bloom + GodRay → SwapChain RT）
  21. RenderLensFlare（加法混合 → SwapChain RT）
  22. ImGui 渲染（叠加在 SwapChain RT 上）
  23. 资源屏障：SwapChain RT → PRESENT 状态
  24. 提交命令列表
  25. Present（显示画面）
  26. WaitForPreviousFrame（CPU 等待 GPU 完成）
```

### 16.2 各 RT 的状态机

理解资源状态是理解 DX12 的关键。以下是本项目中最重要的 RT 状态变化：

| 资源 | 初始状态 | 用作写入时 | 用作读取时 | 帧末清理 |
|------|----------|------------|------------|----------|
| HDR RT | RT | RT（场景渲染） | PSR（Bloom/GodRay/DOF 读取） | RT（为下帧准备） |
| SkySnapshot | COPY_DEST | COPY_DEST（接收拷贝） | PSR（海洋着色器读取） | COPY_DEST（为下帧准备） |
| BloomExtractRT | RT | RT（亮度提取） | PSR（ToneMap 读取） | RT |
| GodRayRT | RT | RT（径向模糊） | PSR（ToneMap 读取） | RT |
| DOFRT | RT | RT（DOF 模糊） | PSR（ToneMap 读取） | RT |
| 深度缓冲 | DEPTH_WRITE | DEPTH_WRITE（场景渲染） | PSR（DOF 读取） | DEPTH_WRITE |

### 16.3 性能瓶颈分析

目前渲染管线中最耗 GPU 时间的操作：

1. **FFT（计算通道）**：256×256 的三次 IFFT，每帧约 0.2-0.5ms
2. **海洋网格绘制**：512×512 格（26 万三角形）+ 像素着色器采样 FFT 纹理
3. **Bloom 模糊**：4 次全分辨率高斯模糊（最耗带宽的操作）
4. **DOF**：每像素 20 次纹理采样 × 全分辨率（高频次采样）
5. **God Rays**：64 次采样 × 半分辨率

---

*本文档尽量用通俗语言解释原理，如有疑问欢迎查阅各着色器文件中的注释（日语）以获取更多技术细节。*
