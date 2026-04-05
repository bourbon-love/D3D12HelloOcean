
#include "Renderer.h"
#include <d3dx12_barriers.h>
#include <d3dx12_core.h>
#include "../DXSampleHelper.h"
#include "GridMesh.h"
#include "SkyDome.h"

void Renderer::InitPSO(
    ComPtr<ID3D12Device> device,
    ComPtr<ID3D12RootSignature> rootSignature,
    UINT width, UINT height,
    const UINT8* vsData, UINT vsSize,
    const UINT8* psData, UINT psSize)
{
    m_device = device;
    m_rootSignature = rootSignature;
    m_camera.aspect = static_cast<float>(width) / static_cast<float>(height);

    // 保存字节码
    m_vertexShaderData.assign(vsData, vsData + vsSize);
    m_pixelShaderData.assign(psData, psData + psSize);

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

    // Input Layout（两个PSO共用）
    D3D12_INPUT_ELEMENT_DESC inputElementDescs[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    // 实体PSO
    {
        
        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.InputLayout = { inputElementDescs, _countof(inputElementDescs) };
        psoDesc.pRootSignature = m_rootSignature.Get();
        psoDesc.VS = CD3DX12_SHADER_BYTECODE(
            m_vertexShaderData.data(), m_vertexShaderData.size());
        psoDesc.PS = CD3DX12_SHADER_BYTECODE(
            m_pixelShaderData.data(), m_pixelShaderData.size());
        psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
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

    // 线框PSO
    CreateWireframePSO();

    // Depth Buffer
    CreateDepthBuffer(width, height);
}

void Renderer::InitResources(ComPtr<ID3D12GraphicsCommandList> commandList)
{
    m_commandList = commandList;
    CreateGridBuffers(commandList);  // 录制Grid上传命令
}

void Renderer::Render(RenderContext& ctx)
{
    auto* pso = m_wireframe
        ? m_wireframePSO.Get() : m_pipelineState.Get();
    
    ctx.cmd->SetPipelineState(pso);
    ctx.cmd->SetGraphicsRootSignature(m_rootSignature.Get());
    ctx.cmd->SetGraphicsRootConstantBufferView(0, m_constantBuffer->GetGPUVirtualAddress());
    ctx.cmd->OMSetRenderTargets(1, &ctx.rtv, FALSE, &ctx.dsv);
    ctx.cmd->RSSetViewports(1, &ctx.viewport);
    ctx.cmd->RSSetScissorRects(1, &ctx.scissor);
    ctx.cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx.cmd->IASetVertexBuffers(0, 1, &ctx.vb);
    ctx.cmd->IASetIndexBuffer(&ctx.ib);
    ctx.cmd->DrawIndexedInstanced(ctx.indexCount, 1, 0, 0, 0);
   

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

    // 从天空系统读取数据
    if (m_skyDome)
    {
        cb.sunDir = m_skyDome->GetSunDirection();
        cb.sunIntensity = m_skyDome->GetSunIntensity();
        cb.sunColor = m_skyDome->GetSunColor();
        cb.padSun = 0.0f;
        cb.skyColor = m_skyDome->GetSkyColor();
        cb.padSky = 0.0f;
    }
    else
    {
        // 没有天空系统时的默认值
        cb.sunDir = { 0.5f, 1.0f, 0.3f };
        cb.sunIntensity = 1.0f;
        cb.sunColor = { 1.0f, 0.95f, 0.8f };
        cb.padSun = 0.0f;
        cb.skyColor = { 0.4f, 0.6f, 0.9f };
        cb.padSky = 0.0f;
    }
    // 4个叠加波的参数
    //               方向              振幅   波长   速度   陡度
    cb.waves[0] = { {1.0f,  0.0f},  1.5f,  60.0f, 1.0f, 0.06f };  // 主浪，大波长
    cb.waves[1] = { {0.7f,  0.7f},  0.8f,  35.0f, 1.3f, 0.05f };  // 斜向中浪
    cb.waves[2] = { {0.2f, -0.9f},  0.4f,  20.0f, 1.6f, 0.04f };  // 小波纹
    cb.waves[3] = { {-0.5f, 0.8f},  0.2f,  12.0f, 1.1f, 0.03f };  // 反向细纹


    memcpy(m_pCbvDataBegin, &cb, sizeof(cb));

    float speed = 0.05f;
    float forward = 0.0f,right = 0.0f;

    if (GetAsyncKeyState('W') & 0x8000) forward += speed;
    if (GetAsyncKeyState('S') & 0x8000) forward -= speed;
    if (GetAsyncKeyState('D') & 0x8000) right += speed;
    if (GetAsyncKeyState('A') & 0x8000) right -= speed;

    m_camera.Move(forward, right);
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

//create DSVHeap and Depth Buffer
{
    //DSV Descriptor Heap 
    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
    dsvHeapDesc.NumDescriptors = 1;
    dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ThrowIfFailed(m_device->CreateDescriptorHeap(
        &dsvHeapDesc, IID_PPV_ARGS(&m_dsvHeap)));

    //Depth Buffer Resource
    D3D12_RESOURCE_DESC depthDesc = {};
    depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    depthDesc.Width = width;
    depthDesc.Height = height;
    depthDesc.DepthOrArraySize = 1;
    depthDesc.MipLevels = 1;
    depthDesc.Format = DXGI_FORMAT_D32_FLOAT;
    depthDesc.SampleDesc.Count = 1;
    depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    // 清除值 — GPU优化用，告诉驱动每帧清除的目标值
    D3D12_CLEAR_VALUE clearValue = {};
    clearValue.Format = DXGI_FORMAT_D32_FLOAT;
    clearValue.DepthStencil.Depth = 1.0f; //1.0 = 最远
    clearValue.DepthStencil.Stencil = 0;

    auto heapProp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    ThrowIfFailed(m_device->CreateCommittedResource(
        &heapProp,
        D3D12_HEAP_FLAG_NONE,
        &depthDesc,
        D3D12_RESOURCE_STATE_DEPTH_WRITE,//初始状态直接是depth_write
        &clearValue,
        IID_PPV_ARGS(&m_depthBuffer)));

    //create DSV Discrib in heap num0
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
    // 生成32x32网格，世界空间20x20单位大小
    GridMeshData grid = GenerateGrid(GRID_SIZE, GRID_SIZE, GRID_WORLD_SIZE);
    m_gridIndexCount = static_cast<UINT>(grid.indices.size());

    // ---- Vertex Buffer ----
    UINT vbSize = static_cast<UINT>(
        grid.vertices.size() * sizeof(GridVertex));

    auto defaultHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    auto uploadHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    auto vbDesc = CD3DX12_RESOURCE_DESC::Buffer(vbSize);

    // Default Heap（GPU只读，最快）
    ThrowIfFailed(m_device->CreateCommittedResource(
        &defaultHeap, D3D12_HEAP_FLAG_NONE, &vbDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        IID_PPV_ARGS(&m_gridVB)));

    // Upload Heap（CPU写入，作为中转）
    ThrowIfFailed(m_device->CreateCommittedResource(
        &uploadHeap, D3D12_HEAP_FLAG_NONE, &vbDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&m_gridVBUpload)));

    // CPU把顶点数据写入Upload Heap
    void* pData = nullptr;
    CD3DX12_RANGE readRange(0, 0);
    ThrowIfFailed(m_gridVBUpload->Map(0, &readRange, &pData));
    memcpy(pData, grid.vertices.data(), vbSize);
    m_gridVBUpload->Unmap(0, nullptr);

    // 录制GPU复制命令：Upload → Default
    cmdList->CopyBufferRegion(
        m_gridVB.Get(), 0, m_gridVBUpload.Get(), 0, vbSize);

    auto vbBarrier = CD3DX12_RESOURCE_BARRIER::Transition(
        m_gridVB.Get(),
        D3D12_RESOURCE_STATE_COMMON,
        D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    cmdList->ResourceBarrier(1, &vbBarrier);

    // VBV
    m_gridVBView.BufferLocation = m_gridVB->GetGPUVirtualAddress();
    m_gridVBView.StrideInBytes = sizeof(GridVertex);
    m_gridVBView.SizeInBytes = vbSize;

    // ---- Index Buffer ----
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
    rasterDesc.FillMode = D3D12_FILL_MODE_WIREFRAME;  // 线框
    rasterDesc.CullMode = D3D12_CULL_MODE_NONE;       // 关闭背面剔除

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = { inputElementDescs, _countof(inputElementDescs) };
    psoDesc.pRootSignature = m_rootSignature.Get();

    // 复用已保存的字节码，和实体PSO用同一套Shader
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