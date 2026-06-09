// ============================================================
// IBLSystem.h
// Image-based lighting infrastructure (Phase A): captures the procedural sky
// into a small cubemap (rolling, one face per frame) and bakes the split-sum
// BRDF integration LUT once at init. Purely data-generation in this phase —
// nothing samples these resources yet (see the tutorial's IBL chapter).
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
        const UINT8* captureCSData, UINT captureCSSize,
        const UINT8* brdfLutCSData, UINT brdfLutCSSize);

    // Call once per frame; internally rotates which cubemap face gets refreshed
    // (rolling 6-frame cycle — see design doc's "update cadence" section).
    // Captured cubemap stays in UAV state between dispatches; transitions to
    // NON_PIXEL_SHADER_RESOURCE only when something needs to sample it (Phase B+).
    void Dispatch(ComPtr<ID3D12GraphicsCommandList> cmdList, SkyDome* skyDome, float time);

    ID3D12Resource*       GetCaptureCubemap() const { return m_captureCubemap.Get(); }
    ID3D12Resource*       GetBRDFLut()        const { return m_brdfLut.Get(); }
    ID3D12DescriptorHeap* GetDebugSRVHeap()   const { return m_debugSRVHeap.Get(); }

private:
    void CreateTextures(UINT cubemapFaceSize, UINT brdfLutSize);
    void CreateDescriptorHeaps();
    void CreateRootSignatures();
    void CreatePSOs(const UINT8* captureCSData, UINT captureCSSize,
                    const UINT8* brdfLutCSData, UINT brdfLutCSSize);
    void CreateConstantBuffers();
    void RunBRDFLutOnce(ComPtr<ID3D12CommandQueue> cmdQueue);

    ComPtr<ID3D12Device> m_device;
    UINT m_faceSize = 64;
    UINT m_lutSize  = 128;
    int  m_currentFace = 0;       // rolling capture index, 0..5

    ComPtr<ID3D12Resource> m_captureCubemap;  // TextureCube, RGBA16F, UAV+SRV
    ComPtr<ID3D12Resource> m_brdfLut;         // Texture2D,  RG16F,   UAV+SRV (written once)

    // One UAV per face (TEXTURE2DARRAY view, ArraySlice = face) for SkyCaptureCS,
    // plus a TEXTURECUBE SRV for later sampling / debug preview
    ComPtr<ID3D12DescriptorHeap> m_captureUAVHeap;  // 6 slots
    ComPtr<ID3D12DescriptorHeap> m_lutUAVHeap;      // 1 slot
    ComPtr<ID3D12DescriptorHeap> m_debugSRVHeap;    // cubemap SRV + LUT SRV, for ImGui preview

    ComPtr<ID3D12RootSignature> m_captureRootSig;
    ComPtr<ID3D12PipelineState> m_capturePSO;
    ComPtr<ID3D12Resource>      m_captureCB;     // SkyParams + faceIndex/faceSize
    UINT8*                      m_captureCBMapped = nullptr;

    ComPtr<ID3D12RootSignature> m_lutRootSig;
    ComPtr<ID3D12PipelineState> m_lutPSO;

    bool m_lutGenerated = false;
};
