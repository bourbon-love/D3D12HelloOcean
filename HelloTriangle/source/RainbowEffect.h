// RainbowEffect.h
// Fullscreen additive-blend rainbow pass (primary + optional secondary bow).
#pragma once
#include <d3d12.h>
#include <DirectXMath.h>
#include <wrl.h>
#include <d3dx12_core.h>
#include "../DXSampleHelper.h"

using namespace DirectX;
using Microsoft::WRL::ComPtr;

class RainbowEffect
{
public:
    bool  enabled      = true;
    float intensity    = 1.0f;
    float bandWidth    = 2.0f;  // colour-band width multiplier; 1=narrow, 4=very wide
    bool  secondaryBow = false; // secondary bow is subtle in reality; off by default

    void Init(
        ComPtr<ID3D12Device> device,
        UINT width, UINT height,
        const UINT8* vsData, UINT vsLen,
        const UINT8* psData, UINT psLen);

    struct Params
    {
        XMFLOAT3 cameraPos;
        XMFLOAT3 sunDir;
        float    rainFactor;
        float    sunIntensity;
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
    struct alignas(256) RainbowCB
    {
        XMMATRIX invViewProj;           // 64 bytes
        XMFLOAT3 cameraPos; float pad0; // 16 bytes
        XMFLOAT3 sunDir;    float intensity; // 16 bytes
        float rainFactor;
        float sunIntensity;
        float secondaryBow;
        float bandWidth;                // 16 bytes
        float _pad[36];                 // 144 bytes → total 256
    };
    static_assert(sizeof(RainbowCB) == 256);

    ComPtr<ID3D12Device>        m_device;
    ComPtr<ID3D12RootSignature> m_rootSig;
    ComPtr<ID3D12PipelineState> m_pso;
    ComPtr<ID3D12Resource>      m_cb;
    RainbowCB*                  m_cbMapped = nullptr;
};
