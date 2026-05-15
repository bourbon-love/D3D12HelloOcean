#include "VolumetricClouds.h"
#include <d3dx12_root_signature.h>
#include <d3dx12_barriers.h>

void VolumetricClouds::Init(
    ComPtr<ID3D12Device> device,
    UINT width, UINT height,
    const UINT8* vsData,          UINT vsLen,
    const UINT8* psData,          UINT psLen,
    const UINT8* compositePsData, UINT compositePsLen)
{
    m_device = device;
    m_width  = width;
    m_height = height;

    // Constant buffer (upload heap, persistently mapped)
    {
        auto hp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        auto bd = CD3DX12_RESOURCE_DESC::Buffer(sizeof(CloudCB));
        ThrowIfFailed(m_device->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&m_cb)));
        CD3DX12_RANGE r(0, 0);
        ThrowIfFailed(m_cb->Map(0, &r, reinterpret_cast<void**>(&m_cbMapped)));
    }

    // Root signature for ray-march pass: single CBV at b0
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

    // Ray-march PSO (alpha blend, no depth test, writes to R16G16B16A16_FLOAT half-res RT)
    {
        D3D12_RENDER_TARGET_BLEND_DESC rtBlend = {};
        rtBlend.BlendEnable           = TRUE;
        rtBlend.SrcBlend              = D3D12_BLEND_SRC_ALPHA;
        rtBlend.DestBlend             = D3D12_BLEND_INV_SRC_ALPHA;
        rtBlend.BlendOp               = D3D12_BLEND_OP_ADD;
        rtBlend.SrcBlendAlpha         = D3D12_BLEND_ONE;
        rtBlend.DestBlendAlpha        = D3D12_BLEND_INV_SRC_ALPHA;
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

        ThrowIfFailed(m_device->CreateGraphicsPipelineState(
            &pd, IID_PPV_ARGS(&m_pso)));
    }

    // Half-resolution render target (width/2 × height/2)
    // Starts in PIXEL_SHADER_RESOURCE so Render() can always open with PSR→RT transition.
    {
        auto hp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        D3D12_CLEAR_VALUE clearVal = {};
        clearVal.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;

        auto desc = CD3DX12_RESOURCE_DESC::Tex2D(
            DXGI_FORMAT_R16G16B16A16_FLOAT,
            width / 2, height / 2, 1, 1, 1, 0,
            D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        ThrowIfFailed(m_device->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            &clearVal,
            IID_PPV_ARGS(&m_halfResRT)));

        // RTV (non-shader-visible)
        D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
        rtvHeapDesc.NumDescriptors = 1;
        rtvHeapDesc.Type  = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        ThrowIfFailed(m_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_halfResRTVHeap)));
        m_halfResRTVCPU = m_halfResRTVHeap->GetCPUDescriptorHandleForHeapStart();
        m_device->CreateRenderTargetView(m_halfResRT.Get(), nullptr, m_halfResRTVCPU);
    }

    // Composite SRV heap (shader-visible, 1 slot)
    {
        D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
        srvHeapDesc.NumDescriptors = 1;
        srvHeapDesc.Type  = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ThrowIfFailed(m_device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&m_compositeSRVHeap)));
        m_compositeSRVGPU = m_compositeSRVHeap->GetGPUDescriptorHandleForHeapStart();

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format                        = DXGI_FORMAT_R16G16B16A16_FLOAT;
        srvDesc.ViewDimension                 = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping       = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels           = 1;
        m_device->CreateShaderResourceView(
            m_halfResRT.Get(), &srvDesc,
            m_compositeSRVHeap->GetCPUDescriptorHandleForHeapStart());
    }

    // Composite root signature: 1 SRV descriptor table (t0) + 1 static bilinear-clamp sampler
    {
        D3D12_DESCRIPTOR_RANGE range = {};
        range.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors     = 1;
        range.BaseShaderRegister = 0;

        CD3DX12_ROOT_PARAMETER param;
        param.InitAsDescriptorTable(1, &range, D3D12_SHADER_VISIBILITY_PIXEL);

        D3D12_STATIC_SAMPLER_DESC sampler = {};
        sampler.Filter           = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        sampler.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.ShaderRegister   = 0;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        CD3DX12_ROOT_SIGNATURE_DESC rsd;
        rsd.Init(1, &param, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE);
        ComPtr<ID3DBlob> sig, err;
        ThrowIfFailed(D3D12SerializeRootSignature(
            &rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err));
        ThrowIfFailed(m_device->CreateRootSignature(
            0, sig->GetBufferPointer(), sig->GetBufferSize(),
            IID_PPV_ARGS(&m_compositeRootSig)));
    }

    // Composite PSO (reuses CloudVS, alpha blend, writes to full-res R16G16B16A16_FLOAT HDR RT)
    {
        D3D12_RENDER_TARGET_BLEND_DESC rtBlend = {};
        rtBlend.BlendEnable           = TRUE;
        rtBlend.SrcBlend              = D3D12_BLEND_SRC_ALPHA;
        rtBlend.DestBlend             = D3D12_BLEND_INV_SRC_ALPHA;
        rtBlend.BlendOp               = D3D12_BLEND_OP_ADD;
        rtBlend.SrcBlendAlpha         = D3D12_BLEND_ONE;
        rtBlend.DestBlendAlpha        = D3D12_BLEND_INV_SRC_ALPHA;
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
        pd.pRootSignature        = m_compositeRootSig.Get();
        pd.VS                    = CD3DX12_SHADER_BYTECODE(vsData, vsLen);   // reuse CloudVS
        pd.PS                    = CD3DX12_SHADER_BYTECODE(compositePsData, compositePsLen);
        pd.RasterizerState       = raster;
        pd.BlendState            = blendDesc;
        pd.DepthStencilState     = ds;
        pd.DSVFormat             = DXGI_FORMAT_UNKNOWN;
        pd.SampleMask            = UINT_MAX;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets      = 1;
        pd.RTVFormats[0]         = DXGI_FORMAT_R16G16B16A16_FLOAT;
        pd.SampleDesc.Count      = 1;

        ThrowIfFailed(m_device->CreateGraphicsPipelineState(
            &pd, IID_PPV_ARGS(&m_compositePSO)));
    }
}

void VolumetricClouds::Render(
    ID3D12GraphicsCommandList*  cmd,
    D3D12_CPU_DESCRIPTOR_HANDLE hdrRTV,
    D3D12_VIEWPORT              viewport,
    D3D12_RECT                  scissor,
    const Params&               p)
{
    if (!enabled) return;

    // Build invViewProj
    XMMATRIX vp    = p.view * p.proj;
    XMMATRIX invVP = XMMatrixInverse(nullptr, vp);

    // Update CB
    CloudCB cb = {};
    cb.invViewProj       = XMMatrixTranspose(invVP);
    cb.cameraPos         = p.cameraPos;
    cb.time              = p.time;
    cb.sunDir            = p.sunDir;
    cb.cloudCoverage     = cloudCoverage;
    cb.sunColor          = p.sunColor;
    cb.sunIntensity      = p.sunIntensity;
    cb.cloudBase         = cloudBase;
    cb.cloudTop          = cloudTop;
    cb.windX             = p.windX;
    cb.windZ             = p.windZ;
    cb.weatherIntensity  = p.weatherIntensity;
    cb.cloudScale        = cloudScale;
    cb.densityMult       = densityMult;
    cb.nightFactor       = p.nightFactor;
    memcpy(m_cbMapped, &cb, sizeof(cb));

    // === Phase 1: Ray-march clouds into half-resolution RT ===
    {
        auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
            m_halfResRT.Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_RENDER_TARGET);
        cmd->ResourceBarrier(1, &barrier);
    }

    float clearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    cmd->ClearRenderTargetView(m_halfResRTVCPU, clearColor, 0, nullptr);

    D3D12_VIEWPORT halfVP = { 0, 0, (float)(m_width / 2), (float)(m_height / 2), 0.0f, 1.0f };
    D3D12_RECT     halfSC = { 0, 0, (LONG)(m_width / 2), (LONG)(m_height / 2) };

    cmd->SetPipelineState(m_pso.Get());
    cmd->SetGraphicsRootSignature(m_rootSig.Get());
    cmd->SetGraphicsRootConstantBufferView(0, m_cb->GetGPUVirtualAddress());
    cmd->OMSetRenderTargets(1, &m_halfResRTVCPU, FALSE, nullptr);
    cmd->RSSetViewports(1, &halfVP);
    cmd->RSSetScissorRects(1, &halfSC);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->DrawInstanced(3, 1, 0, 0);

    // === Phase 2: Bilinear upsample onto the full HDR RT ===
    {
        auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
            m_halfResRT.Get(),
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cmd->ResourceBarrier(1, &barrier);
    }

    cmd->SetPipelineState(m_compositePSO.Get());
    cmd->SetGraphicsRootSignature(m_compositeRootSig.Get());
    cmd->SetDescriptorHeaps(1, m_compositeSRVHeap.GetAddressOf());
    cmd->SetGraphicsRootDescriptorTable(0, m_compositeSRVGPU);
    cmd->OMSetRenderTargets(1, &hdrRTV, FALSE, nullptr);
    cmd->RSSetViewports(1, &viewport);
    cmd->RSSetScissorRects(1, &scissor);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->DrawInstanced(3, 1, 0, 0);
}
