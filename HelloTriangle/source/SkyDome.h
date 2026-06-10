// ============================================================
// SkyDome.h
// Sky sphere rendering class. Manages sun/moon orbital tracking,
// lightning state machine, and dynamic sky color palette.
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
// Helper function
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

    // Sun direction for ocean wave shader to read
    XMFLOAT3 GetSunDirection() const { return m_sunDir; }
    float GetSunIntensity() const;
	XMFLOAT3 GetSunColor() const;
    XMFLOAT3 GetSkyColor() const;

	// Moon and parameters for the weather system to read
    bool     IsDaytime()          const { return m_sunDir.y > -0.1f; }
    XMFLOAT3 GetMoonDirection()   const { return m_moonDir; }
    float    GetMoonIntensity()   const { return 0.15f; }
    XMFLOAT3 GetMoonColor()       const { return XMFLOAT3(0.6f, 0.7f, 1.0f); }

    // ---- Unified single light source (sun/moon blended by sun elevation) ----
    // All directly-lit surfaces (ocean, ship, floating objects, fish, ...) should pull
    // their light from these three getters instead of combining GetSun*/GetMoon*
    // themselves. A single shared blend keeps every surface in sync and prevents two
    // simultaneously-active lights from opposite directions (sun + moon) from producing
    // conflicting highlights at dusk/dawn — see the ship PBR fix for the symptom this
    // caused (moonlight leaking onto the sunlit side of the hull at full daylight,
    // because GetMoonIntensity() never faded with the moon's elevation).
    // Effects that are inherently sun-only (rainbow, lens flare, god rays, weather
    // state) should keep reading GetSunDirection/Intensity/Color directly.
    //
    // These return exponentially-smoothed values (0.50s real-time constant).
    // Call SmoothLighting(realDt) each frame before reading them.
    XMFLOAT3 GetActiveLightDirection() const;
    XMFLOAT3 GetActiveLightColor()     const;
    float    GetActiveLightIntensity() const;

    // Must be called each frame with real (unscaled) delta time, after Update().
    // Applies an exponential filter so fast simulation time-scales do not cause
    // the sunset colour to snap abruptly to moonlight within a single frame.
    void SmoothLighting(float realDt);
    // Setters for weather system
    void SetCloudParams(float density, float scale, float sharpness)
    {
        m_cloudDensity = density;
        m_cloudScale = scale;
        m_cloudSharpness = sharpness;
    }
    void SetWeatherIntensity(float intensity) { m_weatherIntensity = intensity; }
    void SetShowcaseMode(bool showcase) { m_showcaseMode = showcase; }
    void SetWindDir(float x, float y) { m_windDirX = x; m_windDirY = y; }
    void SetCameraY(float y)          { m_cameraY = y; }

    float GetLightningIntensity() const { return m_lightningIntensity; }

    // Snapshot of all fields IBLSystem needs to populate CaptureCB.
    // Mirrors the gradient computation inside UpdateCB so both stay in sync.
    struct CaptureState
    {
        XMFLOAT3 topColor, middleColor, bottomColor;
        XMFLOAT3 sunPosition;
        float    time;
        XMFLOAT3 sunColor;
        float    lightningIntensity;
        XMFLOAT3 moonPosition;
        float    cameraY;
        XMFLOAT3 moonCrescentDir;
        float    moonBodyPow;
        float    moonOccludePow;
        float    crescentOffsetAmt;
    };
    CaptureState GetCaptureState() const;

    // Moon parameters (read/write by ImGui)
    float GetCrescentRotSpeed()  const { return m_crescentRotSpeed; }
    float GetMoonBodyPow()       const { return m_moonBodyPow; }
    float GetMoonOccludePow()    const { return m_moonOccludePow; }
    float GetCrescentOffsetAmt() const { return m_crescentOffsetAmt; }
    void SetCrescentRotSpeed(float v)  { m_crescentRotSpeed  = v; }
    void SetMoonBodyPow(float v)       { m_moonBodyPow       = v; }
    void SetMoonOccludePow(float v)    { m_moonOccludePow    = v; }
    void SetCrescentOffsetAmt(float v) { m_crescentOffsetAmt = v; }


private:
    // Day/night blend factor: 0 = sun fully below horizon (moon active), 1 = sun fully
    // risen (sun active); smoothly transitions over sunDir.y in [-0.25, +0.20].
    // Shared by all three GetActiveLight*() getters so they always agree.
    float    ComputeDayBlend()               const;
    // Raw (instant) active-light values used by SmoothLighting to compute targets.
    XMFLOAT3 RawActiveLightColor()           const;
    XMFLOAT3 RawActiveLightDirection()       const;
    float    RawActiveLightIntensity()       const;

    void CreateSphereMesh(ComPtr<ID3D12GraphicsCommandList> cmdList);
    void CreateSkyPSO(const UINT8* vsData, UINT vsSize,
        const UINT8* psData, UINT psSize);
    void CreateSkyDepthState();

    // Sky CBV structure, exactly matching the cbuffer in skyshader.hlsl
    // Note: DX12 requires CBV size to be a multiple of 256 bytes
    struct __declspec(align(256)) SkyCB
    {
        XMMATRIX  viewProj;       // 64 bytes
        XMFLOAT4  topColor;       // 16 bytes
        XMFLOAT4  middleColor;    // 16 bytes
        XMFLOAT4  bottomColor;    // 16 bytes
        XMFLOAT3  sunPosition;    // 12 bytes
        float     time;           // 4 bytes
        float     cloudDensity;   // 4 bytes
        float     cloudScale;     // 4 bytes
        float     cloudSharpness; // 4 bytes
        float     weatherIntensity;
        XMFLOAT3  sunColor;
        float     padSunColor;
        XMFLOAT3  moonPosition;
        float     padMoon;
        XMFLOAT3  moonCrescentDir; // Independent crescent direction, slowly rotating each frame
        float     padCrescent;
        float     moonBodyPow;       // Moon disc size (pow exponent; higher = smaller)
        float     moonOccludePow;    // Occlusion circle size (pow exponent)
        float     crescentOffsetAmt; // Crescent offset amount
        float     padMoonParams;
        float     lightningIntensity;
        float     cloudDriftX;   // wind X * speed * time (cloud movement offset)
        float     cloudDriftY;   // wind Z * speed * time
        float     padLightning;
        float     cameraY;       // world Y of camera (negative = underwater)
        float     padCam1;
        float     padCam2;
        float     padCam3;
        float     _pad256[4];   // explicit padding to reach 256 bytes (240 + 16)
    };
    // Total = 64+16+16+16+12+4+4+4+4+4 = 144 bytes
    // __declspec(align(256)) ensures the entire struct starts at a 256-byte aligned address

    ComPtr<ID3D12Device>              m_device;
    ComPtr<ID3D12RootSignature>       m_rootSignature;
    ComPtr<ID3D12PipelineState>       m_skyPSO;

    // Sphere geometry
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

    // Sky parameters
    // Start at sunDir.y ~= -0.43 (below the [-0.25, +0.20] day/night blend band, sun
    // ascending). ComputeDayBlend() == 0 here, so RawActiveLightDirection() == m_moonDir
    // exactly on frame 0 -- avoids a mid-blend snap that made SmoothLighting() sweep
    // the ocean's specular highlight toward the sun during the first few seconds.
    float     m_time = 17.5f;
    XMFLOAT3  m_sunDir = { 1.0f, 0.0f, 0.0f };
	XMFLOAT3  m_moonDir = { -1.0f, 0.2f, 0.0f };
    XMFLOAT3  m_crescentDir = { 0.0f, 0.0f, 1.0f }; // Crescent occlusion circle direction, slowly rotates around the moon axis
    float     m_crescentRotSpeed  = 0.07f;
    float     m_moonBodyPow       = 1000.0f;
    float     m_moonOccludePow    = 1300.0f;
    float     m_crescentOffsetAmt = 0.012f;
    float     m_cloudDensity = 0.5f;
    float     m_cloudScale = 0.85f;
    float     m_cloudSharpness = 0.6f;
    bool      m_showcaseMode = false;
    float m_weatherIntensity = 0.0f; // 0=clear, 1=storm
    float m_windDirX = 1.0f;  // cloud drift wind direction X
    float m_windDirY = 0.0f;  // cloud drift wind direction Z
    float m_lightningIntensity = 0.0f;
    float m_cameraY            = 0.0f;
    float m_lightningCooldown  = 3.0f; // Wait time before the first trigger
    UINT m_width = 0;
    UINT m_height = 0;

    // Smoothed active-light cache — updated by SmoothLighting(), read by GetActiveLight*().
    XMFLOAT3 m_smoothLightColor     = { 1.0f, 0.95f, 0.8f };
    float    m_smoothLightIntensity = 0.8f;
    XMFLOAT3 m_smoothLightDir       = { 0.5f, 0.8f, 0.3f };
    bool     m_smoothInitialized    = false;
};