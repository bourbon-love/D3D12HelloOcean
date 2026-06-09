// ============================================================
// IBLSystem.h
// Image-based lighting infrastructure:
//   Phase A — rolling sky capture (one cubemap face/frame) + one-time BRDF LUT bake
//   Phase B — SH9 diffuse irradiance convolution (every ~30 frames) + CB for shaders
// ============================================================
#pragma once
#include <d3d12.h>
#include <wrl.h>
#include <d3dx12_core.h>
#include "../DXSampleHelper.h"
using Microsoft::WRL::ComPtr;

class SkyDome;

class IBLSystem
{
public:
    void Init(
        ComPtr<ID3D12Device>       device,
        ComPtr<ID3D12CommandQueue> cmdQueue,
        UINT cubemapFaceSize,      // e.g. 64
        UINT brdfLutSize,          // e.g. 128
        const UINT8* captureCSData,  UINT captureCSSize,
        const UINT8* brdfLutCSData,  UINT brdfLutCSSize,
        const UINT8* irradCSData,    UINT irradCSSize);

    // Call once per frame — rotates one cubemap face and, every ~30 frames,
    // dispatches IrradianceConvolveCS and copies SH output to the staging buffer.
    void Dispatch(ComPtr<ID3D12GraphicsCommandList> cmdList, SkyDome* skyDome, float time);

    // Call at the START of OnUpdate (after WaitForPreviousFrame) to copy staged
    // SH coefficients into the SHCB upload buffer.  Safe because the GPU has
    // already finished writing the staging buffer in the previous frame.
    void ReadbackSH();

    ID3D12Resource*           GetCaptureCubemap() const { return m_captureCubemap.Get(); }
    ID3D12Resource*           GetBRDFLut()        const { return m_brdfLut.Get(); }
    ID3D12DescriptorHeap*     GetDebugSRVHeap()   const { return m_debugSRVHeap.Get(); }
    D3D12_GPU_VIRTUAL_ADDRESS GetSHCB()           const { return m_shCB->GetGPUVirtualAddress(); }

private:
    void CreateTextures(UINT cubemapFaceSize, UINT brdfLutSize);
    void CreateDescriptorHeaps();
    void CreateRootSignatures();
    void CreatePSOs(const UINT8* captureCSData, UINT captureCSSize,
                    const UINT8* brdfLutCSData, UINT brdfLutCSSize);
    void CreateConstantBuffers();
    void RunBRDFLutOnce(ComPtr<ID3D12CommandQueue> cmdQueue);
    void CreateSHResources(const UINT8* irradCSData, UINT irradCSSize);
    void DispatchIrradiance(ComPtr<ID3D12GraphicsCommandList> cmdList);

    ComPtr<ID3D12Device> m_device;
    UINT m_faceSize     = 64;
    UINT m_lutSize      = 128;
    int  m_currentFace  = 0;   // rolling capture index, 0..5
    int  m_shFrameCount = 0;   // total Dispatch calls; drives 30-frame convolve cadence

    // --- Phase A: sky capture + BRDF LUT ---
    ComPtr<ID3D12Resource>       m_captureCubemap;   // RGBA16F, 6 faces, stays in UAV between frames
    ComPtr<ID3D12Resource>       m_brdfLut;          // RG16F, written once, stays in NON_PIXEL_SRV

    ComPtr<ID3D12DescriptorHeap> m_captureUAVHeap;   // 6 slots, one TEXTURE2DARRAY UAV per face
    ComPtr<ID3D12DescriptorHeap> m_lutUAVHeap;       // 1 slot, TEXTURE2D UAV for BRDFLutCS
    ComPtr<ID3D12DescriptorHeap> m_debugSRVHeap;     // TEXTURECUBE SRV + TEXTURE2D SRV (ImGui preview)

    ComPtr<ID3D12RootSignature>  m_captureRootSig;
    ComPtr<ID3D12PipelineState>  m_capturePSO;
    ComPtr<ID3D12Resource>       m_captureCB;        // CaptureCB, 256 bytes, persistent-mapped
    UINT8*                       m_captureCBMapped = nullptr;

    ComPtr<ID3D12RootSignature>  m_lutRootSig;
    ComPtr<ID3D12PipelineState>  m_lutPSO;
    bool                         m_lutGenerated = false;

    // --- Phase B: SH9 irradiance convolution ---
    ComPtr<ID3D12Resource>       m_shOutputBuffer;   // RWBuffer<float4>[9], DEFAULT heap, UAV
    ComPtr<ID3D12Resource>       m_shStagingBuffer;  // READBACK heap, 144 bytes
    ComPtr<ID3D12Resource>       m_shCB;             // SHCB (256 bytes), UPLOAD heap, persistent-mapped
    ComPtr<ID3D12DescriptorHeap> m_shConvolveHeap;   // 2 slots: slot0=SRV(cubemap), slot1=UAV(buffer)
    ComPtr<ID3D12RootSignature>  m_shConvolveRootSig;
    ComPtr<ID3D12PipelineState>  m_shConvolvePSO;
    UINT8*                       m_shCBMapped      = nullptr;
    UINT8*                       m_shStagingMapped = nullptr;
    bool                         m_shStagingReady  = false;
};
