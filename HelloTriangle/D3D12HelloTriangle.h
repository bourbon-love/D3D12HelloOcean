// ============================================================
// D3D12HelloTriangle.h
// アプリケーションメインクラス。DX12デバイス・スワップチェーン管理、
// HDRパイプライン（Bloom/GodRay/DOF/LensFlare/SSR/ToneMap）、
// ImGUI制御を担当する。
// ============================================================
//*********************************************************
//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
//*********************************************************

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
#include "ImGUI/imgui.h"
#include "ImGUI/imgui_impl_win32.h"
#include "ImGUI/imgui_impl_dx12.h"

using namespace DirectX;

// Note that while ComPtr is used to manage the lifetime of resources on the CPU,
// it has no understanding of the lifetime of resources on the GPU. Apps must account
// for the GPU lifetime of resources to avoid destroying objects that may still be
// referenced by the GPU.
// An example of this can be found in the class method: OnDestroy().
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
    std::unique_ptr<Renderer> m_renderer;
    std::unique_ptr<SkyDome>  m_skyDome;
    std::unique_ptr<OceanFFT> m_oceanFFT;
    std::unique_ptr<WeatherSystem> m_weatherSystem;
    std::unique_ptr<RainSystem>      m_rainSystem;
    std::unique_ptr<FloatingObject>  m_floatingObject;

    std::chrono::steady_clock::time_point m_lastTime;
   

    struct SceneCB
    {
        XMMATRIX view;
        XMMATRIX proj;
    };

    // Pipeline objects.
    CD3DX12_VIEWPORT m_viewport;
    CD3DX12_RECT m_scissorRect;
    ComPtr<IDXGISwapChain3> m_swapChain;
    ComPtr<ID3D12Device> m_device;
    ComPtr<ID3D12Resource> m_renderTargets[FrameCount];
    ComPtr<ID3D12CommandAllocator> m_commandAllocator;
    ComPtr<ID3D12CommandQueue> m_commandQueue;
    ComPtr<ID3D12RootSignature> m_rootSignature;
    ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    ComPtr<ID3D12DescriptorHeap> m_imguiSrvHeap;
    ComPtr<ID3D12GraphicsCommandList> m_commandList;
    UINT m_rtvDescriptorSize;

    // HDR render target
    ComPtr<ID3D12Resource>       m_hdrRT;
    ComPtr<ID3D12RootSignature>  m_toneMappingRootSig;
    ComPtr<ID3D12PipelineState>  m_toneMappingPSO;
    float m_exposure = 1.0f;

    // God rays
    ComPtr<ID3D12Resource>       m_godRayRT;
    ComPtr<ID3D12RootSignature>  m_godRayRootSig;
    ComPtr<ID3D12PipelineState>  m_godRayPSO;
    bool  m_godRaysEnabled  = true;
    float m_godRayStrength  = 1.0f;

    // Lens flare
    ComPtr<ID3D12RootSignature>  m_lensFlareRootSig;
    ComPtr<ID3D12PipelineState>  m_lensFlarePSO;
    bool  m_lensFlareEnabled = true;
    float m_lensFlareStrength = 1.0f;

    // Depth of Field
    ComPtr<ID3D12Resource>       m_dofRT;
    ComPtr<ID3D12DescriptorHeap> m_dofSRVHeap;   // [0]=hdrRT  [1]=depthBuffer
    ComPtr<ID3D12RootSignature>  m_dofRootSig;
    ComPtr<ID3D12PipelineState>  m_dofPSO;
    UINT                         m_dofSRVIncrSize = 0;
    bool  m_dofEnabled    = false;
    float m_dofFocusDepth = 0.92f;  // NDC depth [0,1] to focus on
    float m_dofFocusRange = 0.12f;  // half-width of sharp zone in NDC
    float m_dofMaxRadius  = 0.010f; // max CoC radius in UV space

    // SSAO
    struct alignas(256) SSAOCB
    {
        XMFLOAT4 kernel[8]; // 128 bytes
        float screenW, screenH, nearZ, farZ;
        float radius, bias, projX, projY;   // 32 bytes
        float pad[24];                       // 96 bytes → total 256
    };
    static_assert(sizeof(SSAOCB) == 256, "SSAOCB must be 256 bytes");

    ComPtr<ID3D12Resource>       m_ssaoRT;
    ComPtr<ID3D12Resource>       m_ssaoBlurRT;
    ComPtr<ID3D12DescriptorHeap> m_ssaoSRVHeap;   // [0]=depth [1]=ssaoRT(for blur)
    ComPtr<ID3D12RootSignature>  m_ssaoRootSig;
    ComPtr<ID3D12RootSignature>  m_ssaoBlurRootSig;
    ComPtr<ID3D12PipelineState>  m_ssaoPSO;
    ComPtr<ID3D12PipelineState>  m_ssaoBlurPSO;
    ComPtr<ID3D12Resource>       m_ssaoCB;
    UINT                         m_ssaoSRVIncrSize = 0;
    bool                         m_ssaoEnabled  = true;
    float                        m_ssaoStrength = 0.8f;
    float                        m_ssaoRadius   = 0.5f;
    SSAOCB*                      m_mappedSSAOCB = nullptr;

    void InitSSAO();
    void RenderSSAO();

    // Lightning bolt overlay
    struct LightningCB
    {
        XMFLOAT4 pts[13];    // [0..numBolt-1]=main, [numBolt..]=branch
        float    intensity;
        float    aspect;
        int      numBolt;
        int      numBranch;
        float    pad[8];     // total 256 bytes
    };
    static_assert(sizeof(LightningCB) == 256, "LightningCB must be 256 bytes");

    ComPtr<ID3D12Resource>      m_lightningCB;
    ComPtr<ID3D12RootSignature> m_lightningRootSig;
    ComPtr<ID3D12PipelineState> m_lightningPSO;
    LightningCB*                m_mappedLightningCB = nullptr;
    float                       m_prevLightningIntensity = 0.0f;

    void InitLightning();
    void RenderLightning();
    void GenerateLightningBolt();

    // TAA
    ComPtr<ID3D12Resource>       m_taaRT;
    ComPtr<ID3D12Resource>       m_taaHistoryRT;
    ComPtr<ID3D12DescriptorHeap> m_taaSRVHeap;
    ComPtr<ID3D12RootSignature>  m_taaRootSig;
    ComPtr<ID3D12PipelineState>  m_taaPSO;
    UINT  m_taaSRVIncrSize  = 0;
    bool  m_taaEnabled      = true;
    float m_taaBlend        = 0.9f;
    int   m_jitterIndex     = 0;
    bool  m_taaHistoryInPSR = false;
    bool  m_taaHistoryValid = false;
    XMFLOAT2 m_currentJitter = { 0.0f, 0.0f };

    // SSR
    ComPtr<ID3D12Resource>       m_skySnapshotRT;
    ComPtr<ID3D12DescriptorHeap> m_oceanSRVHeap;
    UINT                         m_oceanSRVIncrSize = 0;
    bool                         m_skySnapshotInPSR = false;
    float                        m_ssrStrength = 1.0f;

    // Shadow map
    static const UINT kShadowSize = 2048;
    struct alignas(256) ShadowSceneCB
    {
        XMMATRIX lightViewProj;  // 64
        float    shadowBias;     //  4
        float    shadowStrength; //  4
        float    shadowEnabled;  //  4
        float    pad;            //  4
        float    pad2[44];       // 176  → total 256
    };
    static_assert(sizeof(ShadowSceneCB) == 256, "ShadowSceneCB must be 256 bytes");

    ComPtr<ID3D12Resource>       m_shadowMap;
    ComPtr<ID3D12DescriptorHeap> m_shadowDSVHeap;
    ComPtr<ID3D12RootSignature>  m_shadowRootSig;
    ComPtr<ID3D12PipelineState>  m_shadowPSO;
    ComPtr<ID3D12Resource>       m_shadowSceneCB;
    UINT8*                       m_mappedShadowSceneCB = nullptr;
    XMMATRIX                     m_lightViewProj;
    bool                         m_shadowEnabled  = true;
    float                        m_shadowStrength = 0.75f;
    float                        m_shadowBias     = 0.002f;

    void InitShadowMap();
    void RenderShadowMap();

    // Bloom post-process
    ComPtr<ID3D12Resource>       m_bloomExtractRT;
    ComPtr<ID3D12Resource>       m_bloomBlurRT;
    ComPtr<ID3D12DescriptorHeap> m_bloomSRVHeap;
    ComPtr<ID3D12RootSignature>  m_bloomRootSig;
    ComPtr<ID3D12PipelineState>  m_bloomBrightPSO;
    ComPtr<ID3D12PipelineState>  m_bloomBlurPSO;
    UINT  m_bloomSRVIncrSize = 0;
    bool  m_bloomEnabled     = true;
    float m_bloomThreshold   = 1.0f;
    float m_bloomStrength    = 0.8f;

 
    //ConstantBuffer m_cbData;
    // Synchronization objects.
    UINT m_frameIndex;
    HANDLE m_fenceEvent;
    ComPtr<ID3D12Fence> m_fence;
    UINT64 m_fenceValue;


    // UI 状态
    float m_timeScale   = 1.0f;
    bool  m_timePaused  = false;

    // Camera post-processing
    bool  m_autoExposure     = true;
    float m_vignetteStrength = 0.45f;
    float m_grainStrength    = 0.018f;

    void LoadPipeline();
    void LoadAssets();
    void PopulateCommandList();
    void WaitForPreviousFrame();
    void BuildImGuiUI();
    void InitBloom();
    void InitHDR();
    void InitGodRays();
    void InitLensFlare();
    void InitSSR();
    void InitDOF();
    void InitTAA();
    void RenderBloom();
    void RenderDOF();
    void RenderGodRays();
    void RenderToneMap();
    void RenderLensFlare();
    void RenderTAA();
};
