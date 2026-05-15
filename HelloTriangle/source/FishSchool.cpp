#include "FishSchool.h"
#include <d3dx12_barriers.h>
#include <d3dx12_core.h>
#include <d3dx12_root_signature.h>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <algorithm>   // std::for_each
#include <execution>   // std::execution::par_unseq
#include <numeric>     // std::iota

// ---------------------------------------------------------------
void FishSchool::Init(
    ComPtr<ID3D12Device> device,
    const UINT8* vsData, UINT vsLen,
    const UINT8* psData, UINT psLen)
{
    m_device = device;

    // Root signature: [0] inline CBV b0, [1] descriptor table SRV t0
    {
        CD3DX12_ROOT_PARAMETER params[2];
        params[0].InitAsConstantBufferView(0, 0, D3D12_SHADER_VISIBILITY_ALL);

        CD3DX12_DESCRIPTOR_RANGE srvRange;
        srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
        params[1].InitAsDescriptorTable(1, &srvRange, D3D12_SHADER_VISIBILITY_VERTEX);

        CD3DX12_ROOT_SIGNATURE_DESC rsd;
        rsd.Init(2, params, 0, nullptr,
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

        ComPtr<ID3DBlob> sig, err;
        ThrowIfFailed(D3D12SerializeRootSignature(
            &rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err));
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
}

// ---------------------------------------------------------------
void FishSchool::InitBuffers(ComPtr<ID3D12GraphicsCommandList> cmdList)
{
    // Fish body: 6-vertex diamond (torpedo), +X = forward, scale ~3m body length
    const float S = 3.5f;
    XMFLOAT3 pos[6] = {
        { 0.50f*S,  0.00f*S,  0.00f*S},
        { 0.05f*S,  0.10f*S,  0.00f*S},
        { 0.05f*S, -0.07f*S,  0.00f*S},
        { 0.05f*S,  0.00f*S,  0.14f*S},
        { 0.05f*S,  0.00f*S, -0.14f*S},
        {-0.50f*S,  0.00f*S,  0.00f*S},
    };
    int tris[8][3] = {
        {0,1,3}, {0,3,2}, {0,2,4}, {0,4,1},
        {5,3,1}, {5,2,3}, {5,4,2}, {5,1,4},
    };

    // Accumulate face normals per vertex for smooth shading
    XMVECTOR acc[6];
    for (int i = 0; i < 6; ++i) acc[i] = XMVectorZero();
    for (int f = 0; f < 8; ++f)
    {
        XMVECTOR a  = XMLoadFloat3(&pos[tris[f][0]]);
        XMVECTOR b  = XMLoadFloat3(&pos[tris[f][1]]);
        XMVECTOR c  = XMLoadFloat3(&pos[tris[f][2]]);
        XMVECTOR fn = XMVector3Cross(b - a, c - a);
        acc[tris[f][0]] = acc[tris[f][0]] + fn;
        acc[tris[f][1]] = acc[tris[f][1]] + fn;
        acc[tris[f][2]] = acc[tris[f][2]] + fn;
    }
    Vertex verts[6];
    for (int i = 0; i < 6; ++i)
    {
        verts[i].pos = pos[i];
        XMStoreFloat3(&verts[i].normal, XMVector3Normalize(acc[i]));
    }

    UINT16 idxs[24];
    for (int f = 0; f < 8; ++f)
        for (int k = 0; k < 3; ++k)
            idxs[f * 3 + k] = (UINT16)tris[f][k];
    m_indexCount = 24;

    // Vertex buffer (DEFAULT heap + UPLOAD staging)
    {
        UINT sz  = sizeof(verts);
        auto hp  = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        auto hp2 = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        auto bd  = CD3DX12_RESOURCE_DESC::Buffer(sz);
        ThrowIfFailed(m_device->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_vb)));
        ThrowIfFailed(m_device->CreateCommittedResource(
            &hp2, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_vbUpload)));
        void* p = nullptr;
        CD3DX12_RANGE r(0, 0);
        m_vbUpload->Map(0, &r, &p);
        memcpy(p, verts, sz);
        m_vbUpload->Unmap(0, nullptr);
        cmdList->CopyBufferRegion(m_vb.Get(), 0, m_vbUpload.Get(), 0, sz);
        auto bar = CD3DX12_RESOURCE_BARRIER::Transition(m_vb.Get(),
            D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
        cmdList->ResourceBarrier(1, &bar);
        m_vbView = { m_vb->GetGPUVirtualAddress(), sz, sizeof(Vertex) };
    }

    // Index buffer
    {
        UINT sz  = sizeof(idxs);
        auto hp  = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        auto hp2 = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        auto bd  = CD3DX12_RESOURCE_DESC::Buffer(sz);
        ThrowIfFailed(m_device->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_ib)));
        ThrowIfFailed(m_device->CreateCommittedResource(
            &hp2, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_ibUpload)));
        void* p = nullptr;
        CD3DX12_RANGE r(0, 0);
        m_ibUpload->Map(0, &r, &p);
        memcpy(p, idxs, sz);
        m_ibUpload->Unmap(0, nullptr);
        cmdList->CopyBufferRegion(m_ib.Get(), 0, m_ibUpload.Get(), 0, sz);
        auto bar = CD3DX12_RESOURCE_BARRIER::Transition(m_ib.Get(),
            D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_INDEX_BUFFER);
        cmdList->ResourceBarrier(1, &bar);
        m_ibView = { m_ib->GetGPUVirtualAddress(), sz, DXGI_FORMAT_R16_UINT };
    }

    // Instance StructuredBuffer (UPLOAD heap, persistent map)
    {
        UINT64 sz = sizeof(FishInstance) * MAX_FISH;
        auto hp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        auto bd = CD3DX12_RESOURCE_DESC::Buffer(sz);
        ThrowIfFailed(m_device->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&m_instanceBuf)));
        m_instanceBuf->Map(0, nullptr,
            reinterpret_cast<void**>(&m_mappedInstances));
    }

    // Scene CB (UPLOAD heap, persistent map)
    {
        auto hp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        auto bd = CD3DX12_RESOURCE_DESC::Buffer(sizeof(SceneCB));
        ThrowIfFailed(m_device->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&m_sceneCB)));
        m_sceneCB->Map(0, nullptr,
            reinterpret_cast<void**>(&m_mappedSceneCB));
    }

    // Descriptor heap + StructuredBuffer SRV
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 1;
        hd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ThrowIfFailed(m_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_srvHeap)));

        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.ViewDimension              = D3D12_SRV_DIMENSION_BUFFER;
        sd.Format                     = DXGI_FORMAT_UNKNOWN;
        sd.Shader4ComponentMapping    = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Buffer.NumElements         = MAX_FISH;
        sd.Buffer.StructureByteStride = sizeof(FishInstance);
        m_device->CreateShaderResourceView(m_instanceBuf.Get(), &sd,
            m_srvHeap->GetCPUDescriptorHandleForHeapStart());
    }
}

// ---------------------------------------------------------------
void FishSchool::SpawnSchool(int n)
{
    m_fish.clear();
    m_fish.reserve(n);
    for (int i = 0; i < n; ++i)
    {
        Fish f;
        float angle = (float)rand() / RAND_MAX * 6.2832f;
        float r     = 15.0f + (float)rand() / RAND_MAX * 35.0f;
        f.pos.x = cosf(angle) * r;
        f.pos.y = -3.0f - (float)rand() / RAND_MAX * 5.0f;
        f.pos.z = sinf(angle) * r;

        float va = (float)rand() / RAND_MAX * 6.2832f;
        float spd = 3.0f + (float)rand() / RAND_MAX * 2.5f;
        f.vel.x = cosf(va) * spd;
        f.vel.y = ((float)rand() / RAND_MAX - 0.5f) * 0.4f;
        f.vel.z = sinf(va) * spd;

        f.phase = (float)rand() / RAND_MAX * 6.2832f;

        // Daylight: silver-blue body (rgb). Alpha stores a unique hue (0-1)
        // for bioluminescence — reuses the random phase so each fish glows
        // a different color without adding extra data.
        float v   = 0.60f + (float)rand() / RAND_MAX * 0.25f;
        float hue = f.phase / (2.0f * 3.14159265f);   // phase is already random [0, 2π]
        f.color = XMFLOAT4(v * 0.80f, v * 0.88f, v * 1.05f, hue);

        m_fish.push_back(f);
    }
}

// ---------------------------------------------------------------
void FishSchool::Boids(float dt)
{
    constexpr float SEP_R   = 2.5f,  ALN_R  = 8.0f,  COH_R  = 15.0f;
    constexpr float SEP_W   = 2.0f,  ALN_W  = 1.0f,  COH_W  = 0.7f;
    constexpr float BOUND_W = 2.5f;
    constexpr float ZONE_R  = 70.0f, DMIN   = -18.0f, DMAX  = -5.0f;
    constexpr float MINSPD  = 2.5f,  MAXSPD = 7.0f;

    const int N = (int)m_fish.size();
    if (!N) return;

    // Build index array for use with parallel algorithms.
    // We cannot use a raw pointer range here because par_unseq requires
    // the iterator to be random-access, and int* qualifies.
    std::vector<int>     indices(N);
    std::iota(indices.begin(), indices.end(), 0);

    std::vector<XMFLOAT3> newVel(N);

    // --- Phase 1: compute new velocity for every fish ---
    // Safe to parallelize: each i reads m_fish[*] (read-only) and
    // writes newVel[i] exclusively. No two threads share a write target.
    std::for_each(std::execution::par_unseq,
        indices.begin(), indices.end(),
        [&](int i)
    {
        XMVECTOR pi = XMLoadFloat3(&m_fish[i].pos);
        XMVECTOR vi = XMLoadFloat3(&m_fish[i].vel);

        XMVECTOR sep = XMVectorZero();
        XMVECTOR aln = XMVectorZero();
        XMVECTOR coh = XMVectorZero();
        int alnN = 0, cohN = 0;

        for (int j = 0; j < N; ++j)
        {
            if (i == j) continue;
            XMVECTOR pj   = XMLoadFloat3(&m_fish[j].pos);
            XMVECTOR diff = pi - pj;
            float    d2   = XMVectorGetX(XMVector3Dot(diff, diff));
            float    d    = sqrtf(d2);

            if (d < SEP_R && d > 0.001f)
                sep = sep + XMVector3Normalize(diff) * (1.0f / (d + 0.01f));
            if (d < ALN_R)
            {
                aln = aln + XMLoadFloat3(&m_fish[j].vel);
                alnN++;
            }
            if (d < COH_R)
            {
                coh = coh + pj;
                cohN++;
            }
        }

        XMVECTOR steer = sep * SEP_W;
        if (alnN > 0)
            steer = steer + XMVector3Normalize(aln * (1.0f / alnN)) * ALN_W;
        if (cohN > 0)
            steer = steer + XMVector3Normalize(coh * (1.0f / cohN) - pi) * COH_W;

        // Boundary avoidance
        XMFLOAT3 p3;
        XMStoreFloat3(&p3, pi);
        float xzr = sqrtf(p3.x * p3.x + p3.z * p3.z);
        if (xzr > ZONE_R)
            steer = steer +
                XMVector3Normalize(XMVectorSet(-p3.x, 0, -p3.z, 0)) * BOUND_W;
        if (p3.y > DMAX)
            steer = steer + XMVectorSet(0, -BOUND_W, 0, 0);
        else if (p3.y < DMIN)
            steer = steer + XMVectorSet(0,  BOUND_W, 0, 0);

        XMVECTOR nv  = vi + steer * (dt * 3.0f);
        float    spd = XMVectorGetX(XMVector3Length(nv));
        if      (spd > MAXSPD)                 nv = XMVector3Normalize(nv) * MAXSPD;
        else if (spd < MINSPD && spd > 1e-4f)  nv = XMVector3Normalize(nv) * MINSPD;
        else if (spd <= 1e-4f)                 nv = vi;

        XMStoreFloat3(&newVel[i], nv);
    });

    // --- Phase 2: integrate positions ---
    // Safe to parallelize: each i reads/writes only m_fish[i].
    std::for_each(std::execution::par_unseq,
        indices.begin(), indices.end(),
        [&](int i)
    {
        m_fish[i].vel = newVel[i];
        XMVECTOR p = XMLoadFloat3(&m_fish[i].pos);
        XMVECTOR v = XMLoadFloat3(&m_fish[i].vel);
        XMFLOAT3 np;
        XMStoreFloat3(&np, p + v * dt);

        if (np.y > DMAX)
        {
            np.y = DMAX;
            if (m_fish[i].vel.y > 0.0f) m_fish[i].vel.y = 0.0f;
        }
        m_fish[i].pos = np;
    });
}

// ---------------------------------------------------------------
void FishSchool::Update(float dt, float /*time*/)
{
    Boids(dt);
}

// ---------------------------------------------------------------
void FishSchool::Render(
    RenderContext& ctx,
    XMFLOAT3 sunDir, float sunIntensity, XMFLOAT3 sunColor,
    XMFLOAT3 cameraPos, float time)
{
    const int N = (int)m_fish.size();
    if (!N) return;

    // Update SceneCB
    {
        XMMATRIX vp = XMMatrixTranspose(ctx.view * ctx.proj);
        m_mappedSceneCB->viewProj     = vp;
        m_mappedSceneCB->sunDir       = sunDir;
        m_mappedSceneCB->sunIntensity = sunIntensity;
        m_mappedSceneCB->sunColor     = sunColor;
        m_mappedSceneCB->time         = time;
        m_mappedSceneCB->cameraPos    = cameraPos;
    }

    // Update instance data
    for (int i = 0; i < N; ++i)
    {
        const Fish& f    = m_fish[i];
        FishInstance& inst = m_mappedInstances[i];

        float spd = sqrtf(
            f.vel.x*f.vel.x + f.vel.y*f.vel.y + f.vel.z*f.vel.z);
        XMVECTOR fwdV = (spd > 0.001f)
            ? XMVector3Normalize(XMLoadFloat3(&f.vel))
            : XMVectorSet(1, 0, 0, 0);

        // Build up vector orthogonal to forward
        XMVECTOR worldUp = XMVectorSet(0, 1, 0, 0);
        XMVECTOR right   = XMVector3Cross(worldUp, fwdV);
        if (XMVectorGetX(XMVector3LengthSq(right)) < 0.01f)
            right = XMVectorSet(1, 0, 0, 0);
        right = XMVector3Normalize(right);
        XMVECTOR upV = XMVector3Normalize(XMVector3Cross(fwdV, right));

        inst.pos   = f.pos;
        inst.phase = f.phase;
        XMStoreFloat3(&inst.forward, fwdV);
        inst.speed = spd;
        XMStoreFloat3(&inst.up, upV);
        inst.pad   = 0.0f;
        inst.color = f.color;
    }

    auto* cmd = ctx.cmd;
    cmd->SetGraphicsRootSignature(m_rootSig.Get());
    cmd->SetPipelineState(m_pso.Get());

    ID3D12DescriptorHeap* heaps[] = { m_srvHeap.Get() };
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetGraphicsRootConstantBufferView(0, m_sceneCB->GetGPUVirtualAddress());
    cmd->SetGraphicsRootDescriptorTable(1,
        m_srvHeap->GetGPUDescriptorHandleForHeapStart());

    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->IASetVertexBuffers(0, 1, &m_vbView);
    cmd->IASetIndexBuffer(&m_ibView);
    cmd->OMSetRenderTargets(1, &ctx.rtv, FALSE, &ctx.dsv);
    cmd->RSSetViewports(1, &ctx.viewport);
    cmd->RSSetScissorRects(1, &ctx.scissor);

    cmd->DrawIndexedInstanced(m_indexCount, (UINT)N, 0, 0, 0);
}
