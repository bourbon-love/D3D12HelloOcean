// ============================================================
// FloatingObject.h
// Wooden crates floating on the ocean surface.
// Supports multiple instances; new boxes can be spawned mid-air
// and fall under gravity until they reach the wave surface.
// ============================================================
#pragma once
#include <d3d12.h>
#include <wrl.h>
#include <DirectXMath.h>
#include <vector>
#include "../DXSampleHelper.h"
#include "renderer/RendererContext.h"

using Microsoft::WRL::ComPtr;
using namespace DirectX;

class FloatingObject
{
public:
    void Init(
        ComPtr<ID3D12Device> device,
        ID3D12Resource*      heightMap,
        const UINT8* vsData, UINT vsLen,
        const UINT8* psData, UINT psLen);

    void InitBuffers(ComPtr<ID3D12GraphicsCommandList> cmdList);

    void Update(float dt);
    void SpawnBox();
    void ClearBoxes() { m_boxes.clear(); }
    int  GetBoxCount() const { return (int)m_boxes.size(); }

    void Render(
        RenderContext& ctx,
        XMFLOAT3 sunDir, float sunIntensity, XMFLOAT3 sunColor,
        XMFLOAT3 cameraPos);

    // Shadow depth pass
    void InitShadowResources(ID3D12Device* device);
    void RenderDepth(ID3D12GraphicsCommandList* cmd,
                     ID3D12RootSignature* rootSig,
                     ID3D12PipelineState* pso,
                     const XMMATRIX& lightViewProj);

    float scale = 1.8f;

    static constexpr int MAX_BOXES = 20;

private:
    struct Vertex { XMFLOAT3 pos; XMFLOAT3 normal; };

    struct BoxInstance
    {
        XMFLOAT2 position;   // world XZ
        float    dropOffset; // Y above wave surface (0 = landed)
    };

    struct alignas(256) ObjectCB
    {
        XMMATRIX viewProj;
        XMFLOAT3 worldPos;    float objectScale;
        XMFLOAT3 sunDir;      float sunIntensity;
        XMFLOAT3 sunColor;    float gridWorldSize;
        XMFLOAT3 cameraPos;   float dropOffset;
    };

    static constexpr float HW = 1.0f;
    static constexpr float HH = 0.7f;
    static constexpr float HD = 1.5f;
    static constexpr float FALL_SPEED    = 22.0f;
    static constexpr float SPAWN_HEIGHT  = 45.0f;
    static constexpr float SPAWN_RADIUS  = 120.0f;

    std::vector<BoxInstance>     m_boxes;
    ComPtr<ID3D12Device>         m_device;
    ComPtr<ID3D12Resource>       m_vb, m_vbUpload;
    ComPtr<ID3D12Resource>       m_ib, m_ibUpload;
    ComPtr<ID3D12Resource>       m_cb;   // MAX_BOXES * sizeof(ObjectCB)
    D3D12_VERTEX_BUFFER_VIEW     m_vbView  = {};
    D3D12_INDEX_BUFFER_VIEW      m_ibView  = {};
    UINT                         m_indexCount = 0;

    ComPtr<ID3D12DescriptorHeap> m_srvHeap;
    ComPtr<ID3D12RootSignature>  m_rootSig;
    ComPtr<ID3D12PipelineState>  m_pso;

    ObjectCB* m_mappedCBs = nullptr;

    // Shadow depth pass resources
    struct alignas(256) ShadowInstCB
    {
        XMMATRIX lightViewProj;  // 64
        XMFLOAT3 worldPos;       // 12
        float    objectScale;    //  4
        float    dropOffset;     //  4
        float    pad[43];        // 172  → total 256
    };
    static_assert(sizeof(ShadowInstCB) == 256);

    ComPtr<ID3D12Resource> m_shadowInstCB;
    UINT8*                 m_mappedShadowCB = nullptr;
};
