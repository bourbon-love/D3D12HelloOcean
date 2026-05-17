#include "RainSystem.h"
#include <d3dx12_barriers.h>
#include <algorithm>
#include <d3dx12_root_signature.h>

void RainSystem::Init(
    ComPtr<ID3D12Device> device,
    ComPtr<ID3D12RootSignature> rootSignature,
    const UINT8* vsData, UINT vsSize,
    const UINT8* psData, UINT psSize)
{
    m_device = device;
    m_rainRootSig = rootSignature;
    m_drops.reserve(MAX_RAINDROPS);

    // Create an independent root signature that only needs a CBV
    ComPtr<ID3DBlob> sig, err;
    CD3DX12_ROOT_PARAMETER1 params[1];
    params[0].InitAsConstantBufferView(0);

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC desc;
    desc.Init_1_1(1, params, 0, nullptr,
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

    ThrowIfFailed(D3DX12SerializeVersionedRootSignature(
        &desc, D3D_ROOT_SIGNATURE_VERSION_1_1, &sig, &err));
    ThrowIfFailed(m_device->CreateRootSignature(
        0, sig->GetBufferPointer(), sig->GetBufferSize(),
        IID_PPV_ARGS(&m_rainRootSig)));

    // Dynamic VB (Upload Heap, CPU-written every frame)
    auto heapProp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    auto bufDesc = CD3DX12_RESOURCE_DESC::Buffer(
        MAX_RAINDROPS * VERTS_PER_DROP * sizeof(RainVertex));
    ThrowIfFailed(m_device->CreateCommittedResource(
        &heapProp, D3D12_HEAP_FLAG_NONE, &bufDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&m_rainVB)));

    CD3DX12_RANGE readRange(0, 0);
    ThrowIfFailed(m_rainVB->Map(
        0, &readRange, reinterpret_cast<void**>(&m_rainVBMapped)));

    m_rainVBView.BufferLocation = m_rainVB->GetGPUVirtualAddress();
    m_rainVBView.StrideInBytes = sizeof(RainVertex);
    m_rainVBView.SizeInBytes = MAX_RAINDROPS * VERTS_PER_DROP * sizeof(RainVertex);

    // Rain CB
    auto cbDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(RainCB));
    ThrowIfFailed(m_device->CreateCommittedResource(
        &heapProp, D3D12_HEAP_FLAG_NONE, &cbDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&m_rainCB)));
    ThrowIfFailed(m_rainCB->Map(
        0, &readRange, reinterpret_cast<void**>(&m_rainCBMapped)));
	// Ripple CB
    auto rippleCBDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(RippleCB));
    ThrowIfFailed(m_device->CreateCommittedResource(
        &heapProp, D3D12_HEAP_FLAG_NONE, &rippleCBDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&m_rippleCB)));
    ThrowIfFailed(m_rippleCB->Map(
        0, &readRange, reinterpret_cast<void**>(&m_rippleCBMapped)));
    memset(m_rippleCBMapped, 0, sizeof(RippleCB));

    CreatePSO(vsData, vsSize, psData, psSize);
}

void RainSystem::CreatePSO(
    const UINT8* vsData, UINT vsSize,
    const UINT8* psData, UINT psSize)
{
    D3D12_INPUT_ELEMENT_DESC layout[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32_FLOAT, 0, 12,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_BLEND_DESC blendDesc = {};
    blendDesc.RenderTarget[0].BlendEnable = TRUE;
    blendDesc.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
    blendDesc.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
    blendDesc.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    CD3DX12_DEPTH_STENCIL_DESC depthDesc(D3D12_DEFAULT);
    depthDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;

    CD3DX12_RASTERIZER_DESC rasterDesc(D3D12_DEFAULT);
    rasterDesc.CullMode = D3D12_CULL_MODE_NONE;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = { layout, _countof(layout) };
    psoDesc.pRootSignature = m_rainRootSig.Get();
    psoDesc.VS = CD3DX12_SHADER_BYTECODE(vsData, vsSize);
    psoDesc.PS = CD3DX12_SHADER_BYTECODE(psData, psSize);
    psoDesc.RasterizerState = rasterDesc;
    psoDesc.BlendState = blendDesc;
    psoDesc.DepthStencilState = depthDesc;
    psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.SampleDesc.Count = 1;

    ThrowIfFailed(m_device->CreateGraphicsPipelineState(
        &psoDesc, IID_PPV_ARGS(&m_rainPSO)));
}

void RainSystem::SpawnRainDrop(const XMFLOAT3& cameraPos)
{
    std::uniform_real_distribution<float> distAngle(0.0f, 6.28318f);
    std::uniform_real_distribution<float> distR    (0.0f, 1.0f);
    std::uniform_real_distribution<float> distY    (25.0f, 65.0f);
    std::uniform_real_distribution<float> distSpeed(45.0f, 90.0f);
    std::uniform_real_distribution<float> distLen  (1.0f, 2.2f);

    // sqrt(r) gives uniform disk distribution; plain r clusters drops near the centre
    float angle = distAngle(m_rng);
    float r     = sqrtf(distR(m_rng)) * 110.0f;

    RainDrop drop;
    drop.position = XMFLOAT3(
        cameraPos.x + r * cosf(angle),
        cameraPos.y + distY(m_rng),
        cameraPos.z + r * sinf(angle));
    drop.speed  = distSpeed(m_rng);
    drop.length = distLen(m_rng);
    m_drops.push_back(drop);
}

void RainSystem::Update(float deltaTime, float intensity, float windDirX, float windDirY,
                        const XMFLOAT3& cameraPos)
{
    m_intensity = intensity;
    m_windDirX = windDirX;
    m_windDirY = windDirY;

    // Target raindrop count varies with intensity
    UINT targetDrops = static_cast<UINT>(intensity * MAX_RAINDROPS);

    // Update existing raindrop positions
    UINT activeCount = 0;
    // Spawn ripples when raindrops hit the surface
    for (auto& drop : m_drops)
    {
        // Update position
        drop.position.y -= drop.speed * deltaTime;
        drop.position.x += m_windDirX * m_intensity * 20.0f * deltaTime;
        drop.position.z += m_windDirY * m_intensity * 20.0f * deltaTime;

        // Detect surface contact and generate ripples
        if (drop.position.y <= 0.0f && drop.position.y > -drop.speed * deltaTime)
        {
            if (m_ripples.size() < MAX_RIPPLES && m_intensity > 0.01f)
            {
                Ripple r;
                r.position = XMFLOAT2(drop.position.x, drop.position.z);
                r.radius = 0.0f;
                r.maxRadius = 5.0f + m_intensity * 10.0f;
                r.age = 0.0f;
                r.lifetime = 1.5f;
                m_ripples.push_back(r);
            }
        }
    }

    // Update ripples
    for (auto& r : m_ripples)
    {
        r.age += deltaTime;
        r.radius = (r.age / r.lifetime) * r.maxRadius;
    }

    // Remove expired ripples
    m_ripples.erase(
        std::remove_if(m_ripples.begin(), m_ripples.end(),
            [](const Ripple& r) { return r.age >= r.lifetime; }),
        m_ripples.end());

    

    // Write RippleCB
    UINT count = static_cast<UINT>(
        min(m_ripples.size(), (size_t)MAX_RIPPLES));
    for (UINT i = 0; i < count; ++i)
    {
        auto& r = m_ripples[i];
        float t = r.age / r.lifetime;
        m_rippleCBMapped->ripples[i].positions = r.position;
        m_rippleCBMapped->ripples[i].radius = r.radius;
        m_rippleCBMapped->ripples[i].strength = (1.0f - t) * m_intensity; // fades over time
    }
    m_rippleCBMapped->rippleCount = count;
    // Remove drops that hit the surface or drifted out of the spawn radius
    const float kRecycleR2 = 115.0f * 115.0f;
    m_drops.erase(
        std::remove_if(m_drops.begin(), m_drops.end(),
            [&cameraPos, kRecycleR2](const RainDrop& d)
            {
                if (d.position.y <= -2.0f) return true;
                float dx = d.position.x - cameraPos.x;
                float dz = d.position.z - cameraPos.z;
                return (dx * dx + dz * dz) > kRecycleR2;
            }),
        m_drops.end());

    // Replenish using actual camera position so drops always surround the player
    while (m_drops.size() < targetDrops)
        SpawnRainDrop(cameraPos);

    // Write VB
    m_activeDrops = static_cast<UINT>(
        min(m_drops.size(), (size_t)MAX_RAINDROPS));

    for (UINT i = 0; i < m_activeDrops; ++i)
    {
        auto& drop = m_drops[i];
        UINT base = i * 2;

        // Top vertex
        m_rainVBMapped[base].position = drop.position;
        m_rainVBMapped[base].alpha = intensity * 0.35f;

        // Wind offset: greater tilt in storms, direction follows windDir
        float windOffsetX = drop.length * m_windDirX * m_intensity * 0.8f;
        float windOffsetZ = drop.length * m_windDirY * m_intensity * 0.8f;

        m_rainVBMapped[base + 1].position = XMFLOAT3(
            drop.position.x + windOffsetX,
            drop.position.y - drop.length,
            drop.position.z + windOffsetZ);
        m_rainVBMapped[base + 1].alpha = 0.0f; // bottom transparent for gradient effect
    }
}

void RainSystem::Render(
    RenderContext& ctx,
    const XMMATRIX& view, const XMMATRIX& proj,
    const XMFLOAT3& cameraPos)
{
    (void)cameraPos;
    if (m_activeDrops == 0 || m_intensity < 0.01f) return;

    // Update CB
    m_rainCBMapped->viewProj = XMMatrixTranspose(view * proj);
    m_rainCBMapped->alpha = m_intensity;

    ctx.cmd->SetPipelineState(m_rainPSO.Get());
    ctx.cmd->SetGraphicsRootSignature(m_rainRootSig.Get());
    ctx.cmd->SetGraphicsRootConstantBufferView(
        0, m_rainCB->GetGPUVirtualAddress());
    ctx.cmd->OMSetRenderTargets(1, &ctx.rtv, FALSE, &ctx.dsv);
    ctx.cmd->RSSetViewports(1, &ctx.viewport);
    ctx.cmd->RSSetScissorRects(1, &ctx.scissor);
    ctx.cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
    ctx.cmd->IASetVertexBuffers(0, 1, &m_rainVBView);
    ctx.cmd->DrawInstanced(m_activeDrops * 2, 1, 0, 0);
}

void RainSystem::InitResources(ComPtr<ID3D12GraphicsCommandList> cmdList)
{
    (void)cmdList; // Upload Heap does not need a copy
}