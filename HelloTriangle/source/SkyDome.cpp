#include "SkyDome.h"
#include <d3dx12_barriers.h>
#include <cmath>
#include <algorithm>

// 球体顶点，只有Position，和原来的SKY_VERTEX一样
struct SkyVertex { XMFLOAT3 position; };

// 直接复用原来的CreatSphereVertices逻辑
static void BuildSphereMesh(
    float radius, int slices, int stacks,
    std::vector<SkyVertex>& outVerts,
    std::vector<uint32_t>& outIdx)
{
    // 顶点生成：和原来完全一样的公式
    for (int stack = 0; stack <= stacks; ++stack)
    {
        float phi = XM_PI * stack / stacks;
        float y = radius * cosf(phi);
        float r = radius * sinf(phi);

        for (int slice = 0; slice <= slices; ++slice)
        {
            float theta = 2.0f * XM_PI * slice / slices;
            outVerts.push_back({ XMFLOAT3(r * sinf(theta), y, r * cosf(theta)) });
        }
    }

    // 索引生成：和原来完全一样的绕序
    for (int stack = 0; stack < stacks; ++stack)
    {
        for (int slice = 0; slice < slices; ++slice)
        {
            uint32_t v1 = stack * (slices + 1) + slice;
            uint32_t v2 = stack * (slices + 1) + slice + 1;
            uint32_t v3 = (stack + 1) * (slices + 1) + slice;
            uint32_t v4 = (stack + 1) * (slices + 1) + slice + 1;

            // 原来的绕序
            outIdx.push_back(v1); outIdx.push_back(v3); outIdx.push_back(v2);
            outIdx.push_back(v2); outIdx.push_back(v3); outIdx.push_back(v4);
        }
    }
}

void SkyDome::InitPSO(
    ComPtr<ID3D12Device>        device,
    ComPtr<ID3D12RootSignature> rootSignature,
    UINT width, UINT height,
    const UINT8* vsData, UINT vsSize,
    const UINT8* psData, UINT psSize)
{
    m_device = device;
    m_rootSignature = rootSignature;
    m_width = width;
    m_height = height;

    // CBV — Upload Heap，每帧更新
    {
        auto heapProp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        auto bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(SkyCB));
        ThrowIfFailed(m_device->CreateCommittedResource(
            &heapProp, D3D12_HEAP_FLAG_NONE, &bufferDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&m_cbuffer)));
        CD3DX12_RANGE readRange(0, 0);
        ThrowIfFailed(m_cbuffer->Map(0, &readRange,
            reinterpret_cast<void**>(&m_cbMapped)));
    }

    CreateSkyPSO(vsData, vsSize, psData, psSize);
}

void SkyDome::CreateSkyPSO(
    const UINT8* vsData, UINT vsSize,
    const UINT8* psData, UINT psSize)
{
    // 天空Dome只需要Position，和SkyVertex完全对应
    D3D12_INPUT_ELEMENT_DESC layout[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    // RasterizerState：剔除前面（摄像机在球内部，看的是内面）
    CD3DX12_RASTERIZER_DESC rasterDesc(D3D12_DEFAULT);
    rasterDesc.CullMode = D3D12_CULL_MODE_FRONT;

    // DepthStencilState：开启深度测试但不写入深度
    // 天空在最远处，不能遮挡海浪
    CD3DX12_DEPTH_STENCIL_DESC depthDesc(D3D12_DEFAULT);
    depthDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO; // 不写深度
    depthDesc.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL; // 用LESS_EQUAL

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = { layout, _countof(layout) };
    psoDesc.pRootSignature = m_rootSignature.Get();
    psoDesc.VS = CD3DX12_SHADER_BYTECODE(vsData, vsSize);
    psoDesc.PS = CD3DX12_SHADER_BYTECODE(psData, psSize);
    psoDesc.RasterizerState = rasterDesc;
    psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    psoDesc.DepthStencilState = depthDesc;
    psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.SampleDesc.Count = 1;

    ThrowIfFailed(m_device->CreateGraphicsPipelineState(
        &psoDesc, IID_PPV_ARGS(&m_skyPSO)));
}

void SkyDome::InitResources(ComPtr<ID3D12GraphicsCommandList> cmdList)
{
    CreateSphereMesh(cmdList);
}

void SkyDome::CreateSphereMesh(ComPtr<ID3D12GraphicsCommandList> cmdList)
{
    std::vector<SkyVertex> verts;
    std::vector<uint32_t>  indices;

    // 50x50，和原来一样
    BuildSphereMesh(1.0f, 50, 50, verts, indices);
    m_indexCount = static_cast<UINT>(indices.size());

    UINT vbSize = static_cast<UINT>(verts.size() * sizeof(SkyVertex));
    UINT ibSize = static_cast<UINT>(indices.size() * sizeof(uint32_t));

    auto uploadHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    void* pData = nullptr;
    CD3DX12_RANGE readRange(0, 0);

    // VB — Upload Heap（球体是静态的，但数据量小，用Upload也没问题）
    auto vbDesc = CD3DX12_RESOURCE_DESC::Buffer(vbSize);
    ThrowIfFailed(m_device->CreateCommittedResource(
        &uploadHeap, D3D12_HEAP_FLAG_NONE, &vbDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&m_vb)));
    ThrowIfFailed(m_vb->Map(0, &readRange, &pData));
    memcpy(pData, verts.data(), vbSize);
    m_vb->Unmap(0, nullptr);

    m_vbView.BufferLocation = m_vb->GetGPUVirtualAddress();
    m_vbView.StrideInBytes = sizeof(SkyVertex);
    m_vbView.SizeInBytes = vbSize;

    // IB
    auto ibDesc = CD3DX12_RESOURCE_DESC::Buffer(ibSize);
    ThrowIfFailed(m_device->CreateCommittedResource(
        &uploadHeap, D3D12_HEAP_FLAG_NONE, &ibDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&m_ib)));
    ThrowIfFailed(m_ib->Map(0, &readRange, &pData));
    memcpy(pData, indices.data(), ibSize);
    m_ib->Unmap(0, nullptr);

    m_ibView.BufferLocation = m_ib->GetGPUVirtualAddress();
    m_ibView.Format = DXGI_FORMAT_R32_UINT;
    m_ibView.SizeInBytes = ibSize;

    // Upload Heap不需要CopyBufferRegion，cmdList参数暂时不使用
    (void)cmdList;
}

void SkyDome::Update(float deltaTime)
{
    m_time += deltaTime * 0.5f;

    float angle = m_time * 0.3f; // 公转速度

    // 太阳：水平圆形轨道，略微倾斜
    float orbitRadius = 0.95f;   // 轨道半径（归一化方向向量）
    float orbitHeight = 0.15f;   // 轨道高度，始终在地平线略上方

    m_sunDir.x = cosf(angle) * orbitRadius;
    m_sunDir.y = sinf(angle * 0.5f) * 0.4f; // Y方向小幅变化，模拟轨道倾斜
    m_sunDir.z = sinf(angle) * orbitRadius;

    // 归一化
    float len = sqrtf(m_sunDir.x * m_sunDir.x +
        m_sunDir.y * m_sunDir.y +
        m_sunDir.z * m_sunDir.z);
    m_sunDir.x /= len;
    m_sunDir.y /= len;
    m_sunDir.z /= len;

    // 月亮：太阳对面，角度差180度
    m_moonDir.x = -m_sunDir.x;
    m_moonDir.y = -m_sunDir.y * 0.5f + 0.2f; // 月亮轨道略有不同
    m_moonDir.z = -m_sunDir.z;

    float moonLen = sqrtf(m_moonDir.x * m_moonDir.x +
        m_moonDir.y * m_moonDir.y +
        m_moonDir.z * m_moonDir.z);
    m_moonDir.x /= moonLen;
    m_moonDir.y /= moonLen;
    m_moonDir.z /= moonLen;

    // 云参数
    float cycle1 = sinf(m_time * 0.2f) * 0.5f + 0.5f;
    float cycle2 = cosf(m_time * 0.15f) * 0.5f + 0.5f;
    m_cloudDensity = 0.5f + cycle1 * 0.1f;
    m_cloudScale = 0.85f + cycle2 * 0.15f;
    m_cloudSharpness = 0.6f + sinf(m_time * 0.1f) * 0.1f;
}
void SkyDome::Render(RenderContext& ctx)
{
    // 从ctx里拿view/proj，天空球跟随摄像机（只用旋转，去掉平移）
    // 外部在构建ctx时需要传入view和proj矩阵
    // 天空球缩放1000倍（和原来Scale=1000一致）
    XMMATRIX scale = XMMatrixScaling(1000.0f, 1000.0f, 1000.0f);

    // 注意：天空球要去掉平移分量，只保留旋转
    // 从view矩阵提取旋转部分（清除第四列的平移）
    XMMATRIX viewNoTrans = ctx.view;
    viewNoTrans.r[3] = XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f);

    XMMATRIX viewProj = scale * viewNoTrans * ctx.proj;

    // 更新CBV
    SkyCB cb;
    cb.viewProj = XMMatrixTranspose(viewProj);
    cb.topColor = XMFLOAT4(0.1f, 0.3f, 0.7f, 1.0f);
    cb.middleColor = XMFLOAT4(0.4f, 0.6f, 0.9f, 1.0f);
    cb.bottomColor = XMFLOAT4(0.7f, 0.8f, 1.0f, 1.0f);
    cb.sunPosition = m_sunDir;
    cb.time = m_time;
    cb.cloudDensity = m_cloudDensity;
    cb.cloudScale = m_cloudScale;
    cb.cloudSharpness = m_cloudSharpness;
    cb.weatherIntensity = m_weatherIntensity;
    cb.sunColor = GetSunColor();
    cb.padSunColor = 0.0f;
	cb.moonPosition = m_moonDir;
	cb.padMoon = 0.0f;
    memcpy(m_cbMapped, &cb, sizeof(cb));

    // 切换到天空PSO
    ctx.cmd->SetPipelineState(m_skyPSO.Get());
    ctx.cmd->SetGraphicsRootSignature(m_rootSignature.Get());
    ctx.cmd->SetGraphicsRootConstantBufferView(
        0, m_cbuffer->GetGPUVirtualAddress());
    ctx.cmd->OMSetRenderTargets(1, &ctx.rtv, FALSE, &ctx.dsv);
    ctx.cmd->RSSetViewports(1, &ctx.viewport);
    ctx.cmd->RSSetScissorRects(1, &ctx.scissor);

    ctx.cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx.cmd->IASetVertexBuffers(0, 1, &m_vbView);
    ctx.cmd->IASetIndexBuffer(&m_ibView);
    ctx.cmd->DrawIndexedInstanced(m_indexCount, 1, 0, 0, 0);
}


// 根据太阳高度计算强度：太阳在地平线以下时强度为0
float SkyDome::GetSunIntensity() const
{
    // m_sunDir.y是太阳的垂直分量
    // 正午y≈1（最亮），日落y≈0（地平线），夜晚y<0（熄灭）
    // 加0.1让日落时还有一点余晖
    float baseIntensity = saturate(m_sunDir.y + 0.1f); 
    // 暴风时太阳强度降低到20%
    return baseIntensity * (1.0f - m_weatherIntensity * 0.8f);
}

// 根据太阳高度计算颜色：正午白色，日出日落橙红
XMFLOAT3 SkyDome::GetSunColor() const
{
    float h = saturate(m_sunDir.y + 0.2f);
    XMFLOAT3 nightColor = { 0.05f, 0.05f, 0.15f };
    XMFLOAT3 dayColor = { 0.4f,  0.6f,  0.9f };
    XMFLOAT3 stormColor = { 0.15f, 0.15f, 0.2f }; // 暴风灰色

    XMFLOAT3 baseColor = XMFLOAT3(
        nightColor.x + (dayColor.x - nightColor.x) * h,
        nightColor.y + (dayColor.y - nightColor.y) * h,
        nightColor.z + (dayColor.z - nightColor.z) * h);

    // 根据天气强度插值到暴风色
    return XMFLOAT3(
        baseColor.x + (stormColor.x - baseColor.x) * m_weatherIntensity,
        baseColor.y + (stormColor.y - baseColor.y) * m_weatherIntensity,
        baseColor.z + (stormColor.z - baseColor.z) * m_weatherIntensity);
}

// 天空主色：根据太阳高度从夜蓝到日蓝
XMFLOAT3 SkyDome::GetSkyColor() const
{
    float h = saturate(m_sunDir.y + 0.2f);  // 稍微提前变亮

    // 夜晚深蓝
    XMFLOAT3 nightColor = { 0.05f, 0.05f, 0.15f };
    // 白天天蓝
    XMFLOAT3 dayColor = { 0.4f,  0.6f,  0.9f };

    return XMFLOAT3(
        nightColor.x + (dayColor.x - nightColor.x) * h,
        nightColor.y + (dayColor.y - nightColor.y) * h,
        nightColor.z + (dayColor.z - nightColor.z) * h
    );
}