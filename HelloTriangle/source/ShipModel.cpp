// ============================================================
// ShipModel.cpp
// ============================================================
#define _CRT_SECURE_NO_WARNINGS
#define CGLTF_IMPLEMENTATION
#include "cgltf.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "ShipModel.h"
#include <d3dx12_barriers.h>
#include <d3dx12_core.h>
#include <d3dx12_root_signature.h>
#include <stdexcept>
#include <vector>
#include <cstring>

void ShipModel::Init(
    ComPtr<ID3D12Device>  device,
    ID3D12Resource*       heightMap,
    const std::string&    gltfDir,
    const UINT8* vsData,       UINT vsLen,
    const UINT8* psData,       UINT psLen,
    const UINT8* shadowVsData, UINT shadowVsLen)
{
    m_device    = device;
    m_heightMap = heightMap;
    m_gltfDir   = gltfDir;
    m_srvIncr   = device->GetDescriptorHandleIncrementSize(
                      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // Constant buffer for main pass
    {
        auto hp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        auto bd = CD3DX12_RESOURCE_DESC::Buffer(sizeof(ShipCB));
        ThrowIfFailed(device->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&m_cb)));
        CD3DX12_RANGE r(0, 0);
        m_cb->Map(0, &r, reinterpret_cast<void**>(&m_mappedCB));
    }

    // Constant buffer for shadow pass
    {
        auto hp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        auto bd = CD3DX12_RESOURCE_DESC::Buffer(sizeof(ShadowCB));
        ThrowIfFailed(device->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&m_shadowCB)));
        CD3DX12_RANGE r(0, 0);
        m_shadowCB->Map(0, &r, reinterpret_cast<void**>(&m_mappedShadowCB));
    }

    // Main root signature: [0]=CBV(b0), [1]=Table(4 SRVs: t0..t3), sampler s0
    {
        CD3DX12_ROOT_PARAMETER params[2];
        params[0].InitAsConstantBufferView(0, 0, D3D12_SHADER_VISIBILITY_ALL);
        CD3DX12_DESCRIPTOR_RANGE srvRange;
        srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4, 0);
        params[1].InitAsDescriptorTable(1, &srvRange, D3D12_SHADER_VISIBILITY_ALL);
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
        ThrowIfFailed(device->CreateRootSignature(0,
            sig->GetBufferPointer(), sig->GetBufferSize(),
            IID_PPV_ARGS(&m_rootSig)));
    }

    // Main PSO
    {
        D3D12_INPUT_ELEMENT_DESC layout[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0,
              D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12,
              D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24,
              D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        };
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature        = m_rootSig.Get();
        pd.VS                    = CD3DX12_SHADER_BYTECODE(vsData, vsLen);
        pd.PS                    = CD3DX12_SHADER_BYTECODE(psData, psLen);
        pd.InputLayout           = { layout, _countof(layout) };
        pd.RasterizerState       = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        pd.RasterizerState.CullMode               = D3D12_CULL_MODE_BACK;
        pd.RasterizerState.FrontCounterClockwise  = TRUE; // GLTF uses CCW front faces
        pd.BlendState            = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        pd.DepthStencilState     = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        pd.DSVFormat             = DXGI_FORMAT_D32_FLOAT;
        pd.SampleMask            = UINT_MAX;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets      = 1;
        pd.RTVFormats[0]         = DXGI_FORMAT_R16G16B16A16_FLOAT;
        pd.SampleDesc.Count      = 1;
        ThrowIfFailed(device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_pso)));

        // Double-sided PSO for rigging/sails (thin geometry, visible from both sides)
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        ThrowIfFailed(device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_psoDoubleSided)));
    }

    // Shadow root signature: [0]=CBV(b0) only
    {
        CD3DX12_ROOT_PARAMETER params[1];
        params[0].InitAsConstantBufferView(0, 0, D3D12_SHADER_VISIBILITY_VERTEX);
        CD3DX12_ROOT_SIGNATURE_DESC rsd;
        rsd.Init(1, params, 0, nullptr,
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
        ComPtr<ID3DBlob> sig, err;
        ThrowIfFailed(D3D12SerializeRootSignature(&rsd,
            D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err));
        ThrowIfFailed(device->CreateRootSignature(0,
            sig->GetBufferPointer(), sig->GetBufferSize(),
            IID_PPV_ARGS(&m_shadowRootSig)));
    }

    // Shadow PSO (depth-only, no PS)
    {
        D3D12_INPUT_ELEMENT_DESC layout[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0,
              D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12,
              D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24,
              D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        };
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature        = m_shadowRootSig.Get();
        pd.VS                    = CD3DX12_SHADER_BYTECODE(shadowVsData, shadowVsLen);
        pd.InputLayout           = { layout, _countof(layout) };
        pd.RasterizerState       = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        pd.RasterizerState.CullMode              = D3D12_CULL_MODE_BACK;
        pd.RasterizerState.FrontCounterClockwise = TRUE;
        pd.RasterizerState.DepthBias             = 5000;
        pd.RasterizerState.SlopeScaledDepthBias  = 2.0f;
        pd.BlendState            = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        pd.DepthStencilState     = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        pd.DSVFormat             = DXGI_FORMAT_D32_FLOAT;
        pd.SampleMask            = UINT_MAX;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets      = 0;
        pd.SampleDesc.Count      = 1;
        ThrowIfFailed(device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_shadowPSO)));
    }
}

void ShipModel::UploadBuffer(
    ComPtr<ID3D12GraphicsCommandList> cmd,
    ComPtr<ID3D12Resource>& dest, ComPtr<ID3D12Resource>& upload,
    const void* data, UINT64 byteSize,
    D3D12_RESOURCE_STATES finalState)
{
    auto hp  = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    auto hp2 = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    auto bd  = CD3DX12_RESOURCE_DESC::Buffer(byteSize);

    ThrowIfFailed(m_device->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &bd,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&dest)));
    ThrowIfFailed(m_device->CreateCommittedResource(
        &hp2, D3D12_HEAP_FLAG_NONE, &bd,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)));

    void* pData = nullptr;
    CD3DX12_RANGE r(0, 0);
    upload->Map(0, &r, &pData);
    memcpy(pData, data, (size_t)byteSize);
    upload->Unmap(0, nullptr);

    cmd->CopyBufferRegion(dest.Get(), 0, upload.Get(), 0, byteSize);
    auto bar = CD3DX12_RESOURCE_BARRIER::Transition(
        dest.Get(), D3D12_RESOURCE_STATE_COPY_DEST, finalState);
    cmd->ResourceBarrier(1, &bar);
}

void ShipModel::LoadGLTF(const std::string& gltfDir)
{
    std::string dirPath = gltfDir;
    while (!dirPath.empty() &&
           (dirPath.back() == '\\' || dirPath.back() == '/'))
        dirPath.pop_back();

    size_t lastSep = dirPath.find_last_of("/\\");
    std::string basename = (lastSep != std::string::npos)
        ? dirPath.substr(lastSep + 1) : dirPath;
    // basename may already contain ".gltf" (Poly Haven uses folder-name == file-name)
    if (basename.size() >= 5 && basename.compare(basename.size() - 5, 5, ".gltf") == 0)
        basename.resize(basename.size() - 5);
    std::string gltfPath = dirPath + "\\" + basename + ".gltf";

    std::string baseDir = gltfDir;
    if (!baseDir.empty() && baseDir.back() != '\\' && baseDir.back() != '/')
        baseDir += '\\';

    cgltf_options opts = {};
    cgltf_data*   data = nullptr;
    if (cgltf_parse_file(&opts, gltfPath.c_str(), &data) != cgltf_result_success)
        throw std::runtime_error("cgltf_parse_file failed: " + gltfPath);
    if (cgltf_load_buffers(&opts, data, gltfPath.c_str()) != cgltf_result_success)
        throw std::runtime_error("cgltf_load_buffers failed");

    const char* kGroupNames[3] = { "hull", "rigging", "sails" };
    std::vector<Vertex>   verts[3];
    std::vector<uint32_t> indices[3];

    for (size_t mi = 0; mi < data->meshes_count; ++mi)
    {
        cgltf_mesh& mesh = data->meshes[mi];
        for (size_t pi = 0; pi < mesh.primitives_count; ++pi)
        {
            cgltf_primitive& prim = mesh.primitives[pi];
            if (prim.type != cgltf_primitive_type_triangles) continue;

            int grp = -1;
            if (prim.material && prim.material->name)
                for (int g = 0; g < 3; ++g)
                    if (strstr(prim.material->name, kGroupNames[g])) { grp = g; break; }
            if (grp < 0) grp = 0;

            cgltf_accessor* posAcc  = nullptr;
            cgltf_accessor* normAcc = nullptr;
            cgltf_accessor* uvAcc   = nullptr;
            for (size_t ai = 0; ai < prim.attributes_count; ++ai)
            {
                cgltf_attribute& attr = prim.attributes[ai];
                if (attr.type == cgltf_attribute_type_position)  posAcc  = attr.data;
                if (attr.type == cgltf_attribute_type_normal)    normAcc = attr.data;
                if (attr.type == cgltf_attribute_type_texcoord && attr.index == 0)
                    uvAcc = attr.data;
            }
            if (!posAcc) continue;

            UINT baseVtx  = (UINT)verts[grp].size();
            UINT vtxCount = (UINT)posAcc->count;
            verts[grp].resize(baseVtx + vtxCount);

            for (UINT vi = 0; vi < vtxCount; ++vi)
            {
                Vertex& v  = verts[grp][baseVtx + vi];
                float tmp[4] = {};
                cgltf_accessor_read_float(posAcc, vi, tmp, 3);
                v.pos = { tmp[0], tmp[1], -tmp[2] };
                if (normAcc)
                {
                    cgltf_accessor_read_float(normAcc, vi, tmp, 3);
                    v.normal = { tmp[0], tmp[1], -tmp[2] };
                }
                else v.normal = { 0.f, 1.f, 0.f };
                if (uvAcc)
                {
                    cgltf_accessor_read_float(uvAcc, vi, tmp, 2);
                    v.uv = { tmp[0], 1.0f - tmp[1] };
                }
                else v.uv = { 0.f, 0.f };
            }

            if (prim.indices)
            {
                UINT idxCount = (UINT)prim.indices->count;
                for (UINT ii = 0; ii < idxCount; ++ii)
                    indices[grp].push_back(
                        (uint32_t)cgltf_accessor_read_index(prim.indices, ii) + baseVtx);
            }
            else
                for (UINT ii = 0; ii < vtxCount; ++ii)
                    indices[grp].push_back(baseVtx + ii);
        }
    }

    for (int g = 0; g < 3; ++g)
    {
        m_groups[g].indexCount = (UINT)indices[g].size();
        m_cpuVerts[g]   = std::move(verts[g]);
        m_cpuIndices[g] = std::move(indices[g]);
    }

    // Extract diffuse texture paths from materials
    for (size_t mi = 0; mi < data->materials_count; ++mi)
    {
        cgltf_material& mat = data->materials[mi];
        if (!mat.name) continue;
        int grp = -1;
        for (int g = 0; g < 3; ++g)
            if (strstr(mat.name, kGroupNames[g])) { grp = g; break; }
        if (grp < 0) continue;

        if (mat.has_pbr_metallic_roughness &&
            mat.pbr_metallic_roughness.base_color_texture.texture &&
            mat.pbr_metallic_roughness.base_color_texture.texture->image)
        {
            const char* uri = mat.pbr_metallic_roughness
                .base_color_texture.texture->image->uri;
            if (uri)
            {
                m_texPaths[grp] = baseDir + uri;
                auto subst = [](const std::string& s,
                                const std::string& from,
                                const std::string& to) -> std::string {
                    size_t pos = s.find(from);
                    if (pos == std::string::npos) return s;
                    return s.substr(0, pos) + to + s.substr(pos + from.size());
                };
                m_normalPaths[grp] = subst(m_texPaths[grp], "_diff_", "_nor_gl_");
                m_armPaths[grp]    = subst(m_texPaths[grp], "_diff_", "_arm_");
            }
        }
    }

    cgltf_free(data);
}

void ShipModel::LoadTexture(
    ComPtr<ID3D12Resource>& tex, ComPtr<ID3D12Resource>& upload,
    const std::string& jpgPath,
    ComPtr<ID3D12GraphicsCommandList> uploadCmd)
{
    int w, h, comp;
    stbi_uc* pixels = stbi_load(jpgPath.c_str(), &w, &h, &comp, 4);
    if (!pixels)
        throw std::runtime_error("stbi_load failed: " + jpgPath);

    UINT64 rowPitch = (UINT64)w * 4;

    {
        auto hp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        auto td = CD3DX12_RESOURCE_DESC::Tex2D(
            DXGI_FORMAT_R8G8B8A8_UNORM, (UINT64)w, (UINT)h, 1, 1);
        ThrowIfFailed(m_device->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &td,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&tex)));
    }

    UINT64 alignedRowPitch = (rowPitch + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1)
                           & ~(UINT64)(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
    UINT64 uploadSize = alignedRowPitch * h;

    {
        auto hp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        auto bd = CD3DX12_RESOURCE_DESC::Buffer(uploadSize);
        ThrowIfFailed(m_device->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&upload)));
    }

    {
        UINT8* pDst = nullptr;
        CD3DX12_RANGE r(0, 0);
        upload->Map(0, &r, reinterpret_cast<void**>(&pDst));
        for (int row = 0; row < h; ++row)
            memcpy(pDst + row * alignedRowPitch,
                   pixels + row * rowPitch, (size_t)rowPitch);
        upload->Unmap(0, nullptr);
    }
    stbi_image_free(pixels);

    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource        = tex.Get();
    dst.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource                          = upload.Get();
    src.Type                               = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Offset             = 0;
    src.PlacedFootprint.Footprint.Format   = DXGI_FORMAT_R8G8B8A8_UNORM;
    src.PlacedFootprint.Footprint.Width    = (UINT)w;
    src.PlacedFootprint.Footprint.Height   = (UINT)h;
    src.PlacedFootprint.Footprint.Depth    = 1;
    src.PlacedFootprint.Footprint.RowPitch = (UINT)alignedRowPitch;

    uploadCmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    auto bar = CD3DX12_RESOURCE_BARRIER::Transition(
        tex.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    uploadCmd->ResourceBarrier(1, &bar);
}

void ShipModel::InitBuffers(ComPtr<ID3D12GraphicsCommandList> cmdList)
{
    LoadGLTF(m_gltfDir);

    for (int g = 0; g < 3; ++g)
    {
        auto& grp = m_groups[g];
        if (grp.indexCount == 0) continue;

        UINT64 vbSize = m_cpuVerts[g].size()   * sizeof(Vertex);
        UINT64 ibSize = m_cpuIndices[g].size()  * sizeof(uint32_t);

        UploadBuffer(cmdList, grp.vb, grp.vbUpload,
                     m_cpuVerts[g].data(), vbSize,
                     D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
        grp.vbView = { grp.vb->GetGPUVirtualAddress(),
                       (UINT)vbSize, sizeof(Vertex) };

        UploadBuffer(cmdList, grp.ib, grp.ibUpload,
                     m_cpuIndices[g].data(), ibSize,
                     D3D12_RESOURCE_STATE_INDEX_BUFFER);
        grp.ibView = { grp.ib->GetGPUVirtualAddress(),
                       (UINT)ibSize, DXGI_FORMAT_R32_UINT };

        if (!m_texPaths[g].empty())
            LoadTexture(grp, m_texPaths[g], cmdList);

        // SRV heap: [0]=diffuse, [1]=heightmap
        {
            D3D12_DESCRIPTOR_HEAP_DESC hd = {};
            hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            hd.NumDescriptors = 2;
            hd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            ThrowIfFailed(m_device->CreateDescriptorHeap(&hd,
                IID_PPV_ARGS(&grp.srvHeap)));
        }

        CD3DX12_CPU_DESCRIPTOR_HANDLE handle(
            grp.srvHeap->GetCPUDescriptorHandleForHeapStart());

        // Slot 0: diffuse texture
        if (grp.diffuseTex)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
            sd.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
            sd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.Texture2D.MipLevels     = 1;
            m_device->CreateShaderResourceView(grp.diffuseTex.Get(), &sd, handle);
        }
        handle.Offset(1, m_srvIncr);

        // Slot 1: heightmap
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
            sd.Format                  = DXGI_FORMAT_R32G32B32A32_FLOAT;
            sd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.Texture2D.MipLevels     = 1;
            m_device->CreateShaderResourceView(m_heightMap, &sd, handle);
        }

        m_cpuVerts[g].clear();
        m_cpuVerts[g].shrink_to_fit();
        m_cpuIndices[g].clear();
        m_cpuIndices[g].shrink_to_fit();
    }
}

void ShipModel::Render(
    RenderContext& ctx,
    XMFLOAT3 sunDir, float sunIntensity,
    XMFLOAT3 sunColor, XMFLOAT3 cameraPos)
{
    XMMATRIX vp = XMMatrixTranspose(ctx.view * ctx.proj);

    ShipCB& cb      = *m_mappedCB;
    cb.viewProj     = vp;
    cb.worldPos     = worldPos;
    cb.scale        = scale;
    cb.sunDir       = sunDir;
    cb.sunIntensity = sunIntensity;
    cb.sunColor     = sunColor;
    cb.yaw          = yaw;
    cb.cameraPos    = cameraPos;

    auto* cmd = ctx.cmd;
    cmd->SetGraphicsRootSignature(m_rootSig.Get());
    cmd->SetPipelineState(m_pso.Get());
    cmd->SetGraphicsRootConstantBufferView(0, m_cb->GetGPUVirtualAddress());
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->OMSetRenderTargets(1, &ctx.rtv, FALSE, &ctx.dsv);
    cmd->RSSetViewports(1, &ctx.viewport);
    cmd->RSSetScissorRects(1, &ctx.scissor);

    for (int g = 0; g < 3; ++g)
    {
        auto& grp = m_groups[g];
        if (grp.indexCount == 0) continue;

        // group 0 = hull (solid, cull back), groups 1/2 = rigging/sails (double-sided)
        cmd->SetPipelineState(g == 0 ? m_pso.Get() : m_psoDoubleSided.Get());

        ID3D12DescriptorHeap* heaps[] = { grp.srvHeap.Get() };
        cmd->SetDescriptorHeaps(1, heaps);
        cmd->SetGraphicsRootDescriptorTable(1,
            grp.srvHeap->GetGPUDescriptorHandleForHeapStart());
        cmd->IASetVertexBuffers(0, 1, &grp.vbView);
        cmd->IASetIndexBuffer(&grp.ibView);
        cmd->DrawIndexedInstanced(grp.indexCount, 1, 0, 0, 0);
    }
}

void ShipModel::RenderDepth(
    ID3D12GraphicsCommandList* cmd,
    const XMMATRIX&            lightViewProj)
{
    ShadowCB& cb      = *m_mappedShadowCB;
    cb.lightViewProj  = XMMatrixTranspose(lightViewProj);
    cb.worldPos       = worldPos;
    cb.scale          = scale;
    cb.yaw            = yaw;

    cmd->SetGraphicsRootSignature(m_shadowRootSig.Get());
    cmd->SetPipelineState(m_shadowPSO.Get());
    cmd->SetGraphicsRootConstantBufferView(0, m_shadowCB->GetGPUVirtualAddress());
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    for (int g = 0; g < 3; ++g)
    {
        auto& grp = m_groups[g];
        if (grp.indexCount == 0) continue;
        cmd->IASetVertexBuffers(0, 1, &grp.vbView);
        cmd->IASetIndexBuffer(&grp.ibView);
        cmd->DrawIndexedInstanced(grp.indexCount, 1, 0, 0, 0);
    }
}
