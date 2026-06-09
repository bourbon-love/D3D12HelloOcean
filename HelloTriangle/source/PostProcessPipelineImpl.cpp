#include "PostProcessPipeline.h"
#include "Renderer.h"
#include "SkyDome.h"
#include "FloatingObject.h"
#include "ShipModel.h"
#include <d3dx12_root_signature.h>
#include <d3dx12_barriers.h>
#include "../DXSampleHelper.h"
#include <algorithm>

// ============================================================

// ============================================================
D3D12_CPU_DESCRIPTOR_HANDLE PostProcessPipeline::RTV(UINT rel) const
{
    return CD3DX12_CPU_DESCRIPTOR_HANDLE(
        m_rtvHeap->GetCPUDescriptorHandleForHeapStart(),
        m_rtvBase + rel, m_rtvIncrSize);
}

D3D12_CPU_DESCRIPTOR_HANDLE PostProcessPipeline::GetHDRRTV() const
{
    return RTV(2); // slot: rtvBase+2
}

D3D12_GPU_VIRTUAL_ADDRESS PostProcessPipeline::GetShadowSceneCBAddr() const
{
    return m_shadowSceneCB->GetGPUVirtualAddress();
}

// ============================================================

// ============================================================
void PostProcessPipeline::Init(
    ComPtr<ID3D12Device> device,
    ID3D12DescriptorHeap* rtvHeap,
    UINT  rtvIncrSize,
    UINT  rtvBase,
    UINT  width,
    UINT  height,
    const std::wstring& assetDir)
{
    m_device      = device;
    m_rtvHeap     = rtvHeap;
    m_rtvIncrSize = rtvIncrSize;
    m_rtvBase     = rtvBase;
    m_width       = width;
    m_height      = height;
    m_assetDir    = assetDir;
    m_viewport    = CD3DX12_VIEWPORT(0.0f, 0.0f, (float)width, (float)height);
    m_scissor     = CD3DX12_RECT(0, 0, (LONG)width, (LONG)height);

    InitBloom();
    InitHDR();
    InitGodRays();
    InitLensFlare();
    InitTAA();
    InitLightning();
}

// ============================================================

// ============================================================
void PostProcessPipeline::InitSceneResources(
    ID3D12Resource* depthBuffer,
    ID3D12Resource* heightMap,
    ID3D12Resource* dztMap,
    FloatingObject* fo,
    IBLSystem*      ibl)
{
    InitSSR(heightMap, dztMap);
    InitDOF(depthBuffer);
    InitSSAO(depthBuffer);
    InitShadowMap(fo);

    // Extend ocean SRV heap with IBL textures (slots 5=prefilter, 6=brdfLUT).
    // Must be called after InitSSR which creates the heap.
    if (ibl)
    {
        m_device->CopyDescriptorsSimple(1,
            CD3DX12_CPU_DESCRIPTOR_HANDLE(
                m_oceanSRVHeap->GetCPUDescriptorHandleForHeapStart(), 5, m_oceanSRVIncrSize),
            ibl->GetPrefilterSRVCPU(),
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        m_device->CopyDescriptorsSimple(1,
            CD3DX12_CPU_DESCRIPTOR_HANDLE(
                m_oceanSRVHeap->GetCPUDescriptorHandleForHeapStart(), 6, m_oceanSRVIncrSize),
            ibl->GetBRDFLutSRVCPU(),
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
}

// ============================================================
// InitBloom
// ============================================================
void PostProcessPipeline::InitBloom()
{

    {
        auto hp  = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        auto rd  = CD3DX12_RESOURCE_DESC::Tex2D(
            DXGI_FORMAT_R16G16B16A16_FLOAT, m_width, m_height, 1, 1);
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE cv = {}; cv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        ThrowIfFailed(m_device->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&m_bloomExtractRT)));
        ThrowIfFailed(m_device->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&m_bloomBlurRT)));
        m_device->CreateRenderTargetView(m_bloomExtractRT.Get(), nullptr, RTV(0));
        m_device->CreateRenderTargetView(m_bloomBlurRT.Get(),    nullptr, RTV(1));
    }


    {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 5;
        hd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ThrowIfFailed(m_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_bloomSRVHeap)));
        m_bloomSRVIncrSize = m_device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }


    {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format                  = DXGI_FORMAT_R16G16B16A16_FLOAT;
        sd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels     = 1;
        m_device->CreateShaderResourceView(m_bloomExtractRT.Get(), &sd,
            CD3DX12_CPU_DESCRIPTOR_HANDLE(m_bloomSRVHeap->GetCPUDescriptorHandleForHeapStart(),
                1, m_bloomSRVIncrSize));
        m_device->CreateShaderResourceView(m_bloomBlurRT.Get(), &sd,
            CD3DX12_CPU_DESCRIPTOR_HANDLE(m_bloomSRVHeap->GetCPUDescriptorHandleForHeapStart(),
                2, m_bloomSRVIncrSize));
    }


    {
        CD3DX12_ROOT_PARAMETER params[2];
        CD3DX12_DESCRIPTOR_RANGE sr;
        sr.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
        params[0].InitAsDescriptorTable(1, &sr, D3D12_SHADER_VISIBILITY_PIXEL);
        params[1].InitAsConstants(4, 0);
        CD3DX12_STATIC_SAMPLER_DESC samp(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
        CD3DX12_ROOT_SIGNATURE_DESC rsd;
        rsd.Init(2, params, 1, &samp, D3D12_ROOT_SIGNATURE_FLAG_NONE);
        ComPtr<ID3DBlob> sig, err;
        ThrowIfFailed(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err));
        ThrowIfFailed(m_device->CreateRootSignature(0,
            sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&m_bloomRootSig)));
    }

    UINT8 *pVS = nullptr, *pBright = nullptr, *pBlur = nullptr;
    UINT   vsLen = 0, brightLen = 0, blurLen = 0;
    ThrowIfFailed(ReadDataFromFile(AssetPath(L"bloom_BloomVS.cso").c_str(),      &pVS,     &vsLen));
    ThrowIfFailed(ReadDataFromFile(AssetPath(L"bloom_BrightPassPS.cso").c_str(), &pBright, &brightLen));
    ThrowIfFailed(ReadDataFromFile(AssetPath(L"bloom_BlurPS.cso").c_str(),       &pBlur,   &blurLen));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature           = m_bloomRootSig.Get();
    pd.VS                       = CD3DX12_SHADER_BYTECODE(pVS, vsLen);
    pd.RasterizerState          = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.BlendState               = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    pd.DepthStencilState.DepthEnable   = FALSE;
    pd.DepthStencilState.StencilEnable = FALSE;
    pd.DSVFormat             = DXGI_FORMAT_UNKNOWN;
    pd.SampleMask            = UINT_MAX;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets      = 1;
    pd.RTVFormats[0]         = DXGI_FORMAT_R16G16B16A16_FLOAT;
    pd.SampleDesc.Count      = 1;

    pd.PS = CD3DX12_SHADER_BYTECODE(pBright, brightLen);
    ThrowIfFailed(m_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_bloomBrightPSO)));
    pd.PS = CD3DX12_SHADER_BYTECODE(pBlur, blurLen);
    ThrowIfFailed(m_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_bloomBlurPSO)));
    free(pVS); free(pBright); free(pBlur);
}

// ============================================================
// InitHDR
// ============================================================
void PostProcessPipeline::InitHDR()
{

    {
        auto hp  = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        auto rd  = CD3DX12_RESOURCE_DESC::Tex2D(
            DXGI_FORMAT_R16G16B16A16_FLOAT, m_width, m_height, 1, 1);
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE cv = {}; cv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        ThrowIfFailed(m_device->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&m_hdrRT)));
        m_device->CreateRenderTargetView(m_hdrRT.Get(), nullptr, RTV(2));
    }

    // bloomSRVHeap[0] = hdrRT
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format                  = DXGI_FORMAT_R16G16B16A16_FLOAT;
        sd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels     = 1;
        m_device->CreateShaderResourceView(m_hdrRT.Get(), &sd,
            m_bloomSRVHeap->GetCPUDescriptorHandleForHeapStart());
    }


    {
        CD3DX12_ROOT_PARAMETER params[3];
        CD3DX12_DESCRIPTOR_RANGE sr1, sr2;
        sr1.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0);
        sr2.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 2);
        params[0].InitAsDescriptorTable(1, &sr1, D3D12_SHADER_VISIBILITY_PIXEL);
        params[1].InitAsDescriptorTable(1, &sr2, D3D12_SHADER_VISIBILITY_PIXEL);
        params[2].InitAsConstants(12, 0);
        CD3DX12_STATIC_SAMPLER_DESC samp(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
        CD3DX12_ROOT_SIGNATURE_DESC rsd;
        rsd.Init(3, params, 1, &samp, D3D12_ROOT_SIGNATURE_FLAG_NONE);
        ComPtr<ID3DBlob> sig, err;
        ThrowIfFailed(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err));
        ThrowIfFailed(m_device->CreateRootSignature(0,
            sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&m_toneMappingRootSig)));
    }

    // ToneMap PSO
    {
        UINT8 *pVS = nullptr, *pPS = nullptr;
        UINT   vsLen = 0,      psLen = 0;
        ThrowIfFailed(ReadDataFromFile(AssetPath(L"tonemapping_ToneMapVS.cso").c_str(), &pVS, &vsLen));
        ThrowIfFailed(ReadDataFromFile(AssetPath(L"tonemapping_ToneMapPS.cso").c_str(), &pPS, &psLen));

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature           = m_toneMappingRootSig.Get();
        pd.VS                       = CD3DX12_SHADER_BYTECODE(pVS, vsLen);
        pd.PS                       = CD3DX12_SHADER_BYTECODE(pPS, psLen);
        pd.RasterizerState          = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.BlendState               = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        pd.DepthStencilState.DepthEnable   = FALSE;
        pd.DepthStencilState.StencilEnable = FALSE;
        pd.DSVFormat             = DXGI_FORMAT_UNKNOWN;
        pd.SampleMask            = UINT_MAX;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets      = 1;
        pd.RTVFormats[0]         = DXGI_FORMAT_R8G8B8A8_UNORM;
        pd.SampleDesc.Count      = 1;
        ThrowIfFailed(m_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_toneMappingPSO)));
        free(pVS); free(pPS);
    }
}

// ============================================================
// InitGodRays
// ============================================================
void PostProcessPipeline::InitGodRays()
{
    UINT grW = m_width / 2, grH = m_height / 2;


    {
        auto hp  = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        auto rd  = CD3DX12_RESOURCE_DESC::Tex2D(
            DXGI_FORMAT_R16G16B16A16_FLOAT, grW, grH, 1, 1);
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE cv = {}; cv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        ThrowIfFailed(m_device->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&m_godRayRT)));
        m_device->CreateRenderTargetView(m_godRayRT.Get(), nullptr, RTV(3));
    }

    // bloomSRVHeap[3] = godRayRT
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format                  = DXGI_FORMAT_R16G16B16A16_FLOAT;
        sd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels     = 1;
        m_device->CreateShaderResourceView(m_godRayRT.Get(), &sd,
            CD3DX12_CPU_DESCRIPTOR_HANDLE(m_bloomSRVHeap->GetCPUDescriptorHandleForHeapStart(),
                3, m_bloomSRVIncrSize));
    }


    {
        CD3DX12_ROOT_PARAMETER params[2];
        CD3DX12_DESCRIPTOR_RANGE sr;
        sr.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
        params[0].InitAsDescriptorTable(1, &sr, D3D12_SHADER_VISIBILITY_PIXEL);
        params[1].InitAsConstants(6, 0);
        CD3DX12_STATIC_SAMPLER_DESC samp(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
        CD3DX12_ROOT_SIGNATURE_DESC rsd;
        rsd.Init(2, params, 1, &samp, D3D12_ROOT_SIGNATURE_FLAG_NONE);
        ComPtr<ID3DBlob> sig, err;
        ThrowIfFailed(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err));
        ThrowIfFailed(m_device->CreateRootSignature(0,
            sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&m_godRayRootSig)));
    }

    {
        UINT8 *pVS = nullptr, *pPS = nullptr;
        UINT   vsLen = 0,      psLen = 0;
        ThrowIfFailed(ReadDataFromFile(AssetPath(L"godrays_GodRayVS.cso").c_str(), &pVS, &vsLen));
        ThrowIfFailed(ReadDataFromFile(AssetPath(L"godrays_GodRayPS.cso").c_str(), &pPS, &psLen));
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature           = m_godRayRootSig.Get();
        pd.VS                       = CD3DX12_SHADER_BYTECODE(pVS, vsLen);
        pd.PS                       = CD3DX12_SHADER_BYTECODE(pPS, psLen);
        pd.RasterizerState          = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.BlendState               = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        pd.DepthStencilState.DepthEnable   = FALSE;
        pd.DepthStencilState.StencilEnable = FALSE;
        pd.DSVFormat             = DXGI_FORMAT_UNKNOWN;
        pd.SampleMask            = UINT_MAX;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets      = 1;
        pd.RTVFormats[0]         = DXGI_FORMAT_R16G16B16A16_FLOAT;
        pd.SampleDesc.Count      = 1;
        ThrowIfFailed(m_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_godRayPSO)));
        free(pVS); free(pPS);
    }
}

// ============================================================
// InitLensFlare
// ============================================================
void PostProcessPipeline::InitLensFlare()
{
    {
        CD3DX12_ROOT_PARAMETER params[1];
        params[0].InitAsConstants(6, 0);
        CD3DX12_ROOT_SIGNATURE_DESC rsd;
        rsd.Init(1, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE);
        ComPtr<ID3DBlob> sig, err;
        ThrowIfFailed(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err));
        ThrowIfFailed(m_device->CreateRootSignature(0,
            sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&m_lensFlareRootSig)));
    }

    {
        UINT8 *pVS = nullptr, *pPS = nullptr;
        UINT   vsLen = 0,      psLen = 0;
        ThrowIfFailed(ReadDataFromFile(AssetPath(L"lensflare_LensFlareVS.cso").c_str(), &pVS, &vsLen));
        ThrowIfFailed(ReadDataFromFile(AssetPath(L"lensflare_LensFlarePS.cso").c_str(), &pPS, &psLen));

        D3D12_BLEND_DESC blend = {};
        blend.RenderTarget[0].BlendEnable           = TRUE;
        blend.RenderTarget[0].SrcBlend              = D3D12_BLEND_ONE;
        blend.RenderTarget[0].DestBlend             = D3D12_BLEND_ONE;
        blend.RenderTarget[0].BlendOp               = D3D12_BLEND_OP_ADD;
        blend.RenderTarget[0].SrcBlendAlpha         = D3D12_BLEND_ONE;
        blend.RenderTarget[0].DestBlendAlpha        = D3D12_BLEND_ONE;
        blend.RenderTarget[0].BlendOpAlpha          = D3D12_BLEND_OP_ADD;
        blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature           = m_lensFlareRootSig.Get();
        pd.VS                       = CD3DX12_SHADER_BYTECODE(pVS, vsLen);
        pd.PS                       = CD3DX12_SHADER_BYTECODE(pPS, psLen);
        pd.RasterizerState          = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.BlendState               = blend;
        pd.DepthStencilState.DepthEnable   = FALSE;
        pd.DepthStencilState.StencilEnable = FALSE;
        pd.DSVFormat             = DXGI_FORMAT_UNKNOWN;
        pd.SampleMask            = UINT_MAX;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets      = 1;
        pd.RTVFormats[0]         = DXGI_FORMAT_R8G8B8A8_UNORM;
        pd.SampleDesc.Count      = 1;
        ThrowIfFailed(m_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_lensFlarePSO)));
        free(pVS); free(pPS);
    }
}

// ============================================================
// InitDOF
// ============================================================
void PostProcessPipeline::InitDOF(ID3D12Resource* depthBuffer)
{

    {
        auto hp  = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        auto rd  = CD3DX12_RESOURCE_DESC::Tex2D(
            DXGI_FORMAT_R16G16B16A16_FLOAT, m_width, m_height, 1, 1);
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE cv = {}; cv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        ThrowIfFailed(m_device->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&m_dofRT)));
        m_device->CreateRenderTargetView(m_dofRT.Get(), nullptr, RTV(4));
    }

    m_depthBuffer = depthBuffer;


    {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 2;
        hd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ThrowIfFailed(m_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_dofSRVHeap)));
        m_dofSRVIncrSize = m_device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format                  = DXGI_FORMAT_R16G16B16A16_FLOAT;
        sd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels     = 1;
        m_device->CreateShaderResourceView(m_hdrRT.Get(), &sd,
            CD3DX12_CPU_DESCRIPTOR_HANDLE(
                m_dofSRVHeap->GetCPUDescriptorHandleForHeapStart(), 0, m_dofSRVIncrSize));

        sd.Format = DXGI_FORMAT_R32_FLOAT;
        m_device->CreateShaderResourceView(depthBuffer, &sd,
            CD3DX12_CPU_DESCRIPTOR_HANDLE(
                m_dofSRVHeap->GetCPUDescriptorHandleForHeapStart(), 1, m_dofSRVIncrSize));
    }


    {
        CD3DX12_ROOT_PARAMETER params[2];
        CD3DX12_DESCRIPTOR_RANGE sr;
        sr.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0);
        params[0].InitAsDescriptorTable(1, &sr, D3D12_SHADER_VISIBILITY_PIXEL);
        params[1].InitAsConstants(4, 0);
        CD3DX12_STATIC_SAMPLER_DESC samp(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
        CD3DX12_ROOT_SIGNATURE_DESC rsd;
        rsd.Init(2, params, 1, &samp, D3D12_ROOT_SIGNATURE_FLAG_NONE);
        ComPtr<ID3DBlob> sig, err;
        ThrowIfFailed(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err));
        ThrowIfFailed(m_device->CreateRootSignature(0,
            sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&m_dofRootSig)));
    }

    {
        UINT8 *pVS = nullptr, *pPS = nullptr;
        UINT   vsLen = 0,      psLen = 0;
        ThrowIfFailed(ReadDataFromFile(AssetPath(L"dof_DOFVS.cso").c_str(), &pVS, &vsLen));
        ThrowIfFailed(ReadDataFromFile(AssetPath(L"dof_DOFPS.cso").c_str(), &pPS, &psLen));
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature           = m_dofRootSig.Get();
        pd.VS                       = CD3DX12_SHADER_BYTECODE(pVS, vsLen);
        pd.PS                       = CD3DX12_SHADER_BYTECODE(pPS, psLen);
        pd.RasterizerState          = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.BlendState               = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        pd.DepthStencilState.DepthEnable   = FALSE;
        pd.DepthStencilState.StencilEnable = FALSE;
        pd.DSVFormat             = DXGI_FORMAT_UNKNOWN;
        pd.SampleMask            = UINT_MAX;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets      = 1;
        pd.RTVFormats[0]         = DXGI_FORMAT_R16G16B16A16_FLOAT;
        pd.SampleDesc.Count      = 1;
        ThrowIfFailed(m_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_dofPSO)));
        free(pVS); free(pPS);
    }
}

// ============================================================
// InitSSR
// ============================================================
void PostProcessPipeline::InitSSR(ID3D12Resource* heightMap, ID3D12Resource* dztMap)
{
    auto hp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);

    // skySnapshotRT
    {
        auto rd = CD3DX12_RESOURCE_DESC::Tex2D(
            DXGI_FORMAT_R16G16B16A16_FLOAT, m_width, m_height, 1, 1);
        ThrowIfFailed(m_device->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_skySnapshotRT)));
    }

    // refractionRT
    {
        auto rd = CD3DX12_RESOURCE_DESC::Tex2D(
            DXGI_FORMAT_R16G16B16A16_FLOAT, m_width, m_height, 1, 1);
        ThrowIfFailed(m_device->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_refractionRT)));
    }

    // oceanSRVHeap: [0]=heightMap [1]=dztMap [2]=skySnapshot [3]=shadowMap [4]=refractionRT
    //               [5]=prefilter TextureCube (IBL) [6]=brdfLUT (IBL)  — slots 5/6 filled by InitSceneResources
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 7;
        hd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ThrowIfFailed(m_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_oceanSRVHeap)));
        m_oceanSRVIncrSize = m_device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }


    {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format                  = DXGI_FORMAT_R32G32B32A32_FLOAT;
        sd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels     = 1;
        m_device->CreateShaderResourceView(heightMap, &sd,
            CD3DX12_CPU_DESCRIPTOR_HANDLE(
                m_oceanSRVHeap->GetCPUDescriptorHandleForHeapStart(), 0, m_oceanSRVIncrSize));
        m_device->CreateShaderResourceView(dztMap, &sd,
            CD3DX12_CPU_DESCRIPTOR_HANDLE(
                m_oceanSRVHeap->GetCPUDescriptorHandleForHeapStart(), 1, m_oceanSRVIncrSize));
    }

    // slot 2: skySnapshot, slot 4: refractionRT
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format                  = DXGI_FORMAT_R16G16B16A16_FLOAT;
        sd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels     = 1;
        m_device->CreateShaderResourceView(m_skySnapshotRT.Get(), &sd,
            CD3DX12_CPU_DESCRIPTOR_HANDLE(
                m_oceanSRVHeap->GetCPUDescriptorHandleForHeapStart(), 2, m_oceanSRVIncrSize));
        m_device->CreateShaderResourceView(m_refractionRT.Get(), &sd,
            CD3DX12_CPU_DESCRIPTOR_HANDLE(
                m_oceanSRVHeap->GetCPUDescriptorHandleForHeapStart(), 4, m_oceanSRVIncrSize));
    }
}

// ============================================================
// InitTAA
// ============================================================
void PostProcessPipeline::InitTAA()
{
    auto fmt      = DXGI_FORMAT_R16G16B16A16_FLOAT;
    auto heapProp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    D3D12_CLEAR_VALUE cv = {}; cv.Format = fmt;


    {
        auto rd  = CD3DX12_RESOURCE_DESC::Tex2D(fmt, m_width, m_height, 1, 1);
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        ThrowIfFailed(m_device->CreateCommittedResource(
            &heapProp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&m_taaRT)));
        m_device->CreateRenderTargetView(m_taaRT.Get(), nullptr, RTV(5));
    }


    {
        auto rd  = CD3DX12_RESOURCE_DESC::Tex2D(fmt, m_width, m_height, 1, 1);
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        ThrowIfFailed(m_device->CreateCommittedResource(
            &heapProp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&m_taaHistoryRT)));
        m_device->CreateRenderTargetView(m_taaHistoryRT.Get(), nullptr, RTV(6));
    }


    {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 2;
        hd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ThrowIfFailed(m_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_taaSRVHeap)));
        m_taaSRVIncrSize = m_device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format                  = fmt;
        sd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels     = 1;
        m_device->CreateShaderResourceView(m_hdrRT.Get(), &sd,
            CD3DX12_CPU_DESCRIPTOR_HANDLE(
                m_taaSRVHeap->GetCPUDescriptorHandleForHeapStart(), 0, m_taaSRVIncrSize));
        m_device->CreateShaderResourceView(m_taaHistoryRT.Get(), &sd,
            CD3DX12_CPU_DESCRIPTOR_HANDLE(
                m_taaSRVHeap->GetCPUDescriptorHandleForHeapStart(), 1, m_taaSRVIncrSize));
    }


    {
        CD3DX12_ROOT_PARAMETER params[2];
        CD3DX12_DESCRIPTOR_RANGE sr;
        sr.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0);
        params[0].InitAsDescriptorTable(1, &sr, D3D12_SHADER_VISIBILITY_PIXEL);
        params[1].InitAsConstants(4, 0);
        CD3DX12_STATIC_SAMPLER_DESC samplers[2] = {};
        samplers[0].Init(0, D3D12_FILTER_MIN_MAG_MIP_POINT,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
        samplers[1].Init(1, D3D12_FILTER_MIN_MAG_MIP_LINEAR,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
        CD3DX12_ROOT_SIGNATURE_DESC rsd;
        rsd.Init(2, params, 2, samplers, D3D12_ROOT_SIGNATURE_FLAG_NONE);
        ComPtr<ID3DBlob> sig, err;
        ThrowIfFailed(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err));
        ThrowIfFailed(m_device->CreateRootSignature(0,
            sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&m_taaRootSig)));
    }

    {
        UINT8 *pVS = nullptr, *pPS = nullptr;
        UINT   vsLen = 0,      psLen = 0;
        ThrowIfFailed(ReadDataFromFile(AssetPath(L"taa_TAAVS.cso").c_str(), &pVS, &vsLen));
        ThrowIfFailed(ReadDataFromFile(AssetPath(L"taa_TAAPS.cso").c_str(), &pPS, &psLen));
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature           = m_taaRootSig.Get();
        pd.VS                       = CD3DX12_SHADER_BYTECODE(pVS, vsLen);
        pd.PS                       = CD3DX12_SHADER_BYTECODE(pPS, psLen);
        pd.RasterizerState          = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.BlendState               = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        pd.DepthStencilState.DepthEnable   = FALSE;
        pd.DepthStencilState.StencilEnable = FALSE;
        pd.DSVFormat             = DXGI_FORMAT_UNKNOWN;
        pd.SampleMask            = UINT_MAX;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets      = 1;
        pd.RTVFormats[0]         = fmt;
        pd.SampleDesc.Count      = 1;
        ThrowIfFailed(m_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_taaPSO)));
        free(pVS); free(pPS);
    }
}

// ============================================================
// InitSSAO
// ============================================================
void PostProcessPipeline::InitSSAO(ID3D12Resource* depthBuffer)
{
    UINT hw = m_width / 2, hh = m_height / 2;
    auto hp  = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    auto fmt = DXGI_FORMAT_R16_FLOAT;
    D3D12_CLEAR_VALUE cv = {}; cv.Format = fmt;


    {
        auto rd  = CD3DX12_RESOURCE_DESC::Tex2D(fmt, hw, hh, 1, 1);
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        ThrowIfFailed(m_device->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&m_ssaoRT)));
        m_device->CreateRenderTargetView(m_ssaoRT.Get(), nullptr, RTV(7));
    }


    {
        auto rd  = CD3DX12_RESOURCE_DESC::Tex2D(fmt, hw, hh, 1, 1);
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        ThrowIfFailed(m_device->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&m_ssaoBlurRT)));
        m_device->CreateRenderTargetView(m_ssaoBlurRT.Get(), nullptr, RTV(8));
    }


    {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 2;
        hd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ThrowIfFailed(m_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_ssaoSRVHeap)));
        m_ssaoSRVIncrSize = m_device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format                  = DXGI_FORMAT_R32_FLOAT;
        sd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels     = 1;
        m_device->CreateShaderResourceView(depthBuffer, &sd,
            CD3DX12_CPU_DESCRIPTOR_HANDLE(
                m_ssaoSRVHeap->GetCPUDescriptorHandleForHeapStart(), 0, m_ssaoSRVIncrSize));

        sd.Format = DXGI_FORMAT_R16_FLOAT;
        m_device->CreateShaderResourceView(m_ssaoRT.Get(), &sd,
            CD3DX12_CPU_DESCRIPTOR_HANDLE(
                m_ssaoSRVHeap->GetCPUDescriptorHandleForHeapStart(), 1, m_ssaoSRVIncrSize));
    }


    {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format                  = DXGI_FORMAT_R16_FLOAT;
        sd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels     = 1;
        m_device->CreateShaderResourceView(m_ssaoBlurRT.Get(), &sd,
            CD3DX12_CPU_DESCRIPTOR_HANDLE(
                m_bloomSRVHeap->GetCPUDescriptorHandleForHeapStart(), 4, m_bloomSRVIncrSize));
    }


    {
        auto hp2 = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        auto buf = CD3DX12_RESOURCE_DESC::Buffer(256);
        ThrowIfFailed(m_device->CreateCommittedResource(
            &hp2, D3D12_HEAP_FLAG_NONE, &buf,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_ssaoCB)));
        CD3DX12_RANGE r(0, 0);
        m_ssaoCB->Map(0, &r, reinterpret_cast<void**>(&m_mappedSSAOCB));
        memset(m_mappedSSAOCB, 0, 256);

        static const XMFLOAT4 kKernel[8] = {
            { 0.5381f,  0.1956f,  0.3215f, 0}, { 0.1069f,  0.1149f,  0.2749f, 0},
            {-0.4742f,  0.3518f,  0.3817f, 0}, { 0.2815f,  0.4316f,  0.1954f, 0},
            {-0.1259f, -0.2354f,  0.3926f, 0}, {-0.3134f, -0.2248f,  0.3127f, 0},
            { 0.4348f, -0.3194f,  0.1547f, 0}, {-0.2045f,  0.4285f,  0.2849f, 0}
        };
        memcpy(m_mappedSSAOCB->kernel, kKernel, sizeof(kKernel));
        m_mappedSSAOCB->screenW = (float)hw;
        m_mappedSSAOCB->screenH = (float)hh;
        m_mappedSSAOCB->nearZ   = 0.1f;
        m_mappedSSAOCB->farZ    = 2000.0f;
        m_mappedSSAOCB->radius  = ssaoRadius;
        m_mappedSSAOCB->bias    = 0.025f;
    }


    {
        CD3DX12_ROOT_PARAMETER params[2];
        params[0].InitAsConstantBufferView(0, 0, D3D12_SHADER_VISIBILITY_PIXEL);
        CD3DX12_DESCRIPTOR_RANGE sr;
        sr.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
        params[1].InitAsDescriptorTable(1, &sr, D3D12_SHADER_VISIBILITY_PIXEL);
        CD3DX12_STATIC_SAMPLER_DESC samplers[2];
        samplers[0].Init(0, D3D12_FILTER_MIN_MAG_MIP_POINT,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
        samplers[1].Init(1, D3D12_FILTER_MIN_MAG_MIP_LINEAR,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
        CD3DX12_ROOT_SIGNATURE_DESC rsd;
        rsd.Init(2, params, 2, samplers, D3D12_ROOT_SIGNATURE_FLAG_NONE);
        ComPtr<ID3DBlob> sig, err;
        ThrowIfFailed(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err));
        ThrowIfFailed(m_device->CreateRootSignature(0,
            sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&m_ssaoRootSig)));
    }


    {
        CD3DX12_ROOT_PARAMETER params[2];
        CD3DX12_DESCRIPTOR_RANGE sr;
        sr.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
        params[0].InitAsDescriptorTable(1, &sr, D3D12_SHADER_VISIBILITY_PIXEL);
        params[1].InitAsConstants(4, 0);
        CD3DX12_STATIC_SAMPLER_DESC samp(1, D3D12_FILTER_MIN_MAG_MIP_LINEAR,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
        CD3DX12_ROOT_SIGNATURE_DESC rsd;
        rsd.Init(2, params, 1, &samp, D3D12_ROOT_SIGNATURE_FLAG_NONE);
        ComPtr<ID3DBlob> sig, err;
        ThrowIfFailed(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err));
        ThrowIfFailed(m_device->CreateRootSignature(0,
            sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&m_ssaoBlurRootSig)));
    }

    {
        UINT8 *pVS = nullptr, *pSSAOPS = nullptr, *pBLURPS = nullptr;
        UINT   vsLen = 0, ssaopsLen = 0, blurpsLen = 0;
        ThrowIfFailed(ReadDataFromFile(AssetPath(L"ssao_SSAOVS.cso").c_str(),     &pVS,     &vsLen));
        ThrowIfFailed(ReadDataFromFile(AssetPath(L"ssao_SSAOPS.cso").c_str(),     &pSSAOPS, &ssaopsLen));
        ThrowIfFailed(ReadDataFromFile(AssetPath(L"ssao_SSAOBLURPS.cso").c_str(), &pBLURPS, &blurpsLen));

        auto MakePSO = [&](ID3D12RootSignature* rs,
                           const void* ps, UINT psz,
                           ID3D12PipelineState** out) {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
            pd.pRootSignature           = rs;
            pd.VS                       = { pVS, vsLen };
            pd.PS                       = { ps,  psz  };
            pd.RasterizerState          = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
            pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            pd.BlendState               = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
            pd.DepthStencilState.DepthEnable   = FALSE;
            pd.DepthStencilState.StencilEnable = FALSE;
            pd.DSVFormat             = DXGI_FORMAT_UNKNOWN;
            pd.SampleMask            = UINT_MAX;
            pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            pd.NumRenderTargets      = 1;
            pd.RTVFormats[0]         = DXGI_FORMAT_R16_FLOAT;
            pd.SampleDesc.Count      = 1;
            ThrowIfFailed(m_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(out)));
        };
        MakePSO(m_ssaoRootSig.Get(),     pSSAOPS, ssaopsLen, &m_ssaoPSO);
        MakePSO(m_ssaoBlurRootSig.Get(), pBLURPS, blurpsLen, &m_ssaoBlurPSO);
        free(pVS); free(pSSAOPS); free(pBLURPS);
    }
}

// ============================================================
// InitShadowMap
// ============================================================
void PostProcessPipeline::InitShadowMap(FloatingObject* fo)
{

    {
        auto hp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        auto rd = CD3DX12_RESOURCE_DESC::Tex2D(
            DXGI_FORMAT_R32_TYPELESS, kShadowSize, kShadowSize, 1, 1);
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE cv = {};
        cv.Format = DXGI_FORMAT_D32_FLOAT;
        cv.DepthStencil.Depth = 1.0f;
        ThrowIfFailed(m_device->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv, IID_PPV_ARGS(&m_shadowMap)));
    }


    {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        hd.NumDescriptors = 1;
        ThrowIfFailed(m_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_shadowDSVHeap)));
        D3D12_DEPTH_STENCIL_VIEW_DESC dsvd = {};
        dsvd.Format        = DXGI_FORMAT_D32_FLOAT;
        dsvd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        m_device->CreateDepthStencilView(m_shadowMap.Get(), &dsvd,
            m_shadowDSVHeap->GetCPUDescriptorHandleForHeapStart());
    }


    {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format                  = DXGI_FORMAT_R32_FLOAT;
        sd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels     = 1;
        m_device->CreateShaderResourceView(m_shadowMap.Get(), &sd,
            CD3DX12_CPU_DESCRIPTOR_HANDLE(
                m_oceanSRVHeap->GetCPUDescriptorHandleForHeapStart(),
                3, m_oceanSRVIncrSize));
    }

    // ShadowSceneCB
    {
        auto hp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        auto bd = CD3DX12_RESOURCE_DESC::Buffer(sizeof(ShadowSceneCB));
        ThrowIfFailed(m_device->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_shadowSceneCB)));
        CD3DX12_RANGE r(0, 0);
        m_shadowSceneCB->Map(0, &r, reinterpret_cast<void**>(&m_mappedShadowSceneCB));
        memset(m_mappedShadowSceneCB, 0, sizeof(ShadowSceneCB));
    }


    {
        CD3DX12_ROOT_PARAMETER param;
        param.InitAsConstantBufferView(0);
        CD3DX12_ROOT_SIGNATURE_DESC rsd;
        rsd.Init(1, &param, 0, nullptr,
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
        ComPtr<ID3DBlob> sig, err;
        ThrowIfFailed(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err));
        ThrowIfFailed(m_device->CreateRootSignature(0,
            sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&m_shadowRootSig)));
    }


    {
        UINT8* pVS = nullptr; UINT vsLen = 0;
        ThrowIfFailed(ReadDataFromFile(AssetPath(L"shadowmap_ShadowVS.cso").c_str(), &pVS, &vsLen));
        D3D12_INPUT_ELEMENT_DESC layout[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
              D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        };
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
        pd.InputLayout            = { layout, 1 };
        pd.pRootSignature         = m_shadowRootSig.Get();
        pd.VS                     = { pVS, vsLen };
        pd.RasterizerState        = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        pd.RasterizerState.DepthBias            = 8000;
        pd.RasterizerState.SlopeScaledDepthBias = 3.0f;
        pd.RasterizerState.CullMode             = D3D12_CULL_MODE_FRONT;
        pd.BlendState             = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        pd.DepthStencilState      = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        pd.DSVFormat              = DXGI_FORMAT_D32_FLOAT;
        pd.SampleMask             = UINT_MAX;
        pd.PrimitiveTopologyType  = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets       = 0;
        pd.SampleDesc.Count       = 1;
        ThrowIfFailed(m_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_shadowPSO)));
        free(pVS);
    }

    fo->InitShadowResources(m_device.Get());
    m_lightViewProj = XMMatrixIdentity();
}

// ============================================================
// InitLightning
// ============================================================
void PostProcessPipeline::InitLightning()
{
    {
        auto hp  = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        auto buf = CD3DX12_RESOURCE_DESC::Buffer(256);
        ThrowIfFailed(m_device->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &buf,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_lightningCB)));
        CD3DX12_RANGE r(0, 0);
        m_lightningCB->Map(0, &r, reinterpret_cast<void**>(&m_mappedLightningCB));
        memset(m_mappedLightningCB, 0, 256);
        m_mappedLightningCB->aspect = (float)m_width / m_height;
    }

    {
        CD3DX12_ROOT_PARAMETER params[1];
        params[0].InitAsConstantBufferView(0, 0, D3D12_SHADER_VISIBILITY_PIXEL);
        CD3DX12_ROOT_SIGNATURE_DESC rsd;
        rsd.Init(1, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE);
        ComPtr<ID3DBlob> sig, err;
        ThrowIfFailed(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err));
        ThrowIfFailed(m_device->CreateRootSignature(0,
            sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&m_lightningRootSig)));
    }

    {
        UINT8 *pVS = nullptr, *pPS = nullptr;
        UINT   vsLen = 0, psLen = 0;
        ThrowIfFailed(ReadDataFromFile(AssetPath(L"lightning_LightningVS.cso").c_str(), &pVS, &vsLen));
        ThrowIfFailed(ReadDataFromFile(AssetPath(L"lightning_LightningPS.cso").c_str(), &pPS, &psLen));
        D3D12_BLEND_DESC blend = {};
        blend.RenderTarget[0].BlendEnable           = TRUE;
        blend.RenderTarget[0].SrcBlend              = D3D12_BLEND_ONE;
        blend.RenderTarget[0].DestBlend             = D3D12_BLEND_ONE;
        blend.RenderTarget[0].BlendOp               = D3D12_BLEND_OP_ADD;
        blend.RenderTarget[0].SrcBlendAlpha         = D3D12_BLEND_ONE;
        blend.RenderTarget[0].DestBlendAlpha        = D3D12_BLEND_ZERO;
        blend.RenderTarget[0].BlendOpAlpha          = D3D12_BLEND_OP_ADD;
        blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature           = m_lightningRootSig.Get();
        pd.VS                       = CD3DX12_SHADER_BYTECODE(pVS, vsLen);
        pd.PS                       = CD3DX12_SHADER_BYTECODE(pPS, psLen);
        pd.RasterizerState          = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.BlendState               = blend;
        pd.DepthStencilState.DepthEnable   = FALSE;
        pd.DepthStencilState.StencilEnable = FALSE;
        pd.DSVFormat             = DXGI_FORMAT_UNKNOWN;
        pd.SampleMask            = UINT_MAX;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets      = 1;
        pd.RTVFormats[0]         = DXGI_FORMAT_R16G16B16A16_FLOAT;
        pd.SampleDesc.Count      = 1;
        ThrowIfFailed(m_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_lightningPSO)));
        free(pVS); free(pPS);
    }
}

// ============================================================
// UpdateLightning / GenerateLightningBolt
// ============================================================
void PostProcessPipeline::UpdateLightning(float intensity, bool newStrike)
{
    m_mappedLightningCB->intensity = intensity;
    if (newStrike)
        GenerateLightningBolt();
    m_prevLightningIntensity = intensity;
}

void PostProcessPipeline::GenerateLightningBolt()
{
    auto& cb = *m_mappedLightningCB;
    cb.aspect = (float)m_width / m_height;

    auto rng = [](float lo, float hi) -> float {
        static unsigned s = 12345;
        s = s * 1664525u + 1013904223u;
        return lo + (s >> 16) / 65535.0f * (hi - lo);
    };

    float sx = rng(0.25f, 0.75f), sy = rng(0.05f, 0.25f);
    float ex = sx + rng(-0.2f, 0.2f), ey = rng(0.55f, 0.72f);
    constexpr int NB = 8;
    cb.pts[0] = { sx, sy, 0, 0 };
    for (int i = 1; i < NB - 1; i++) {
        float t = (float)i / (NB - 1);
        cb.pts[i] = { sx + (ex-sx)*t + rng(-0.055f,0.055f), sy + (ey-sy)*t, 0, 0 };
    }
    cb.pts[NB-1] = { ex, ey, 0, 0 };
    cb.numBolt = NB;

    int split = 2 + (int)rng(0, 3);
    float bx0 = cb.pts[split].x, by0 = cb.pts[split].y;
    float bex = bx0 + rng(-0.12f,0.12f), bey = by0 + rng(0.08f,0.18f);
    constexpr int NBRANCH = 5;
    cb.pts[NB] = { bx0, by0, 0, 0 };
    for (int i = 1; i < NBRANCH-1; i++) {
        float t = (float)i / (NBRANCH-1);
        cb.pts[NB+i] = { bx0+(bex-bx0)*t+rng(-0.022f,0.022f), by0+(bey-by0)*t, 0, 0 };
    }
    cb.pts[NB+NBRANCH-1] = { bex, bey, 0, 0 };
    cb.numBranch = NBRANCH;
}

// ============================================================
// RenderShadowMap
// ============================================================
void PostProcessPipeline::RenderShadowMap(
    ID3D12GraphicsCommandList* cmd,
    SkyDome*        skyDome,
    Renderer*       renderer,
    FloatingObject* fo,
    ShipModel*      ship)
{
    auto dsv = m_shadowDSVHeap->GetCPUDescriptorHandleForHeapStart();
    cmd->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    float sunY    = skyDome->GetSunDirection().y;
    bool  doShadow = shadowEnabled && sunY > 0.05f;

    if (doShadow)
    {
        XMFLOAT3 sunDir = skyDome->GetSunDirection();
        XMFLOAT3 camPos = renderer->GetCameraPos();

        XMVECTOR eye = XMVectorSet(
            camPos.x + sunDir.x * 150.f,
            camPos.y + sunDir.y * 150.f + 80.f,
            camPos.z + sunDir.z * 150.f, 1.f);
        XMVECTOR at  = XMVectorSet(camPos.x, 0.f, camPos.z, 0.f);
        XMVECTOR up  = fabsf(sunDir.y) > 0.98f ?
            XMVectorSet(0,0,1,0) : XMVectorSet(0,1,0,0);

        XMMATRIX view = XMMatrixLookAtLH(eye, at, up);
        XMMATRIX proj = XMMatrixOrthographicLH(200.f, 200.f, 1.f, 400.f);
        m_lightViewProj = view * proj;

        D3D12_VIEWPORT vp = { 0, 0, (float)kShadowSize, (float)kShadowSize, 0, 1 };
        D3D12_RECT     sc = { 0, 0, (LONG)kShadowSize,  (LONG)kShadowSize  };
        cmd->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
        cmd->RSSetViewports(1, &vp);
        cmd->RSSetScissorRects(1, &sc);
        fo->RenderDepth(cmd, m_shadowRootSig.Get(), m_shadowPSO.Get(), m_lightViewProj);
        if (ship) ship->RenderDepth(cmd, m_lightViewProj);
    }


    auto* cb         = reinterpret_cast<ShadowSceneCB*>(m_mappedShadowSceneCB);
    cb->lightViewProj  = XMMatrixTranspose(m_lightViewProj);
    cb->shadowBias     = shadowBias;
    cb->shadowStrength = doShadow ? shadowStrength : 0.0f;
    cb->shadowEnabled  = doShadow ? 1.0f : 0.0f;
    cb->screenW        = (float)m_width;
    cb->screenH        = (float)m_height;
    cb->waterBodyStr   = waterBodyStr;
    cb->waterRefract   = waterRefract;
    cb->waterMinTrans  = waterMinTrans;

    auto b = CD3DX12_RESOURCE_BARRIER::Transition(m_shadowMap.Get(),
        D3D12_RESOURCE_STATE_DEPTH_WRITE,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmd->ResourceBarrier(1, &b);
}

// ============================================================
// TakeSkySnapshot / TakeRefractionSnapshot
// ============================================================
void PostProcessPipeline::TakeSkySnapshot(ID3D12GraphicsCommandList* cmd)
{
    auto b0 = CD3DX12_RESOURCE_BARRIER::Transition(m_hdrRT.Get(),
        D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmd->ResourceBarrier(1, &b0);

    if (m_skySnapshotInPSR) {
        auto b1 = CD3DX12_RESOURCE_BARRIER::Transition(m_skySnapshotRT.Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        cmd->ResourceBarrier(1, &b1);
    }
    cmd->CopyResource(m_skySnapshotRT.Get(), m_hdrRT.Get());

    auto b2 = CD3DX12_RESOURCE_BARRIER::Transition(m_skySnapshotRT.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmd->ResourceBarrier(1, &b2);
    m_skySnapshotInPSR = true;

    auto b3 = CD3DX12_RESOURCE_BARRIER::Transition(m_hdrRT.Get(),
        D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    cmd->ResourceBarrier(1, &b3);
}

void PostProcessPipeline::TakeRefractionSnapshot(ID3D12GraphicsCommandList* cmd)
{
    auto bSrc = CD3DX12_RESOURCE_BARRIER::Transition(m_hdrRT.Get(),
        D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmd->ResourceBarrier(1, &bSrc);

    if (m_refractionInPSR) {
        auto bDst = CD3DX12_RESOURCE_BARRIER::Transition(m_refractionRT.Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        cmd->ResourceBarrier(1, &bDst);
    }
    cmd->CopyResource(m_refractionRT.Get(), m_hdrRT.Get());

    auto bPSR = CD3DX12_RESOURCE_BARRIER::Transition(m_refractionRT.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmd->ResourceBarrier(1, &bPSR);
    m_refractionInPSR = true;

    auto bRT = CD3DX12_RESOURCE_BARRIER::Transition(m_hdrRT.Get(),
        D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    cmd->ResourceBarrier(1, &bRT);
}

// ============================================================
// RenderLightning
// ============================================================
void PostProcessPipeline::RenderLightning(ID3D12GraphicsCommandList* cmd)
{
    if (m_mappedLightningCB->intensity <= 0.001f) return;

    auto hdrRtv = GetHDRRTV();
    cmd->SetGraphicsRootSignature(m_lightningRootSig.Get());
    cmd->SetPipelineState(m_lightningPSO.Get());
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->RSSetViewports(1, &m_viewport);
    cmd->RSSetScissorRects(1, &m_scissor);
    cmd->SetGraphicsRootConstantBufferView(0, m_lightningCB->GetGPUVirtualAddress());
    cmd->OMSetRenderTargets(1, &hdrRtv, FALSE, nullptr);
    cmd->DrawInstanced(3, 1, 0, 0);
}

// ============================================================
// RenderSSAO
// ============================================================
void PostProcessPipeline::RenderSSAO(ID3D12GraphicsCommandList* cmd, Renderer* renderer)
{
    if (!ssaoEnabled) return;

    UINT  hw = m_width / 2, hh = m_height / 2;
    D3D12_VIEWPORT halfVP = { 0, 0, (float)hw, (float)hh, 0, 1 };
    D3D12_RECT     halfSC = { 0, 0, (LONG)hw,  (LONG)hh  };

    XMMATRIX proj = renderer->GetCamera().GetProjMatrix();
    m_mappedSSAOCB->projX  = XMVectorGetX(proj.r[0]);
    m_mappedSSAOCB->projY  = XMVectorGetY(proj.r[1]);
    m_mappedSSAOCB->radius = ssaoRadius;

    auto b0 = CD3DX12_RESOURCE_BARRIER::Transition(renderer->GetDepthBuffer(),
        D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmd->ResourceBarrier(1, &b0);

    auto ssaoRtv = RTV(7);
    auto blurRtv = RTV(8);
    const float kWhite[] = { 1, 1, 1, 1 };

    ID3D12DescriptorHeap* heaps[] = { m_ssaoSRVHeap.Get() };
    cmd->SetDescriptorHeaps(1, heaps);

    cmd->SetGraphicsRootSignature(m_ssaoRootSig.Get());
    cmd->SetPipelineState(m_ssaoPSO.Get());
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->RSSetViewports(1, &halfVP);
    cmd->RSSetScissorRects(1, &halfSC);
    cmd->SetGraphicsRootConstantBufferView(0, m_ssaoCB->GetGPUVirtualAddress());
    cmd->SetGraphicsRootDescriptorTable(1, m_ssaoSRVHeap->GetGPUDescriptorHandleForHeapStart());
    cmd->OMSetRenderTargets(1, &ssaoRtv, FALSE, nullptr);
    cmd->ClearRenderTargetView(ssaoRtv, kWhite, 0, nullptr);
    cmd->DrawInstanced(3, 1, 0, 0);

    {
        D3D12_RESOURCE_BARRIER bars[2] = {
            CD3DX12_RESOURCE_BARRIER::Transition(m_ssaoRT.Get(),
                D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(renderer->GetDepthBuffer(),
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE),
        };
        cmd->ResourceBarrier(2, bars);
    }

    float blurConst[4] = { 1.0f / hw, 1.0f / hh, 0, 0 };
    cmd->SetGraphicsRootSignature(m_ssaoBlurRootSig.Get());
    cmd->SetPipelineState(m_ssaoBlurPSO.Get());
    cmd->SetGraphicsRoot32BitConstants(1, 4, blurConst, 0);
    cmd->SetGraphicsRootDescriptorTable(0,
        CD3DX12_GPU_DESCRIPTOR_HANDLE(m_ssaoSRVHeap->GetGPUDescriptorHandleForHeapStart(),
            1, m_ssaoSRVIncrSize));
    cmd->OMSetRenderTargets(1, &blurRtv, FALSE, nullptr);
    cmd->DrawInstanced(3, 1, 0, 0);

    auto b1 = CD3DX12_RESOURCE_BARRIER::Transition(m_ssaoBlurRT.Get(),
        D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmd->ResourceBarrier(1, &b1);
}

// ============================================================
// RenderPostProcess
// ============================================================
void PostProcessPipeline::RenderPostProcess(
    ID3D12GraphicsCommandList* cmd,
    UINT                        frameIndex,
    D3D12_CPU_DESCRIPTOR_HANDLE swapRTV,
    Renderer*                   renderer,
    SkyDome*                    skyDome)
{

    {
        auto bar = CD3DX12_RESOURCE_BARRIER::Transition(m_hdrRT.Get(),
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cmd->ResourceBarrier(1, &bar);
    }

    RenderTAA(cmd);
    RenderBloom(cmd);
    RenderGodRays(cmd, renderer, skyDome);
    RenderDOF(cmd);
    RenderToneMap(cmd, frameIndex, swapRTV, renderer);
    RenderLensFlare(cmd, frameIndex, swapRTV, renderer, skyDome);
}

// ============================================================
// RenderBloom
// ============================================================
void PostProcessPipeline::RenderBloom(ID3D12GraphicsCommandList* cmd)
{
    if (!bloomEnabled) return;

    const float kBlack[] = { 0, 0, 0, 0 };
    auto rtvExtract = RTV(0);
    auto rtvBlur    = RTV(1);
    auto gpuSlot0 = m_bloomSRVHeap->GetGPUDescriptorHandleForHeapStart();
    auto gpuSlot1 = CD3DX12_GPU_DESCRIPTOR_HANDLE(
        m_bloomSRVHeap->GetGPUDescriptorHandleForHeapStart(), 1, m_bloomSRVIncrSize);
    auto gpuSlot2 = CD3DX12_GPU_DESCRIPTOR_HANDLE(
        m_bloomSRVHeap->GetGPUDescriptorHandleForHeapStart(), 2, m_bloomSRVIncrSize);

    cmd->SetGraphicsRootSignature(m_bloomRootSig.Get());
    ID3D12DescriptorHeap* heaps[] = { m_bloomSRVHeap.Get() };
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->RSSetViewports(1, &m_viewport);
    cmd->RSSetScissorRects(1, &m_scissor);

    float pBright[4] = { bloomThreshold, 0.f, 0.f, 0.f };
    cmd->SetPipelineState(m_bloomBrightPSO.Get());
    cmd->SetGraphicsRoot32BitConstants(1, 4, pBright, 0);
    cmd->SetGraphicsRootDescriptorTable(0, gpuSlot0);
    cmd->OMSetRenderTargets(1, &rtvExtract, FALSE, nullptr);
    cmd->ClearRenderTargetView(rtvExtract, kBlack, 0, nullptr);
    cmd->DrawInstanced(3, 1, 0, 0);

    float pBlurH[4] = { 0.f, 0.f, 1.f, 0.f };
    float pBlurV[4] = { 0.f, 0.f, 0.f, 1.f };
    for (int iter = 0; iter < 2; ++iter)
    {
        {
            auto bar = CD3DX12_RESOURCE_BARRIER::Transition(m_bloomExtractRT.Get(),
                D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            cmd->ResourceBarrier(1, &bar);
        }
        if (iter > 0) {
            auto bar = CD3DX12_RESOURCE_BARRIER::Transition(m_bloomBlurRT.Get(),
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            cmd->ResourceBarrier(1, &bar);
        }
        cmd->SetPipelineState(m_bloomBlurPSO.Get());
        cmd->SetGraphicsRoot32BitConstants(1, 4, pBlurH, 0);
        cmd->SetGraphicsRootDescriptorTable(0, gpuSlot1);
        cmd->OMSetRenderTargets(1, &rtvBlur, FALSE, nullptr);
        cmd->ClearRenderTargetView(rtvBlur, kBlack, 0, nullptr);
        cmd->DrawInstanced(3, 1, 0, 0);

        {
            D3D12_RESOURCE_BARRIER bars[2] = {
                CD3DX12_RESOURCE_BARRIER::Transition(m_bloomBlurRT.Get(),
                    D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
                CD3DX12_RESOURCE_BARRIER::Transition(m_bloomExtractRT.Get(),
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
            };
            cmd->ResourceBarrier(2, bars);
        }
        cmd->SetGraphicsRoot32BitConstants(1, 4, pBlurV, 0);
        cmd->SetGraphicsRootDescriptorTable(0, gpuSlot2);
        cmd->OMSetRenderTargets(1, &rtvExtract, FALSE, nullptr);
        cmd->ClearRenderTargetView(rtvExtract, kBlack, 0, nullptr);
        cmd->DrawInstanced(3, 1, 0, 0);
    }
}

// ============================================================
// RenderGodRays
// ============================================================
void PostProcessPipeline::RenderGodRays(
    ID3D12GraphicsCommandList* cmd,
    Renderer* renderer,
    SkyDome*  skyDome)
{
    auto godRayRtv = RTV(3);
    const float kBlack[] = { 0, 0, 0, 0 };
    cmd->ClearRenderTargetView(godRayRtv, kBlack, 0, nullptr);

    if (!godRaysEnabled) return;

    XMFLOAT3 sd3 = skyDome->GetSunDirection();
    if (sd3.y < -0.04f) return;

    XMVECTOR sunWorld = XMVectorSetW(XMVectorScale(XMVector3Normalize(XMLoadFloat3(&sd3)), 999.0f), 1.0f);
    XMMATRIX vp       = XMMatrixMultiply(renderer->GetViewMatrix(), renderer->GetProjMatrix());
    XMVECTOR clip     = XMVector4Transform(sunWorld, vp);

    float w = XMVectorGetW(clip);
    float sunScreenX = 0.5f, sunScreenY = 0.5f, sunVis = 0.0f;
    if (w > 0.0f) {
        sunScreenX = XMVectorGetX(clip) / w * 0.5f + 0.5f;
        sunScreenY = -XMVectorGetY(clip) / w * 0.5f + 0.5f;
        sunVis = std::clamp(sd3.y * 3.0f + 0.3f, 0.0f, 1.0f);
        float ax = fabsf(sunScreenX - 0.5f), ay = fabsf(sunScreenY - 0.5f);
        float offScreen = ax > ay ? ax : ay;
        sunVis *= std::clamp(1.0f - (offScreen - 0.5f) * 3.0f, 0.0f, 1.0f);
    }
    if (sunVis <= 0.001f) return;

    UINT grW = m_width / 2, grH = m_height / 2;
    D3D12_VIEWPORT grVP = { 0, 0, (float)grW, (float)grH, 0, 1 };
    D3D12_RECT     grSC = { 0, 0, (LONG)grW,  (LONG)grH };

    struct GRParams { float sx, sy, density, decay, weight, vis; }
        p = { sunScreenX, sunScreenY, 0.96f, 0.97f, 0.04f, sunVis };

    cmd->SetGraphicsRootSignature(m_godRayRootSig.Get());
    ID3D12DescriptorHeap* heaps[] = { m_bloomSRVHeap.Get() };
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->RSSetViewports(1, &grVP);
    cmd->RSSetScissorRects(1, &grSC);
    cmd->SetPipelineState(m_godRayPSO.Get());
    cmd->SetGraphicsRoot32BitConstants(1, 6, &p, 0);
    cmd->SetGraphicsRootDescriptorTable(0, m_bloomSRVHeap->GetGPUDescriptorHandleForHeapStart());
    cmd->OMSetRenderTargets(1, &godRayRtv, FALSE, nullptr);
    cmd->DrawInstanced(3, 1, 0, 0);
}

// ============================================================
// RenderDOF
// ============================================================
void PostProcessPipeline::RenderDOF(ID3D12GraphicsCommandList* cmd)
{
    if (!dofEnabled) return;


    auto b0 = CD3DX12_RESOURCE_BARRIER::Transition(m_depthBuffer,
        D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmd->ResourceBarrier(1, &b0);

    auto dofRtv = RTV(4);
    const float kBlack[] = { 0, 0, 0, 0 };
    struct DOFParams { float focusDepth, focusRange, maxRadius, aspect; }
        p = { dofFocusDepth, dofFocusRange, dofMaxRadius, (float)m_width / m_height };

    cmd->SetGraphicsRootSignature(m_dofRootSig.Get());
    ID3D12DescriptorHeap* heaps[] = { m_dofSRVHeap.Get() };
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->RSSetViewports(1, &m_viewport);
    cmd->RSSetScissorRects(1, &m_scissor);
    cmd->SetPipelineState(m_dofPSO.Get());
    cmd->SetGraphicsRoot32BitConstants(1, 4, &p, 0);
    cmd->SetGraphicsRootDescriptorTable(0, m_dofSRVHeap->GetGPUDescriptorHandleForHeapStart());
    cmd->OMSetRenderTargets(1, &dofRtv, FALSE, nullptr);
    cmd->ClearRenderTargetView(dofRtv, kBlack, 0, nullptr);
    cmd->DrawInstanced(3, 1, 0, 0);


    auto b1 = CD3DX12_RESOURCE_BARRIER::Transition(m_depthBuffer,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    cmd->ResourceBarrier(1, &b1);

    auto b2 = CD3DX12_RESOURCE_BARRIER::Transition(m_dofRT.Get(),
        D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmd->ResourceBarrier(1, &b2);


    D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format                  = DXGI_FORMAT_R16G16B16A16_FLOAT;
    sd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels     = 1;
    m_device->CreateShaderResourceView(m_dofRT.Get(), &sd,
        m_bloomSRVHeap->GetCPUDescriptorHandleForHeapStart());
}

// ============================================================
// RenderTAA
// ============================================================
void PostProcessPipeline::RenderTAA(ID3D12GraphicsCommandList* cmd)
{
    if (!taaEnabled) return;

    if (!m_taaHistoryInPSR) {
        auto bar = CD3DX12_RESOURCE_BARRIER::Transition(m_taaHistoryRT.Get(),
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cmd->ResourceBarrier(1, &bar);
        m_taaHistoryInPSR = true;
    }

    auto taaRtv = RTV(5);
    struct TAACBData { float texW, texH, blend, pad; }
        cb = { 1.0f/m_width, 1.0f/m_height, m_taaHistoryValid ? taaBlend : 0.0f, 0.0f };

    cmd->SetGraphicsRootSignature(m_taaRootSig.Get());
    ID3D12DescriptorHeap* heaps[] = { m_taaSRVHeap.Get() };
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->RSSetViewports(1, &m_viewport);
    cmd->RSSetScissorRects(1, &m_scissor);
    cmd->SetPipelineState(m_taaPSO.Get());
    cmd->SetGraphicsRoot32BitConstants(1, 4, &cb, 0);
    cmd->SetGraphicsRootDescriptorTable(0, m_taaSRVHeap->GetGPUDescriptorHandleForHeapStart());
    cmd->OMSetRenderTargets(1, &taaRtv, FALSE, nullptr);
    cmd->DrawInstanced(3, 1, 0, 0);
    m_taaHistoryValid = true;

    {
        D3D12_RESOURCE_BARRIER bars[2] = {
            CD3DX12_RESOURCE_BARRIER::Transition(m_taaRT.Get(),
                D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(m_taaHistoryRT.Get(),
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST),
        };
        cmd->ResourceBarrier(2, bars);
    }
    cmd->CopyResource(m_taaHistoryRT.Get(), m_taaRT.Get());
    {
        D3D12_RESOURCE_BARRIER bars[2] = {
            CD3DX12_RESOURCE_BARRIER::Transition(m_taaRT.Get(),
                D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(m_taaHistoryRT.Get(),
                D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
        };
        cmd->ResourceBarrier(2, bars);
    }


    D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format                  = DXGI_FORMAT_R16G16B16A16_FLOAT;
    sd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels     = 1;
    m_device->CreateShaderResourceView(m_taaRT.Get(), &sd,
        m_bloomSRVHeap->GetCPUDescriptorHandleForHeapStart());
}

// ============================================================
// RenderToneMap
// ============================================================
void PostProcessPipeline::RenderToneMap(
    ID3D12GraphicsCommandList* cmd,
    UINT                        /*frameIndex*/,
    D3D12_CPU_DESCRIPTOR_HANDLE swapRTV,
    Renderer*                   renderer)
{
    {
        D3D12_RESOURCE_BARRIER bars[2] = {
            CD3DX12_RESOURCE_BARRIER::Transition(m_bloomExtractRT.Get(),
                D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(m_godRayRT.Get(),
                D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
        };
        cmd->ResourceBarrier(2, bars);
    }

    auto gpuSlot0 = m_bloomSRVHeap->GetGPUDescriptorHandleForHeapStart();
    auto gpuSlot3 = CD3DX12_GPU_DESCRIPTOR_HANDLE(
        m_bloomSRVHeap->GetGPUDescriptorHandleForHeapStart(), 3, m_bloomSRVIncrSize);

    cmd->SetGraphicsRootSignature(m_toneMappingRootSig.Get());
    ID3D12DescriptorHeap* heaps[] = { m_bloomSRVHeap.Get() };
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->RSSetViewports(1, &m_viewport);
    cmd->RSSetScissorRects(1, &m_scissor);

    float camY = renderer->GetCameraPos().y;
    float aboveWater  = saturate( camY / 0.5f);
    float underWater  = saturate(-camY / 0.5f) * 0.5f;
    float godRayFactor = max(aboveWater, underWater);
    float params[12] = {
        bloomStrength, exposure, godRayStrength * godRayFactor,
        vignetteStrength, grainStrength, renderer->GetTime(),
        ssaoEnabled ? ssaoStrength : 0.0f, 0.0f,
        camY, 0.0f, 0.0f, 0.0f
    };
    cmd->SetPipelineState(m_toneMappingPSO.Get());
    cmd->SetGraphicsRoot32BitConstants(2, 12, params, 0);
    cmd->SetGraphicsRootDescriptorTable(0, gpuSlot0);
    cmd->SetGraphicsRootDescriptorTable(1, gpuSlot3);
    cmd->OMSetRenderTargets(1, &swapRTV, FALSE, nullptr);
    cmd->DrawInstanced(3, 1, 0, 0);


    D3D12_RESOURCE_BARRIER cleanup[3] = {
        CD3DX12_RESOURCE_BARRIER::Transition(m_hdrRT.Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
        CD3DX12_RESOURCE_BARRIER::Transition(m_bloomExtractRT.Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
        CD3DX12_RESOURCE_BARRIER::Transition(m_godRayRT.Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
    };
    cmd->ResourceBarrier(3, cleanup);

    if (bloomEnabled) {
        auto bar = CD3DX12_RESOURCE_BARRIER::Transition(m_bloomBlurRT.Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        cmd->ResourceBarrier(1, &bar);
    }
    if (dofEnabled) {
        auto bar = CD3DX12_RESOURCE_BARRIER::Transition(m_dofRT.Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        cmd->ResourceBarrier(1, &bar);
    }
    if (taaEnabled && m_taaHistoryValid) {
        auto bar = CD3DX12_RESOURCE_BARRIER::Transition(m_taaRT.Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        cmd->ResourceBarrier(1, &bar);
    }
    if (m_skySnapshotInPSR) {
        auto bar = CD3DX12_RESOURCE_BARRIER::Transition(m_skySnapshotRT.Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        cmd->ResourceBarrier(1, &bar);
        m_skySnapshotInPSR = false;
    }
    if (ssaoEnabled) {
        D3D12_RESOURCE_BARRIER bars[2] = {
            CD3DX12_RESOURCE_BARRIER::Transition(m_ssaoRT.Get(),
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
            CD3DX12_RESOURCE_BARRIER::Transition(m_ssaoBlurRT.Get(),
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
        };
        cmd->ResourceBarrier(2, bars);
    }
    {
        auto b = CD3DX12_RESOURCE_BARRIER::Transition(m_shadowMap.Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        cmd->ResourceBarrier(1, &b);
    }
    if (m_refractionInPSR) {
        auto b = CD3DX12_RESOURCE_BARRIER::Transition(m_refractionRT.Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        cmd->ResourceBarrier(1, &b);
        m_refractionInPSR = false;
    }


    {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format                  = DXGI_FORMAT_R16G16B16A16_FLOAT;
        sd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels     = 1;
        m_device->CreateShaderResourceView(m_hdrRT.Get(), &sd,
            m_bloomSRVHeap->GetCPUDescriptorHandleForHeapStart());
    }
}

// ============================================================
// RenderLensFlare
// ============================================================
void PostProcessPipeline::RenderLensFlare(
    ID3D12GraphicsCommandList* cmd,
    UINT /*frameIndex*/,
    D3D12_CPU_DESCRIPTOR_HANDLE swapRTV,
    Renderer*  renderer,
    SkyDome*   skyDome)
{
    if (!lensFlareEnabled) return;
    if (renderer->GetCameraPos().y < -0.3f) return;  // hidden underwater

    XMFLOAT3 sd3 = skyDome->GetSunDirection();
    if (sd3.y < -0.04f) return;

    XMVECTOR sunWorld = XMVectorSetW(XMVectorScale(XMVector3Normalize(XMLoadFloat3(&sd3)), 999.0f), 1.0f);
    XMMATRIX vp       = XMMatrixMultiply(renderer->GetViewMatrix(), renderer->GetProjMatrix());
    XMVECTOR clip     = XMVector4Transform(sunWorld, vp);

    float w = XMVectorGetW(clip);
    float sunScreenX = 0.5f, sunScreenY = 0.5f, sunVis = 0.0f;
    if (w > 0.0f) {
        sunScreenX = XMVectorGetX(clip) / w * 0.5f + 0.5f;
        sunScreenY = -XMVectorGetY(clip) / w * 0.5f + 0.5f;
        sunVis = std::clamp(sd3.y * 3.0f + 0.3f, 0.0f, 1.0f);
        float ax = fabsf(sunScreenX - 0.5f), ay = fabsf(sunScreenY - 0.5f);
        float offScreen = ax > ay ? ax : ay;
        sunVis *= std::clamp(1.0f - (offScreen - 0.5f) * 3.0f, 0.0f, 1.0f);
    }
    if (sunVis <= 0.001f) return;

    struct LFParams { float sx, sy, vis, strength, aspect, time; }
        p = { sunScreenX, sunScreenY, sunVis, lensFlareStrength,
              (float)m_width / m_height, renderer->GetTime() };

    cmd->SetGraphicsRootSignature(m_lensFlareRootSig.Get());
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->RSSetViewports(1, &m_viewport);
    cmd->RSSetScissorRects(1, &m_scissor);
    cmd->SetPipelineState(m_lensFlarePSO.Get());
    cmd->SetGraphicsRoot32BitConstants(0, 6, &p, 0);
    cmd->OMSetRenderTargets(1, &swapRTV, FALSE, nullptr);
    cmd->DrawInstanced(3, 1, 0, 0);
}
