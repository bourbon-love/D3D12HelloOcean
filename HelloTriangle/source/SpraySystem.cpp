#include "SpraySystem.h"
#include <d3dx12_root_signature.h>
#include <algorithm>

static constexpr float kGravity = -9.8f;

// quad corner table: 2 triangles (CCW)
static const float kCorners[6][2] = {
    {-1,-1}, {+1,-1}, {-1,+1},
    {+1,-1}, {+1,+1}, {-1,+1}
};

void SpraySystem::Init(
    ComPtr<ID3D12Device> device,
    const UINT8* vsData, UINT vsSize,
    const UINT8* psData, UINT psSize)
{
    m_device = device;
    m_particles.reserve(MAX_PARTICLES);

    // root signature: one CBV at b0
    {
        CD3DX12_ROOT_PARAMETER1 param;
        param.InitAsConstantBufferView(0);

        CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC desc;
        desc.Init_1_1(1, &param, 0, nullptr,
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

        ComPtr<ID3DBlob> sig, err;
        ThrowIfFailed(D3DX12SerializeVersionedRootSignature(
            &desc, D3D_ROOT_SIGNATURE_VERSION_1_1, &sig, &err));
        ThrowIfFailed(m_device->CreateRootSignature(
            0, sig->GetBufferPointer(), sig->GetBufferSize(),
            IID_PPV_ARGS(&m_rootSig)));
    }

    // dynamic VB (upload heap, CPU-written every frame)
    {
        auto hp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        auto bd = CD3DX12_RESOURCE_DESC::Buffer(
            MAX_PARTICLES * VERTS_PER_PART * sizeof(SprayVertex));
        ThrowIfFailed(m_device->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&m_vb)));
        CD3DX12_RANGE r(0, 0);
        ThrowIfFailed(m_vb->Map(0, &r,
            reinterpret_cast<void**>(&m_vbMapped)));

        m_vbView.BufferLocation = m_vb->GetGPUVirtualAddress();
        m_vbView.StrideInBytes  = sizeof(SprayVertex);
        m_vbView.SizeInBytes    =
            MAX_PARTICLES * VERTS_PER_PART * sizeof(SprayVertex);
    }

    // constant buffer
    {
        auto hp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        auto bd = CD3DX12_RESOURCE_DESC::Buffer(sizeof(SprayCB));
        ThrowIfFailed(m_device->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&m_cb)));
        CD3DX12_RANGE r(0, 0);
        ThrowIfFailed(m_cb->Map(0, &r,
            reinterpret_cast<void**>(&m_cbMapped)));
        memset(m_cbMapped, 0, sizeof(SprayCB));
    }

    // PSO
    {
        D3D12_INPUT_ELEMENT_DESC layout[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0,
              D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32_FLOAT,       0, 12,
              D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 1, DXGI_FORMAT_R32_FLOAT,       0, 16,
              D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 2, DXGI_FORMAT_R32_FLOAT,       0, 20,
              D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 3, DXGI_FORMAT_R32_FLOAT,       0, 24,
              D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        };

        D3D12_BLEND_DESC blend = {};
        blend.RenderTarget[0].BlendEnable    = TRUE;
        blend.RenderTarget[0].SrcBlend       = D3D12_BLEND_SRC_ALPHA;
        blend.RenderTarget[0].DestBlend      = D3D12_BLEND_INV_SRC_ALPHA; // normal alpha blend
        blend.RenderTarget[0].BlendOp        = D3D12_BLEND_OP_ADD;
        blend.RenderTarget[0].SrcBlendAlpha  = D3D12_BLEND_ONE;
        blend.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
        blend.RenderTarget[0].BlendOpAlpha   = D3D12_BLEND_OP_ADD;
        blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        CD3DX12_DEPTH_STENCIL_DESC ds(D3D12_DEFAULT);
        ds.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;

        CD3DX12_RASTERIZER_DESC rs(D3D12_DEFAULT);
        rs.CullMode = D3D12_CULL_MODE_NONE;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
        pd.InputLayout           = { layout, _countof(layout) };
        pd.pRootSignature        = m_rootSig.Get();
        pd.VS                    = { vsData, vsSize };
        pd.PS                    = { psData, psSize };
        pd.RasterizerState       = rs;
        pd.BlendState            = blend;
        pd.DepthStencilState     = ds;
        pd.DSVFormat             = DXGI_FORMAT_D32_FLOAT;
        pd.SampleMask            = UINT_MAX;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets      = 1;
        pd.RTVFormats[0]         = DXGI_FORMAT_R16G16B16A16_FLOAT;
        pd.SampleDesc.Count      = 1;
        ThrowIfFailed(m_device->CreateGraphicsPipelineState(
            &pd, IID_PPV_ARGS(&m_pso)));
    }
}

void SpraySystem::Update(
    float dt, float weatherIntensity,
    const XMFLOAT3& cameraPos,
    float windDirX, float windDirY)
{
    static constexpr float kSpawnThreshold = 0.15f;
    float spawnRate = 0.0f;
    if (weatherIntensity > kSpawnThreshold)
        spawnRate = (weatherIntensity - kSpawnThreshold) / (1.0f - kSpawnThreshold);

    // update existing particles
    for (auto& p : m_particles)
    {
        p.vel.y  += kGravity * dt;
        p.pos.x  += p.vel.x * dt;
        p.pos.y  += p.vel.y * dt;
        p.pos.z  += p.vel.z * dt;
        p.life   -= dt;
    }
    m_particles.erase(
        std::remove_if(m_particles.begin(), m_particles.end(),
            [](const Particle& p){ return p.life <= 0.0f || p.pos.y < 0.1f; }),
        m_particles.end());

    // spawn new particles — many small ones, short lifetime, natural burst feel
    m_spawnAccum += spawnRate * 35.0f * dt;
    while (m_spawnAccum >= 1.0f && m_particles.size() < MAX_PARTICLES)
    {
        m_spawnAccum -= 1.0f;

        std::uniform_real_distribution<float> rOcean(-150.0f, 150.0f);
        std::uniform_real_distribution<float> rSide(-2.0f, 2.0f);
        std::uniform_real_distribution<float> rLife(0.4f, 1.2f);         // short lifetime
        std::uniform_real_distribution<float> rSize(0.10f, 0.35f);       // small fragments

        // Spawn y: slightly above wave crests (scales with weather)
        float peakY = 0.5f + spawnRate * 6.0f;
        std::uniform_real_distribution<float> rSpawnY(peakY * 0.6f, peakY * 1.1f);
        // Moderate upward velocity — not rocket-like
        std::uniform_real_distribution<float> rUp(2.0f + spawnRate * 5.0f,
                                                   4.0f + spawnRate * 8.0f);

        Particle p;
        p.pos = { rOcean(m_rng), rSpawnY(m_rng), rOcean(m_rng) };
        p.vel = {
            windDirX * spawnRate * 3.0f + rSide(m_rng),
            rUp(m_rng),
            windDirY * spawnRate * 3.0f + rSide(m_rng)
        };
        p.maxLife = rLife(m_rng);
        p.life    = p.maxLife;
        p.size    = rSize(m_rng);
        m_particles.push_back(p);
    }
    if (m_spawnAccum > 2.0f) m_spawnAccum = 0.0f;

    // write vertex buffer
    UINT count = static_cast<UINT>(
        (std::min)(m_particles.size(), (size_t)MAX_PARTICLES));
    m_activeVerts = count * VERTS_PER_PART;

    for (UINT i = 0; i < count; ++i)
    {
        const Particle& p  = m_particles[i];
        float           t  = p.life / p.maxLife; // 1=just spawned, 0=dead
        float           a  = t;                               // linear fade out

        for (UINT v = 0; v < VERTS_PER_PART; ++v)
        {
            SprayVertex& sv = m_vbMapped[i * VERTS_PER_PART + v];
            sv.center  = p.pos;
            sv.size    = p.size;
            sv.alpha   = a * 0.85f;
            sv.cornerU = kCorners[v][0];
            sv.cornerV = kCorners[v][1];
        }
    }
}

void SpraySystem::Render(
    RenderContext& ctx,
    const XMMATRIX& view,
    const XMMATRIX& proj)
{
    if (m_activeVerts == 0) return;

    // extract camera right/up from view matrix rows
    XMFLOAT4X4 v;
    XMStoreFloat4x4(&v, view);
    m_cbMapped->viewProj     = XMMatrixTranspose(view * proj);
    m_cbMapped->cameraRight  = { v._11, v._21, v._31 };
    m_cbMapped->cameraUp     = { v._12, v._22, v._32 };

    auto* cmd = ctx.cmd;
    cmd->SetPipelineState(m_pso.Get());
    cmd->SetGraphicsRootSignature(m_rootSig.Get());
    cmd->SetGraphicsRootConstantBufferView(0, m_cb->GetGPUVirtualAddress());
    cmd->OMSetRenderTargets(1, &ctx.rtv, FALSE, &ctx.dsv);
    cmd->RSSetViewports(1, &ctx.viewport);
    cmd->RSSetScissorRects(1, &ctx.scissor);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->IASetVertexBuffers(0, 1, &m_vbView);
    cmd->DrawInstanced(m_activeVerts, 1, 0, 0);
}
