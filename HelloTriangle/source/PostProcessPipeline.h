#pragma once
#include <wrl/client.h>
#include <d3d12.h>
#include <DirectXMath.h>
#include <d3dx12_core.h>
#include <string>
#include "IBLSystem.h"

using Microsoft::WRL::ComPtr;
using namespace DirectX;

class Renderer;
class SkyDome;
class FloatingObject;
class ShipModel;

class PostProcessPipeline
{
public:
    // --- UI params (read/write by BuildImGuiUI) ---
    float  exposure          = 1.0f;
    bool   bloomEnabled      = true;
    float  bloomThreshold    = 1.0f;
    float  bloomStrength     = 0.8f;
    bool   godRaysEnabled    = true;
    float  godRayStrength    = 1.0f;
    bool   lensFlareEnabled  = true;
    float  lensFlareStrength = 1.0f;
    bool   dofEnabled        = false;
    float  dofFocusDepth     = 0.90f;
    float  dofFocusRange     = 0.04f;
    float  dofMaxRadius      = 0.018f;
    bool   ssaoEnabled       = true;
    float  ssaoStrength      = 0.8f;
    float  ssaoRadius        = 0.5f;
    bool   taaEnabled        = true;
    float  taaBlend          = 0.9f;
    float  ssrStrength       = 1.0f;
    float  vignetteStrength  = 0.45f;
    float  grainStrength     = 0.018f;
    bool   shadowEnabled     = true;
    float  shadowStrength    = 0.75f;
    float  shadowBias        = 0.002f;
    float  waterBodyStr      = 1.0f;
    float  waterRefract      = 0.018f;
    float  waterMinTrans     = 0.25f;

    // Phase 1: device + resolution only
    void Init(
        ComPtr<ID3D12Device> device,
        ID3D12DescriptorHeap* rtvHeap,
        UINT  rtvIncrSize,
        UINT  rtvBase,
        UINT  width,
        UINT  height,
        const std::wstring& assetDir);

    // Phase 2: after scene resources (depth, FFT maps, floating objects) are ready
    void InitSceneResources(
        ID3D12Resource* depthBuffer,
        ID3D12Resource* heightMap,
        ID3D12Resource* dztMap,
        FloatingObject* fo,
        IBLSystem*      ibl);  // for ocean SRV heap IBL slots (prefilter t5, brdfLUT t6)

    // Scene-side accessors
    D3D12_CPU_DESCRIPTOR_HANDLE GetHDRRTV()            const;
    ID3D12Resource*             GetHDRRT()             const { return m_hdrRT.Get(); }
    ID3D12DescriptorHeap*       GetOceanSRVHeap()      const { return m_oceanSRVHeap.Get(); }
    D3D12_GPU_VIRTUAL_ADDRESS   GetShadowSceneCBAddr() const;

    // Shadow map pass (before scene)
    void RenderShadowMap(
        ID3D12GraphicsCommandList* cmd,
        SkyDome*        skyDome,
        Renderer*       renderer,
        FloatingObject* fo,
        ShipModel*      ship = nullptr);

    // SSR snapshots
    void TakeSkySnapshot(ID3D12GraphicsCommandList* cmd);
    void TakeRefractionSnapshot(ID3D12GraphicsCommandList* cmd);

    // Lightning overlay (writes to HDR RT, during scene pass)
    void RenderLightning(ID3D12GraphicsCommandList* cmd);

    // SSAO (during scene pass)
    void RenderSSAO(ID3D12GraphicsCommandList* cmd, Renderer* renderer);

    // All post-process passes (after scene is complete)
    void RenderPostProcess(
        ID3D12GraphicsCommandList* cmd,
        UINT                        frameIndex,
        D3D12_CPU_DESCRIPTOR_HANDLE swapRTV,
        Renderer*                   renderer,
        SkyDome*                    skyDome);

    // Called from OnUpdate
    void UpdateLightning(float intensity, bool newStrike);
    void GenerateLightningBolt();

private:
    struct alignas(256) SSAOCB
    {
        XMFLOAT4 kernel[8];
        float screenW, screenH, nearZ, farZ;
        float radius, bias, projX, projY;
        float pad[24];
    };
    static_assert(sizeof(SSAOCB) == 256);

    struct alignas(256) ShadowSceneCB
    {
        XMMATRIX lightViewProj;
        float    shadowBias;
        float    shadowStrength;
        float    shadowEnabled;
        float    pad;
        float    screenW;
        float    screenH;
        float    waterBodyStr;
        float    waterRefract;
        float    waterMinTrans;
        float    pad2[39];
    };
    static_assert(sizeof(ShadowSceneCB) == 256);

    struct LightningCB
    {
        XMFLOAT4 pts[13];
        float    intensity;
        float    aspect;
        int      numBolt;
        int      numBranch;
        float    pad[8];
    };
    static_assert(sizeof(LightningCB) == 256);

    // Device / resolution
    ComPtr<ID3D12Device> m_device;
    UINT m_width  = 0;
    UINT m_height = 0;
    std::wstring m_assetDir;

    // Shared RTV heap (non-owning)
    ID3D12DescriptorHeap* m_rtvHeap     = nullptr;
    UINT                  m_rtvIncrSize = 0;
    UINT                  m_rtvBase     = 0;

    D3D12_VIEWPORT m_viewport = {};
    RECT           m_scissor  = {};

    // HDR RT / ToneMap
    ComPtr<ID3D12Resource>      m_hdrRT;
    ComPtr<ID3D12RootSignature> m_toneMappingRootSig;
    ComPtr<ID3D12PipelineState> m_toneMappingPSO;

    // Bloom  (bloomSRVHeap: [0]=hdrRT [1]=extract [2]=blur [3]=godRay [4]=ssaoBlur)
    ComPtr<ID3D12Resource>       m_bloomExtractRT;
    ComPtr<ID3D12Resource>       m_bloomBlurRT;
    ComPtr<ID3D12DescriptorHeap> m_bloomSRVHeap;
    ComPtr<ID3D12RootSignature>  m_bloomRootSig;
    ComPtr<ID3D12PipelineState>  m_bloomBrightPSO;
    ComPtr<ID3D12PipelineState>  m_bloomBlurPSO;
    UINT m_bloomSRVIncrSize = 0;

    // God Rays
    ComPtr<ID3D12Resource>       m_godRayRT;
    ComPtr<ID3D12RootSignature>  m_godRayRootSig;
    ComPtr<ID3D12PipelineState>  m_godRayPSO;

    // Lens Flare
    ComPtr<ID3D12RootSignature>  m_lensFlareRootSig;
    ComPtr<ID3D12PipelineState>  m_lensFlarePSO;

    // DOF
    ComPtr<ID3D12Resource>       m_dofRT;
    ComPtr<ID3D12DescriptorHeap> m_dofSRVHeap;
    ComPtr<ID3D12RootSignature>  m_dofRootSig;
    ComPtr<ID3D12PipelineState>  m_dofPSO;
    UINT            m_dofSRVIncrSize = 0;
    ID3D12Resource* m_depthBuffer    = nullptr;   // non-owning

    // TAA
    ComPtr<ID3D12Resource>       m_taaRT;
    ComPtr<ID3D12Resource>       m_taaHistoryRT;
    ComPtr<ID3D12DescriptorHeap> m_taaSRVHeap;
    ComPtr<ID3D12RootSignature>  m_taaRootSig;
    ComPtr<ID3D12PipelineState>  m_taaPSO;
    UINT m_taaSRVIncrSize   = 0;
    bool m_taaHistoryInPSR  = false;
    bool m_taaHistoryValid  = false;

    // SSAO
    ComPtr<ID3D12Resource>       m_ssaoRT;
    ComPtr<ID3D12Resource>       m_ssaoBlurRT;
    ComPtr<ID3D12DescriptorHeap> m_ssaoSRVHeap;
    ComPtr<ID3D12RootSignature>  m_ssaoRootSig;
    ComPtr<ID3D12RootSignature>  m_ssaoBlurRootSig;
    ComPtr<ID3D12PipelineState>  m_ssaoPSO;
    ComPtr<ID3D12PipelineState>  m_ssaoBlurPSO;
    ComPtr<ID3D12Resource>       m_ssaoCB;
    UINT    m_ssaoSRVIncrSize = 0;
    SSAOCB* m_mappedSSAOCB   = nullptr;

    // Shadow map
    static const UINT kShadowSize = 2048;
    ComPtr<ID3D12Resource>       m_shadowMap;
    ComPtr<ID3D12DescriptorHeap> m_shadowDSVHeap;
    ComPtr<ID3D12RootSignature>  m_shadowRootSig;
    ComPtr<ID3D12PipelineState>  m_shadowPSO;
    ComPtr<ID3D12Resource>       m_shadowSceneCB;
    UINT8*   m_mappedShadowSceneCB = nullptr;
    XMMATRIX m_lightViewProj;

    // Lightning
    ComPtr<ID3D12Resource>      m_lightningCB;
    ComPtr<ID3D12RootSignature> m_lightningRootSig;
    ComPtr<ID3D12PipelineState> m_lightningPSO;
    LightningCB* m_mappedLightningCB      = nullptr;
    float        m_prevLightningIntensity = 0.0f;

    // SSR / Refraction
    ComPtr<ID3D12Resource>       m_skySnapshotRT;
    ComPtr<ID3D12Resource>       m_refractionRT;
    ComPtr<ID3D12DescriptorHeap> m_oceanSRVHeap;
    UINT m_oceanSRVIncrSize = 0;
    bool m_skySnapshotInPSR = false;
    bool m_refractionInPSR  = false;

    // Internal init
    void InitBloom();
    void InitHDR();
    void InitGodRays();
    void InitLensFlare();
    void InitDOF(ID3D12Resource* depthBuffer);
    void InitSSR(ID3D12Resource* heightMap, ID3D12Resource* dztMap);
    void InitTAA();
    void InitSSAO(ID3D12Resource* depthBuffer);
    void InitShadowMap(FloatingObject* fo);
    void InitLightning();

    // Internal render
    void RenderBloom(ID3D12GraphicsCommandList* cmd);
    void RenderGodRays(ID3D12GraphicsCommandList* cmd, Renderer* renderer, SkyDome* skyDome);
    void RenderToneMap(ID3D12GraphicsCommandList* cmd, UINT frameIndex,
                       D3D12_CPU_DESCRIPTOR_HANDLE swapRTV, Renderer* renderer);
    void RenderLensFlare(ID3D12GraphicsCommandList* cmd, UINT frameIndex,
                         D3D12_CPU_DESCRIPTOR_HANDLE swapRTV,
                         Renderer* renderer, SkyDome* skyDome);
    void RenderDOF(ID3D12GraphicsCommandList* cmd);
    void RenderTAA(ID3D12GraphicsCommandList* cmd);

    std::wstring AssetPath(const wchar_t* name) const { return m_assetDir + L"shaders\\" + name; }
    D3D12_CPU_DESCRIPTOR_HANDLE RTV(UINT relativeSlot) const;
};
