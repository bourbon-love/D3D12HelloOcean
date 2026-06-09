#include "SkyDome.h"
#include <d3dx12_barriers.h>
#include <cmath>
#include <algorithm>

// Sphere vertex. Position only. Same as the original SKY_VERTEX.
struct SkyVertex { XMFLOAT3 position; };

// Reuse the same logic as the original CreateSphereVertices.
static void BuildSphereMesh(
    float radius, int slices, int stacks,
    std::vector<SkyVertex>& outVerts,
    std::vector<uint32_t>& outIdx)
{
    // Vertex generation: exactly slices vertices per ring — no duplicate seam vertex.
    // The old (slices+1) layout had slice=0 and slice=slices as separate vertex objects at
    // nearly the same position (sinf(2π) ≈ -8.7e-8, not exactly 0), so the rasterizer
    // treated the shared seam edge as two distinct edges and drew a 1-pixel visible line.
    for (int stack = 0; stack <= stacks; ++stack)
    {
        float phi = XM_PI * stack / stacks;
        float y = radius * cosf(phi);
        float r = radius * sinf(phi);

        for (int slice = 0; slice < slices; ++slice)   // < slices, no duplicate
        {
            float theta = 2.0f * XM_PI * slice / slices;
            outVerts.push_back({ XMFLOAT3(r * sinf(theta), y, r * cosf(theta)) });
        }
    }

    // Index generation: wrap the last slice back to slice=0 so the seam triangles
    // share the exact same vertex objects — zero gap, zero overlap.
    for (int stack = 0; stack < stacks; ++stack)
    {
        for (int slice = 0; slice < slices; ++slice)
        {
            int nextSlice = (slice + 1) % slices;           // wraps 49→0 at the seam
            uint32_t v1 = stack       * slices + slice;
            uint32_t v2 = stack       * slices + nextSlice;
            uint32_t v3 = (stack + 1) * slices + slice;
            uint32_t v4 = (stack + 1) * slices + nextSlice;

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

    // CBV — upload heap, updated every frame
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
    // Sky dome uses Position only. Exactly matches SkyVertex.
    D3D12_INPUT_ELEMENT_DESC layout[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    // RasterizerState: cull front faces (camera is inside the sphere, looking at inner faces)
    CD3DX12_RASTERIZER_DESC rasterDesc(D3D12_DEFAULT);
    rasterDesc.CullMode = D3D12_CULL_MODE_FRONT;

    // DepthStencilState: enable depth test but disable depth writes.
    // The sky is at the farthest position and must not occlude the ocean.
    CD3DX12_DEPTH_STENCIL_DESC depthDesc(D3D12_DEFAULT);
    depthDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO; // no depth write
    depthDesc.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL; // use LESS_EQUAL

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

    // 50x50, same size as the original
    BuildSphereMesh(1.0f, 50, 50, verts, indices);
    m_indexCount = static_cast<UINT>(indices.size());

    UINT vbSize = static_cast<UINT>(verts.size() * sizeof(SkyVertex));
    UINT ibSize = static_cast<UINT>(indices.size() * sizeof(uint32_t));

    auto uploadHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    void* pData = nullptr;
    CD3DX12_RANGE readRange(0, 0);

    // VB — upload heap (sphere is static but data is small, so upload heap is fine)
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

    // Index buffer
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

    // Upload heap does not require CopyBufferRegion. cmdList parameter is unused.
    (void)cmdList;
}

void SkyDome::Update(float deltaTime)
{
    m_time += deltaTime * 0.5f;

    float tilt = 0.5f;

    // Sun orbit
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

    // Moon is on the opposite side from the sun. Smoothly crosses the horizon at sunrise/sunset (avoids hard jumps).
    {
        // sunBlend: 0 when sun is fully below horizon, 1 when fully risen. Smooth transition over [-0.1, +0.1].
        float sunBlend = std::clamp((m_sunDir.y + 0.1f) / 0.2f, 0.0f, 1.0f);
        sunBlend = sunBlend * sunBlend * (3.0f - 2.0f * sunBlend); // smoothstep
        // When sun sets: +0.1 (moon slightly above horizon); when sun rises: -0.1 (moon slightly below).
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
    // Crescent direction: slowly rotates around the moon axis. Independent from the sun, completes one revolution in ~90 seconds.
    {
        // Project m_crescentDir onto the plane perpendicular to moonDir to prevent numerical drift.
        float dotCM = m_crescentDir.x * m_moonDir.x + m_crescentDir.y * m_moonDir.y + m_crescentDir.z * m_moonDir.z;
        m_crescentDir.x -= dotCM * m_moonDir.x;
        m_crescentDir.y -= dotCM * m_moonDir.y;
        m_crescentDir.z -= dotCM * m_moonDir.z;
        float clen = sqrtf(m_crescentDir.x * m_crescentDir.x + m_crescentDir.y * m_crescentDir.y + m_crescentDir.z * m_crescentDir.z);
        if (clen > 0.001f) { m_crescentDir.x /= clen; m_crescentDir.y /= clen; m_crescentDir.z /= clen; }

        // Rotate by a small angle around moonDir using Rodrigues' formula.
        float rotSpeed = deltaTime * m_crescentRotSpeed;
        float cosA = cosf(rotSpeed), sinA = sinf(rotSpeed);
        XMFLOAT3 k = m_moonDir, v = m_crescentDir;
        // k×v (v is already perpendicular to k; since dot(k,v)≈0 the formula simplifies to v*cos + (k×v)*sin)
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

    // Lightning
    m_lightningCooldown -= deltaTime;
    if (m_weatherIntensity > 0.7f && m_lightningCooldown <= 0.0f && m_lightningIntensity <= 0.0f)
    {
        float r = fabsf(sinf(m_time * 127.3f)); // pseudo-random 0..1
        m_lightningIntensity = 0.4f + r * 0.6f;
        m_lightningCooldown  = 2.0f + r * 6.0f; // next strike interval: 2~8 seconds
    }
    if (m_lightningIntensity > 0.0f)
    {
        m_lightningIntensity -= deltaTime * 5.0f; // decays to 0 in ~0.2 seconds
        if (m_lightningIntensity < 0.0f) m_lightningIntensity = 0.0f;
    }

    // Cloud parameters
    float cycle1 = sinf(m_time * 0.2f) * 0.5f + 0.5f;
    float cycle2 = cosf(m_time * 0.15f) * 0.5f + 0.5f;
    m_cloudDensity = 0.5f + cycle1 * 0.1f;
    m_cloudScale = 0.85f + cycle2 * 0.15f;
    m_cloudSharpness = 0.6f + sinf(m_time * 0.1f) * 0.1f;
}

void SkyDome::Render(RenderContext& ctx)
{
    // Obtain view/proj from ctx. The sky sphere follows the camera (rotation only, no translation).
    // The view and proj matrices must be passed when constructing ctx.

    float skyScale = m_showcaseMode ? 400.0f : 1000.0f;
    XMMATRIX scale = XMMatrixScaling(skyScale, skyScale, skyScale);
    // Note: remove the translation component from the view matrix for the sky sphere, keeping rotation only.
    // Extract the rotation part from the view matrix (clear the translation in row[3]).
    XMMATRIX viewForSky = ctx.view;
    if (!m_showcaseMode) // remove translation only in normal mode
    {
        viewForSky.r[3] = XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f);
    }

    

    XMMATRIX viewProj = scale * viewForSky * ctx.proj;

    // Update CBV
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
    cb.cameraY      = m_cameraY;
    cb.padCam1 = cb.padCam2 = cb.padCam3 = 0.0f;
    memcpy(m_cbMapped, &cb, sizeof(cb));

    // Switch to sky PSO
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


// Calculate intensity based on sun elevation: intensity falls to 0 below the horizon.
float SkyDome::GetSunIntensity() const
{
    // m_sunDir.y is the vertical component of the sun direction.
    // Noon: y≈1 (brightest), sunset: y≈0 (horizon), night: y<0 (off).
    // Adding 0.1 provides a slight afterglow after sunset.
    float baseIntensity = saturate(m_sunDir.y + 0.1f);
    // During storms sun intensity drops to 20%.
    return baseIntensity * (1.0f - m_weatherIntensity * 0.8f);
}

XMFLOAT3 SkyDome::GetSunColor() const
{
    float h = m_sunDir.y; // -1 to 1

    // Sunset leans toward orange-red, noon leans toward white.
    float t = saturate(h);
    XMFLOAT3 sunsetColor = { 1.0f, 0.4f, 0.1f }; // sunset orange-red
    XMFLOAT3 noonColor = { 1.0f, 0.95f, 0.8f }; // warm white at noon

    XMFLOAT3 baseColor = XMFLOAT3(
        sunsetColor.x + (noonColor.x - sunsetColor.x) * t,
        sunsetColor.y + (noonColor.y - sunsetColor.y) * t,
        sunsetColor.z + (noonColor.z - sunsetColor.z) * t);

    // During storms the color shifts toward gray.
    XMFLOAT3 stormColor = { 0.6f, 0.6f, 0.65f };
    return XMFLOAT3(
        baseColor.x + (stormColor.x - baseColor.x) * m_weatherIntensity,
        baseColor.y + (stormColor.y - baseColor.y) * m_weatherIntensity,
        baseColor.z + (stormColor.z - baseColor.z) * m_weatherIntensity);
}

// dayBlend: 0 when the sun is fully below the horizon (moon is the active light),
// 1 when fully risen (sun is the active light). Smooth transition over [-0.1, 0.1],
// matching the band SkyDome::Update() uses to fade the moon in/out (m_moonDir).
float SkyDome::ComputeDayBlend() const
{
    return std::clamp((m_sunDir.y + 0.1f) / 0.2f, 0.0f, 1.0f);
}

XMFLOAT3 SkyDome::GetActiveLightDirection() const
{
    float t = ComputeDayBlend();
    return XMFLOAT3(
        std::lerp(m_moonDir.x, m_sunDir.x, t),
        std::lerp(m_moonDir.y, m_sunDir.y, t),
        std::lerp(m_moonDir.z, m_sunDir.z, t));
}

XMFLOAT3 SkyDome::GetActiveLightColor() const
{
    float t = ComputeDayBlend();
    XMFLOAT3 sunCol  = GetSunColor();
    XMFLOAT3 moonCol = GetMoonColor();
    return XMFLOAT3(
        std::lerp(moonCol.x, sunCol.x, t),
        std::lerp(moonCol.y, sunCol.y, t),
        std::lerp(moonCol.z, sunCol.z, t));
}

float SkyDome::GetActiveLightIntensity() const
{
    return std::lerp(GetMoonIntensity(), GetSunIntensity(), ComputeDayBlend());
}

// Primary sky color: transitions from night blue to daytime blue based on sun elevation.
XMFLOAT3 SkyDome::GetSkyColor() const
{
    float h = saturate(m_sunDir.y + 0.2f);  // brightens slightly earlier

    // Deep blue of night
    XMFLOAT3 nightColor = { 0.05f, 0.05f, 0.15f };
    // Daytime sky blue
    XMFLOAT3 dayColor = { 0.4f,  0.6f,  0.9f };

    return XMFLOAT3(
        nightColor.x + (dayColor.x - nightColor.x) * h,
        nightColor.y + (dayColor.y - nightColor.y) * h,
        nightColor.z + (dayColor.z - nightColor.z) * h
    );
}

// Mirrors the gradient computation inside UpdateCB so IBLSystem always gets
// the same sky colors the rasterized dome uses.
SkyDome::CaptureState SkyDome::GetCaptureState() const
{
    float sunH    = m_sunDir.y;
    float dayT    = std::clamp((sunH + 0.15f) / 0.35f, 0.0f, 1.0f);
    dayT          = dayT * dayT * (3.0f - 2.0f * dayT);
    float sunsetT = std::clamp(1.0f - fabsf(sunH) / 0.22f, 0.0f, 1.0f);
    sunsetT       = sunsetT * sunsetT;

    float topN[3] = { 0.01f, 0.01f, 0.06f };
    float midN[3] = { 0.02f, 0.02f, 0.09f };
    float botN[3] = { 0.03f, 0.03f, 0.12f };
    float topS[3] = { 0.10f, 0.16f, 0.48f };
    float midS[3] = { 0.95f, 0.40f, 0.08f };
    float botS[3] = { 1.60f, 0.72f, 0.12f };
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

    XMFLOAT3 sc = GetSunColor();

    CaptureState s;
    s.topColor         = { top[0], top[1], top[2] };
    s.middleColor      = { mid[0], mid[1], mid[2] };
    s.bottomColor      = { bot[0], bot[1], bot[2] };
    s.sunPosition      = m_sunDir;
    s.time             = m_time;
    s.sunColor         = sc;
    s.lightningIntensity = m_lightningIntensity;
    s.moonPosition     = m_moonDir;
    s.cameraY          = m_cameraY;
    s.moonCrescentDir  = m_crescentDir;
    s.moonBodyPow      = m_moonBodyPow;
    s.moonOccludePow   = m_moonOccludePow;
    s.crescentOffsetAmt= m_crescentOffsetAmt;
    return s;
}