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

    ID3D12Resource* GetHeightMap() const { return m_heightMap.Get(); }
    ID3D12Resource* GetDztMap()    const { return m_dztMap.Get(); }   // 新增：Dz结果
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
    ComPtr<ID3D12Resource> m_h0Map;       // Phillips初始化结果，只写一次
    ComPtr<ID3D12Resource> m_hktMap;      // h(k,t) + Dx(k,t) 频域，每帧更新；IFFT后存 h+Dx 结果
    ComPtr<ID3D12Resource> m_tempMap;     // h+Dx IFFT 的 pingpong 缓冲
    ComPtr<ID3D12Resource> m_heightMap;   // 最终 h+Dx 场，Wave Shader 采样 (.x=h, .z=Dx)
    ComPtr<ID3D12Resource> m_dztMap;      // Dz(k,t) 频域；IFFT后存 Dz 结果
    ComPtr<ID3D12Resource> m_dztTempMap;  // Dz IFFT 的 pingpong 缓冲

    
    // 供时域演化计算使用，slot0=cbv(timeCB), slot1=h0SRV, slot2=hktUAV, slot3=dztUAV
	ComPtr<ID3D12DescriptorHeap> m_timeEvoHeap;
    // 供 IFFT 计算使用，slot0=hktUAV/tempMapUAV 交替使用，slot1=dztUAV/dztTempUAV 交替使用
	ComPtr<ID3D12DescriptorHeap> m_ifftHeap;  
    // 专门给 Dz IFFT 用的描述符堆，slot0=dztUAV/tempMapUAV 交替使用
	ComPtr<ID3D12DescriptorHeap> m_ifftDzHeap; 
    // srvHeap 供 Wave Shader 采样
    // slot0 = heightMap SRV (.x=h, .z=Dx)
    // slot1 = dztMap    SRV (.x=Dz)
    ComPtr<ID3D12DescriptorHeap> m_srvHeap;
    // Phillips heap（持久化）
    ComPtr<ID3D12DescriptorHeap> m_phillipsHeap;
    // Phillips管线（初始化时跑一次）
    ComPtr<ID3D12RootSignature>  m_phillipsRootSig;
    ComPtr<ID3D12PipelineState>  m_phillipsPSO;
    ComPtr<ID3D12Resource>       m_phillipsCB;
    UINT8* m_phillipsCBMapped = nullptr;

    // 时域演化管线（每帧）
    ComPtr<ID3D12RootSignature>  m_timeEvoRootSig;
    ComPtr<ID3D12PipelineState>  m_timeEvoPSO;
    ComPtr<ID3D12Resource>       m_timeCB;
    UINT8* m_timeCBMapped = nullptr;

    // IFFT管线（每帧，h+Dx 和 Dz 复用同一个 PSO）
    ComPtr<ID3D12RootSignature>  m_ifftRootSig;
    ComPtr<ID3D12PipelineState>  m_ifftPSO;
    ComPtr<ID3D12Resource>       m_ifftCB;
    UINT8* m_ifftCBMapped = nullptr;

    // 初始化用临时对象
    ComPtr<ID3D12CommandAllocator>    m_initAllocator;
    ComPtr<ID3D12GraphicsCommandList> m_initCmdList;
};