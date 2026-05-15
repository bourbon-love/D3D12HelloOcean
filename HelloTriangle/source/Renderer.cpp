
#include "Renderer.h"
#include <d3dx12_barriers.h>
#include <d3dx12_core.h>
#include "../DXSampleHelper.h"
#include "GridMesh.h"
#include "SkyDome.h"
#include <chrono>

void Renderer::InitPSO(
    ComPtr<ID3D12Device> device,
    ComPtr<ID3D12RootSignature> rootSignature,
    UINT width, UINT height,
    const UINT8* vsData, UINT vsSize,
    const UINT8* psData, UINT psSize,
    const UINT8* waterBoxVSData, UINT waterBoxVSSize,
    const UINT8* waterBoxPSData, UINT waterBoxPSSize)
{
    m_device = device;
    m_rootSignature = rootSignature;
    m_camera.aspect = static_cast<float>(width) / static_cast<float>(height);

    // Save shader bytecode
    m_vertexShaderData.assign(vsData, vsData + vsSize);
    m_pixelShaderData.assign(psData, psData + psSize);

    m_waterBoxVSData.assign(waterBoxVSData, waterBoxVSData + waterBoxVSSize);
    m_waterBoxPSData.assign(waterBoxPSData, waterBoxPSData + waterBoxPSSize);
    // CBV
    {
        auto heapProp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        auto bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(1024 * 64);
        ThrowIfFailed(m_device->CreateCommittedResource(
            &heapProp, D3D12_HEAP_FLAG_NONE, &bufferDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&m_constantBuffer)));
        CD3DX12_RANGE readRange(0, 0);
        ThrowIfFailed(m_constantBuffer->Map(
            0, &readRange, reinterpret_cast<void**>(&m_pCbvDataBegin)));
    }

    // Input layout
    D3D12_INPUT_ELEMENT_DESC inputElementDescs[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    // Solid PSO (double-sided: camera may view ocean surface from below when underwater)
    {

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.InputLayout = { inputElementDescs, _countof(inputElementDescs) };
        psoDesc.pRootSignature = m_rootSignature.Get();
        psoDesc.VS = CD3DX12_SHADER_BYTECODE(
            m_vertexShaderData.data(), m_vertexShaderData.size());
        psoDesc.PS = CD3DX12_SHADER_BYTECODE(
            m_pixelShaderData.data(), m_pixelShaderData.size());
        CD3DX12_RASTERIZER_DESC rasterDesc(D3D12_DEFAULT);
        rasterDesc.CullMode = D3D12_CULL_MODE_NONE;
        psoDesc.RasterizerState = rasterDesc;
        psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        psoDesc.SampleMask = UINT_MAX;
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets = 1;
        psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        psoDesc.SampleDesc.Count = 1;
        ThrowIfFailed(m_device->CreateGraphicsPipelineState(
            &psoDesc, IID_PPV_ARGS(&m_pipelineState)));
    }
    // Translucent PSO
    {


        // Enable alpha blending
        D3D12_BLEND_DESC blendDesc = {};
        blendDesc.RenderTarget[0].BlendEnable = TRUE;
        blendDesc.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
        blendDesc.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        blendDesc.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
        blendDesc.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
        blendDesc.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
        blendDesc.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        // Disable backface culling to make both sides visible
        CD3DX12_RASTERIZER_DESC rasterDesc(D3D12_DEFAULT);
        rasterDesc.CullMode = D3D12_CULL_MODE_NONE;

        // Enable depth test but disable depth writes (transparent objects do not write depth)
        CD3DX12_DEPTH_STENCIL_DESC depthDesc(D3D12_DEFAULT);
        depthDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.InputLayout = { inputElementDescs, _countof(inputElementDescs) };
        psoDesc.pRootSignature = m_rootSignature.Get();
        psoDesc.VS = CD3DX12_SHADER_BYTECODE(m_waterBoxVSData.data(), m_waterBoxVSData.size());
        psoDesc.PS = CD3DX12_SHADER_BYTECODE(m_waterBoxPSData.data(), m_waterBoxPSData.size());
        psoDesc.RasterizerState = rasterDesc;
        psoDesc.BlendState = blendDesc;
        psoDesc.DepthStencilState = depthDesc;
        psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        psoDesc.SampleMask = UINT_MAX;
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets = 1;
        psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        psoDesc.SampleDesc.Count = 1;

        ThrowIfFailed(m_device->CreateGraphicsPipelineState(
            &psoDesc, IID_PPV_ARGS(&m_waterBoxPSO)));
    }
    // Wireframe PSO
    CreateWireframePSO();

    // Depth Buffer
    CreateDepthBuffer(width, height);
}

void Renderer::InitResources(ComPtr<ID3D12GraphicsCommandList> commandList)
{
    m_commandList = commandList;
    CreateGridBuffers(commandList);
    CreateLodGridBuffers(commandList);
    CreateWaterBoxBuffers(commandList);
}

void Renderer::Render(RenderContext& ctx)
{
    ExtractFrustumPlanes();

    int camTX = (int)floorf(m_camera.position.x / TILE_SIZE);
    int camTZ = (int)floorf(m_camera.position.z / TILE_SIZE);

    auto* pso = m_wireframe ? m_wireframePSO.Get() : m_pipelineState.Get();
    ctx.cmd->SetPipelineState(pso);
    ctx.cmd->SetGraphicsRootSignature(m_rootSignature.Get());
    ctx.cmd->OMSetRenderTargets(1, &ctx.rtv, FALSE, &ctx.dsv);
    ctx.cmd->RSSetViewports(1, &ctx.viewport);
    ctx.cmd->RSSetScissorRects(1, &ctx.scissor);
    ctx.cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    UINT slot = 0;
    for (int tz = camTZ - TILE_RADIUS; tz <= camTZ + TILE_RADIUS && slot < CB_MAX_TILES; ++tz)
    {
        for (int tx = camTX - TILE_RADIUS; tx <= camTX + TILE_RADIUS && slot < CB_MAX_TILES; ++tx)
        {
            if (!IsTileVisible(tx, tz)) continue;

            // Write CB per tile (256-byte aligned slot)
            UINT cbOffset = slot * CB_SLOT_SIZE;
            auto* tileCB = reinterpret_cast<SceneCB*>(m_pCbvDataBegin + cbOffset);
            *tileCB = m_lastSceneCB;
            tileCB->tileOffset = { tx * TILE_SIZE, tz * TILE_SIZE };

            ctx.cmd->SetGraphicsRootConstantBufferView(
                0, m_constantBuffer->GetGPUVirtualAddress() + cbOffset);

            // LOD: tiles within 5x5 around camera use high-res, others use low-res
            bool hiRes = (abs(tx - camTX) <= 2 && abs(tz - camTZ) <= 2);
            if (hiRes)
            {
                ctx.cmd->IASetVertexBuffers(0, 1, &ctx.vb);
                ctx.cmd->IASetIndexBuffer(&ctx.ib);
                ctx.cmd->DrawIndexedInstanced(ctx.indexCount, 1, 0, 0, 0);
            }
            else
            {
                ctx.cmd->IASetVertexBuffers(0, 1, &m_lodVBView);
                ctx.cmd->IASetIndexBuffer(&m_lodIBView);
                ctx.cmd->DrawIndexedInstanced(m_lodIndexCount, 1, 0, 0, 0);
            }
            ++slot;
        }
    }
}



void Renderer::Update(float deltaTime)
{
    m_time += deltaTime;

    SceneCB cb;
    cb.view = XMMatrixTranspose(m_camera.GetViewMatrix());
    cb.proj = XMMatrixTranspose(m_camera.GetProjMatrix());
    cb.time = m_time;
    //cb.pad0 = cb.pad1 = cb.pad2 = 0.0f;
	cb.cameraPos = m_camera.position;

    // Read data from the sky system
    if (m_skyDome)
    {
        cb.sunDir = m_skyDome->GetSunDirection();
        cb.sunColor = m_skyDome->GetSunColor();
        cb.padSun = 0.0f;
        cb.skyColor = m_skyDome->GetSkyColor();
        cb.padSky = 0.0f;

        // Smoothly interpolate between sunlight and moonlight based on sun elevation. Transition band: sunY in [-0.1, 0.1]
        float sunY = m_skyDome->GetSunDirection().y;
        float dayBlend = std::clamp((sunY + 0.1f) / 0.2f, 0.0f, 1.0f);

        XMFLOAT3 sunDir  = m_skyDome->GetSunDirection();
        XMFLOAT3 moonDir = m_skyDome->GetMoonDirection();
        cb.sunDir = XMFLOAT3(
            std::lerp(moonDir.x, sunDir.x, dayBlend),
            std::lerp(moonDir.y, sunDir.y, dayBlend),
            std::lerp(moonDir.z, sunDir.z, dayBlend));

        cb.sunIntensity = std::lerp(m_skyDome->GetMoonIntensity(), m_skyDome->GetSunIntensity(), dayBlend);

        XMFLOAT3 sunCol  = m_skyDome->GetSunColor();
        XMFLOAT3 moonCol = m_skyDome->GetMoonColor();
        cb.sunColor = XMFLOAT3(
            std::lerp(moonCol.x, sunCol.x, dayBlend),
            std::lerp(moonCol.y, sunCol.y, dayBlend),
            std::lerp(moonCol.z, sunCol.z, dayBlend));


        // Fog
        {
            float weatherIntensity = m_weatherSystem ? m_weatherSystem->GetWeatherIntensity() : 0.0f;
            if (m_showcaseMode)
            {
                cb.fogStart = 1200.0f;
                cb.fogEnd   = 2000.0f;
            }
            else
            {
                // Infinite ocean support: hide the LOD transition line (~800m) with fog
                cb.fogStart = 900.0f  - weatherIntensity * 300.0f;
                cb.fogEnd   = 1600.0f - weatherIntensity * 400.0f;
            }
            cb.foamIntensity = 0.15f + weatherIntensity * 0.85f;
            cb.ssrMix = m_ssrMix;
        }
    }
    else
    {
        // Default values when the sky system is absent
        cb.sunDir = { 0.5f, 1.0f, 0.3f };
        cb.sunIntensity = 1.0f;
        cb.sunColor = { 1.0f, 0.95f, 0.8f };
        cb.padSun = 0.0f;
        cb.skyColor = { 0.4f, 0.6f, 0.9f };
        cb.padSky = 0.0f;
        
    }
    //               direction         amplitude  wavelength  speed  steepness
    cb.waves[0] = { {1.0f,  0.0f},  0.3f,  60.0f, 1.0f, 0.06f };
    cb.waves[1] = { {0.7f,  0.7f},  0.15f, 35.0f, 1.3f, 0.05f };
    cb.waves[2] = { {0.2f, -0.9f},  0.08f, 20.0f, 1.6f, 0.04f };
    cb.waves[3] = { {-0.5f, 0.8f},  0.05f, 12.0f, 1.1f, 0.03f };

    m_lastSceneCB = cb;
    memcpy(m_pCbvDataBegin, &cb, sizeof(cb));

    float speed = 0.55f;
    float forward = 0.0f,right = 0.0f;

    if (GetAsyncKeyState('W') & 0x8000) forward += speed;
    if (GetAsyncKeyState('S') & 0x8000) forward -= speed;
    if (GetAsyncKeyState('D') & 0x8000) right += speed;
    if (GetAsyncKeyState('A') & 0x8000) right -= speed;

    m_camera.Move(forward, right);
	// Auto-orbit in showcase mode
    if (m_showcaseMode)
        m_camera.UpdateShowcase(deltaTime);
}

void Renderer::OnMouseMove(float dx, float dy)
{
    m_camera.ProcessMouse(dx, dy);
}



void Renderer::ToggleWireframe()
{
    m_wireframe = !m_wireframe;
}

void Renderer::CreateDepthBuffer(UINT width, UINT height)

// Creates the DSV heap and depth buffer
{
    // DSV descriptor heap
    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
    dsvHeapDesc.NumDescriptors = 1;
    dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ThrowIfFailed(m_device->CreateDescriptorHeap(
        &dsvHeapDesc, IID_PPV_ARGS(&m_dsvHeap)));

    // Depth buffer resource
    D3D12_RESOURCE_DESC depthDesc = {};
    depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    depthDesc.Width = width;
    depthDesc.Height = height;
    depthDesc.DepthOrArraySize = 1;
    depthDesc.MipLevels = 1;
    depthDesc.Format = DXGI_FORMAT_R32_TYPELESS; // R32_TYPELESS allows creation of both DSV(D32) and SRV(R32)
    depthDesc.SampleDesc.Count = 1;
    depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    // Clear value — for GPU optimization. Tells the driver the intended clear value each frame
    D3D12_CLEAR_VALUE clearValue = {};
    clearValue.Format = DXGI_FORMAT_D32_FLOAT;
    clearValue.DepthStencil.Depth = 1.0f; // 1.0 = farthest
    clearValue.DepthStencil.Stencil = 0;

    auto heapProp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    ThrowIfFailed(m_device->CreateCommittedResource(
        &heapProp,
        D3D12_HEAP_FLAG_NONE,
        &depthDesc,
        D3D12_RESOURCE_STATE_DEPTH_WRITE, // Initial state is directly DEPTH_WRITE
        &clearValue,
        IID_PPV_ARGS(&m_depthBuffer)));

    // Create DSV in heap slot 0
    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
    dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    dsvDesc.Flags = D3D12_DSV_FLAG_NONE;

    m_device->CreateDepthStencilView(
        m_depthBuffer.Get(),
        &dsvDesc,
        m_dsvHeap->GetCPUDescriptorHandleForHeapStart());

}

void Renderer::CreateGridBuffers(ComPtr<ID3D12GraphicsCommandList> cmdList)
{
    // Generate grid (world-space size GRID_WORLD_SIZE)
    GridMeshData grid = GenerateGrid(GRID_SIZE, GRID_SIZE, GRID_WORLD_SIZE);
    m_gridIndexCount = static_cast<UINT>(grid.indices.size());

    // ---- Vertex buffer ----
    UINT vbSize = static_cast<UINT>(
        grid.vertices.size() * sizeof(GridVertex));

    auto defaultHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    auto uploadHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    auto vbDesc = CD3DX12_RESOURCE_DESC::Buffer(vbSize);

    // Default heap (GPU read-only, fastest)
    ThrowIfFailed(m_device->CreateCommittedResource(
        &defaultHeap, D3D12_HEAP_FLAG_NONE, &vbDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        IID_PPV_ARGS(&m_gridVB)));

    // Upload heap (CPU-writable, used as staging)
    ThrowIfFailed(m_device->CreateCommittedResource(
        &uploadHeap, D3D12_HEAP_FLAG_NONE, &vbDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&m_gridVBUpload)));

    // CPU writes vertex data to the upload heap
    void* pData = nullptr;
    CD3DX12_RANGE readRange(0, 0);
    ThrowIfFailed(m_gridVBUpload->Map(0, &readRange, &pData));
    memcpy(pData, grid.vertices.data(), vbSize);
    m_gridVBUpload->Unmap(0, nullptr);

    // Record GPU copy command: Upload → Default
    cmdList->CopyBufferRegion(
        m_gridVB.Get(), 0, m_gridVBUpload.Get(), 0, vbSize);

    auto vbBarrier = CD3DX12_RESOURCE_BARRIER::Transition(
        m_gridVB.Get(),
        D3D12_RESOURCE_STATE_COMMON,
        D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    cmdList->ResourceBarrier(1, &vbBarrier);

    // Set up VBV
    m_gridVBView.BufferLocation = m_gridVB->GetGPUVirtualAddress();
    m_gridVBView.StrideInBytes = sizeof(GridVertex);
    m_gridVBView.SizeInBytes = vbSize;

    // ---- Index buffer ----
    UINT ibSize = static_cast<UINT>(
        grid.indices.size() * sizeof(uint32_t));
    auto ibDesc = CD3DX12_RESOURCE_DESC::Buffer(ibSize);

    ThrowIfFailed(m_device->CreateCommittedResource(
        &defaultHeap, D3D12_HEAP_FLAG_NONE, &ibDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        IID_PPV_ARGS(&m_gridIB)));

    ThrowIfFailed(m_device->CreateCommittedResource(
        &uploadHeap, D3D12_HEAP_FLAG_NONE, &ibDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&m_gridIBUpload)));

    ThrowIfFailed(m_gridIBUpload->Map(0, &readRange, &pData));
    memcpy(pData, grid.indices.data(), ibSize);
    m_gridIBUpload->Unmap(0, nullptr);

    cmdList->CopyBufferRegion(
        m_gridIB.Get(), 0, m_gridIBUpload.Get(), 0, ibSize);

    auto ibBarrier = CD3DX12_RESOURCE_BARRIER::Transition(
        m_gridIB.Get(),
        D3D12_RESOURCE_STATE_COMMON,
        D3D12_RESOURCE_STATE_INDEX_BUFFER);  
    cmdList->ResourceBarrier(1, &ibBarrier);

    m_gridIBView.BufferLocation = m_gridIB->GetGPUVirtualAddress();
    m_gridIBView.Format = DXGI_FORMAT_R32_UINT;
    m_gridIBView.SizeInBytes = ibSize;
}

void Renderer::CreateLodGridBuffers(ComPtr<ID3D12GraphicsCommandList> cmdList)
{
    // Low-resolution grid for distant tiles (64x64)
    GridMeshData lod = GenerateGrid(64, 64, GRID_WORLD_SIZE);
    m_lodIndexCount = static_cast<UINT>(lod.indices.size());

    auto defaultHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    auto uploadHeap  = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    CD3DX12_RANGE readRange(0, 0);
    void* pData = nullptr;

    UINT vbSize = static_cast<UINT>(lod.vertices.size() * sizeof(GridVertex));
    auto vbDesc = CD3DX12_RESOURCE_DESC::Buffer(vbSize);
    ThrowIfFailed(m_device->CreateCommittedResource(
        &defaultHeap, D3D12_HEAP_FLAG_NONE, &vbDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_lodVB)));
    ThrowIfFailed(m_device->CreateCommittedResource(
        &uploadHeap, D3D12_HEAP_FLAG_NONE, &vbDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_lodVBUpload)));
    ThrowIfFailed(m_lodVBUpload->Map(0, &readRange, &pData));
    memcpy(pData, lod.vertices.data(), vbSize);
    m_lodVBUpload->Unmap(0, nullptr);
    cmdList->CopyBufferRegion(m_lodVB.Get(), 0, m_lodVBUpload.Get(), 0, vbSize);
    auto vbBarrier = CD3DX12_RESOURCE_BARRIER::Transition(
        m_lodVB.Get(), D3D12_RESOURCE_STATE_COMMON,
        D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    cmdList->ResourceBarrier(1, &vbBarrier);
    m_lodVBView.BufferLocation = m_lodVB->GetGPUVirtualAddress();
    m_lodVBView.StrideInBytes  = sizeof(GridVertex);
    m_lodVBView.SizeInBytes    = vbSize;

    UINT ibSize = static_cast<UINT>(lod.indices.size() * sizeof(uint32_t));
    auto ibDesc = CD3DX12_RESOURCE_DESC::Buffer(ibSize);
    ThrowIfFailed(m_device->CreateCommittedResource(
        &defaultHeap, D3D12_HEAP_FLAG_NONE, &ibDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_lodIB)));
    ThrowIfFailed(m_device->CreateCommittedResource(
        &uploadHeap, D3D12_HEAP_FLAG_NONE, &ibDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_lodIBUpload)));
    ThrowIfFailed(m_lodIBUpload->Map(0, &readRange, &pData));
    memcpy(pData, lod.indices.data(), ibSize);
    m_lodIBUpload->Unmap(0, nullptr);
    cmdList->CopyBufferRegion(m_lodIB.Get(), 0, m_lodIBUpload.Get(), 0, ibSize);
    auto ibBarrier = CD3DX12_RESOURCE_BARRIER::Transition(
        m_lodIB.Get(), D3D12_RESOURCE_STATE_COMMON,
        D3D12_RESOURCE_STATE_INDEX_BUFFER);
    cmdList->ResourceBarrier(1, &ibBarrier);
    m_lodIBView.BufferLocation = m_lodIB->GetGPUVirtualAddress();
    m_lodIBView.Format         = DXGI_FORMAT_R32_UINT;
    m_lodIBView.SizeInBytes    = ibSize;
}

void Renderer::ExtractFrustumPlanes()
{
    XMMATRIX vp = m_camera.GetViewMatrix() * m_camera.GetProjMatrix();
    XMFLOAT4X4 m;
    XMStoreFloat4x4(&m, vp);

    // Gribb-Hartmann method: extract clip planes from row vectors x VP matrix
    m_frustumPlanes[0] = { m._11+m._14, m._21+m._24, m._31+m._34, m._41+m._44 }; // Left
    m_frustumPlanes[1] = { m._14-m._11, m._24-m._21, m._34-m._31, m._44-m._41 }; // Right
    m_frustumPlanes[2] = { m._12+m._14, m._22+m._24, m._32+m._34, m._42+m._44 }; // Bottom
    m_frustumPlanes[3] = { m._14-m._12, m._24-m._22, m._34-m._32, m._44-m._42 }; // Top
    m_frustumPlanes[4] = { m._13,       m._23,       m._33,       m._43       }; // Near
    m_frustumPlanes[5] = { m._14-m._13, m._24-m._23, m._34-m._33, m._44-m._43 }; // Far
}

bool Renderer::IsTileVisible(int tx, int tz) const
{
    // Grid is centered in local space at [-TILE_SIZE/2, +TILE_SIZE/2]
    // World range = tileOffset ± TILE_SIZE/2
    float half  = TILE_SIZE * 0.5f;
    float minX  = tx * TILE_SIZE - half, maxX = tx * TILE_SIZE + half;
    float minZ  = tz * TILE_SIZE - half, maxZ = tz * TILE_SIZE + half;
    constexpr float minY = -40.0f, maxY = 40.0f;

    // Expand frustum bounds by TILE_SIZE/2 to prevent edge popping
    constexpr float CULL_BIAS = TILE_SIZE * 0.5f;
    for (const auto& p : m_frustumPlanes)
    {
        float dx = (p.x > 0.0f) ? maxX : minX;
        float dy = (p.y > 0.0f) ? maxY : minY;
        float dz = (p.z > 0.0f) ? maxZ : minZ;
        if (p.x * dx + p.y * dy + p.z * dz + p.w < -CULL_BIAS)
            return false;
    }
    return true;
}


void Renderer::CreateWireframePSO()
{
    D3D12_INPUT_ELEMENT_DESC inputElementDescs[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 12,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    CD3DX12_RASTERIZER_DESC rasterDesc(D3D12_DEFAULT);
    rasterDesc.FillMode = D3D12_FILL_MODE_WIREFRAME;  // Wireframe
    rasterDesc.CullMode = D3D12_CULL_MODE_NONE;       // Disable backface culling

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = { inputElementDescs, _countof(inputElementDescs) };
    psoDesc.pRootSignature = m_rootSignature.Get();

    // Reuse saved bytecode. Uses the same shader set as the solid PSO
    psoDesc.VS = CD3DX12_SHADER_BYTECODE(
        m_vertexShaderData.data(), m_vertexShaderData.size());
    psoDesc.PS = CD3DX12_SHADER_BYTECODE(
        m_pixelShaderData.data(), m_pixelShaderData.size());

    psoDesc.RasterizerState = rasterDesc;                          
    psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.SampleDesc.Count = 1;

    ThrowIfFailed(m_device->CreateGraphicsPipelineState(
        &psoDesc, IID_PPV_ARGS(&m_wireframePSO)));
}

void Renderer::CreateWaterBoxBuffers(ComPtr<ID3D12GraphicsCommandList> cmdList)
{
    GridMeshData box = GenerateWaterBox(GRID_WORLD_SIZE, GRID_WORLD_SIZE * 0.5f);
    m_boxIndexCount = static_cast<UINT>(box.indices.size());

    

    UINT vbSize = static_cast<UINT>(box.vertices.size() * sizeof(GridVertex));
    UINT ibSize = static_cast<UINT>(box.indices.size() * sizeof(uint32_t));

    auto defaultHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    auto uploadHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);

    // Vertex buffer
    auto vbDesc = CD3DX12_RESOURCE_DESC::Buffer(vbSize);
    ThrowIfFailed(m_device->CreateCommittedResource(
        &defaultHeap, D3D12_HEAP_FLAG_NONE, &vbDesc,
        D3D12_RESOURCE_STATE_COMMON, nullptr,
        IID_PPV_ARGS(&m_boxVB)));
    ThrowIfFailed(m_device->CreateCommittedResource(
        &uploadHeap, D3D12_HEAP_FLAG_NONE, &vbDesc,
        D3D12_RESOURCE_STATE_COMMON, nullptr,
        IID_PPV_ARGS(&m_boxVBUpload)));

    void* pData = nullptr;
    CD3DX12_RANGE readRange(0, 0);
    ThrowIfFailed(m_boxVBUpload->Map(0, &readRange, &pData));
    memcpy(pData, box.vertices.data(), vbSize);
    m_boxVBUpload->Unmap(0, nullptr);
    cmdList->CopyBufferRegion(m_boxVB.Get(), 0, m_boxVBUpload.Get(), 0, vbSize);
    auto vbBarrier = CD3DX12_RESOURCE_BARRIER::Transition(
        m_boxVB.Get(),
        D3D12_RESOURCE_STATE_COMMON,
        D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    cmdList->ResourceBarrier(1, &vbBarrier);

    m_boxVBView.BufferLocation = m_boxVB->GetGPUVirtualAddress();
    m_boxVBView.StrideInBytes = sizeof(GridVertex);
    m_boxVBView.SizeInBytes = vbSize;

    // Index buffer
    auto ibDesc = CD3DX12_RESOURCE_DESC::Buffer(ibSize);
    ThrowIfFailed(m_device->CreateCommittedResource(
        &defaultHeap, D3D12_HEAP_FLAG_NONE, &ibDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        IID_PPV_ARGS(&m_boxIB)));
    ThrowIfFailed(m_device->CreateCommittedResource(
        &uploadHeap, D3D12_HEAP_FLAG_NONE, &ibDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&m_boxIBUpload)));

    ThrowIfFailed(m_boxIBUpload->Map(0, &readRange, &pData));
    memcpy(pData, box.indices.data(), ibSize);
    m_boxIBUpload->Unmap(0, nullptr);
    cmdList->CopyBufferRegion(m_boxIB.Get(), 0, m_boxIBUpload.Get(), 0, ibSize);
    auto ibBarrier = CD3DX12_RESOURCE_BARRIER::Transition(
        m_boxIB.Get(),
        D3D12_RESOURCE_STATE_COMMON,
        D3D12_RESOURCE_STATE_INDEX_BUFFER);
    cmdList->ResourceBarrier(1, &ibBarrier);

    m_boxIBView.BufferLocation = m_boxIB->GetGPUVirtualAddress();
    m_boxIBView.Format = DXGI_FORMAT_R32_UINT;
    m_boxIBView.SizeInBytes = ibSize;
}

void Renderer::RenderWaterBox(RenderContext& ctx)
{
    ctx.cmd->SetPipelineState(m_waterBoxPSO.Get());
    ctx.cmd->SetGraphicsRootSignature(m_rootSignature.Get());
    ctx.cmd->SetGraphicsRootConstantBufferView(0,
        m_constantBuffer->GetGPUVirtualAddress());
    ctx.cmd->OMSetRenderTargets(1, &ctx.rtv, FALSE, &ctx.dsv);
    ctx.cmd->RSSetViewports(1, &ctx.viewport);
    ctx.cmd->RSSetScissorRects(1, &ctx.scissor);
    ctx.cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx.cmd->IASetVertexBuffers(0, 1, &m_boxVBView);
    ctx.cmd->IASetIndexBuffer(&m_boxIBView);
    ctx.cmd->DrawIndexedInstanced(m_boxIndexCount, 1, 0, 0, 0);
}