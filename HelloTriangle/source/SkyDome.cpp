#include "SkyDome.h"
#include <d3dx12_barriers.h>
#include <cmath>
#include <algorithm>

// 球体頂点。Positionのみ。元のSKY_VERTEXと同じ
struct SkyVertex { XMFLOAT3 position; };

// 元のCreateSphereVerticesのロジックをそのまま再利用する
static void BuildSphereMesh(
    float radius, int slices, int stacks,
    std::vector<SkyVertex>& outVerts,
    std::vector<uint32_t>& outIdx)
{
    // 頂点生成：元と全く同じ公式
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

    // インデックス生成：元と全く同じ巻き順
    for (int stack = 0; stack < stacks; ++stack)
    {
        for (int slice = 0; slice < slices; ++slice)
        {
            uint32_t v1 = stack * (slices + 1) + slice;
            uint32_t v2 = stack * (slices + 1) + slice + 1;
            uint32_t v3 = (stack + 1) * (slices + 1) + slice;
            uint32_t v4 = (stack + 1) * (slices + 1) + slice + 1;

            // 元の巻き順
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

    // CBV — アップロードヒープ。毎フレーム更新する
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
    // 天空DomeはPositionのみ。SkyVertexと完全に対応している
    D3D12_INPUT_ELEMENT_DESC layout[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    // RasterizerState：前面をカリングする（カメラが球の内部にいるため、内面を見ている）
    CD3DX12_RASTERIZER_DESC rasterDesc(D3D12_DEFAULT);
    rasterDesc.CullMode = D3D12_CULL_MODE_FRONT;

    // DepthStencilState：深度テストを有効にするが深度を書き込まない
    // 空は最も遠い位置にあり、海洋を覆ってはいけない
    CD3DX12_DEPTH_STENCIL_DESC depthDesc(D3D12_DEFAULT);
    depthDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO; // 深度書き込みなし
    depthDesc.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL; // LESS_EQUALを使用

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

    // 50x50。元のサイズと同じ
    BuildSphereMesh(1.0f, 50, 50, verts, indices);
    m_indexCount = static_cast<UINT>(indices.size());

    UINT vbSize = static_cast<UINT>(verts.size() * sizeof(SkyVertex));
    UINT ibSize = static_cast<UINT>(indices.size() * sizeof(uint32_t));

    auto uploadHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    void* pData = nullptr;
    CD3DX12_RANGE readRange(0, 0);

    // VB — アップロードヒープ（球体は静的だが、データ量が小さいのでアップロードヒープでも問題ない）
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

    // インデックスバッファ
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

    // アップロードヒープではCopyBufferRegionが不要。cmdListパラメータは未使用
    (void)cmdList;
}

void SkyDome::Update(float deltaTime)
{
    m_time += deltaTime * 0.5f;

    float tilt = 0.5f;

    // 太陽の軌道
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

    // 月の独立軌道。速度がやや遅く、軌道面も若干傾いている
    float moonAngle = m_time * 0.23f; // 太陽より遅く。月相周期を生成する
    float moonTilt = 0.4f;           // 軌道傾角が若干異なる

    // 月は太陽と反対側。日の出・日の入り時に地平線を滑らかに越える（硬い跳びを避ける）
    {
        // sunBlend: 太陽が完全に地平線下で=0、完全に昇ると=1。[-0.1,+0.1]の範囲で滑らかに遷移
        float sunBlend = std::clamp((m_sunDir.y + 0.1f) / 0.2f, 0.0f, 1.0f);
        sunBlend = sunBlend * sunBlend * (3.0f - 2.0f * sunBlend); // smoothstep
        // 太陽が沈む時は+0.1（月が地平線より少し高い）、太陽が昇る時は-0.1（月が少し低い）
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
    // 月牙の向き：月軸周りに緩やかに回転する。太陽と独立しており約90秒で一周
    {
        // m_crescentDirをmoonDirに垂直な平面に投影して数値ドリフトを防ぐ
        float dotCM = m_crescentDir.x * m_moonDir.x + m_crescentDir.y * m_moonDir.y + m_crescentDir.z * m_moonDir.z;
        m_crescentDir.x -= dotCM * m_moonDir.x;
        m_crescentDir.y -= dotCM * m_moonDir.y;
        m_crescentDir.z -= dotCM * m_moonDir.z;
        float clen = sqrtf(m_crescentDir.x * m_crescentDir.x + m_crescentDir.y * m_crescentDir.y + m_crescentDir.z * m_crescentDir.z);
        if (clen > 0.001f) { m_crescentDir.x /= clen; m_crescentDir.y /= clen; m_crescentDir.z /= clen; }

        // RodriguesでmoonDir周りに小さな角度だけ回転する
        float rotSpeed = deltaTime * m_crescentRotSpeed;
        float cosA = cosf(rotSpeed), sinA = sinf(rotSpeed);
        XMFLOAT3 k = m_moonDir, v = m_crescentDir;
        // k×v（vは既にkに垂直。dot(k,v)≈0なので公式はv*cos + (k×v)*sinに簡略化）
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

    // 稲妻
    m_lightningCooldown -= deltaTime;
    if (m_weatherIntensity > 0.7f && m_lightningCooldown <= 0.0f && m_lightningIntensity <= 0.0f)
    {
        float r = fabsf(sinf(m_time * 127.3f)); // 疑似乱数 0..1
        m_lightningIntensity = 0.4f + r * 0.6f;
        m_lightningCooldown  = 2.0f + r * 6.0f; // 次の発生間隔：2〜8秒
    }
    if (m_lightningIntensity > 0.0f)
    {
        m_lightningIntensity -= deltaTime * 5.0f; // 約0.2秒で0に減衰する
        if (m_lightningIntensity < 0.0f) m_lightningIntensity = 0.0f;
    }

    // 雲パラメータ
    float cycle1 = sinf(m_time * 0.2f) * 0.5f + 0.5f;
    float cycle2 = cosf(m_time * 0.15f) * 0.5f + 0.5f;
    m_cloudDensity = 0.5f + cycle1 * 0.1f;
    m_cloudScale = 0.85f + cycle2 * 0.15f;
    m_cloudSharpness = 0.6f + sinf(m_time * 0.1f) * 0.1f;
}

void SkyDome::Render(RenderContext& ctx)
{
    // ctxからview/projを取得する。天空球はカメラに追従する（回転のみ、平行移動は除外）
    // ctxを構築する際にview行列とproj行列を渡す必要がある

    float skyScale = m_showcaseMode ? 400.0f : 1000.0f;
    XMMATRIX scale = XMMatrixScaling(skyScale, skyScale, skyScale);
    // 注意：天空球からは平行移動成分を除き、回転のみを保持する
    // ビュー行列から回転部分を抽出する（第4列の平行移動をクリアする）
    XMMATRIX viewForSky = ctx.view;
    if (!m_showcaseMode) // 通常モードのみ平行移動を除去する
    {
        viewForSky.r[3] = XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f);
    }

    

    XMMATRIX viewProj = scale * viewForSky * ctx.proj;

    // CBVを更新する
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
    // Cloud drift: wind direction * speed scaled by weather intensity
    float windStrength = 1.0f + m_weatherIntensity * 2.5f;
    cb.cloudDriftX  = m_windDirX * windStrength * 0.08f;
    cb.cloudDriftY  = m_windDirY * windStrength * 0.08f;
    cb.padLightning = 0.0f;
    memcpy(m_cbMapped, &cb, sizeof(cb));

    // 天空PSOに切り替える
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


// 太陽の高さに応じて強度を計算する：地平線以下では強度が0になる
float SkyDome::GetSunIntensity() const
{
    // m_sunDir.yは太陽の垂直成分
    // 正午はy≈1（最明）、日没はy≈0（地平線）、夜はy<0（消灯）
    // 0.1を加えることで日没後もわずかな余光を持たせる
    float baseIntensity = saturate(m_sunDir.y + 0.1f);
    // 嵐時は太陽強度が20%に低下する
    return baseIntensity * (1.0f - m_weatherIntensity * 0.8f);
}

XMFLOAT3 SkyDome::GetSunColor() const
{
    float h = m_sunDir.y; // -1〜1

    // 日没はオレンジレッド寄り、正午は白寄り
    float t = saturate(h);
    XMFLOAT3 sunsetColor = { 1.0f, 0.4f, 0.1f }; // 日没オレンジレッド
    XMFLOAT3 noonColor = { 1.0f, 0.95f, 0.8f }; // 正午の暖かい白

    XMFLOAT3 baseColor = XMFLOAT3(
        sunsetColor.x + (noonColor.x - sunsetColor.x) * t,
        sunsetColor.y + (noonColor.y - sunsetColor.y) * t,
        sunsetColor.z + (noonColor.z - sunsetColor.z) * t);

    // 嵐時は色がグレーに変わる
    XMFLOAT3 stormColor = { 0.6f, 0.6f, 0.65f };
    return XMFLOAT3(
        baseColor.x + (stormColor.x - baseColor.x) * m_weatherIntensity,
        baseColor.y + (stormColor.y - baseColor.y) * m_weatherIntensity,
        baseColor.z + (stormColor.z - baseColor.z) * m_weatherIntensity);
}

// 空の主色：太陽の高さに応じて夜の青から昼の青へ変化する
XMFLOAT3 SkyDome::GetSkyColor() const
{
    float h = saturate(m_sunDir.y + 0.2f);  // 若干早めに明るくなる

    // 夜の深い青
    XMFLOAT3 nightColor = { 0.05f, 0.05f, 0.15f };
    // 昼の空色
    XMFLOAT3 dayColor = { 0.4f,  0.6f,  0.9f };

    return XMFLOAT3(
        nightColor.x + (dayColor.x - nightColor.x) * h,
        nightColor.y + (dayColor.y - nightColor.y) * h,
        nightColor.z + (dayColor.z - nightColor.z) * h
    );
}