#include "OceanFFT.h"
#include <d3dx12_barriers.h>
#include <d3dx12_root_signature.h>

struct PhillipsCB
{
    UINT  N;
    float A;
    float windSpeed;
    float windDirX;
    float windDirY;
    float pad0, pad1, pad2;
};

struct TimeCB
{
    UINT  N;
    float time;
    float pad0, pad1;
};

struct IFFTCB
{
    UINT N;
    UINT passIdx;
    UINT stepSize;
    UINT pingpong;
};

// -----------------------------------------------
void OceanFFT::Init(
    ComPtr<ID3D12Device>       device,
    ComPtr<ID3D12CommandQueue> cmdQueue,
    UINT textureSize,
    const UINT8* phillipsCSData, UINT phillipsCSSize,
    const UINT8* timeEvoCSData, UINT timeEvoCSSize,
    const UINT8* ifftCSData, UINT ifftCSSize)
{
    m_device = device;
    m_textureSize = textureSize;

    CreateTextures();
    CreateDescriptorHeaps();
    CreateRootSignatures();
    CreatePSOs(phillipsCSData, phillipsCSSize,
        timeEvoCSData, timeEvoCSSize,
        ifftCSData, ifftCSSize);
    CreateConstantBuffers();
    RunPhillipsInit(cmdQueue);
}

// -----------------------------------------------
void OceanFFT::CreateTextures()
{
    auto heapProp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);

    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = m_textureSize;
    texDesc.Height = m_textureSize;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    texDesc.SampleDesc.Count = 1;
    texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    // h0
    ThrowIfFailed(m_device->CreateCommittedResource(
        &heapProp, D3D12_HEAP_FLAG_NONE, &texDesc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
        IID_PPV_ARGS(&m_h0Map)));

    // hkt: h(k,t) + Dx(k,t)
    ThrowIfFailed(m_device->CreateCommittedResource(
        &heapProp, D3D12_HEAP_FLAG_NONE, &texDesc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
        IID_PPV_ARGS(&m_hktMap)));

    // heightMap: h+Dx IFFT最终结果
    ThrowIfFailed(m_device->CreateCommittedResource(
        &heapProp, D3D12_HEAP_FLAG_NONE, &texDesc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
        IID_PPV_ARGS(&m_heightMap)));

    // tempMap: h+Dx IFFT pingpong缓冲
    ThrowIfFailed(m_device->CreateCommittedResource(
        &heapProp, D3D12_HEAP_FLAG_NONE, &texDesc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
        IID_PPV_ARGS(&m_tempMap)));

    // dztMap: Dz(k,t) 频域；IFFT后存Dz结果
    ThrowIfFailed(m_device->CreateCommittedResource(
        &heapProp, D3D12_HEAP_FLAG_NONE, &texDesc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
        IID_PPV_ARGS(&m_dztMap)));

    // dztTempMap: Dz IFFT pingpong缓冲
    ThrowIfFailed(m_device->CreateCommittedResource(
        &heapProp, D3D12_HEAP_FLAG_NONE, &texDesc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
        IID_PPV_ARGS(&m_dztTempMap)));
}
// -----------------------------------------------
void OceanFFT::CreateDescriptorHeaps()
{
    UINT descSize = m_device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format        = DXGI_FORMAT_R32G32B32A32_FLOAT;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format                  = DXGI_FORMAT_R32G32B32A32_FLOAT;
    srvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels     = 1;

    // phillipsHeap: 1个槽，h0 UAV
    {
        D3D12_DESCRIPTOR_HEAP_DESC desc = {};
        desc.NumDescriptors = 1;
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ThrowIfFailed(m_device->CreateDescriptorHeap(
            &desc, IID_PPV_ARGS(&m_phillipsHeap)));

        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        m_device->CreateUnorderedAccessView(
            m_h0Map.Get(), nullptr, &uavDesc,
            m_phillipsHeap->GetCPUDescriptorHandleForHeapStart());
    }

    // TimeEvo heap: slot0=h0 SRV, slot1=hkt UAV, slot2=dztMap UAV
    {
        D3D12_DESCRIPTOR_HEAP_DESC desc = {};
        desc.NumDescriptors = 3;
        desc.Type  = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ThrowIfFailed(m_device->CreateDescriptorHeap(
            &desc, IID_PPV_ARGS(&m_timeEvoHeap)));

        auto cpu = m_timeEvoHeap->GetCPUDescriptorHandleForHeapStart();
        m_device->CreateShaderResourceView(m_h0Map.Get(), &srvDesc, cpu);
        cpu.ptr += descSize;
        m_device->CreateUnorderedAccessView(m_hktMap.Get(), nullptr, &uavDesc, cpu);
        cpu.ptr += descSize;
        m_device->CreateUnorderedAccessView(m_dztMap.Get(), nullptr, &uavDesc, cpu);
    }

    // IFFT h+Dx heap: slot0=hkt UAV, slot1=tempMap UAV
    {
        D3D12_DESCRIPTOR_HEAP_DESC desc = {};
        desc.NumDescriptors = 2;
        desc.Type  = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ThrowIfFailed(m_device->CreateDescriptorHeap(
            &desc, IID_PPV_ARGS(&m_ifftHeap)));

        auto cpu = m_ifftHeap->GetCPUDescriptorHandleForHeapStart();
        m_device->CreateUnorderedAccessView(m_hktMap.Get(), nullptr, &uavDesc, cpu);
        cpu.ptr += descSize;
        m_device->CreateUnorderedAccessView(m_tempMap.Get(), nullptr, &uavDesc, cpu);
    }

    // IFFT Dz heap: slot0=dztMap UAV, slot1=dztTempMap UAV
    {
        D3D12_DESCRIPTOR_HEAP_DESC desc = {};
        desc.NumDescriptors = 2;
        desc.Type  = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ThrowIfFailed(m_device->CreateDescriptorHeap(
            &desc, IID_PPV_ARGS(&m_ifftDzHeap)));

        auto cpu = m_ifftDzHeap->GetCPUDescriptorHandleForHeapStart();
        m_device->CreateUnorderedAccessView(m_dztMap.Get(), nullptr, &uavDesc, cpu);
        cpu.ptr += descSize;
        m_device->CreateUnorderedAccessView(m_dztTempMap.Get(), nullptr, &uavDesc, cpu);
    }

    // srvHeap: slot0=heightMap SRV, slot1=dztMap SRV
    {
        D3D12_DESCRIPTOR_HEAP_DESC desc = {};
        desc.NumDescriptors = 2;
        desc.Type  = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ThrowIfFailed(m_device->CreateDescriptorHeap(
            &desc, IID_PPV_ARGS(&m_srvHeap)));

        auto cpu = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
        m_device->CreateShaderResourceView(m_heightMap.Get(), &srvDesc, cpu);
        cpu.ptr += descSize;
        m_device->CreateShaderResourceView(m_dztMap.Get(), &srvDesc, cpu);
    }
}

// -----------------------------------------------
void OceanFFT::CreateRootSignatures()
{
    // Phillips Root Sig: UAV(u0) + CBV(b0)
    {
        CD3DX12_DESCRIPTOR_RANGE1 uavRange;
        uavRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);

        CD3DX12_ROOT_PARAMETER1 params[2];
        params[0].InitAsDescriptorTable(1, &uavRange);
        params[1].InitAsConstantBufferView(0);

        CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC desc;
        desc.Init_1_1(2, params);

        ComPtr<ID3DBlob> sig, err;
        ThrowIfFailed(D3DX12SerializeVersionedRootSignature(
            &desc, D3D_ROOT_SIGNATURE_VERSION_1_1, &sig, &err));
        ThrowIfFailed(m_device->CreateRootSignature(
            0, sig->GetBufferPointer(), sig->GetBufferSize(),
            IID_PPV_ARGS(&m_phillipsRootSig)));
    }

    // TimeEvo Root Sig: SRV(t0) + UAV(u0) + CBV(b0)
    {
        CD3DX12_DESCRIPTOR_RANGE1 srvRange, uavRange;
        srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
        uavRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0);

        CD3DX12_ROOT_PARAMETER1 params[3];
        params[0].InitAsDescriptorTable(1, &srvRange);  // h0
        params[1].InitAsDescriptorTable(1, &uavRange);  // hkt + dzt
        params[2].InitAsConstantBufferView(0);

        CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC desc;
        desc.Init_1_1(3, params);

        ComPtr<ID3DBlob> sig, err;
        ThrowIfFailed(D3DX12SerializeVersionedRootSignature(
            &desc, D3D_ROOT_SIGNATURE_VERSION_1_1, &sig, &err));
        ThrowIfFailed(m_device->CreateRootSignature(
            0, sig->GetBufferPointer(), sig->GetBufferSize(),
            IID_PPV_ARGS(&m_timeEvoRootSig)));
    }

    // IFFT Root Sig: UAV(u0) + CBV(b0)
    {
        CD3DX12_DESCRIPTOR_RANGE1 uavRange;
        uavRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0);

        CD3DX12_ROOT_PARAMETER1 params[2];
        params[0].InitAsDescriptorTable(1, &uavRange);
        params[1].InitAsConstantBufferView(0);

        CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC desc;
        desc.Init_1_1(2, params);

        ComPtr<ID3DBlob> sig, err;
        ThrowIfFailed(D3DX12SerializeVersionedRootSignature(
            &desc, D3D_ROOT_SIGNATURE_VERSION_1_1, &sig, &err));
        ThrowIfFailed(m_device->CreateRootSignature(
            0, sig->GetBufferPointer(), sig->GetBufferSize(),
            IID_PPV_ARGS(&m_ifftRootSig)));
    }
}

// -----------------------------------------------
void OceanFFT::CreatePSOs(
    const UINT8* phillipsCSData, UINT phillipsCSSize,
    const UINT8* timeEvoCSData, UINT timeEvoCSSize,
    const UINT8* ifftCSData, UINT ifftCSSize)
{
    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};

    psoDesc.pRootSignature = m_phillipsRootSig.Get();
    psoDesc.CS = CD3DX12_SHADER_BYTECODE(phillipsCSData, phillipsCSSize);
    ThrowIfFailed(m_device->CreateComputePipelineState(
        &psoDesc, IID_PPV_ARGS(&m_phillipsPSO)));

    psoDesc.pRootSignature = m_timeEvoRootSig.Get();
    psoDesc.CS = CD3DX12_SHADER_BYTECODE(timeEvoCSData, timeEvoCSSize);
    ThrowIfFailed(m_device->CreateComputePipelineState(
        &psoDesc, IID_PPV_ARGS(&m_timeEvoPSO)));

    psoDesc.pRootSignature = m_ifftRootSig.Get();
    psoDesc.CS = CD3DX12_SHADER_BYTECODE(ifftCSData, ifftCSSize);
    ThrowIfFailed(m_device->CreateComputePipelineState(
        &psoDesc, IID_PPV_ARGS(&m_ifftPSO)));
}

// -----------------------------------------------
void OceanFFT::CreateConstantBuffers()
{
    auto heapProp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    CD3DX12_RANGE readRange(0, 0);

    // Phillips CB
    auto desc = CD3DX12_RESOURCE_DESC::Buffer(256 *32);
    ThrowIfFailed(m_device->CreateCommittedResource(
        &heapProp, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&m_phillipsCB)));
    ThrowIfFailed(m_phillipsCB->Map(
        0, &readRange, reinterpret_cast<void**>(&m_phillipsCBMapped)));

    // TimeCB
    ThrowIfFailed(m_device->CreateCommittedResource(
        &heapProp, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&m_timeCB)));
    ThrowIfFailed(m_timeCB->Map(
        0, &readRange, reinterpret_cast<void**>(&m_timeCBMapped)));

    // IFFT CB
    ThrowIfFailed(m_device->CreateCommittedResource(
        &heapProp, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&m_ifftCB)));
    ThrowIfFailed(m_ifftCB->Map(
        0, &readRange, reinterpret_cast<void**>(&m_ifftCBMapped)));
}

// -----------------------------------------------
void OceanFFT::RunPhillipsInit(ComPtr<ID3D12CommandQueue> cmdQueue)
{
    ThrowIfFailed(m_device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(&m_initAllocator)));
    ThrowIfFailed(m_device->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        m_initAllocator.Get(), nullptr,
        IID_PPV_ARGS(&m_initCmdList)));

    // Phillips CB数据
    PhillipsCB cb;
    cb.N = m_textureSize;
    cb.A =0.3f;
    cb.windSpeed = 20.0f;
    cb.windDirX = windDirX;
    cb.windDirY = windDirY;
    cb.pad0 = cb.pad1 = cb.pad2 = 0.0f;
    memcpy(m_phillipsCBMapped, &cb, sizeof(cb));

    // 录制Phillips CS
    // 注意：Phillips需要h0的UAV，而h0在csHeap的slot0是SRV
    // 所以Phillips用独立的临时UAV Heap
    D3D12_DESCRIPTOR_HEAP_DESC tmpHeapDesc = {};
    tmpHeapDesc.NumDescriptors = 1;
    tmpHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    tmpHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> tmpUavHeap;
    ThrowIfFailed(m_device->CreateDescriptorHeap(
        &tmpHeapDesc, IID_PPV_ARGS(&tmpUavHeap)));

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    m_device->CreateUnorderedAccessView(
        m_h0Map.Get(), nullptr, &uavDesc,
        tmpUavHeap->GetCPUDescriptorHandleForHeapStart());

    m_initCmdList->SetComputeRootSignature(m_phillipsRootSig.Get());
    m_initCmdList->SetPipelineState(m_phillipsPSO.Get());

    ID3D12DescriptorHeap* heaps[] = { tmpUavHeap.Get() };
    m_initCmdList->SetDescriptorHeaps(1, heaps);
    m_initCmdList->SetComputeRootDescriptorTable(
        0, tmpUavHeap->GetGPUDescriptorHandleForHeapStart());
    m_initCmdList->SetComputeRootConstantBufferView(
        1, m_phillipsCB->GetGPUVirtualAddress());

    m_initCmdList->Dispatch(
        m_textureSize / 8, m_textureSize / 8, 1);

    // h0写完转为SRV，供TimeEvo读取
    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        m_h0Map.Get(),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    m_initCmdList->ResourceBarrier(1, &barrier);

    ThrowIfFailed(m_initCmdList->Close());
    ID3D12CommandList* cmds[] = { m_initCmdList.Get() };
    cmdQueue->ExecuteCommandLists(1, cmds);

    // 等待GPU完成，用局部Fence
    ComPtr<ID3D12Fence> fence;
    ThrowIfFailed(m_device->CreateFence(
        0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    HANDLE event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    cmdQueue->Signal(fence.Get(), 1);
    if (fence->GetCompletedValue() < 1)
    {
        fence->SetEventOnCompletion(1, event);
        WaitForSingleObject(event, INFINITE);
    }
    CloseHandle(event);
}

void OceanFFT::Dispatch(
    ComPtr<ID3D12GraphicsCommandList> cmdList, float time)
{
    UINT descSize = m_device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // Pass 0: Phillips 重新生成 h0
    {
        // h0 先从 SRV 转回 UAV
        auto toUAV = CD3DX12_RESOURCE_BARRIER::Transition(
            m_h0Map.Get(),
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmdList->ResourceBarrier(1, &toUAV);

        PhillipsCB cb;
        cb.N = m_textureSize;
        cb.A = phillipsA;
        cb.windSpeed = windSpeed;
        cb.windDirX = windDirX;
        cb.windDirY = windDirY;
        cb.pad0 = cb.pad1 = cb.pad2 = 0.0f;
        memcpy(m_phillipsCBMapped, &cb, sizeof(cb));

        ID3D12DescriptorHeap* heaps[] = { m_phillipsHeap.Get() };
        cmdList->SetDescriptorHeaps(1, heaps);

        cmdList->SetComputeRootSignature(m_phillipsRootSig.Get());
        cmdList->SetPipelineState(m_phillipsPSO.Get());
        cmdList->SetComputeRootDescriptorTable(
            0, m_phillipsHeap->GetGPUDescriptorHandleForHeapStart());
        cmdList->SetComputeRootConstantBufferView(
            1, m_phillipsCB->GetGPUVirtualAddress());
        cmdList->Dispatch(m_textureSize / 8, m_textureSize / 8, 1);

        // h0 写完转回 SRV
        auto toSRV = CD3DX12_RESOURCE_BARRIER::Transition(
            m_h0Map.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        cmdList->ResourceBarrier(1, &toSRV);
    }

    // Pass 1: TimeEvo
    {
        ID3D12DescriptorHeap* heaps[] = { m_timeEvoHeap.Get() };
        cmdList->SetDescriptorHeaps(1, heaps);
        auto gpu = m_timeEvoHeap->GetGPUDescriptorHandleForHeapStart();
        D3D12_GPU_DESCRIPTOR_HANDLE h0SRV = gpu;
        D3D12_GPU_DESCRIPTOR_HANDLE hktUAV = gpu; hktUAV.ptr += descSize;

        TimeCB timeCB = { m_textureSize, time, 0.0f, 0.0f };
        memcpy(m_timeCBMapped, &timeCB, sizeof(timeCB));

        cmdList->SetComputeRootSignature(m_timeEvoRootSig.Get());
        cmdList->SetPipelineState(m_timeEvoPSO.Get());
        cmdList->SetComputeRootDescriptorTable(0, h0SRV);
        cmdList->SetComputeRootDescriptorTable(1, hktUAV); // u0=hkt, u1=dztMap 连续
        cmdList->SetComputeRootConstantBufferView(2, m_timeCB->GetGPUVirtualAddress());
        cmdList->Dispatch(m_textureSize / 8, m_textureSize / 8, 1);

        D3D12_RESOURCE_BARRIER bs[2] = {
            CD3DX12_RESOURCE_BARRIER::UAV(m_hktMap.Get()),
            CD3DX12_RESOURCE_BARRIER::UAV(m_dztMap.Get())
        };
        cmdList->ResourceBarrier(2, bs);
    }

    // Pass 2: h+Dx IFFT
    {
        ID3D12DescriptorHeap* heaps[] = { m_ifftHeap.Get() };
        cmdList->SetDescriptorHeaps(1, heaps);
        auto pp0UAV = m_ifftHeap->GetGPUDescriptorHandleForHeapStart();

        cmdList->SetComputeRootSignature(m_ifftRootSig.Get());
        cmdList->SetPipelineState(m_ifftPSO.Get());

        IFFTCB ifftCB; ifftCB.N = m_textureSize;
        UINT passCount = 0;

        auto dispatchAndBarrier = [&]()
            {
                UINT offset = passCount * 256;
                memcpy(m_ifftCBMapped + offset, &ifftCB, sizeof(ifftCB));
                cmdList->SetComputeRootDescriptorTable(0, pp0UAV);
                cmdList->SetComputeRootConstantBufferView(
                    1, m_ifftCB->GetGPUVirtualAddress() + offset);
                cmdList->Dispatch(m_textureSize / 8, m_textureSize / 8, 1);
                D3D12_RESOURCE_BARRIER bs[2] = {
                    CD3DX12_RESOURCE_BARRIER::UAV(m_hktMap.Get()),
                    CD3DX12_RESOURCE_BARRIER::UAV(m_tempMap.Get())
                };
                cmdList->ResourceBarrier(2, bs);
                passCount++;
            };

        ifftCB.passIdx = 0;
        for (UINT step = 2, pp = 0; step <= m_textureSize; step <<= 1, pp ^= 1)
        {
            ifftCB.stepSize = step; ifftCB.pingpong = pp; dispatchAndBarrier();
        }

        ifftCB.passIdx = 1;
        for (UINT step = 2, pp = 0; step <= m_textureSize; step <<= 1, pp ^= 1)
        {
            ifftCB.stepSize = step; ifftCB.pingpong = pp; dispatchAndBarrier();
        }
    }

    // Pass 3: Dz IFFT
    {
        ID3D12DescriptorHeap* heaps[] = { m_ifftDzHeap.Get() };
        cmdList->SetDescriptorHeaps(1, heaps);
        auto pp0UAV = m_ifftDzHeap->GetGPUDescriptorHandleForHeapStart();

        cmdList->SetComputeRootSignature(m_ifftRootSig.Get());
        cmdList->SetPipelineState(m_ifftPSO.Get());

        IFFTCB ifftCB; ifftCB.N = m_textureSize;
        UINT passCount = 16; // h+Dx已用0~15，从16开始

        auto dispatchAndBarrier = [&]()
            {
                UINT offset = passCount * 256;
                memcpy(m_ifftCBMapped + offset, &ifftCB, sizeof(ifftCB));
                cmdList->SetComputeRootDescriptorTable(0, pp0UAV);
                cmdList->SetComputeRootConstantBufferView(
                    1, m_ifftCB->GetGPUVirtualAddress() + offset);
                cmdList->Dispatch(m_textureSize / 8, m_textureSize / 8, 1);
                D3D12_RESOURCE_BARRIER bs[2] = {
                    CD3DX12_RESOURCE_BARRIER::UAV(m_dztMap.Get()),
                    CD3DX12_RESOURCE_BARRIER::UAV(m_dztTempMap.Get())
                };
                cmdList->ResourceBarrier(2, bs);
                passCount++;
            };

        ifftCB.passIdx = 0;
        for (UINT step = 2, pp = 0; step <= m_textureSize; step <<= 1, pp ^= 1)
        {
            ifftCB.stepSize = step; ifftCB.pingpong = pp; dispatchAndBarrier();
        }

        ifftCB.passIdx = 1;
        for (UINT step = 2, pp = 0; step <= m_textureSize; step <<= 1, pp ^= 1)
        {
            ifftCB.stepSize = step; ifftCB.pingpong = pp; dispatchAndBarrier();
        }
    }

    // Pass 4: CopyResource hkt → heightMap
    {
        D3D12_RESOURCE_BARRIER tocopy[2] = {
            CD3DX12_RESOURCE_BARRIER::Transition(
                m_hktMap.Get(),
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_COPY_SOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(
                m_heightMap.Get(),
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_COPY_DEST)
        };
        cmdList->ResourceBarrier(2, tocopy);
        cmdList->CopyResource(m_heightMap.Get(), m_hktMap.Get());

        D3D12_RESOURCE_BARRIER toUAV[2] = {
            CD3DX12_RESOURCE_BARRIER::Transition(
                m_hktMap.Get(),
                D3D12_RESOURCE_STATE_COPY_SOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            CD3DX12_RESOURCE_BARRIER::Transition(
                m_heightMap.Get(),
                D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
        };
        cmdList->ResourceBarrier(2, toUAV);
    }
}