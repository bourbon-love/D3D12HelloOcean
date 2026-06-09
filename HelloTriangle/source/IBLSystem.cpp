// ============================================================
// IBLSystem.cpp
// ============================================================
#include "IBLSystem.h"
#include "SkyDome.h"
#include <d3dx12_barriers.h>
#include <d3dx12_root_signature.h>
#include <DirectXMath.h>
using namespace DirectX;

// C++ mirror of CaptureCB in SkyCaptureCS.hlsl.
// Layout: 7 float3+pad rows (112 bytes) + 4 scalar fields (16 bytes) + 8×float4 pad = 256 bytes.
struct alignas(256) CaptureCB
{
    XMFLOAT3 topColor;          float padTop;
    XMFLOAT3 middleColor;       float padMiddle;
    XMFLOAT3 bottomColor;       float padBottom;
    XMFLOAT3 sunPosition;       float time;
    XMFLOAT3 sunColor;          float lightningIntensity;
    XMFLOAT3 moonPosition;      float cameraY;
    XMFLOAT3 moonCrescentDir;   float moonBodyPow;
    float    moonOccludePow;
    float    crescentOffsetAmt;
    int      faceIndex;
    int      faceSize;
    XMFLOAT4 _reserved[8];
};
static_assert(sizeof(CaptureCB) == 256, "CaptureCB must be exactly 256 bytes");

// -----------------------------------------------
void IBLSystem::Init(
    ComPtr<ID3D12Device>       device,
    ComPtr<ID3D12CommandQueue> cmdQueue,
    UINT cubemapFaceSize,
    UINT brdfLutSize,
    const UINT8* captureCSData, UINT captureCSSize,
    const UINT8* brdfLutCSData, UINT brdfLutCSSize)
{
    m_device   = device;
    m_faceSize = cubemapFaceSize;
    m_lutSize  = brdfLutSize;

    CreateTextures(cubemapFaceSize, brdfLutSize);
    CreateDescriptorHeaps();
    CreateRootSignatures();
    CreatePSOs(captureCSData, captureCSSize, brdfLutCSData, brdfLutCSSize);
    CreateConstantBuffers();
    RunBRDFLutOnce(cmdQueue);
}

// -----------------------------------------------
void IBLSystem::CreateTextures(UINT cubemapFaceSize, UINT brdfLutSize)
{
    auto heapProp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);

    // Cubemap: 6-face RGBA16F array, UAV-writable (SkyCaptureCS writes one face per dispatch).
    // Created in UAV state; transitions to NON_PIXEL_SHADER_RESOURCE only when sampled (Phase B+).
    {
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width            = cubemapFaceSize;
        desc.Height           = cubemapFaceSize;
        desc.DepthOrArraySize = 6;
        desc.MipLevels        = 1;
        desc.Format           = DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ThrowIfFailed(m_device->CreateCommittedResource(
            &heapProp, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
            IID_PPV_ARGS(&m_captureCubemap)));
    }

    // BRDF LUT: single RG16F texture, UAV-writable (BRDFLutCS writes it once at init).
    {
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width            = brdfLutSize;
        desc.Height           = brdfLutSize;
        desc.DepthOrArraySize = 1;
        desc.MipLevels        = 1;
        desc.Format           = DXGI_FORMAT_R16G16_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ThrowIfFailed(m_device->CreateCommittedResource(
            &heapProp, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
            IID_PPV_ARGS(&m_brdfLut)));
    }
}

// -----------------------------------------------
void IBLSystem::CreateDescriptorHeaps()
{
    UINT descSize = m_device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // m_captureUAVHeap: 6 slots, one TEXTURE2DARRAY UAV per face.
    // Cubemaps have no native UAV view — each face is addressed via
    // FirstArraySlice = faceIndex, ArraySize = 1 in the array-slice mechanism.
    {
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.NumDescriptors = 6;
        heapDesc.Type  = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ThrowIfFailed(m_device->CreateDescriptorHeap(
            &heapDesc, IID_PPV_ARGS(&m_captureUAVHeap)));

        auto cpu = m_captureUAVHeap->GetCPUDescriptorHandleForHeapStart();
        for (UINT face = 0; face < 6; ++face)
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
            uavDesc.Format                         = DXGI_FORMAT_R16G16B16A16_FLOAT;
            uavDesc.ViewDimension                  = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
            uavDesc.Texture2DArray.MipSlice        = 0;
            uavDesc.Texture2DArray.FirstArraySlice = face;
            uavDesc.Texture2DArray.ArraySize       = 1;
            m_device->CreateUnorderedAccessView(
                m_captureCubemap.Get(), nullptr, &uavDesc, cpu);
            cpu.ptr += descSize;
        }
    }

    // m_lutUAVHeap: 1 slot, TEXTURE2D UAV for BRDFLutCS output.
    {
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.NumDescriptors = 1;
        heapDesc.Type  = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ThrowIfFailed(m_device->CreateDescriptorHeap(
            &heapDesc, IID_PPV_ARGS(&m_lutUAVHeap)));

        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.Format             = DXGI_FORMAT_R16G16_FLOAT;
        uavDesc.ViewDimension      = D3D12_UAV_DIMENSION_TEXTURE2D;
        uavDesc.Texture2D.MipSlice = 0;
        m_device->CreateUnorderedAccessView(
            m_brdfLut.Get(), nullptr, &uavDesc,
            m_lutUAVHeap->GetCPUDescriptorHandleForHeapStart());
    }

    // m_debugSRVHeap: 2 slots — TEXTURECUBE SRV (slot 0) + TEXTURE2D SRV (slot 1).
    // Used by ImGui's debug preview (Task 7) to display the captured cubemap and LUT.
    {
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.NumDescriptors = 2;
        heapDesc.Type  = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ThrowIfFailed(m_device->CreateDescriptorHeap(
            &heapDesc, IID_PPV_ARGS(&m_debugSRVHeap)));

        auto cpu = m_debugSRVHeap->GetCPUDescriptorHandleForHeapStart();

        // Slot 0: TEXTURECUBE SRV for the captured environment cubemap
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Format                      = DXGI_FORMAT_R16G16B16A16_FLOAT;
            srvDesc.ViewDimension               = D3D12_SRV_DIMENSION_TEXTURECUBE;
            srvDesc.Shader4ComponentMapping     = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.TextureCube.MipLevels       = 1;
            srvDesc.TextureCube.MostDetailedMip = 0;
            m_device->CreateShaderResourceView(m_captureCubemap.Get(), &srvDesc, cpu);
            cpu.ptr += descSize;
        }

        // Slot 1: TEXTURE2D SRV for the BRDF LUT
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Format                    = DXGI_FORMAT_R16G16_FLOAT;
            srvDesc.ViewDimension             = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Shader4ComponentMapping   = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Texture2D.MipLevels       = 1;
            srvDesc.Texture2D.MostDetailedMip = 0;
            m_device->CreateShaderResourceView(m_brdfLut.Get(), &srvDesc, cpu);
        }
    }
}

// -----------------------------------------------
void IBLSystem::CreateRootSignatures()
{
    // Capture root sig: [0]=CBV(b0) for CaptureCB, [1]=Table(1 UAV: u0)
    {
        CD3DX12_DESCRIPTOR_RANGE1 uavRange;
        uavRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);

        CD3DX12_ROOT_PARAMETER1 params[2];
        params[0].InitAsConstantBufferView(0);
        params[1].InitAsDescriptorTable(1, &uavRange);

        CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC desc;
        desc.Init_1_1(2, params);

        ComPtr<ID3DBlob> sig, err;
        ThrowIfFailed(D3DX12SerializeVersionedRootSignature(
            &desc, D3D_ROOT_SIGNATURE_VERSION_1_1, &sig, &err));
        ThrowIfFailed(m_device->CreateRootSignature(
            0, sig->GetBufferPointer(), sig->GetBufferSize(),
            IID_PPV_ARGS(&m_captureRootSig)));
    }

    // LUT root sig: [0]=Table(1 UAV: u0) only — BRDFLutCS declares no cbuffer,
    // so no CBV slot is needed and binding a null address is avoided.
    {
        CD3DX12_DESCRIPTOR_RANGE1 uavRange;
        uavRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);

        CD3DX12_ROOT_PARAMETER1 param;
        param.InitAsDescriptorTable(1, &uavRange);

        CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC desc;
        desc.Init_1_1(1, &param);

        ComPtr<ID3DBlob> sig, err;
        ThrowIfFailed(D3DX12SerializeVersionedRootSignature(
            &desc, D3D_ROOT_SIGNATURE_VERSION_1_1, &sig, &err));
        ThrowIfFailed(m_device->CreateRootSignature(
            0, sig->GetBufferPointer(), sig->GetBufferSize(),
            IID_PPV_ARGS(&m_lutRootSig)));
    }
}

// -----------------------------------------------
void IBLSystem::CreatePSOs(
    const UINT8* captureCSData, UINT captureCSSize,
    const UINT8* brdfLutCSData, UINT brdfLutCSSize)
{
    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};

    psoDesc.pRootSignature = m_captureRootSig.Get();
    psoDesc.CS = CD3DX12_SHADER_BYTECODE(captureCSData, captureCSSize);
    ThrowIfFailed(m_device->CreateComputePipelineState(
        &psoDesc, IID_PPV_ARGS(&m_capturePSO)));

    psoDesc.pRootSignature = m_lutRootSig.Get();
    psoDesc.CS = CD3DX12_SHADER_BYTECODE(brdfLutCSData, brdfLutCSSize);
    ThrowIfFailed(m_device->CreateComputePipelineState(
        &psoDesc, IID_PPV_ARGS(&m_lutPSO)));
}

// -----------------------------------------------
void IBLSystem::CreateConstantBuffers()
{
    auto heapProp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    CD3DX12_RANGE readRange(0, 0);
    // Single 256-byte slot; CaptureCB is updated via memcpy each Dispatch call.
    auto bufDesc = CD3DX12_RESOURCE_DESC::Buffer(256);
    ThrowIfFailed(m_device->CreateCommittedResource(
        &heapProp, D3D12_HEAP_FLAG_NONE, &bufDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&m_captureCB)));
    ThrowIfFailed(m_captureCB->Map(
        0, &readRange, reinterpret_cast<void**>(&m_captureCBMapped)));
}

// -----------------------------------------------
void IBLSystem::RunBRDFLutOnce(ComPtr<ID3D12CommandQueue> cmdQueue)
{
    ComPtr<ID3D12CommandAllocator>    alloc;
    ComPtr<ID3D12GraphicsCommandList> cmd;
    ThrowIfFailed(m_device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)));
    ThrowIfFailed(m_device->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr,
        IID_PPV_ARGS(&cmd)));

    cmd->SetComputeRootSignature(m_lutRootSig.Get());
    cmd->SetPipelineState(m_lutPSO.Get());

    ID3D12DescriptorHeap* heaps[] = { m_lutUAVHeap.Get() };
    cmd->SetDescriptorHeaps(1, heaps);
    // LUT root sig has only one slot: [0] = UAV table
    cmd->SetComputeRootDescriptorTable(
        0, m_lutUAVHeap->GetGPUDescriptorHandleForHeapStart());

    cmd->Dispatch(m_lutSize / 8, m_lutSize / 8, 1);

    // Transition LUT from UAV to NON_PIXEL_SHADER_RESOURCE so the future
    // Phase B/C samplers (running in VS/CS) can read it without a per-frame barrier.
    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        m_brdfLut.Get(),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cmd->ResourceBarrier(1, &barrier);

    ThrowIfFailed(cmd->Close());
    ID3D12CommandList* cmds[] = { cmd.Get() };
    cmdQueue->ExecuteCommandLists(1, cmds);

    ComPtr<ID3D12Fence> fence;
    ThrowIfFailed(m_device->CreateFence(
        0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    HANDLE evt = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    cmdQueue->Signal(fence.Get(), 1);
    if (fence->GetCompletedValue() < 1)
    {
        fence->SetEventOnCompletion(1, evt);
        WaitForSingleObject(evt, INFINITE);
    }
    CloseHandle(evt);

    m_lutGenerated = true;
}

// -----------------------------------------------
void IBLSystem::Dispatch(
    ComPtr<ID3D12GraphicsCommandList> cmdList,
    SkyDome* skyDome,
    float /*time*/)
{
    // Fill CaptureCB from SkyDome's current live state (gradient + celestial params).
    SkyDome::CaptureState cs = skyDome->GetCaptureState();

    CaptureCB cb = {};
    cb.topColor          = cs.topColor;
    cb.middleColor       = cs.middleColor;
    cb.bottomColor       = cs.bottomColor;
    cb.sunPosition       = cs.sunPosition;
    cb.time              = cs.time;
    cb.sunColor          = cs.sunColor;
    cb.lightningIntensity= cs.lightningIntensity;
    cb.moonPosition      = cs.moonPosition;
    cb.cameraY           = cs.cameraY;
    cb.moonCrescentDir   = cs.moonCrescentDir;
    cb.moonBodyPow       = cs.moonBodyPow;
    cb.moonOccludePow    = cs.moonOccludePow;
    cb.crescentOffsetAmt = cs.crescentOffsetAmt;
    cb.faceIndex         = m_currentFace;
    cb.faceSize          = static_cast<int>(m_faceSize);
    memcpy(m_captureCBMapped, &cb, sizeof(cb));

    // Dispatch SkyCaptureCS for the current face.
    UINT descSize = m_device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    cmdList->SetComputeRootSignature(m_captureRootSig.Get());
    cmdList->SetPipelineState(m_capturePSO.Get());

    ID3D12DescriptorHeap* heaps[] = { m_captureUAVHeap.Get() };
    cmdList->SetDescriptorHeaps(1, heaps);

    cmdList->SetComputeRootConstantBufferView(
        0, m_captureCB->GetGPUVirtualAddress());

    D3D12_GPU_DESCRIPTOR_HANDLE faceHandle =
        m_captureUAVHeap->GetGPUDescriptorHandleForHeapStart();
    faceHandle.ptr += static_cast<UINT64>(m_currentFace) * descSize;
    cmdList->SetComputeRootDescriptorTable(1, faceHandle);

    cmdList->Dispatch(m_faceSize / 8, m_faceSize / 8, 1);

    // UAV barrier: ensure the write to this face is visible before any later reader.
    // Nothing reads the cubemap in Phase A, but correct barrier discipline now
    // means Phase B doesn't have to retrofit it.
    auto uavBarrier = CD3DX12_RESOURCE_BARRIER::UAV(m_captureCubemap.Get());
    cmdList->ResourceBarrier(1, &uavBarrier);

    // Advance to the next face (rolling 6-frame cycle).
    m_currentFace = (m_currentFace + 1) % 6;
}
