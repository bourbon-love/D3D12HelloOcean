// ============================================================
// D3D12HelloTriangle.h



// ============================================================
#pragma once

#include "DXSample.h"
#include <wrl/client.h>
#include "source/Renderer.h"
#include <memory>
#include <DirectXMath.h>
#include <d3dx12_core.h>
#include <dxgi1_6.h>
#include <chrono>
#include "source/SkyDome.h"
#include "source/OceanFFT.h"
#include "source/WeatherSystem.h"
#include "source/RainSystem.h"
#include "source/FloatingObject.h"
#include "source/FishSchool.h"
#include "source/PostProcessPipeline.h"
#include "source/VolumetricClouds.h"
#include "source/RainbowEffect.h"
#include "source/ShipModel.h"
#include "source/IBLSystem.h"
#include "ImGUI/imgui.h"
#include "ImGUI/imgui_impl_win32.h"
#include "ImGUI/imgui_impl_dx12.h"

using namespace DirectX;
using Microsoft::WRL::ComPtr;

class D3D12HelloTriangle : public DXSample
{
public:
    D3D12HelloTriangle(UINT width, UINT height, std::wstring name);

    virtual void OnInit();
    virtual void OnUpdate();
    virtual void OnRender();
    virtual void OnDestroy();
    virtual void OnMouseMove(float dx, float dy) override;
    void OnKeyDown(UINT8 key) override;

private:
    static const UINT FrameCount = 2;


    std::unique_ptr<Renderer>           m_renderer;
    std::unique_ptr<SkyDome>            m_skyDome;
    std::unique_ptr<OceanFFT>           m_oceanFFT;
    std::unique_ptr<WeatherSystem>      m_weatherSystem;
    std::unique_ptr<RainSystem>         m_rainSystem;
    std::unique_ptr<FloatingObject>     m_floatingObject;
    std::unique_ptr<FishSchool>         m_fishSchool;
    std::unique_ptr<PostProcessPipeline>  m_pp;
    std::unique_ptr<VolumetricClouds>     m_volumetricClouds;
    std::unique_ptr<RainbowEffect>        m_rainbowEffect;
    std::unique_ptr<ShipModel>           m_shipModel;
    std::unique_ptr<IBLSystem>           m_iblSystem;

    std::chrono::steady_clock::time_point m_lastTime;


    CD3DX12_VIEWPORT               m_viewport;
    CD3DX12_RECT                   m_scissorRect;
    ComPtr<IDXGISwapChain3>        m_swapChain;
    ComPtr<ID3D12Device>           m_device;
    ComPtr<ID3D12Resource>         m_renderTargets[FrameCount];
    ComPtr<ID3D12CommandAllocator> m_commandAllocator;
    ComPtr<ID3D12CommandQueue>     m_commandQueue;
    ComPtr<ID3D12RootSignature>    m_rootSignature;
    ComPtr<ID3D12DescriptorHeap>   m_rtvHeap;
    ComPtr<ID3D12DescriptorHeap>   m_imguiSrvHeap;
    ComPtr<ID3D12GraphicsCommandList> m_commandList;
    UINT m_rtvDescriptorSize = 0;


    UINT   m_frameIndex  = 0;
    HANDLE m_fenceEvent  = nullptr;
    ComPtr<ID3D12Fence> m_fence;
    UINT64 m_fenceValue  = 0;


    float m_timeScale   = 1.0f;
    bool  m_timePaused  = false;
    bool  m_autoExposure            = true;
    bool  m_autoExposureInitialized = false;
    bool  m_showIBLDebug            = false;  // toggle in "Post Process" panel


    int      m_jitterIndex   = 0;
    XMFLOAT2 m_currentJitter = { 0.0f, 0.0f };


    float m_prevLightningIntensity = 0.0f;
    float m_rainMoisture           = 0.0f;
    float m_rainbowCloudFactor     = 1.0f; // 0=overcast/no rainbow, 1=clear sky
    bool  m_autoCloudCoverage      = true;  // weather drives VolumetricClouds coverage

    void LoadPipeline();
    void LoadAssets();
    void WaitForPreviousFrame();
    void BuildImGuiUI();
};
