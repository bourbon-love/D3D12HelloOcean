#include "FloatingObject.h"
#include <d3dx12_barriers.h>
#include <d3dx12_core.h>
#include <d3dx12_root_signature.h>
#include <cstdlib>
#include <cmath>

void FloatingObject::Init(
    ComPtr<ID3D12Device> device,
    ID3D12Resource*      heightMap,
    const UINT8* vsData, UINT vsLen,
    const UINT8* psData, UINT psLen)
{
    m_device = device;

    // Constant buffer array: MAX_BOXES slots, each 256-byte aligned
    {
        UINT64 cbTotal = sizeof(ObjectCB) * MAX_BOXES;
        auto hp  = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        auto buf = CD3DX12_RESOURCE_DESC::Buffer(cbTotal);
        ThrowIfFailed(m_device->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &buf,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&m_cb)));
        CD3DX12_RANGE r(0, 0);
        m_cb->Map(0, &r, reinterpret_cast<void**>(&m_mappedCBs));
    }

    // SRV heap: slot 0 = heightMap
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 1;
        hd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ThrowIfFailed(m_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_srvHeap)));

        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format                  = DXGI_FORMAT_R32G32B32A32_FLOAT;
        sd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels     = 1;
        m_device->CreateShaderResourceView(heightMap, &sd,
            m_srvHeap->GetCPUDescriptorHandleForHeapStart());
    }

    // Root signature: [0] CBV(b0)  [1] SRV table(t0)
    {
        CD3DX12_ROOT_PARAMETER params[2];
        params[0].InitAsConstantBufferView(0, 0, D3D12_SHADER_VISIBILITY_ALL);
        CD3DX12_DESCRIPTOR_RANGE srvRange;
        srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
        params[1].InitAsDescriptorTable(1, &srvRange, D3D12_SHADER_VISIBILITY_VERTEX);

        CD3DX12_STATIC_SAMPLER_DESC sampler(0,
            D3D12_FILTER_MIN_MAG_MIP_LINEAR,
            D3D12_TEXTURE_ADDRESS_MODE_WRAP,
            D3D12_TEXTURE_ADDRESS_MODE_WRAP,
            D3D12_TEXTURE_ADDRESS_MODE_WRAP);

        CD3DX12_ROOT_SIGNATURE_DESC rsd;
        rsd.Init(2, params, 1, &sampler,
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
        ComPtr<ID3DBlob> sig, err;
        ThrowIfFailed(D3D12SerializeRootSignature(&rsd,
            D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err));
        ThrowIfFailed(m_device->CreateRootSignature(0,
            sig->GetBufferPointer(), sig->GetBufferSize(),
            IID_PPV_ARGS(&m_rootSig)));
    }

    // PSO
    {
        D3D12_INPUT_ELEMENT_DESC layout[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0,
              D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12,
              D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        };
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature           = m_rootSig.Get();
        pd.VS                       = CD3DX12_SHADER_BYTECODE(vsData, vsLen);
        pd.PS                       = CD3DX12_SHADER_BYTECODE(psData, psLen);
        pd.InputLayout              = { layout, _countof(layout) };
        pd.RasterizerState          = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.BlendState               = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        pd.DepthStencilState        = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        pd.DSVFormat                = DXGI_FORMAT_D32_FLOAT;
        pd.SampleMask               = UINT_MAX;
        pd.PrimitiveTopologyType    = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets         = 1;
        pd.RTVFormats[0]            = DXGI_FORMAT_R16G16B16A16_FLOAT;
        pd.SampleDesc.Count         = 1;
        ThrowIfFailed(m_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_pso)));
    }

    // Start with one default box
    SpawnBox();
}

void FloatingObject::InitBuffers(ComPtr<ID3D12GraphicsCommandList> cmdList)
{
    Vertex verts[24] = {
        // +Y top
        {{ HW, HH, HD}, {0,1,0}}, {{ HW, HH,-HD}, {0,1,0}},
        {{-HW, HH,-HD}, {0,1,0}}, {{-HW, HH, HD}, {0,1,0}},
        // -Y bottom
        {{-HW,-HH, HD}, {0,-1,0}}, {{-HW,-HH,-HD}, {0,-1,0}},
        {{ HW,-HH,-HD}, {0,-1,0}}, {{ HW,-HH, HD}, {0,-1,0}},
        // +X right
        {{ HW, HH,-HD}, {1,0,0}}, {{ HW, HH, HD}, {1,0,0}},
        {{ HW,-HH, HD}, {1,0,0}}, {{ HW,-HH,-HD}, {1,0,0}},
        // -X left
        {{-HW, HH, HD}, {-1,0,0}}, {{-HW, HH,-HD}, {-1,0,0}},
        {{-HW,-HH,-HD}, {-1,0,0}}, {{-HW,-HH, HD}, {-1,0,0}},
        // +Z front
        {{ HW, HH, HD}, {0,0,1}}, {{-HW, HH, HD}, {0,0,1}},
        {{-HW,-HH, HD}, {0,0,1}}, {{ HW,-HH, HD}, {0,0,1}},
        // -Z back
        {{-HW, HH,-HD}, {0,0,-1}}, {{ HW, HH,-HD}, {0,0,-1}},
        {{ HW,-HH,-HD}, {0,0,-1}}, {{-HW,-HH,-HD}, {0,0,-1}},
    };

    UINT16 idxs[36] = {
         0, 1, 2,  0, 2, 3,
         4, 5, 6,  4, 6, 7,
         8, 9,10,  8,10,11,
        12,13,14, 12,14,15,
        16,17,18, 16,18,19,
        20,21,22, 20,22,23,
    };
    m_indexCount = 36;

    // vertex buffer
    {
        UINT sz   = sizeof(verts);
        auto hp   = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        auto hp2  = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        auto bd   = CD3DX12_RESOURCE_DESC::Buffer(sz);
        ThrowIfFailed(m_device->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_vb)));
        ThrowIfFailed(m_device->CreateCommittedResource(
            &hp2, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_vbUpload)));
        void* pData = nullptr;
        CD3DX12_RANGE readRange(0, 0);
        ThrowIfFailed(m_vbUpload->Map(0, &readRange, &pData));
        memcpy(pData, verts, sz);
        m_vbUpload->Unmap(0, nullptr);
        cmdList->CopyBufferRegion(m_vb.Get(), 0, m_vbUpload.Get(), 0, sz);
        auto bar = CD3DX12_RESOURCE_BARRIER::Transition(m_vb.Get(),
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
        cmdList->ResourceBarrier(1, &bar);
        m_vbView = { m_vb->GetGPUVirtualAddress(), sz, sizeof(Vertex) };
    }

    // index buffer
    {
        UINT sz   = sizeof(idxs);
        auto hp   = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        auto hp2  = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        auto bd   = CD3DX12_RESOURCE_DESC::Buffer(sz);
        ThrowIfFailed(m_device->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_ib)));
        ThrowIfFailed(m_device->CreateCommittedResource(
            &hp2, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_ibUpload)));
        void* pData = nullptr;
        CD3DX12_RANGE readRange(0, 0);
        ThrowIfFailed(m_ibUpload->Map(0, &readRange, &pData));
        memcpy(pData, idxs, sz);
        m_ibUpload->Unmap(0, nullptr);
        cmdList->CopyBufferRegion(m_ib.Get(), 0, m_ibUpload.Get(), 0, sz);
        auto bar = CD3DX12_RESOURCE_BARRIER::Transition(m_ib.Get(),
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDEX_BUFFER);
        cmdList->ResourceBarrier(1, &bar);
        m_ibView = { m_ib->GetGPUVirtualAddress(), sz, DXGI_FORMAT_R16_UINT };
    }
}

void FloatingObject::Update(float dt)
{
    for (auto& box : m_boxes)
    {
        if (box.dropOffset > 0.0f)
        {
            box.dropOffset -= FALL_SPEED * dt;
            if (box.dropOffset < 0.0f) box.dropOffset = 0.0f;
        }
    }
}

void FloatingObject::SpawnBox()
{
    if ((int)m_boxes.size() >= MAX_BOXES) return;

    float r = SPAWN_RADIUS;
    BoxInstance b;
    b.position.x  = ((float)rand() / RAND_MAX * 2.0f - 1.0f) * r;
    b.position.y  = ((float)rand() / RAND_MAX * 2.0f - 1.0f) * r;
    b.dropOffset  = SPAWN_HEIGHT;
    m_boxes.push_back(b);
}

void FloatingObject::Render(
    RenderContext& ctx,
    XMFLOAT3 sunDir, float sunIntensity, XMFLOAT3 sunColor,
    XMFLOAT3 cameraPos)
{
    if (m_boxes.empty()) return;

    // Write all per-box CBs upfront
    XMMATRIX vp = XMMatrixTranspose(ctx.view * ctx.proj);
    for (int i = 0; i < (int)m_boxes.size(); ++i)
    {
        ObjectCB& cb    = m_mappedCBs[i];
        const auto& box = m_boxes[i];
        cb.viewProj      = vp;
        cb.worldPos      = XMFLOAT3(box.position.x, 0.0f, box.position.y);
        cb.objectScale   = scale;
        cb.sunDir        = sunDir;
        cb.sunIntensity  = sunIntensity;
        cb.sunColor      = sunColor;
        cb.gridWorldSize = 400.0f;
        cb.cameraPos     = cameraPos;
        cb.dropOffset    = box.dropOffset;
    }

    auto* cmd = ctx.cmd;
    cmd->SetGraphicsRootSignature(m_rootSig.Get());
    ID3D12DescriptorHeap* heaps[] = { m_srvHeap.Get() };
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetGraphicsRootDescriptorTable(1, m_srvHeap->GetGPUDescriptorHandleForHeapStart());
    cmd->SetPipelineState(m_pso.Get());
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->IASetVertexBuffers(0, 1, &m_vbView);
    cmd->IASetIndexBuffer(&m_ibView);
    cmd->OMSetRenderTargets(1, &ctx.rtv, FALSE, &ctx.dsv);
    cmd->RSSetViewports(1, &ctx.viewport);
    cmd->RSSetScissorRects(1, &ctx.scissor);

    UINT64 cbStride = sizeof(ObjectCB);
    for (int i = 0; i < (int)m_boxes.size(); ++i)
    {
        UINT64 cbAddr = m_cb->GetGPUVirtualAddress() + i * cbStride;
        cmd->SetGraphicsRootConstantBufferView(0, cbAddr);
        cmd->DrawIndexedInstanced(m_indexCount, 1, 0, 0, 0);
    }
}

void FloatingObject::InitShadowResources(ID3D12Device* device)
{
    auto hp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    auto bd = CD3DX12_RESOURCE_DESC::Buffer(MAX_BOXES * sizeof(ShadowInstCB));
    device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&m_shadowInstCB));
    CD3DX12_RANGE r(0, 0);
    m_shadowInstCB->Map(0, &r, reinterpret_cast<void**>(&m_mappedShadowCB));
}

void FloatingObject::RenderDepth(
    ID3D12GraphicsCommandList* cmd,
    ID3D12RootSignature*       rootSig,
    ID3D12PipelineState*       pso,
    const XMMATRIX&            lightViewProj)
{
    if (m_boxes.empty()) return;

    XMMATRIX lvpT = XMMatrixTranspose(lightViewProj);
    for (int i = 0; i < (int)m_boxes.size(); ++i)
    {
        auto* cb = reinterpret_cast<ShadowInstCB*>(
            m_mappedShadowCB + i * sizeof(ShadowInstCB));
        cb->lightViewProj = lvpT;
        cb->worldPos      = XMFLOAT3(m_boxes[i].position.x, 0.0f, m_boxes[i].position.y);
        cb->objectScale   = scale;
        cb->dropOffset    = m_boxes[i].dropOffset;
    }

    cmd->SetPipelineState(pso);
    cmd->SetGraphicsRootSignature(rootSig);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->IASetVertexBuffers(0, 1, &m_vbView);
    cmd->IASetIndexBuffer(&m_ibView);

    for (int i = 0; i < (int)m_boxes.size(); ++i)
    {
        UINT64 addr = m_shadowInstCB->GetGPUVirtualAddress() + i * sizeof(ShadowInstCB);
        cmd->SetGraphicsRootConstantBufferView(0, addr);
        cmd->DrawIndexedInstanced(m_indexCount, 1, 0, 0, 0);
    }
}
