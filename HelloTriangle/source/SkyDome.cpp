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

    float tilt = 0.5f;

    // 太阳轨道
    float sunAngle = m_time * 0.3f;
    m_sunDir.x = cosf(sunAngle);
    m_sunDir.y = sinf(sunAngle) * tilt;
    m_sunDir.z = sinf(sunAngle) * sqrtf(1.0f - tilt * tilt);

    float sunLen = sqrtf(m_sunDir.x * m_sunDir.x +
        m_sunDir.y * m_sunDir.y +
        m_sunDir.z * m_sunDir.z);
    m_sunDir.x /= sunLen;
    m_sunDir.y /= sunLen;
    m_sunDir.z /= sunLen;

    // 月亮独立轨道，速度略慢，轨道平面略有倾斜
    float moonAngle = m_time * 0.23f; // 比太阳慢，产生月相周期
    float moonTilt = 0.4f;           // 轨道倾角略有不同

    // 月亮与太阳对立，在日出/日落时平滑过地平线（避免硬跳）
    {
        // sunBlend: 太阳完全在地平线下时=0，完全升起时=1，[-0.1,+0.1] 范围内平滑过渡
        float sunBlend = std::clamp((m_sunDir.y + 0.1f) / 0.2f, 0.0f, 1.0f);
        sunBlend = sunBlend * sunBlend * (3.0f - 2.0f * sunBlend); // smoothstep
        // 太阳在下时偏移 +0.1（月亮略高于地平线），太阳在上时偏移 -0.1（月亮略低于地平线）
        float moonYOffset = std::lerp(0.1f, -0.1f, sunBlend);

        m_moonDir.x = -m_sunDir.x;
        m_moonDir.y = -m_sunDir.y + moonYOffset;
        m_moonDir.z = -m_sunDir.z;
        float len = sqrtf(m_moonDir.x * m_moonDir.x +
            m_moonDir.y * m_moonDir.y +
            m_moonDir.z * m_moonDir.z);
        m_moonDir.x /= len;
        m_moonDir.y /= len;
        m_moonDir.z /= len;
    }
    // 月牙朝向：绕月亮轴缓慢旋转，与太阳解耦，约 90 秒转一圈
    {
        // 先把 m_crescentDir 投影到垂直于 moonDir 的平面，防止数值漂移
        float dotCM = m_crescentDir.x * m_moonDir.x + m_crescentDir.y * m_moonDir.y + m_crescentDir.z * m_moonDir.z;
        m_crescentDir.x -= dotCM * m_moonDir.x;
        m_crescentDir.y -= dotCM * m_moonDir.y;
        m_crescentDir.z -= dotCM * m_moonDir.z;
        float clen = sqrtf(m_crescentDir.x * m_crescentDir.x + m_crescentDir.y * m_crescentDir.y + m_crescentDir.z * m_crescentDir.z);
        if (clen > 0.001f) { m_crescentDir.x /= clen; m_crescentDir.y /= clen; m_crescentDir.z /= clen; }

        // Rodrigues 绕 moonDir 旋转一小角度
        float rotSpeed = deltaTime * m_crescentRotSpeed;
        float cosA = cosf(rotSpeed), sinA = sinf(rotSpeed);
        XMFLOAT3 k = m_moonDir, v = m_crescentDir;
        // k×v（v 已垂直于 k，dot(k,v)≈0，公式简化为 v*cos + (k×v)*sin）
        XMFLOAT3 crossKV = {
            k.y * v.z - k.z * v.y,
            k.z * v.x - k.x * v.z,
            k.x * v.y - k.y * v.x
        };
        m_crescentDir = {
            v.x * cosA + crossKV.x * sinA,
            v.y * cosA + crossKV.y * sinA,
            v.z * cosA + crossKV.z * sinA
        };
    }

    // 闪电
    m_lightningCooldown -= deltaTime;
    if (m_weatherIntensity > 0.7f && m_lightningCooldown <= 0.0f && m_lightningIntensity <= 0.0f)
    {
        float r = fabsf(sinf(m_time * 127.3f)); // 伪随机 0..1
        m_lightningIntensity = 0.4f + r * 0.6f;
        m_lightningCooldown  = 2.0f + r * 6.0f; // 下次触发间隔 2~8 秒
    }
    if (m_lightningIntensity > 0.0f)
    {
        m_lightningIntensity -= deltaTime * 5.0f; // 约 0.2s 衰减到 0
        if (m_lightningIntensity < 0.0f) m_lightningIntensity = 0.0f;
    }

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
  
    float skyScale = m_showcaseMode ? 400.0f : 1000.0f;
    XMMATRIX scale = XMMatrixScaling(skyScale, skyScale, skyScale);
    // 注意：天空球要去掉平移分量，只保留旋转
    // 从view矩阵提取旋转部分（清除第四列的平移）
    XMMATRIX viewForSky = ctx.view;
    if (!m_showcaseMode) // 只有普通模式才去掉平移
    {
        viewForSky.r[3] = XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f);
    }

    

    XMMATRIX viewProj = scale * viewForSky * ctx.proj;

    // 更新CBV
    SkyCB cb;
    cb.viewProj = XMMatrixTranspose(viewProj);
    // Dynamic sky gradient: night → sunset → day
    {
        float sunH = m_sunDir.y;

        // dayT: 0=night, 1=full day
        float dayT = std::clamp((sunH + 0.15f) / 0.35f, 0.0f, 1.0f);
        dayT = dayT * dayT * (3.0f - 2.0f * dayT); // smoothstep

        // sunsetT: 1 at horizon, 0 when sun is high or deep below
        float sunsetT = std::clamp(1.0f - fabsf(sunH) / 0.22f, 0.0f, 1.0f);
        sunsetT = sunsetT * sunsetT;

        // Night / Sunset / Day palettes
        float topN[3] = { 0.01f, 0.01f, 0.06f };
        float midN[3] = { 0.02f, 0.02f, 0.09f };
        float botN[3] = { 0.03f, 0.03f, 0.12f };

        float topS[3] = { 0.10f, 0.16f, 0.48f };   // blue-purple zenith
        float midS[3] = { 0.95f, 0.40f, 0.08f };   // rich orange
        float botS[3] = { 1.60f, 0.72f, 0.12f };   // HDR gold horizon (triggers bloom)

        float topD[3] = { 0.08f, 0.25f, 0.72f };
        float midD[3] = { 0.38f, 0.62f, 1.05f };
        float botD[3] = { 0.62f, 0.80f, 1.05f };

        float top[3], mid[3], bot[3];
        for (int i = 0; i < 3; i++)
        {
            float baseTop = topN[i] + (topD[i] - topN[i]) * dayT;
            float baseMid = midN[i] + (midD[i] - midN[i]) * dayT;
            float baseBot = botN[i] + (botD[i] - botN[i]) * dayT;

            top[i] = baseTop + (topS[i] - baseTop) * sunsetT * 0.65f;
            mid[i] = baseMid + (midS[i] - baseMid) * sunsetT;
            bot[i] = baseBot + (botS[i] - baseBot) * sunsetT;
        }

        cb.topColor    = XMFLOAT4(top[0], top[1], top[2], 1.0f);
        cb.middleColor = XMFLOAT4(mid[0], mid[1], mid[2], 1.0f);
        cb.bottomColor = XMFLOAT4(bot[0], bot[1], bot[2], 1.0f);
    }
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
    cb.moonCrescentDir   = m_crescentDir;
    cb.padCrescent       = 0.0f;
    cb.moonBodyPow        = m_moonBodyPow;
    cb.moonOccludePow     = m_moonOccludePow;
    cb.crescentOffsetAmt  = m_crescentOffsetAmt;
    cb.padMoonParams      = 0.0f;
    cb.lightningIntensity = m_lightningIntensity;
    cb.padLightning[0] = cb.padLightning[1] = cb.padLightning[2] = 0.0f;
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

XMFLOAT3 SkyDome::GetSunColor() const
{
    float h = m_sunDir.y; // -1到1

    // 日落偏橙红，正午偏白
    float t = saturate(h);
    XMFLOAT3 sunsetColor = { 1.0f, 0.4f, 0.1f }; // 日落橙红
    XMFLOAT3 noonColor = { 1.0f, 0.95f, 0.8f }; // 正午暖白

    XMFLOAT3 baseColor = XMFLOAT3(
        sunsetColor.x + (noonColor.x - sunsetColor.x) * t,
        sunsetColor.y + (noonColor.y - sunsetColor.y) * t,
        sunsetColor.z + (noonColor.z - sunsetColor.z) * t);

    // 暴风时颜色变灰
    XMFLOAT3 stormColor = { 0.6f, 0.6f, 0.65f };
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