#pragma once
#include <d3d12.h>
#include <DirectXMath.h>
#include <wrl.h>
#include <vector>
#include <d3dx12_core.h>
#include "../DXSampleHelper.h"
#include "renderer/RendererContext.h"

using namespace DirectX;
using Microsoft::WRL::ComPtr;

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

    // 太阳方向供海浪Shader读取
    XMFLOAT3 GetSunDirection() const { return m_sunDir; }
    float GetSunIntensity() const;
	XMFLOAT3 GetSunColor() const;
    XMFLOAT3 GetSkyColor() const;

private:
    void CreateSphereMesh(ComPtr<ID3D12GraphicsCommandList> cmdList);
    void CreateSkyPSO(const UINT8* vsData, UINT vsSize,
        const UINT8* psData, UINT psSize);
    void CreateSkyDepthState();

    // 天空CBV结构，和skyshader.hlsl的cbuffer完全对应
    // 注意DX12要求CBV大小是256字节的倍数
    struct __declspec(align(256)) SkyCB
    {
        XMMATRIX  viewProj;       // 64字节
        XMFLOAT4  topColor;       // 16字节
        XMFLOAT4  middleColor;    // 16字节
        XMFLOAT4  bottomColor;    // 16字节
        XMFLOAT3  sunPosition;    // 12字节
        float     time;           // 4字节
        float     cloudDensity;   // 4字节
        float     cloudScale;     // 4字节
        float     cloudSharpness; // 4字节
        float     pad;            // 4字节 补齐
        XMFLOAT3  sunColor;
        float     padSunColor;
    };
    // 总计 = 64+16+16+16+12+4+4+4+4+4 = 144字节
    // __declspec(align(256))保证整个结构体从256字节对齐的地址开始

    ComPtr<ID3D12Device>              m_device;
    ComPtr<ID3D12RootSignature>       m_rootSignature;
    ComPtr<ID3D12PipelineState>       m_skyPSO;

    // 球体几何
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

    // 天空参数
    float     m_time = 0.0f;
    XMFLOAT3  m_sunDir = { 1.0f, 0.0f, 0.0f };
    float     m_cloudDensity = 0.5f;
    float     m_cloudScale = 0.85f;
    float     m_cloudSharpness = 0.6f;

    UINT m_width = 0;
    UINT m_height = 0;
};