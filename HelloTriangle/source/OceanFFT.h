// ============================================================
// OceanFFT.h
// GPU FFT ocean simulation class.
// Generates height/displacement maps every frame via a compute pipeline:
// Phillips spectrum -> time evolution -> IFFT.
// ============================================================
#pragma once
#include <d3d12.h>
#include <wrl.h>
#include <d3dx12_core.h>
#include "../DXSampleHelper.h"
using Microsoft::WRL::ComPtr;

class OceanFFT
{
public:
    void Init(
        ComPtr<ID3D12Device>       device,
        ComPtr<ID3D12CommandQueue> cmdQueue,
        UINT textureSize,
        const UINT8* phillipsCSData, UINT phillipsCSSize,
        const UINT8* timeEvoCSData, UINT timeEvoCSSize,
        const UINT8* ifftCSData, UINT ifftCSSize);
    void Dispatch(ComPtr<ID3D12GraphicsCommandList> cmdList, float time);

    // Height readback: records a copy of 3 texels to a staging buffer.
    // Call after ocean draw (heightmap in NON_PIXEL_SHADER_RESOURCE).
    // Results become available via ReadHeightSamples() on the next frame.
    struct HeightSamples { float h0, hBow, hSide; };
    void RecordHeightSamples(ID3D12GraphicsCommandList* cmd,
                             float worldCX, float worldCZ,   // ship center
                             float worldBX, float worldBZ,   // bow  (+20 m)
                             float worldSX, float worldSZ);  // starboard (+20 m)
    HeightSamples ReadHeightSamples() const;
    bool HasHeightSamples() const { return m_heightSampleReady; }

    ID3D12Resource* GetHeightMap() const { return m_heightMap.Get(); }
    ID3D12Resource* GetDztMap()    const { return m_dztMap.Get(); }   // Added: Dz result
    ID3D12DescriptorHeap* GetSRVHeap()   const { return m_srvHeap.Get(); }

    float windSpeed = 20.0f;
    float windDirX = 1.0f;
    float windDirY = 1.0f;
    float phillipsA = 0.3f;

private:
    void CreateTextures();
    void CreateDescriptorHeaps();
    void CreateRootSignatures();
    void CreatePSOs(
        const UINT8* phillipsCSData, UINT phillipsCSSize,
        const UINT8* timeEvoCSData, UINT timeEvoCSSize,
        const UINT8* ifftCSData, UINT ifftCSSize);
    void CreateConstantBuffers();
    void RunPhillipsInit(ComPtr<ID3D12CommandQueue> cmdQueue);

    ComPtr<ID3D12Device> m_device;
    UINT                 m_textureSize = 256;

    // Textures
    ComPtr<ID3D12Resource> m_h0Map;       // Phillips initialization result, written once
    ComPtr<ID3D12Resource> m_hktMap;      // h(k,t) + Dx(k,t) frequency domain, updated each frame; stores h+Dx result after IFFT
    ComPtr<ID3D12Resource> m_tempMap;     // Ping-pong buffer for h+Dx IFFT
    ComPtr<ID3D12Resource> m_heightMap;   // Final h+Dx field, sampled by Wave Shader (.x=h, .z=Dx)
    ComPtr<ID3D12Resource> m_dztMap;      // Dz(k,t) frequency domain; stores Dz result after IFFT
    ComPtr<ID3D12Resource> m_dztTempMap;  // Ping-pong buffer for Dz IFFT

    
    // Used for time evolution compute; slot0=cbv(timeCB), slot1=h0SRV, slot2=hktUAV, slot3=dztUAV
	ComPtr<ID3D12DescriptorHeap> m_timeEvoHeap;
    // Used for IFFT compute; slot0=hktUAV/tempMapUAV alternating, slot1=dztUAV/dztTempUAV alternating
	ComPtr<ID3D12DescriptorHeap> m_ifftHeap;
    // Dedicated descriptor heap for Dz IFFT; slot0=dztUAV/tempMapUAV alternating
	ComPtr<ID3D12DescriptorHeap> m_ifftDzHeap;
    // srvHeap sampled by Wave Shader
    // slot0 = heightMap SRV (.x=h, .z=Dx)
    // slot1 = dztMap    SRV (.x=Dz)
    ComPtr<ID3D12DescriptorHeap> m_srvHeap;
    // Phillips heap (persistent)
    ComPtr<ID3D12DescriptorHeap> m_phillipsHeap;
    // Phillips pipeline (run once at initialization)
    ComPtr<ID3D12RootSignature>  m_phillipsRootSig;
    ComPtr<ID3D12PipelineState>  m_phillipsPSO;
    ComPtr<ID3D12Resource>       m_phillipsCB;
    UINT8* m_phillipsCBMapped = nullptr;

    // Time evolution pipeline (every frame)
    ComPtr<ID3D12RootSignature>  m_timeEvoRootSig;
    ComPtr<ID3D12PipelineState>  m_timeEvoPSO;
    ComPtr<ID3D12Resource>       m_timeCB;
    UINT8* m_timeCBMapped = nullptr;

    // IFFT pipeline (every frame, h+Dx and Dz share the same PSO)
    ComPtr<ID3D12RootSignature>  m_ifftRootSig;
    ComPtr<ID3D12PipelineState>  m_ifftPSO;
    ComPtr<ID3D12Resource>       m_ifftCB;
    UINT8* m_ifftCBMapped = nullptr;

    // Temporary objects used during initialization
    ComPtr<ID3D12CommandAllocator>    m_initAllocator;
    ComPtr<ID3D12GraphicsCommandList> m_initCmdList;

    // Height readback staging (3 texels, 1-frame latency)
    // Offset per slot = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT (512)
    // RowPitch per slot = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT (256)
    static const UINT      kStagingSlots  = 3;
    static const UINT      kStagingStride = 512;  // placement alignment
    ComPtr<ID3D12Resource> m_heightStagingBuf;
    UINT8*                 m_heightStagingMapped = nullptr;
    bool                   m_heightSampleReady   = false;
};