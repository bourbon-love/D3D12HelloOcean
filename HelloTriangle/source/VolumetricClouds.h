// VolumetricClouds.h
// Volumetric cloud system using ray-marching.
// Renders at half resolution then composites (bilinear upsample) onto the full HDR RT.
#pragma once
#include <d3d12.h>
#include <DirectXMath.h>
#include <wrl.h>
#include <d3dx12_core.h>
#include "../DXSampleHelper.h"

using namespace DirectX;
using Microsoft::WRL::ComPtr;

class VolumetricClouds
{
public:
    // ImGui-editable params
    bool  enabled         = true;
    float cloudCoverage   = 0.45f;  // 0=clear, 1=overcast
    float densityMult     = 0.61f;
    float cloudScale      = 1.09f;
    float cloudBase       = 2000.0f; // meters
    float cloudTop        = 5000.0f;

    void Init(
        ComPtr<ID3D12Device> device,
        UINT width, UINT height,
        const UINT8* vsData,          UINT vsLen,
        const UINT8* psData,          UINT psLen,
        const UINT8* compositePsData, UINT compositePsLen);

    struct Params
    {
        XMFLOAT3 cameraPos;
        float    time;
        XMFLOAT3 sunDir;
        float    sunIntensity;
        XMFLOAT3 sunColor;
        float    weatherIntensity;
        float    windX;
        float    windZ;
        float    nightFactor;
        XMMATRIX view;
        XMMATRIX proj;
    };

    void Render(
        ID3D12GraphicsCommandList*  cmd,
        D3D12_CPU_DESCRIPTOR_HANDLE hdrRTV,
        D3D12_VIEWPORT              viewport,
        D3D12_RECT                  scissor,
        const Params&               p);

private:
    struct alignas(256) CloudCB
    {
        XMMATRIX invViewProj;
        XMFLOAT3 cameraPos;     float time;
        XMFLOAT3 sunDir;        float cloudCoverage;
        XMFLOAT3 sunColor;      float sunIntensity;
        float    cloudBase;
        float    cloudTop;
        float    windX;
        float    windZ;
        float    weatherIntensity;
        float    cloudScale;
        float    densityMult;
        float    nightFactor;
        float    _pad[28];
    };
    static_assert(sizeof(CloudCB) == 256);

    ComPtr<ID3D12Device> m_device;
    UINT m_width = 0, m_height = 0;

    // Ray-march pass
    ComPtr<ID3D12RootSignature>  m_rootSig;
    ComPtr<ID3D12PipelineState>  m_pso;
    ComPtr<ID3D12Resource>       m_cb;
    CloudCB*                     m_cbMapped = nullptr;

    // Half-resolution intermediate RT (width/2 × height/2, R16G16B16A16_FLOAT)
    ComPtr<ID3D12Resource>       m_halfResRT;
    ComPtr<ID3D12DescriptorHeap> m_halfResRTVHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE  m_halfResRTVCPU = {};

    // Composite pass: bilinear upsample half-res → full HDR RT
    ComPtr<ID3D12RootSignature>  m_compositeRootSig;
    ComPtr<ID3D12PipelineState>  m_compositePSO;
    ComPtr<ID3D12DescriptorHeap> m_compositeSRVHeap;   // shader-visible, 1 slot for half-res SRV
    D3D12_GPU_DESCRIPTOR_HANDLE  m_compositeSRVGPU = {};
};
