#include "RainbowEffect.h"
#include <d3dx12_root_signature.h>
#include <d3dx12_barriers.h>

void RainbowEffect::Init(
    ComPtr<ID3D12Device> device,
    UINT /*width*/, UINT /*height*/,
    const UINT8* vsData, UINT vsLen,
    const UINT8* psData, UINT psLen)
{
    m_device = device;

    // Upload-heap CB, persistently mapped
    {
        auto hp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        auto bd = CD3DX12_RESOURCE_DESC::Buffer(sizeof(RainbowCB));
        ThrowIfFailed(m_device->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&m_cb)));
        CD3DX12_RANGE r(0, 0);
        ThrowIfFailed(m_cb->Map(0, &r, reinterpret_cast<void**>(&m_cbMapped)));
    }

    // Root signature: single CBV at b0
    {
        CD3DX12_ROOT_PARAMETER param;
        param.InitAsConstantBufferView(0);
        CD3DX12_ROOT_SIGNATURE_DESC rsd;
        rsd.Init(1, &param, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE);
        ComPtr<ID3DBlob> sig, err;
        ThrowIfFailed(D3D12SerializeRootSignature(
            &rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err));
        ThrowIfFailed(m_device->CreateRootSignature(
            0, sig->GetBufferPointer(), sig->GetBufferSize(),
            IID_PPV_ARGS(&m_rootSig)));
    }

    // PSO: additive blend (ONE + ONE), no depth test, writes to HDR RT
    {
        D3D12_RENDER_TARGET_BLEND_DESC rtBlend = {};
        rtBlend.BlendEnable           = TRUE;
        rtBlend.SrcBlend              = D3D12_BLEND_ONE;
        rtBlend.DestBlend             = D3D12_BLEND_ONE;
        rtBlend.BlendOp               = D3D12_BLEND_OP_ADD;
        rtBlend.SrcBlendAlpha         = D3D12_BLEND_ZERO;
        rtBlend.DestBlendAlpha        = D3D12_BLEND_ONE;
        rtBlend.BlendOpAlpha          = D3D12_BLEND_OP_ADD;
        rtBlend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        CD3DX12_BLEND_DESC blendDesc(D3D12_DEFAULT);
        blendDesc.RenderTarget[0] = rtBlend;

        CD3DX12_RASTERIZER_DESC raster(D3D12_DEFAULT);
        raster.CullMode = D3D12_CULL_MODE_NONE;

        D3D12_DEPTH_STENCIL_DESC ds = {};
        ds.DepthEnable   = FALSE;
        ds.StencilEnable = FALSE;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature        = m_rootSig.Get();
        pd.VS                    = CD3DX12_SHADER_BYTECODE(vsData, vsLen);
        pd.PS                    = CD3DX12_SHADER_BYTECODE(psData, psLen);
        pd.RasterizerState       = raster;
        pd.BlendState            = blendDesc;
        pd.DepthStencilState     = ds;
        pd.DSVFormat             = DXGI_FORMAT_UNKNOWN;
        pd.SampleMask            = UINT_MAX;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets      = 1;
        pd.RTVFormats[0]         = DXGI_FORMAT_R16G16B16A16_FLOAT;
        pd.SampleDesc.Count      = 1;

        ThrowIfFailed(m_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_pso)));
    }
}

void RainbowEffect::Render(
    ID3D12GraphicsCommandList*  cmd,
    D3D12_CPU_DESCRIPTOR_HANDLE hdrRTV,
    D3D12_VIEWPORT              viewport,
    D3D12_RECT                  scissor,
    const Params&               p)
{
    if (!enabled) return;

    XMMATRIX vp    = p.view * p.proj;
    XMMATRIX invVP = XMMatrixInverse(nullptr, vp);

    RainbowCB cb = {};
    cb.invViewProj = XMMatrixTranspose(invVP);
    cb.cameraPos   = p.cameraPos;
    cb.sunDir      = p.sunDir;
    cb.intensity   = intensity;
    cb.rainFactor  = p.rainFactor;
    cb.sunIntensity = p.sunIntensity;
    cb.secondaryBow = secondaryBow ? 1.0f : 0.0f;
    cb.bandWidth    = bandWidth;
    memcpy(m_cbMapped, &cb, sizeof(cb));

    cmd->SetPipelineState(m_pso.Get());
    cmd->SetGraphicsRootSignature(m_rootSig.Get());
    cmd->SetGraphicsRootConstantBufferView(0, m_cb->GetGPUVirtualAddress());
    cmd->OMSetRenderTargets(1, &hdrRTV, FALSE, nullptr);
    cmd->RSSetViewports(1, &viewport);
    cmd->RSSetScissorRects(1, &scissor);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->DrawInstanced(3, 1, 0, 0);
}
