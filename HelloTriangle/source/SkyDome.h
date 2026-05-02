// ============================================================
// SkyDome.h
// 天空球描画クラス。太陽・月の軌道追跡、稲妻ステートマシン、
// 動的空カラーパレットを管理する。
// ============================================================
#pragma once
#include <d3d12.h>
#include <DirectXMath.h>
#include <wrl.h>
#include <vector>
#include <d3dx12_core.h>
#include "../DXSampleHelper.h"
#include "renderer/RendererContext.h"
#include <algorithm>

using namespace DirectX;
using Microsoft::WRL::ComPtr;
// 辅助函数
inline float saturate(float v) { return std::clamp(v, 0.0f, 1.0f); }

class SkyDome
{
public:
    void InitPSO(
        ComPtr<ID3D12Device>        device,
        ComPtr<ID3D12RootSignature> rootSignature,
        UINT width, UINT height,
        const UINT8* vsData, UINT vsSize,
        const UINT8* psData, UINT psSize);

    void InitResources(ComPtr<ID3D12GraphicsCommandList> cmdList);
    void Update(float deltaTime);
    void Render(RenderContext& ctx);

    // 太阳方向供海浪Shader读取
    XMFLOAT3 GetSunDirection() const { return m_sunDir; }
    float GetSunIntensity() const;
	XMFLOAT3 GetSunColor() const;
    XMFLOAT3 GetSkyColor() const;

	// 月亮和参数供天气系统读取
    bool     IsDaytime()          const { return m_sunDir.y > -0.1f; }
    XMFLOAT3 GetMoonDirection()   const { return m_moonDir; }
    float    GetMoonIntensity()   const { return 0.15f; }
    XMFLOAT3 GetMoonColor()       const { return XMFLOAT3(0.6f, 0.7f, 1.0f); }
    // 供天气系统设置
    void SetCloudParams(float density, float scale, float sharpness)
    {
        m_cloudDensity = density;
        m_cloudScale = scale;
        m_cloudSharpness = sharpness;
    }
    void SetWeatherIntensity(float intensity) { m_weatherIntensity = intensity; }
    void SetShowcaseMode(bool showcase) { m_showcaseMode = showcase; }
    void SetWindDir(float x, float y) { m_windDirX = x; m_windDirY = y; }

    float GetLightningIntensity() const { return m_lightningIntensity; }

    // 月亮参数（供 ImGui 読み書き）
    float GetCrescentRotSpeed()  const { return m_crescentRotSpeed; }
    float GetMoonBodyPow()       const { return m_moonBodyPow; }
    float GetMoonOccludePow()    const { return m_moonOccludePow; }
    float GetCrescentOffsetAmt() const { return m_crescentOffsetAmt; }
    void SetCrescentRotSpeed(float v)  { m_crescentRotSpeed  = v; }
    void SetMoonBodyPow(float v)       { m_moonBodyPow       = v; }
    void SetMoonOccludePow(float v)    { m_moonOccludePow    = v; }
    void SetCrescentOffsetAmt(float v) { m_crescentOffsetAmt = v; }


private:
    void CreateSphereMesh(ComPtr<ID3D12GraphicsCommandList> cmdList);
    void CreateSkyPSO(const UINT8* vsData, UINT vsSize,
        const UINT8* psData, UINT psSize);
    void CreateSkyDepthState();

    // 天空CBV结构，和skyshader.hlsl的cbuffer完全对应
    // 注意DX12要求CBV大小是256字节的倍数
    struct __declspec(align(256)) SkyCB
    {
        XMMATRIX  viewProj;       // 64字节
        XMFLOAT4  topColor;       // 16字节
        XMFLOAT4  middleColor;    // 16字节
        XMFLOAT4  bottomColor;    // 16字节
        XMFLOAT3  sunPosition;    // 12字节
        float     time;           // 4字节
        float     cloudDensity;   // 4字节
        float     cloudScale;     // 4字节
        float     cloudSharpness; // 4字节
        float     weatherIntensity;
        XMFLOAT3  sunColor;
        float     padSunColor;
        XMFLOAT3  moonPosition;
        float     padMoon;
        XMFLOAT3  moonCrescentDir; // 独立月牙朝向，每帧缓慢旋转
        float     padCrescent;
        float     moonBodyPow;       // 月盘大小（pow 指数，越大越小）
        float     moonOccludePow;    // 遮挡圆大小（pow 指数）
        float     crescentOffsetAmt; // 月牙偏移量
        float     padMoonParams;
        float     lightningIntensity;
        float     cloudDriftX;   // wind X * speed * time (cloud movement offset)
        float     cloudDriftY;   // wind Z * speed * time
        float     padLightning;
    };
    // 总计 = 64+16+16+16+12+4+4+4+4+4 = 144字节
    // __declspec(align(256))保证整个结构体从256字节对齐的地址开始

    ComPtr<ID3D12Device>              m_device;
    ComPtr<ID3D12RootSignature>       m_rootSignature;
    ComPtr<ID3D12PipelineState>       m_skyPSO;

    // 球体几何
    ComPtr<ID3D12Resource>            m_vb;
    ComPtr<ID3D12Resource>            m_vbUpload;
    ComPtr<ID3D12Resource>            m_ib;
    ComPtr<ID3D12Resource>            m_ibUpload;
    D3D12_VERTEX_BUFFER_VIEW          m_vbView = {};
    D3D12_INDEX_BUFFER_VIEW           m_ibView = {};
    UINT                              m_indexCount = 0;

    // CBV
    ComPtr<ID3D12Resource>            m_cbuffer;
    UINT8* m_cbMapped = nullptr;

    // 天空参数
    float     m_time = 0.0f;
    XMFLOAT3  m_sunDir = { 1.0f, 0.0f, 0.0f };
	XMFLOAT3  m_moonDir = { -1.0f, 0.2f, 0.0f };
    XMFLOAT3  m_crescentDir = { 0.0f, 0.0f, 1.0f }; // 月牙遮挡圆方向，绕月亮轴缓慢旋转
    float     m_crescentRotSpeed  = 0.07f;
    float     m_moonBodyPow       = 1000.0f;
    float     m_moonOccludePow    = 1300.0f;
    float     m_crescentOffsetAmt = 0.012f;
    float     m_cloudDensity = 0.5f;
    float     m_cloudScale = 0.85f;
    float     m_cloudSharpness = 0.6f;
    bool      m_showcaseMode = false;
    float m_weatherIntensity = 0.0f; // 0=晴天, 1=暴风
    float m_windDirX = 1.0f;  // cloud drift wind direction X
    float m_windDirY = 0.0f;  // cloud drift wind direction Z
    float m_lightningIntensity = 0.0f;
    float m_lightningCooldown  = 3.0f; // 首次触发前等待时间
    UINT m_width = 0;
    UINT m_height = 0;
};