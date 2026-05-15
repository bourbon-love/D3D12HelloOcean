# D3D12HelloOcean 项目详解

> 本文档面向希望逐行理解此项目的读者。  
> 重要代码段逐行讲解，辅助性代码以代码块整体说明。

---

## 目录

1. [项目整体是什么](#1-项目整体是什么)
2. [DirectX 12 基础概念速查](#2-directx-12-基础概念速查)
3. [程序入口与窗口](#3-程序入口与窗口)
4. [整体架构与帧循环](#4-整体架构与帧循环)
5. [FFT 海洋模拟（OceanFFT）](#5-fft-海洋模拟oceanfft)
6. [海洋网格与无限瓦片（Renderer）](#6-海洋网格与无限瓦片renderer)
7. [海洋着色器逐行详解（shaders.hlsl）](#7-海洋着色器逐行详解shadershlsl)
8. [天空系统（SkyDome）](#8-天空系统skydome)
9. [后处理管线（PostProcessPipeline）](#9-后处理管线postprocesspipeline)
10. [水下渲染系统](#10-水下渲染系统)
11. [天气系统（WeatherSystem）](#11-天气系统weathersystem)
12. [雨粒子系统（RainSystem）](#12-雨粒子系统rainsystem)
13. [鱼群系统（FishSchool）](#13-鱼群系统fishschool)
14. [漂浮物体（FloatingObject）](#14-漂浮物体floatingobject)
15. [相机系统（Camera）](#15-相机系统camera)
16. [数据流全图](#16-数据流全图)

---

## 1. 项目整体是什么

这是一个用 **DirectX 12** 写成的实时海洋渲染器，运行在 Windows 11 + GPU 上。它不是游戏引擎，是一个技术演示程序，目标是用真实的物理公式渲染出逼真的海洋画面。

主要实现的技术：

| 技术 | 效果 |
|------|------|
| **FFT 海洋模拟** | GPU 上每帧计算真实的海浪高度场 |
| **无限海面瓦片** | 摄像机移动时海面向四方延伸，看不到边界 |
| **PBR 光照** | Fresnel 反射、Beer-Lambert 水体吸收、次表面散射 |
| **昼夜循环** | 太阳/月亮位置随时间变化，天空颜色跟着变 |
| **动态天气** | 从平静到风暴，影响波浪高度、泡沫、雨量、云量 |
| **完整后处理** | Bloom、景深、SSAO、SSR、God Rays、TAA、色调映射等 |
| **水下渲染** | 摄像机潜入水面以下时的 Snell 窗口、光散射、焦散 |

---

## 2. DirectX 12 基础概念速查

在读代码之前，需要理解几个 DX12 的核心概念：

### 命令列表（Command List）
DX12 里，CPU 不能直接控制 GPU。CPU 把"画什么"的指令录制进 **命令列表**（`ID3D12GraphicsCommandList`），然后提交到 **命令队列**（`ID3D12CommandQueue`），GPU 才会执行。

### 资源屏障（Resource Barrier）
GPU 的纹理资源有不同的"使用状态"，比如：
- `RENDER_TARGET`：正在被当作渲染目标写入
- `SHADER_RESOURCE`：被着色器读取（SRV）
- `UNORDERED_ACCESS`：计算着色器可读写（UAV）
- `COPY_SOURCE` / `COPY_DEST`：正在被拷贝

在同一个纹理切换用途时，必须调用 `ResourceBarrier` 通知 GPU，否则会出现竞争条件导致画面错误。这是 DX12 和 DX11 最大的不同之一——程序员要手动管理这些状态切换。

### 描述符堆（Descriptor Heap）
资源本身（纹理、缓冲区）存在显存里。**描述符**是指向资源的"指针+格式信息"，描述符堆是一组描述符的数组。着色器通过描述符来访问资源。

### 根签名（Root Signature）
描述着色器需要什么输入：哪个寄存器绑定什么类型的资源（CBV/SRV/UAV）。可以理解为"函数签名"。

### 管线状态对象（PSO）
把顶点着色器、像素着色器、光栅化设置（剔除方向、填充模式）、混合设置、深度测试设置全部打包在一起，一次性告诉 GPU。DX12 要求提前编译好所有 PSO，运行时切换很快。

### 围栏（Fence）
CPU 和 GPU 并行工作。**围栏**是同步机制：CPU 在命令队列里插入一个信号，然后等这个信号被 GPU 执行到。保证 CPU 不会覆盖 GPU 还在用的数据。

---

## 3. 程序入口与窗口

### `Main.cpp`

```cpp
// 程序的真正入口
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow)
{
    // 创建主应用对象，窗口大小 1920×1080，标题 "D3D12 Ocean"
    D3D12HelloTriangle sample(1920, 1080, L"D3D12 Ocean");
    // 把控制权交给 Win32Application（它负责消息循环和窗口管理）
    return Win32Application::Run(&sample, hInstance, nCmdShow);
}
```

`Win32Application::Run` 里面是标准的 Windows 消息循环：
- 收到 `WM_PAINT` → 调用 `sample.OnUpdate()` 然后 `sample.OnRender()`
- 收到 `WM_KEYDOWN` → 调用 `sample.OnKeyDown()`
- 收到 `WM_MOUSEMOVE` → 调用 `sample.OnMouseMove()`
- 收到 `WM_DESTROY` → 调用 `sample.OnDestroy()` 然后退出

### 按键说明（从 `OnKeyDown` 中整理）

| 键 | 功能 |
|----|------|
| `W` | 切换线框模式 |
| `F` | 切换展示（自动环绕）模式 |
| `P` | 暂停时间 |
| `F11` | 全屏切换 |
| WASD | 移动相机（在 `Renderer::Update` 里通过 `GetAsyncKeyState` 检测） |
| 鼠标右键拖动 | 旋转相机视角 |

---

## 4. 整体架构与帧循环

### `D3D12HelloTriangle` 类的成员（主要部分）

```cpp
// 子系统（unique_ptr 管理所有权和生命周期）
std::unique_ptr<Renderer>            m_renderer;      // 海洋网格渲染
std::unique_ptr<SkyDome>             m_skyDome;       // 天空球
std::unique_ptr<OceanFFT>            m_oceanFFT;      // FFT 海浪计算
std::unique_ptr<WeatherSystem>       m_weatherSystem; // 天气状态机
std::unique_ptr<RainSystem>          m_rainSystem;    // 雨粒子
std::unique_ptr<PostProcessPipeline> m_pp;            // 后处理管线
std::unique_ptr<FloatingObject>      m_floatingObject;// 漂浮物体
std::unique_ptr<FishSchool>          m_fishSchool;    // 鱼群

// DX12 核心对象
ComPtr<ID3D12Device>              m_device;        // 逻辑设备（GPU 的抽象）
ComPtr<ID3D12CommandQueue>        m_commandQueue;  // 命令队列
ComPtr<IDXGISwapChain3>           m_swapChain;     // 交换链（前后缓冲轮转）
ComPtr<ID3D12CommandAllocator>    m_commandAllocator; // 命令内存分配器
ComPtr<ID3D12GraphicsCommandList> m_commandList;   // 每帧录制命令的列表
ComPtr<ID3D12Fence>               m_fence;         // CPU/GPU 同步

// 帧参数
float m_timeScale = 1.0f;    // 时间倍速（ImGui 可调）
bool  m_timePaused = false;  // 时间暂停
bool  m_autoExposure = true; // 自动曝光
```

### `OnInit()` — 初始化阶段

初始化分两步：

**第一步 `LoadPipeline()`**：建立 DX12 基础设施
```
创建 ID3D12Device（选择硬件 GPU）
  → 创建命令队列（DIRECT 类型，既能图形也能计算）
  → 创建 SwapChain（双缓冲，格式 R8G8B8A8_UNORM，1920×1080）
  → 创建 RTV 描述符堆（FrameCount + 9 个槽：前两个是 SwapChain 的后台缓冲，后面的是后处理 RT）
  → 创建命令分配器
```

**第二步 `LoadAssets()`**：创建子系统
```
创建根签名（4 个根参数：CBV b0, SRV表格 t0-t4, CBV b1, CBV b2）
读 CSO 文件（预编译着色器字节码）→ 初始化 OceanFFT
读 shaders VS/PS CSO → 初始化 Renderer（含 PSO、深度缓冲、网格缓冲）
读 skyshaders CSO → 初始化 SkyDome
读 rain CSO → 初始化 RainSystem
初始化 WeatherSystem（链接到 OceanFFT 和 SkyDome）
初始化 FloatingObject（链接到 OceanFFT 的高度图）
初始化 FishSchool
关闭初始化命令列表，执行（上传 GPU 资源），等待 GPU 完成
初始化 PostProcessPipeline（需要深度缓冲和 FFT 高度图）
```

### `OnUpdate()` — 每帧逻辑更新

```cpp
void D3D12HelloTriangle::OnUpdate()
{
    // 计算 deltaTime（上一帧到这帧的秒数）
    float deltaTime = ...;
    float scaledDt  = m_timePaused ? 0.0f : deltaTime * m_timeScale;

    // 1. 天空系统先更新，因为 Renderer 需要从 SkyDome 读太阳方向
    m_skyDome->Update(scaledDt);
    m_skyDome->SetCameraY(m_renderer->GetCameraPos().y); // 用于水下天空变暗

    // 2. 自动曝光：根据太阳高度调整曝光值（夜晚自动提亮）
    if (m_autoExposure) { ... m_pp->exposure 平滑插值到目标值 }

    // 3. 渲染器更新：推进时间，处理键盘/鼠标，写 SceneCB
    m_renderer->Update(scaledDt);

    // 4. 天气状态机更新，把风速/云量/雨量推给 OceanFFT/SkyDome/RainSystem
    m_weatherSystem->Update(scaledDt);

    // 5. 雨粒子更新
    m_rainSystem->Update(scaledDt, weatherIntensity, windDirX, windDirY);

    // 6. 漂浮物体和鱼群更新
    m_floatingObject->Update(scaledDt);
    m_fishSchool->Update(scaledDt, m_renderer->GetTime());

    // 7. 后处理参数更新（闪电、TAA Jitter）
    m_pp->UpdateLightning(...);
    // TAA Halton 序列 Jitter（每帧偏移 proj 矩阵的亚像素）
    m_renderer->SetJitter(jitter.x, jitter.y);

    // 8. ImGui 帧准备（BuildImGuiUI 填充 ImGui 控件）
    ImGui_ImplDX12_NewFrame(); ImGui_ImplWin32_NewFrame(...); ImGui::NewFrame();
    BuildImGuiUI();
    ImGui::Render();
}
```

### `OnRender()` → `PopulateCommandList()` — 每帧 GPU 渲染

```
【命令录制开始】
  重置命令分配器和命令列表

【OceanFFT 计算 Pass】
  调用 m_oceanFFT->Dispatch()  // GPU 上跑 4 个计算着色器 Pass
  // 结束后 m_heightMap 里有了最新的波浪高度

【转换 FFT 输出为 SRV 状态】（资源屏障）

【几何渲染 Pass（渲染到 HDR RT）】
  清除 HDR 颜色目标 + 深度
  m_renderer->Render()        // 海洋瓦片网格
  m_skyDome->Render()         // 天空球
  m_floatingObject->Render()  // 漂浮物体
  m_rainSystem->Render()      // 雨滴 + 涟漪
  m_fishSchool->Render()      // 鱼群
  ImGui::RenderDrawData()     // ImGui UI（渲染到 HDR RT 上）

【转换 HDR RT 为 SRV 状态】

【后处理 Pass（一系列全屏四边形 Pass）】
  SSR → Shadow → SSAO → Bloom → God Rays → Lens Flare
  → DOF → ToneMap → TAA → 输出到 SwapChain 后台缓冲

【命令录制结束，执行，Present，等待上帧完成】
```

---

## 5. FFT 海洋模拟（OceanFFT）

这是整个项目最核心的技术部分，值得详细讲解。

### 5.1 为什么用 FFT

海洋的波浪是由无数不同频率、方向的正弦波叠加而成的。在频域里，每个频率分量 k（波数向量）有一个复数振幅 h(k,t)，代表这个方向、这个频率的波在时刻 t 的状态。

**傅里叶变换**（FFT）能把 N×N 个频域值一次性变换成 N×N 个空域值（即网格上每个点的高度）。如果逐点计算叠加，复杂度是 O(N⁴)；用 FFT 是 O(N² log N)，快几个数量级。

N=256 时，256×256=65536 个频率分量，用 FFT 每帧实时计算。

### 5.2 物理模型：JONSWAP/Phillips 谱

每个频率分量的能量大小由**谱函数**决定。本项目使用的是 Phillips 谱（在代码里叫 `PhillipsCS.hlsl`，但其中加入了 JONSWAP 的峰值增强因子）。

**Phillips 谱公式：**
```
P(k) = A × exp(-1/(kL)²) / k⁴ × |k·w|²
```
其中：
- `A`：总体振幅系数（越大浪越高）
- `k`：波数向量的模（频率，k 越大波长越短）
- `L = windSpeed² / g`：风力能支撑的最大波长（风速 20m/s 时 L≈40m）
- `k·w`：波数方向和风向的点积（顺风方向能量最强）

**实现（`PhillipsCS.hlsl`，简化说明）：**
```
对网格每个像素 (i,j) 计算对应的波数 k：
  k = 2π × (i - N/2, j - N/2) / L_patch    // L_patch=400m（FFT 域对应的物理尺寸）
计算谱值 Ph = Phillips(k, windDir, windSpeed, A)
用高斯随机数 × sqrt(Ph/2) 生成初始复振幅 h0(k)
同时存 h0(-k) 的共轭（IFFT 后得到实数结果必须的对称性）
```

### 5.3 时间演化（`TimeEvolutionCS.hlsl`）

每帧，每个频率分量按**色散关系**随时间旋转相位：

```
ω(k) = sqrt(g × |k|)         // 水面波的色散关系（重力波，深水近似）
h(k, t) = h0(k) × exp(iωt) + h0(-k)* × exp(-iωt)
```

这里 `exp(iωt) = cos(ωt) + i×sin(ωt)`，表示频率为 ω 的复数旋转。

结果写到 `m_hktMap`（.rg = h+Dx 复数），`m_dztMap`（.rg = Dz 复数）。

- **Dx(k,t)**：X 方向水平位移（Gerstner-like 水平涌动）
- **Dz(k,t)**：Z 方向水平位移

### 5.4 IFFT（`IFFTCS.hlsl`）

把频域的 256×256 复数矩阵变换回空域。

**Cooley-Tukey Radix-2 算法：**

IFFT 分为两个方向（先行，再列），每个方向 log₂(256) = 8 次 Dispatch，共 16 次。

每次 Dispatch 是一个"蝶形运算"：把 N 点 IFFT 拆成两个 N/2 点 IFFT，需要参数 `stepSize`（当前合并步长）。

**乒乓缓冲（Ping-Pong）：**

```
初始：数据在 m_hktMap
第1次Dispatch: 读 m_hktMap，写 m_tempMap    (pingpong=0)
第2次Dispatch: 读 m_tempMap，写 m_hktMap    (pingpong=1)
...交替进行...
最终：数据回到 m_hktMap
```

**代码逻辑（从 `OceanFFT::Dispatch` 中）：**
```cpp
// h+Dx 的行 IFFT（passIdx=0，8次）
ifftCB.passIdx = 0;
for (UINT step = 2, pp = 0; step <= m_textureSize; step <<= 1, pp ^= 1)
{
    ifftCB.stepSize = step;   // 步长从2开始，每次×2（2,4,8,...,256）
    ifftCB.pingpong = pp;     // 0→1→0→... 决定读哪个写哪个
    dispatchAndBarrier();     // 提交 Dispatch + UAV Barrier
}
// 列 IFFT（passIdx=1，8次）
ifftCB.passIdx = 1;
for (UINT step = 2, pp = 0; step <= m_textureSize; step <<= 1, pp ^= 1)
{ ... }
```

### 5.5 最终结果

IFFT 完成后：
- `m_heightMap.x` = 海面高度 h（单位：米/1000，需乘 FFT_HEIGHT_SCALE=1/1000）
- `m_heightMap.z` = X 方向水平位移 Dx
- `m_dztMap.x` = Z 方向水平位移 Dz

这三个值在顶点着色器里用来扭曲网格顶点，生成逼真的海浪形状（包括水平方向的"Choppy"涌动效果）。

### 5.6 动态风向支持

注意 Pass 0（h0 重算）**每帧都跑**（不是只在初始化时跑一次）。这允许 `WeatherSystem` 实时修改 `windSpeed`、`windDirX/Y`、`phillipsA` 参数，海浪状态在几秒内平滑过渡到新的风况。

---

## 6. 海洋网格与无限瓦片（Renderer）

### 6.1 网格生成（`GridMesh.cpp`）

`GenerateGrid(512, 512, 400.0f)` 生成一个平坦的正方形网格：

```
顶点数：(512+1) × (512+1) = 263169 个
三角形：512 × 512 × 2 = 524288 个
尺寸：400m × 400m（中心在原点）
```

每个顶点的 Y 坐标全是 0.0f，真正的波浪高度在**顶点着色器**里计算（采样 FFT 高度图 + Gerstner 波函数）。

UV 从 (0,0) 到 (1,1) 均匀分布，用于采样 FFT 贴图。

低 LOD 版本：`GenerateGrid(64, 64, 400.0f)`，同样 400m 大小，只有 65×65=4225 个顶点，远处看细节差别不大但大幅减少 GPU 负担。

### 6.2 无限瓦片系统

#### 核心参数（`Renderer.h`）
```cpp
static const int   TILE_RADIUS  = 4;           // 相机周围 ±4 格
static constexpr float TILE_SIZE = 400.0f;     // 每格 400m
static const UINT  CB_MAX_TILES = 100;         // 最多同时渲染 100 个瓦片
static const UINT  CB_SLOT_SIZE = 512;         // 每个瓦片一个 512B CB 槽（256B 对齐）
```

TILE_RADIUS=4 意味着 9×9=81 个瓦片，覆盖 3600m×3600m 的海面。

#### 常量缓冲区布局

项目在初始化时分配 `1024 * 64 = 65536` 字节的上传堆作为所有瓦片的常量缓冲区池。每个瓦片用 512 字节的对齐槽（DX12 要求 CBV 必须 256 字节对齐）。

```cpp
// Render() 里给第 slot 个瓦片写入 CB 数据：
UINT cbOffset = slot * CB_SLOT_SIZE;          // 计算该槽的字节偏移
auto* tileCB = reinterpret_cast<SceneCB*>(m_pCbvDataBegin + cbOffset);
*tileCB = m_lastSceneCB;                      // 拷贝公共数据（矩阵、光照等）
tileCB->tileOffset = { tx * TILE_SIZE, tz * TILE_SIZE };  // 只有这个字段不同！

// 绑定这个槽的 GPU 虚拟地址给 GPU 使用：
ctx.cmd->SetGraphicsRootConstantBufferView(
    0, m_constantBuffer->GetGPUVirtualAddress() + cbOffset);
```

#### 渲染循环（`Renderer::Render()`）

```cpp
// 确定相机所在的瓦片坐标（整数）
int camTX = (int)floorf(m_camera.position.x / TILE_SIZE);  // 相机 X 坐标折算成瓦片索引
int camTZ = (int)floorf(m_camera.position.z / TILE_SIZE);

UINT slot = 0;
// 遍历相机周围 ±TILE_RADIUS 格的所有瓦片
for (int tz = camTZ - TILE_RADIUS; tz <= camTZ + TILE_RADIUS && slot < CB_MAX_TILES; ++tz)
{
    for (int tx = camTX - TILE_RADIUS; tx <= camTX + TILE_RADIUS && slot < CB_MAX_TILES; ++tx)
    {
        // 视锥剔除：AABB 测试（6 平面）
        if (!IsTileVisible(tx, tz)) continue;  // 不在视野里跳过

        // 写 CB 数据（上面描述的部分）
        // ...

        // LOD 判断：距相机中心 ≤2 格用高清，否则用低清
        bool hiRes = (abs(tx - camTX) <= 2 && abs(tz - camTZ) <= 2);
        if (hiRes)
            // 绑定 512×512 网格，DrawIndexedInstanced
        else
            // 绑定 64×64 网格，DrawIndexedInstanced
        ++slot;
    }
}
```

#### 视锥剔除（`IsTileVisible`）

**Gribb-Hartmann 法**：直接从 VP 矩阵（view×proj 的乘积）提取 6 个裁剪平面，避免手动构建。

```cpp
void Renderer::ExtractFrustumPlanes()
{
    XMMATRIX vp = m_camera.GetViewMatrix() * m_camera.GetProjMatrix();
    XMFLOAT4X4 m;
    XMStoreFloat4x4(&m, vp);

    // 每个平面的 (nx, ny, nz, d)，通过矩阵行的加减得到
    m_frustumPlanes[0] = { m._11+m._14, m._21+m._24, m._31+m._34, m._41+m._44 }; // 左平面
    m_frustumPlanes[1] = { m._14-m._11, m._24-m._21, m._34-m._31, m._44-m._41 }; // 右平面
    // ... 上下近远
}

bool Renderer::IsTileVisible(int tx, int tz) const
{
    // 计算瓦片 AABB 的世界坐标范围
    float half = TILE_SIZE * 0.5f;
    float minX = tx * TILE_SIZE - half, maxX = tx * TILE_SIZE + half;
    float minZ = tz * TILE_SIZE - half, maxZ = tz * TILE_SIZE + half;
    // Y 方向给 ±40m 的范围（包住最大波浪）

    constexpr float CULL_BIAS = TILE_SIZE * 0.5f;  // 偏置防止边缘 pop-in
    for (const auto& p : m_frustumPlanes)
    {
        // 计算 AABB 在该平面法线方向上的最远点（正侧）
        float dx = (p.x > 0.0f) ? maxX : minX;
        float dy = (p.y > 0.0f) ? maxY : minY;
        float dz = (p.z > 0.0f) ? maxZ : minZ;
        // 如果最远点还在平面负侧，整个 AABB 都在视锥外
        if (p.x * dx + p.y * dy + p.z * dz + p.w < -CULL_BIAS)
            return false;  // 剔除
    }
    return true;  // 通过所有 6 个平面检测，在视锥内
}
```

### 6.3 常量缓冲区结构体内存布局

HLSL 和 C++ 的 `SceneCB` 必须**精确对齐**，否则着色器读到的数据是乱的。这里梳理一下字节偏移：

| 偏移（字节） | C++ 字段 | HLSL 字段 | 大小 |
|-------------|---------|---------|------|
| 0 | XMMATRIX view | float4x4 view | 64 |
| 64 | XMMATRIX proj | float4x4 proj | 64 |
| 128 | float time | float time | 4 |
| 132 | XMFLOAT3 cameraPos | float3 cameraPos | 12 |
| 144 | XMFLOAT3 sunDir | float3 sunDir | 12 |
| 156 | float sunIntensity | float sunIntensity | 4 |
| 160 | XMFLOAT3 sunColor | float3 sunColor | 12 |
| 172 | float padSun | float padSun | 4 |
| 176 | XMFLOAT3 skyColor | float3 skyColor | 12 |
| 188 | float padSky | float padSky | 4 |
| 192 | float fogStart | float fogStart | 4 |
| 196 | float fogEnd | float fogEnd | 4 |
| 200 | float foamIntensity | float foamIntensity | 4 |
| 204 | float ssrMix | float ssrMix | 4 |
| 208-335 | WaveParam waves[4] | float2/float×6 ×4 | 128 |
| 336 | XMFLOAT2 tileOffset | float2 tileOffset | 8 |
| 344 | XMFLOAT2 tilePad | float2 tilePad | 8 |
| **总计** | | | **352 字节** |

---

## 7. 海洋着色器逐行详解（shaders.hlsl）

这是整个项目最重要的着色器，几乎所有的海洋视觉效果都在这里实现。

### 7.1 常量缓冲区声明

```hlsl
cbuffer SceneCB : register(b0)
{
    float4x4 view;       // 视图矩阵（相机变换）
    float4x4 proj;       // 投影矩阵（透视变换）
    float    time;       // 全局时间，单位秒
    float3   cameraPos;  // 相机世界坐标

    float3 sunDir;        // 太阳/月亮方向，单位向量（水面以上）
    float  sunIntensity;  // 光源强度（日出日落时低，夜晚≈0）
    float3 sunColor;      // 光源颜色（日出橙色，正午白色，夜晚蓝灰）
    float  padSun;        // 填充保证 16B 对齐
    float3 skyColor;      // 天空颜色，用于 Fresnel 反射的环境光回退
    float  padSky;
    float  fogStart;      // 雾起始距离（米）
    float  fogEnd;        // 雾饱和距离（米）
    float  foamIntensity; // 泡沫强度（0=平静，1=风暴）
    float  ssrMix;        // 屏幕空间反射的混合系数

    // 4 组 Gerstner 波参数（每组 32 字节）
    float2 waveDir0; float waveAmp0; float waveLen0;
    float waveSpd0;  float waveStp0; float2 wavePad0;
    // ... waveDir1~3 同结构

    float2 tileOffset;  // 本瓦片在世界空间的 XZ 偏移（无限海面的关键）
    float2 tilePad;
};

cbuffer RippleCB : register(b1)
{
    RippleData ripples[200];  // 最多 200 个水面涟漪（雨滴落水产生）
    uint rippleCount;
    float3 ripplePad;
};

cbuffer ShadowCB : register(b2)
{
    float4x4 lightViewProj;  // 从光源视角的 VP 矩阵（用于阴影贴图采样）
    float shadowBias;         // 阴影偏置（防止自阴影 z-fighting）
    float shadowStrength;     // 阴影强度
    float shadowEnabled;      // 是否开启阴影
    float pad_shadow;
    float screenW;            // 屏幕宽度（像素）
    float screenH;            // 屏幕高度
    float waterBodyStr;       // 水体颜色乘数
    float waterRefract;       // 折射 UV 扭曲强度
    float waterMinTrans;      // 掠射角时的最小透过率
};
```

### 7.2 纹理绑定

```hlsl
Texture2D<float4> g_heightMap   : register(t0); // FFT 输出：x=高度，z=Dx位移
Texture2D<float4> g_dztMap      : register(t1); // FFT 输出：x=Dz位移
Texture2D<float4> g_skySnapshot : register(t2); // 天空快照（用于 SSR 和水中Snell窗）
Texture2D<float>  g_shadowMap   : register(t3); // 阴影贴图
Texture2D<float4> g_refraction  : register(t4); // 折射（目前未使用）
SamplerState g_sampler : register(s0);           // 线性过滤 + WRAP 地址模式
```

### 7.3 Gerstner 波函数

```hlsl
void GerstnerWave(
    float2 xz,       // 顶点的世界 XZ 坐标（含 tileOffset）
    float2 dir,      // 波的传播方向（单位向量）
    float amp,       // 振幅（米）
    float wavelen,   // 波长（米）
    float spd,       // 速度系数
    float steep,     // 陡峭度（0=正弦波，接近1=尖锐的 Gerstner 波）
    inout float3 disp,      // 累加位移量（XYZ）
    inout float3 tangentX,  // 累加切线向量（用于法线计算）
    inout float3 tangentZ)
{
    float k = 2.0f * 3.14159265f / wavelen;  // 波数（k = 2π/λ）
    // 波的相位：k × (波方向·位置) - 速度 × 时间
    float f = k * dot(dir, xz) - spd * time;

    float Q = steep;   // Q 控制水平位移的幅度
    float sinF = sin(f); float cosF = cos(f);

    // XZ 水平位移（Gerstner 特征：波峰向传播方向偏移）
    disp.x += Q * amp * dir.x * cosF;   // X 方向水平位移
    disp.y += amp * sinF;                // Y 方向垂直位移（正弦形）
    disp.z += Q * amp * dir.y * cosF;   // Z 方向水平位移

    // 切线 X 方向（dPosition/dX，用于叉积算法线）
    tangentX.x += 1.0f - Q * dir.x * dir.x * k * amp * sinF;
    tangentX.y += dir.x * k * amp * cosF;
    tangentX.z -= Q * dir.x * dir.y * k * amp * sinF;

    // 切线 Z 方向（dPosition/dZ）
    tangentZ.x -= Q * dir.x * dir.y * k * amp * sinF;
    tangentZ.y += dir.y * k * amp * cosF;
    tangentZ.z += 1.0f - Q * dir.y * dir.y * k * amp * sinF;
}
```

**为什么需要 Gerstner 波？**

普通正弦波只有 Y 方向位移，水面是圆润的。Gerstner 波在水平方向也有位移（`Q * amp * dir * cosF`），使得波峰更尖锐、波谷更宽平，更接近真实海浪。

### 7.4 顶点着色器（VSMain）

```hlsl
VSOutput VSMain(VSInput vin)
{
    VSOutput vout;

    // ============ 关键：世界坐标 = 本地坐标 + 瓦片偏移 ============
    // 这是无限海面的核心：每个瓦片的顶点用世界坐标来计算 Gerstner 波，
    // 保证相邻瓦片的波形在接缝处连续，没有跳变。
    float2 xz = vin.position.xz + tileOffset;  // 世界 XZ（包含瓦片偏移）

    float3 disp    = float3(0.0f, 0.0f, 0.0f); // 总位移量（初始化为 0）
    float3 tangentX = float3(1.0f, 0.0f, 0.0f); // 初始切线 X（未变形时就是 X 轴）
    float3 tangentZ = float3(0.0f, 0.0f, 1.0f); // 初始切线 Z（未变形时就是 Z 轴）

    // 叠加 4 组 Gerstner 波（每组方向/振幅/波长不同，模拟多频率叠加）
    GerstnerWave(xz, waveDir0, waveAmp0, waveLen0, waveSpd0, waveStp0, disp, tangentX, tangentZ);
    GerstnerWave(xz, waveDir1, waveAmp1, waveLen1, waveSpd1, waveStp1, disp, tangentX, tangentZ);
    GerstnerWave(xz, waveDir2, waveAmp2, waveLen2, waveSpd2, waveStp2, disp, tangentX, tangentZ);
    GerstnerWave(xz, waveDir3, waveAmp3, waveLen3, waveSpd3, waveStp3, disp, tangentX, tangentZ);

    // 从本地坐标 + Gerstner 位移得到中间世界坐标
    float3 worldPos = vin.position + disp;
    worldPos.xz += tileOffset;  // 加上瓦片的世界偏移，把这个瓦片放到正确位置

    // FFT UV：[0,1] 的瓦片内局部 UV
    // 用本地坐标（不含 tileOffset）计算 UV，因为 FFT 纹理是以 WRAP 模式采样的
    // 每个瓦片都从 (0,0) 到 (1,1) 采样同一张 256×256 的 FFT 贴图
    // WRAP 模式保证纹理无缝平铺
    float2 fftUV = vin.position.xz / FFT_TILE_SIZE;  // [-200,200] → [0,1]（注意这里已经是本地坐标）

    // 采样 FFT 高度图（Scale=1/1000，因为 FFT 计算的单位和世界单位差 1000 倍）
    float4 fftSample = g_heightMap.SampleLevel(g_sampler, fftUV, 0);
    float fftHeight  = fftSample.x * FFT_HEIGHT_SCALE;  // .x 是高度
    float fftDx      = fftSample.z * FFT_CHOP_SCALE;    // .z 是 X 方向水平位移
    float fftDz      = g_dztMap.SampleLevel(g_sampler, fftUV, 0).x * FFT_CHOP_SCALE;  // Z 方向位移

    // 把 FFT 的高度和水平位移加到世界坐标上
    // 注意：fftHeight 取反（-1.0f），因为 FFT 坐标系和世界坐标系 Y 轴方向不同
    worldPos.y += fftHeight * -1.0f;
    worldPos.x += fftDx;   // X 水平位移
    worldPos.z += fftDz;   // Z 水平位移

    // 从切线叉积计算法线（cross(tangentZ, tangentX) 给出向上的法线）
    float3 normal = normalize(cross(tangentZ, tangentX));

    // 透视变换：世界坐标 → 视图坐标 → 裁剪坐标
    float4 posV  = mul(float4(worldPos, 1.0f), view);  // 视图变换
    vout.posH    = mul(posV, proj);                     // 投影变换（裁剪坐标）
    vout.posW    = worldPos;   // 保留世界坐标给像素着色器用
    vout.normal  = normal;     // 法线传给像素着色器
    vout.uv      = fftUV;      // UV 传给像素着色器（像素着色器里重新采样邻域算精确法线）
    return vout;
}
```

**重点说明：为什么 FFT UV 用本地坐标，而 Gerstner 用世界坐标？**

- **Gerstner 波**：是确定性数学函数，同一世界坐标处必须得到同一个位移值，所以要用世界坐标（含 tileOffset）。相邻瓦片在接缝处世界坐标相同，Gerstner 值就连续。
- **FFT 贴图**：是周期性纹理（FFT 本质上就是周期信号），用 WRAP 采样器，每个瓦片都从 UV(0,0)~(1,1) 采样同一张贴图，纹理在瓦片边界自然无缝（因为 FFT 的周期边界条件保证了 h(x=0) = h(x=L)）。

### 7.5 像素着色器（PSMain）— 法线重计算

像素着色器不用 VS 传来的法线，而是重新从 FFT 高度图采样邻域，算更精确的法线：

```hlsl
const float texelSize    = 1.0f / 256.0f;    // FFT 贴图 256×256，一个像素的 UV 步长
const float worldPerTexel = FFT_TILE_SIZE / 256.0f;  // 每个 FFT 像素对应的世界距离

// 采样上下左右 4 个邻域的高度（中心差分法）
float hL = -g_heightMap.SampleLevel(g_sampler, pin.uv + float2(-texelSize, 0), 0).x * FFT_HEIGHT_SCALE;
float hR = -g_heightMap.SampleLevel(g_sampler, pin.uv + float2( texelSize, 0), 0).x * FFT_HEIGHT_SCALE;
float hD = -g_heightMap.SampleLevel(g_sampler, pin.uv + float2(0, -texelSize), 0).x * FFT_HEIGHT_SCALE;
float hU = -g_heightMap.SampleLevel(g_sampler, pin.uv + float2(0,  texelSize), 0).x * FFT_HEIGHT_SCALE;

// 同样采样 Dx/Dz 位移的邻域（Jacobian 法线需要考虑水平位移造成的扭曲）
float dxL = ..., dxR = ..., dxD = ..., dxU = ...;
float dzL = ..., dzR = ..., dzD = ..., dzU = ...;

// 数值偏微分
float dHdx = (hR - hL) / (2.0f * worldPerTexel);   // dH/dx
float dHdz = (hU - hD) / (2.0f * worldPerTexel);   // dH/dz（UV 的 v 方向对应世界 z）
float dDxdx = (dxR - dxL) / (2.0f * worldPerTexel);
float dDzdz = (dzU - dzD) / (2.0f * worldPerTexel);
float dDxdz = (dxU - dxD) / (2.0f * worldPerTexel);
float dDzdx = (dzR - dzL) / (2.0f * worldPerTexel);

// Jacobian 法线：考虑了水平位移变形的法线计算
// 变形后顶点 P(u,v) = (u + Dx, h, v + Dz)
// 切线 X = dP/du = (1+dDxdx, dHdx, dDzdx)
// 切线 Z = dP/dv = (dDxdz,   dHdz, 1+dDzdz)
float3 tangentX = float3(1.0f + dDxdx, dHdx, dDzdx);
float3 tangentZ = float3(dDxdz, dHdz, 1.0f + dDzdz);
float3 N = normalize(cross(tangentZ, tangentX));  // 法线 = 切线叉积
```

**为什么在像素着色器里重算法线？**

顶点着色器里的法线是从 Gerstner 波解析求导得到的，精度受顶点密度限制（512×512 = 每格约 0.78m）。像素着色器里从 256×256 FFT 贴图重采样，加入了 FFT 的高频细节（每格约 1.56m），法线更准确，特别是波峰附近的高频细节。

### 7.6 涟漪叠加

```hlsl
// 对每个活跃的涟漪（雨滴落水产生的环形波）
for (uint i = 0; i < rippleCount; ++i)
{
    float2 toPixel = pin.posW.xz - ripples[i].position;  // 当前像素到涟漪中心的向量
    float dist = length(toPixel);
    float r    = ripples[i].radius;      // 涟漪当前半径（随时间扩展）

    float ringWidth = 1.5f;              // 环的宽度（米）
    // inRing：接近环位置时为1，远离时为0（用来只在环附近施加法线扰动）
    float inRing = saturate(1.0f - abs(dist - r) / ringWidth);

    if (inRing > 0.0f)
    {
        float2 dir = dist > 0.001f ? toPixel / dist : float2(1.0f, 0.0f);  // 径向方向
        // 在环的位置处产生正弦形的法线扰动
        float wave = sin((dist - r) * 3.14159f / ringWidth);
        float strength = inRing * ripples[i].strength * wave * 0.3f;
        N.x += dir.x * strength;   // 法线 X 分量扰动
        N.z += dir.y * strength;   // 法线 Z 分量扰动（注意 dir.y 对应 Z 轴）
        N = normalize(N);           // 归一化
    }
}
```

### 7.7 光照计算

```hlsl
float3 V = normalize(cameraPos - pin.posW);  // 视线方向（从片元指向相机）

// 水中视角：法线和视线可能反向（从下往上看时 N 指向下，V 指向下）
// 翻转法线保证始终和视线同侧（为了 Fresnel、Phong 等公式的正确性）
if (dot(N, V) < 0.0f) N = -N;

float3 L = sunDir;               // 光源方向（已是单位向量）
float3 H = normalize(V + L);    // 半程向量（用于高光计算）

// ---- 水体底色（极暗的蓝绿色，视觉上的"水的颜色"来自 Fresnel 反射）----
float3 deepColor    = float3(0.004, 0.014, 0.030);   // 极深处：几乎是黑色
float3 shallowColor = float3(0.007, 0.022, 0.048);   // 浅处稍亮
float heightFactor  = saturate(pin.posW.y * 0.08f + 0.5f); // 波高映射到 [0,1]
float3 waterColor   = lerp(deepColor, shallowColor, heightFactor);

// ---- 漫反射（Diffuse）----
float NdotL = saturate(dot(N, L));   // 法线和光源方向夹角余弦（Lambertian 因子）
// 夜间检测：sunIntensity 低时增加夜间环境光（避免完全黑暗）
float nightT     = saturate(1.0 - sunIntensity * 3.0);   // 0=白天，1=深夜
float3 nightAmbient = float3(0.022, 0.024, 0.028);        // 深夜环境光（极暗蓝灰）
float3 ambLight  = lerp(sunColor, nightAmbient, nightT);  // 昼夜插值环境光
float3 diffuse   = waterColor * (NdotL * 0.5f * sunIntensity + 0.5f);
diffuse *= ambLight;

// ---- 镜面高光（Specular）— 两层结构 ----
float NdotH     = saturate(dot(N, H));
float specTight = pow(NdotH, 128.0f) * 14.0f;   // 紧凑层：极细的太阳高光
float daySpec   = saturate((sunIntensity - 0.35) * 10.0);  // 白天才有宽散层
float specBroad = pow(NdotH, 18.0f) * 0.6f * daySpec;      // 宽散层：海面通透感
float3 specularColor = sunColor * (specTight + specBroad) * sunIntensity;

// ---- Fresnel 反射率 ----
// F0=0.02 是水的基础反射率（垂直入射角，即正面看时只反射 2%）
float F0    = 0.02f;
float NdotV = saturate(dot(N, V));   // 视线和法线夹角余弦（0=掠射，1=正面）
// Schlick 近似：掠射角（NdotV→0）时反射率趋近 1（镜面效果）
// 上限 0.65f 防止反射率过高造成亮白（真实水面也不会全反射）
float fresnel = min(F0 + (1.0f - F0) * pow(1.0f - NdotV, 5.0f), 0.65f);
```

### 7.8 反射采样（SSR）

```hlsl
// 计算反射方向（reflect 函数：reflect(-V, N) 等价于 2(N·V)N - V，取反后得反射方向）
float3 reflectDir = reflect(-V, N);

// 把反射方向延伸 300m 后投影回屏幕空间 UV
float4x4 vp       = mul(view, proj);
float4 reflClip   = mul(float4(pin.posW + reflectDir * 300.0, 1.0), vp);
float2 reflUV     = reflClip.xy / reflClip.w * float2(0.5, -0.5) + 0.5; // NDC→UV（注意 Y 翻转）
reflUV           += N.xz * 0.018;  // 法线扰动（波纹使反射轻微扭曲）
reflUV            = saturate(reflUV);

// 边缘淡出（屏幕边缘 SSR 数据不可靠）
float2 edgeFade = saturate(min(reflUV, 1.0 - reflUV) * 6.0);
float fade = min(edgeFade.x, edgeFade.y);
fade *= saturate(reflectDir.y * 4.0 + 0.3);  // 向下的反射方向不可信（没有数据）
fade *= ssrMix;   // 用户可调的 SSR 强度

// 从天空快照贴图采样（这张贴图由后处理管线在渲染天空后抓取）
float3 ssrSample  = g_skySnapshot.SampleLevel(g_sampler, reflUV, 0).rgb;
// 程序化天空颜色（SSR 无效区域的回退）
float3 procSample = SampleSkyReflection(reflectDir);  // 根据反射方向计算天空渐变
// 混合：有 SSR 数据用 SSR，没有用程序化天空
float reflBright  = lerp(0.08, 1.0, saturate(sunIntensity * 1.8));  // 夜晚反射也要有光
reflectSample     = lerp(procSample, ssrSample, fade) * reflBright;
```

### 7.9 水体透射色（Beer-Lambert）

```hlsl
// NdotV 越小（掠射角），光线穿过的水体路径越长
// 最短 4m（垂直看下去），最长 12m（掠射角）
float waterDepth  = clamp(4.0 / max(NdotV, 0.15), 4.0, 12.0);

// Beer-Lambert 吸收定律：I = I0 × exp(-α × d)
// 红光 α=0.45（吸收最快），绿光 α=0.06，蓝光 α=0.025（最慢，所以水是蓝的）
float3 absorbCoeff = float3(0.45, 0.06, 0.025);

// deepOcean：深水的固有散射色（外洋的蓝绿色）
float3 deepOcean   = float3(0.010, 0.15, 0.32) * waterBodyStr;

// 太阳光强度的透射量
float sunLit      = sunIntensity * 0.55 + 0.12;  // 夜晚也有少量月光透过

// 透射色 = 深水色 × 吸收衰减 × 光照强度
float3 transBody  = deepOcean * exp(-absorbCoeff * waterDepth) * sunLit;

transmitted = transBody;  // 最终透射色
```

### 7.10 Fresnel 合成最终颜色

```hlsl
// 掠射角时保留一定透过率（防止边缘完全变成镜面）
float transWeight = waterMinTrans + (1.0 - waterMinTrans) * (1.0 - fresnel);

// 最终颜色 = 反射 + 透射 + 高光 + 少量漫反射
float3 color = fresnel * reflectSample      // Fresnel 加权的反射（天空/SSR）
             + transWeight * transmitted    // 透过率加权的水体色
             + specularColor                // 太阳镜面高光
             + diffuse * 0.30;              // 少量漫反射（水面整体光感）
```

### 7.11 次表面散射（SSS）

```hlsl
// 波峰处的透射光（逆光时波顶会呈现蓝绿半透明效果）
float3 sssDir    = normalize(-sunDir + N * 0.5);    // 折中方向（光线穿过波面的方向）
float  sssView   = pow(saturate(dot(V, sssDir)), 4.0);  // 视线方向贡献（逆光时最强）
float  sssCrest  = saturate(pin.posW.y * 0.18 + 0.2);   // 只在波峰（y>0）有效
float  sssDaylight = saturate((sunIntensity - 0.35) * 10.0);  // 夜间无 SSS
float3 sssColor  = float3(0.0, 0.38, 0.42)   // 蓝绿色（SSS 典型颜色）
                 * sssView * sssCrest * min(sunIntensity, 1.5) * 1.4 * sssDaylight;
color += sssColor;
```

### 7.12 Jacobian 泡沫

```hlsl
// Jacobian 行列式 J：表示波面被压缩的程度
// J < 1 → 波峰折叠（实际海浪就在这里翻滚产生泡沫）
float J = (1.0f + dDxdx) * (1.0f + dDzdz) - dDxdz * dDzdx;

// rawFoam：J 越小（折叠越剧烈），泡沫越强
float sharpness = lerp(5.0, 2.5, foamIntensity);  // 风暴时泡沫更尖锐
float rawFoam   = pow(saturate(1.0 - J), sharpness);
rawFoam        *= lerp(0.15, 1.0, foamIntensity);  // 天气影响整体泡沫量

// 只在向上的面（波峰）显示泡沫，避免波侧面出现泡沫
float topFace = saturate((N.y - 0.45) / 0.55);

// 程序化泡沫噪声（Wind-streak 纹理效果）
float2 waveDir2D = normalize(waveDir0);   // 主波方向
float2 perpDir2D = float2(-waveDir2D.y, waveDir2D.x);  // 垂直方向
// 在波方向上拉伸（4:1比率）→ 形成顺风的条纹
float2 foamUVBase = float2(perpAxis * 0.25f, alongAxis * 0.065f);
float2 fuv   = foamUVBase + float2(time * 0.10f, time * 0.06f);  // 随时间流动
// FBM 噪声（三倍频叠加）
float fbm    = fn1 * 0.50 + fn2 * 0.32 + fn3 * 0.18;
float foamNoise = smoothstep(0.30, 0.68, fbm);  // 二值化为条纹

float foamMask = saturate(rawFoam * topFace * (0.15 + foamNoise * 1.6));

// 泡沫颜色（HDR 值超过 1.0，触发 Bloom 效果）
float3 foamWhite = float3(2.2, 2.2, 2.2);   // 纯白泡沫，HDR 过曝
float3 foamColor = lerp(float3(1.9,1.9,1.95), foamWhite, foamNoise);
color = lerp(color, foamColor, foamMask * 0.90);  // 泡沫覆盖底色
```

---

## 8. 天空系统（SkyDome）

### 8.1 天空球几何

天空球是一个半径 2000m 的球体（内翻，法线朝内），用**正面剔除（Front-face Culling）**让球的内面可见，深度测试不写（永远在最远处）。

### 8.2 昼夜循环

```
太阳位置：随时间以 (0,0,0) 为中心公转，仰角范围从 -30° 到 +80°
月亮位置：和太阳相差 180°（总是相对）
日出/日落：太阳高度 y ∈ [-0.1, 0.1] 时平滑插值
天空色：
  白天：天顶 #1440B8，地平线 skyColor（随天气变化）
  日落：橙金色 HDR (1.6, 0.72, 0.12)
  夜晚：极暗（0.004, 0.005, 0.010）
```

### 8.3 程序化云层

```hlsl
// 天空着色器里（skyshaders.hlsl）
// 3D Perlin/FBM 噪声生成云层
// cloudScale 控制云的大小，cloudDensity 控制厚度，cloudSharpness 控制边缘
// 通过 WeatherSystem 驱动，风暴时云层更厚更暗
float cloud = FBM(worldDir * cloudScale + float3(time * 0.01, 0, time * 0.007));
cloud = smoothstep(1.0 - cloudDensity, 1.0, cloud * cloudSharpness);
```

### 8.4 月光

夜间月亮作为点光源（小圆盘），月光颜色蓝灰（接近真实月光）。Renderer 里用 `sunIntensity` 的值（夜间 ≈ 0.05）和 `sunColor`（月光色）来进行昼夜无缝插值，水面高光在夜晚变为淡蓝色细线。

---

## 9. 后处理管线（PostProcessPipeline）

后处理在几何渲染完成后运行，输入是 HDR 渲染目标，输出到 SwapChain 后台缓冲。

### 9.1 整体流程

```
HDR RT（含场景颜色）
  ↓
SSR（屏幕空间反射）
  ↓
Shadow Map（阴影贴图，供 shaders.hlsl 的 PCF 采样）
  ↓
SSAO（屏幕空间环境光遮蔽，从深度图计算周围遮蔽）
  ↓
Bloom Extract（提取亮度 > 阈值的像素）
  ↓
Bloom Blur（高斯模糊提取出的亮区）
  ↓
God Rays（64采样放射状模糊，从HDR RT生成体积光）
  ↓
Lens Flare（镜头光晕，水下时跳过）
  ↓
DOF（景深，基于深度图的圆形散焦模糊）
  ↓
ToneMap（ACES色调映射 + Vignette + Film Grain + 水下效果）
  ↓
TAA（时间抗锯齿，混合历史帧）
  ↓
输出到后台缓冲 → Present
```

### 9.2 Bloom（辉光）

```
Extract Pass：
  对 HDR RT 每个像素，计算亮度 lum = dot(color, float3(0.2126,0.7152,0.0722))
  如果 lum > threshold（约 0.8）才输出，否则输出黑色
  → 得到 "只有亮部" 的图像

Blur Pass：
  对提取出的亮部图像做两次高斯模糊（水平+垂直）
  模糊半径越大，辉光越大越柔和

ToneMap 时合并：
  finalColor = ACESFilmic(HDR × exposure + bloom × bloomStrength + godrays × godRayStrength)
```

### 9.3 God Rays（体积光）

```hlsl
// godrays.hlsl
// 核心：以太阳屏幕坐标为中心，向外做64步放射状采样，沿途累加亮度

float2 delta = (i.uv - sunScreenPos) * (density / NUM_SAMPLES);  // 每步偏移量
float2 uv    = i.uv;                                               // 从当前像素出发
float3 color = 0;
float  decay_acc = 1.0;   // 衰减累计值（从太阳向外越来越暗）

for (int j = 0; j < 64; j++)
{
    uv -= delta;   // 向太阳方向走一步（注意是减，方向朝太阳）
    float3 s = g_hdr.SampleLevel(g_sampler, saturate(uv), 0).rgb;  // 采样 HDR
    float lum = dot(s, float3(0.2126, 0.7152, 0.0722));
    // 亮度阈值 0.4：只有亮的天空区域才产生 God Ray，暗的海面不产生
    s *= saturate((lum - 0.4) / 0.6);
    color     += s * decay_acc * weight;  // 累加（越靠近太阳贡献越大）
    decay_acc *= decay;   // 每步衰减（一般 0.97 左右）
}
color *= sunVisibility;   // 太阳可见性（在画面外或夜晚时为 0）
```

**水下光柱效果：**

当相机在水下时（`cameraY < 0`），God Ray 保留 50% 强度。水下时，Snell 窗口区域（约 48.6° 锥角内）可以看到天空，这个明亮的圆形区域正好在 God Ray 的采样路径上，自然形成从水面射入水中的光柱效果。

### 9.4 TAA（时间抗锯齿）

```
每帧用 Halton 序列给投影矩阵加一个亚像素偏移（Jitter）
  → 相当于每帧采样位置轻微偏移，16帧循环

ToneMap 后，把结果和历史帧混合：
  output = lerp(history, current, 0.1)  // 10% 新帧 + 90% 历史帧
  
效果：静止时等价于 16×SSAA 超采样，运动时也有相当的效果
代价：快速运动时会有"拖尾"（Ghost），用速度向量 Reproject 历史帧可缓解
```

### 9.5 ACES 色调映射

```hlsl
// ACESFilmic：把 HDR（可能 >1.0）的颜色压缩到 [0,1] 显示范围
// 参数 a=2.51, b=0.03, c=2.43, d=0.59, e=0.14 是 ACES 标准参数
float3 ACESFilmic(float3 x)
{
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}
```

ACES 的特点：高光部分饱和度保持好（不会像线性压缩那样变灰白），中间调对比度明显，低光轻微提升。

---

## 10. 水下渲染系统

### 10.1 水面着色器中的水下分支

当 `cameraPos.y < -0.3f` 时，PSMain 提前返回，进入水下视角的海面渲染：

```hlsl
if (cameraPos.y < -0.3f)
{
    // ---- Snell 窗口（Snell's Window）----
    // 临界角 arcsin(1/1.333) ≈ 48.6°，对应 cos ≈ 0.664
    // NdotV > critCos：视线方向在 Snell 窗口内，能看到折射的天空
    // NdotV < critCos：全内反射（TIR），只能看到水中深蓝散射色
    float critCos = 0.664f;
    // smoothstep 过渡（critCos-0.12 到 critCos+0.08 之间平滑混合）
    float snellT  = smoothstep(critCos - 0.12f, critCos + 0.08f, NdotV);

    // 窗口内：计算折射方向，在天空快照里采样折射后的天空
    float3 refractDir = refract(-V, N, 1.0f / 1.333f);  // 水→空气的折射（n1/n2）
    // 把折射方向延伸，投影回屏幕空间找对应 UV
    float4x4 vp_uw = mul(view, proj);
    float4 rc      = mul(float4(pin.posW + refractDir * 200.0f, 1.0f), vp_uw);
    float2 snellUV = saturate(rc.xy / rc.w * float2(0.5f, -0.5f) + 0.5f);
    snellUV       += N.xz * 0.025f;  // 波面法线扰动（模拟水面折射的扭曲）
    float3 snellSky = g_skySnapshot.SampleLevel(g_sampler, snellUV, 0).rgb;

    // 窗口边缘集光效果（焦点效应使边缘略微更亮）
    float rimBright = 1.0f + smoothstep(critCos - 0.15f, critCos, NdotV) * 0.4f;
    snellSky *= rimBright;

    // TIR 侧颜色：深水中的散射色
    float3 tirColor = float3(0.003f, 0.018f, 0.055f);  // 深蓝

    // 混合：窗口外全内反射色，窗口内折射天空色
    float3 surfaceColor = lerp(tirColor, snellSky, snellT);

    // 相机深度的光吸收（水越深，越暗越蓝）
    float camDepth    = max(0.1f, -cameraPos.y);
    float3 absorbUW   = float3(0.45f, 0.06f, 0.025f);  // 同水面下的吸收系数
    float3 absorption = exp(-absorbUW * camDepth * 0.4f);
    surfaceColor     *= absorption;

    // 水中高光
    float specUW = pow(saturate(dot(N, H)), 64.0f) * 0.2f * sunIntensity;
    surfaceColor += sunColor * specUW * saturate(1.0f - camDepth * 0.1f);

    // 距离雾（水中视程有限，远处消失在深蓝雾中）
    float dist_uw = length(cameraPos - pin.posW);
    float uwFog   = saturate((dist_uw - 15.0f) / 60.0f);  // 15m 开始，75m 饱和
    float3 deepFog = float3(0.004f, 0.018f, 0.05f) * absorption;
    surfaceColor   = lerp(surfaceColor, deepFog, uwFog);

    return float4(surfaceColor, 1.0f);
}
```

### 10.2 色调映射中的水下效果（tonemapping.hlsl）

```hlsl
// uwBlend：0=水面上，1=完全水下（cameraY=-2m 时饱和）
float uwBlend = saturate(-cameraY / 2.0);
float uwDepth = max(0.0, -cameraY);

if (uwBlend > 0.001)
{
    // UV 波形扭曲（模拟从水面看到的折射波动感）
    float2 warp = float2(
        sin(i.uv.y * 22.0 + time * 2.4) * 0.009,
        cos(i.uv.x * 18.0 + time * 1.8) * 0.007
    ) * uwBlend;
    sampleUV = clamp(i.uv + warp, 0.001, 0.999);

    // Beer-Lambert 色彩吸收
    float3 absorb = float3(
        exp(-0.28 * uwDepth),   // 红光衰减最快
        exp(-0.08 * uwDepth),   // 绿光中等
        exp(-0.025 * uwDepth)); // 蓝光衰减最慢
    ldr *= lerp(float3(1,1,1), absorb, uwBlend);

    // 深海雾（越深越偏向暗蓝绿）
    float3 fogColor = float3(0.01, 0.07, 0.18);
    float  fogAmt   = 1.0 - exp(-uwDepth * 0.09);
    ldr = lerp(ldr, fogColor, fogAmt * uwBlend);

    // 焦散（水面光斑，浅水时最强）
    float causticStr = uwBlend * saturate(1.0 - uwDepth * 0.18);
    if (causticStr > 0.001)
    {
        float2 cuv = i.uv * float2(9.0, 11.0);
        // 两个 sin 函数相乘产生不规则光斑纹理
        float c = sin(cuv.x * 2.1 + sin(cuv.y * 1.8 + time * 1.2) * 1.6 + time * 2.0)
                * sin(cuv.y * 2.4 + sin(cuv.x * 1.6 + time * 0.9) * 1.4 + time * 1.6);
        c = pow(saturate(c * 0.5 + 0.5), 4.0);  // 锐化光斑
        ldr += c * 0.28 * causticStr;
    }

    // 水下周边晕染
    float uwVig = smoothstep(0.15, 0.60, vigDist) * 0.5 * uwBlend;
    ldr *= (1.0 - uwVig);

    // 整体蓝绿色调偏移
    float3 uwTint = float3(0.75, 0.92, 1.08);
    ldr *= lerp(float3(1,1,1), uwTint, uwBlend * 0.35);
}
```

---

## 11. 天气系统（WeatherSystem）

### 状态机

```
状态：CALM → WINDY → STORM → WINDY → CALM（循环）
每个状态持续随机时长，平滑过渡（lerp，速度约 0.01/秒）

CALM:   windSpeed=5,  cloudDensity=0.3, rainAmount=0
WINDY:  windSpeed=15, cloudDensity=0.6, rainAmount=0.3
STORM:  windSpeed=25, cloudDensity=0.9, rainAmount=1.0
```

### 参数推送

每帧把当前插值后的参数推给：
- `OceanFFT`：`windSpeed`、`windDirX/Y`、`phillipsA`（控制波高）
- `SkyDome`：`cloudDensity`、`cloudSharpness`、`weatherIntensity`
- `RainSystem`：`rainAmount`（控制粒子生成速率）
- `Renderer`：通过 `fogStart/fogEnd` 控制雾距离（风暴时雾更浓）

---

## 12. 雨粒子系统（RainSystem）

### 粒子数据

```cpp
struct RainParticle
{
    float x, y, z;    // 世界坐标
    float alpha;       // 透明度（接近水面时淡出）
    float speed;       // 下落速度（m/s）
};
```

最多 2000 个活跃粒子，每帧：
1. 新粒子在相机上方随机生成（高度 +30m，半径 25m 的圆柱区域）
2. 每个粒子向下移动 `speed × deltaTime`（受风向偏斜）
3. 到达 y < 0 时销毁，在落点生成一个涟漪

### 着色器中的水面以下剔除

```hlsl
// rain.hlsl
VSOutput VSMain(VSInput vin)
{
    VSOutput vout;
    vout.posH   = mul(float4(vin.position, 1.0f), viewProj);
    vout.alpha  = vin.alpha;
    vout.worldY = vin.position.y;   // 传递世界 Y 坐标
    return vout;
}

float4 PSMain(VSOutput pin) : SV_TARGET
{
    clip(pin.worldY);   // worldY < 0 时 clip()：直接丢弃这个片元
                        // 这样水面以下的雨粒子不会被渲染
    float3 rainColor = float3(0.7f, 0.8f, 1.0f);   // 淡蓝白色
    return float4(rainColor, pin.alpha);
}
```

### 涟漪系统

```cpp
struct Ripple
{
    float2 position;  // XZ 世界坐标
    float  radius;    // 当前半径（随时间扩大）
    float  strength;  // 强度（随时间衰减）
    float  age;       // 存活时间
};
```

最多 200 个活跃涟漪，半径以约 2m/s 扩展，强度线性衰减，持续约 3 秒。

通过 `RippleCB`（寄存器 b1）每帧传给 `shaders.hlsl`，在像素着色器里扰动法线。

---

## 13. 鱼群系统（FishSchool）

### Boids 算法

Boids 是模拟群体行为的经典算法（Reynolds 1987），每条鱼遵守三条规则：

```
1. 分离（Separation）：避免和邻近的鱼太近
2. 对齐（Alignment）：趋向邻近鱼的平均方向
3. 聚合（Cohesion）：趋向邻近鱼的平均位置
```

```cpp
// FishSchool::Update 里的 CPU 并行计算
std::for_each(std::execution::par_unseq, indices.begin(), indices.end(),
    [&](int i)
    {
        // 对每条鱼，找周围 VIEW_RADIUS 内的邻居
        // 计算分离力、对齐力、聚合力
        // 叠加后更新速度和位置
        // 限制速度在 [MIN_SPEED, MAX_SPEED] 之间
        // 转向速度限制在 MAX_TURN_RATE 以内（防止瞬间转向）
    });
```

`std::execution::par_unseq`：并行无序执行（利用 CPU 多核），对 100 条鱼来说效果明显。

### GPU 渲染

鱼的形状是程序化生成的（纺锤形多边形），通过实例化渲染（Instanced Rendering）：
- 每条鱼一个 `FishInstance`（位置 + 方向矩阵）上传到 GPU
- 一次 `DrawIndexedInstanced` 画出所有鱼

---

## 14. 漂浮物体（FloatingObject）

漂浮物体（比如浮标、木桶等）需要跟随海浪高度起伏。

### 高度采样

```cpp
// FloatingObject::Update
// 从 FFT heightMap 读回 CPU（通过 ReadbackBuffer）
// 对物体所在 XZ 坐标双线性插值采样高度
// 更新物体的 Y 坐标（还有倾斜角度，从相邻点的高度差估算法线）
```

### 阴影投影

漂浮物体会产生投影到海面的阴影：
- 建立 Shadow Map Pass：从光源方向渲染漂浮物体深度
- 海面 `shaders.hlsl` 里的 PCF 采样：`g_shadowMap.Load(...)` 3×3 核心采样

---

## 15. 相机系统（Camera）

### WASD + 鼠标控制

```cpp
// Renderer::Update 里
float forward = 0, right = 0;
if (GetAsyncKeyState('W') & 0x8000) forward += speed;   // W: 前进
if (GetAsyncKeyState('S') & 0x8000) forward -= speed;   // S: 后退
if (GetAsyncKeyState('D') & 0x8000) right += speed;     // D: 右移
if (GetAsyncKeyState('A') & 0x8000) right -= speed;     // A: 左移
m_camera.Move(forward, right);  // 在相机朝向的基础上移动

// 鼠标移动（在 OnMouseMove 回调里）
m_camera.ProcessMouse(dx, dy);  // yaw += dx * 灵敏度; pitch += dy * 灵敏度
```

相机支持下潜到水面以下（Y 坐标可以是负数），触发水下渲染系统。

### 展示模式（Showcase）

```cpp
void Camera::UpdateShowcase(float deltaTime)
{
    // 相机绕 (0, 5, 0) 做圆周运动，半径 80m，高度约 10m
    // 目标始终看向中心
    m_showcaseAngle += deltaTime * 0.12f;  // 0.12 rad/s ≈ 每分钟一圈
    position.x = sin(m_showcaseAngle) * 80.0f;
    position.z = cos(m_showcaseAngle) * 80.0f;
    position.y = 10.0f + sin(m_showcaseAngle * 0.5f) * 5.0f;  // 轻微上下浮动
    // LookAt 计算 yaw/pitch
}
```

### TAA Jitter

```cpp
// OnUpdate() 里
int jIdx = (m_jitterIndex % 16) + 1;  // 16 个 Halton 序列位置循环
m_currentJitter.x = (Halton(jIdx, 2) - 0.5f) * 2.0f / m_width;   // 基数 2 的 Halton
m_currentJitter.y = (Halton(jIdx, 3) - 0.5f) * 2.0f / m_height;  // 基数 3 的 Halton
m_renderer->SetJitter(m_currentJitter.x, m_currentJitter.y);  // 设置 proj 矩阵偏移
```

Halton 序列是一种低差异序列，比随机数分布更均匀，TAA 效果更好。

---

## 16. 数据流全图

```
                     ┌─────────────────────────────────────────────┐
                     │             每帧 OnUpdate()                  │
                     │                                              │
  WeatherSystem ─────┼──→ windSpeed/windDir ──→ OceanFFT           │
       │             │                    └──→ SkyDome（云量）      │
       └─────────────┼──→ fogStart/fogEnd ──→ Renderer::SceneCB    │
                     │  └──→ rainAmount ──→ RainSystem              │
                     └─────────────────────────────────────────────┘

                     ┌─────────────────────────────────────────────┐
                     │             每帧 OnRender()                  │
                     │                                              │
  OceanFFT::Dispatch │                                              │
    PhillipsCS ──→ h0Map ──→ TimeEvolutionCS ──→ hktMap, dztMap    │
                               ↓                                    │
    IFFTCS (×16) ──→ m_heightMap（.x=高度, .z=Dx）                 │
    IFFTCS (×16) ──→ m_dztMap（.x=Dz）                             │
                               ↓                                    │
  ┌── shaders.hlsl VSMain ─────┘                                    │
  │   读 heightMap/dztMap → 顶点位移（含 tileOffset 瓦片偏移）      │
  │                                                                  │
  │   shaders.hlsl PSMain：                                         │
  │   法线(Jacobian) + Fresnel + 反射(SSR) + 透射(Beer-Lambert)     │
  │   + 高光 + SSS + 泡沫 + 阴影 + 雾 → HDR RT                     │
  │                                                                  │
  │   SkyDome → HDR RT                                              │
  │   RainSystem（clip(y) 防穿水面）→ HDR RT                        │
  │   FishSchool → HDR RT                                           │
  │   FloatingObject（阴影投射）→ HDR RT                            │
  │                                                                  │
  │   HDR RT → SRV（屏障切换）                                      │
  │                                                                  │
  └── PostProcessPipeline：                                         │
      SSR → Shadow → SSAO → Bloom → GodRays → LensFlare            │
      → DOF → ToneMap(ACES + 水下效果) → TAA → SwapChain           │
                     └─────────────────────────────────────────────┘
```

---

## 附录：快速参考

### 着色器编译

着色器用 VS2022 自动编译（`.hlsl` → `.cso`），输出在 `bin/x64/Debug/` 目录下，和 exe 同级。修改 `.hlsl` 后需要重新构建才生效。

### 关键数字汇总

| 参数 | 值 | 含义 |
|------|-----|-----|
| 海面瓦片数 | 9×9=81 最多 | TILE_RADIUS=4 |
| 瓦片大小 | 400m × 400m | GRID_WORLD_SIZE |
| 高分辨率网格 | 512×512 | 中心 5×5 格 |
| 低分辨率网格 | 64×64 | 外围格 |
| FFT 分辨率 | 256×256 | textureSize |
| 雨粒子上限 | 2000 | MAX_PARTICLES |
| 涟漪上限 | 200 | MAX_RIPPLES |
| 鱼的数量 | 约 100 | FishSchool |
| God Ray 采样数 | 64 | NUM_SAMPLES |
| TAA 历史帧混合 | 10% 新帧 | lerp 系数 |
| Snell 临界角 cos | 0.664 | arcsin(1/1.333) |
| 水体红光吸收 | 0.45/m | absorbCoeff.r |
| 水体蓝光吸收 | 0.025/m | absorbCoeff.b |

### 常见问题

**Q：修改了着色器但没有效果？**
A：需要重新构建（MSBuild）让 HLSL 重新编译成 CSO，运行时读的是 CSO 文件。

**Q：帧率低怎么办？**
A：在 ImGui 面板里关闭 SSAO、TAA，或降低 TILE_RADIUS，或把 FFT 分辨率从 256 降到 128。

**Q：`SceneCB` 的 HLSL 和 C++ 哪里声明？**
A：C++ 侧在 `Renderer.h` 的 `SceneCB` 结构体；HLSL 侧在 `shaders.hlsl` 顶部的 `cbuffer SceneCB`。两侧的字节偏移必须完全一致（任何一侧改动后另一侧也要同步）。

**Q：`tileOffset` 有什么作用？**
A：它是无限海面系统的核心字段。Renderer 给每个瓦片写不同的 `tileOffset`（如 tx×400, tz×400），HLSL 用它计算世界坐标 XZ，使 Gerstner 波在相邻瓦片接缝处值连续，并将顶点正确放置在世界空间中。
