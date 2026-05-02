#pragma once
#include <d3d12.h>
#include <DirectXMath.h>
#include <wrl.h>
#include <vector>
#include <random>
#include <d3dx12_core.h>
#include "../DXSampleHelper.h"
#include "renderer/RendererContext.h"

using namespace DirectX;
using Microsoft::WRL::ComPtr;

class SpraySystem
{
public:
    void Init(ComPtr<ID3D12Device> device,
              const UINT8* vsData, UINT vsSize,
              const UINT8* psData, UINT psSize);
    void Update(float dt, float weatherIntensity,
                const XMFLOAT3& cameraPos,
                float windDirX, float windDirY);
    void Render(RenderContext& ctx,
                const XMMATRIX& view,
                const XMMATRIX& proj);

private:
    static const UINT MAX_PARTICLES  = 400;
    static const UINT VERTS_PER_PART = 6;   // 2 triangles per quad

    struct Particle
    {
        XMFLOAT3 pos;
        XMFLOAT3 vel;
        float    life;
        float    maxLife;
        float    size;
    };

    struct SprayVertex
    {
        XMFLOAT3 center;
        float    size;
        float    alpha;
        float    cornerU;
        float    cornerV;
    };

    struct alignas(256) SprayCB
    {
        XMMATRIX viewProj;      // 64
        XMFLOAT3 cameraRight;   // 12
        float    pad0;          //  4
        XMFLOAT3 cameraUp;      // 12
        float    pad1;          //  4
        float    pad2[40];      // 160  → total 256
    };
    static_assert(sizeof(SprayCB) == 256, "SprayCB must be 256 bytes");

    ComPtr<ID3D12Device>        m_device;
    ComPtr<ID3D12PipelineState> m_pso;
    ComPtr<ID3D12RootSignature> m_rootSig;
    ComPtr<ID3D12Resource>      m_vb;
    D3D12_VERTEX_BUFFER_VIEW    m_vbView = {};
    SprayVertex*                m_vbMapped  = nullptr;
    ComPtr<ID3D12Resource>      m_cb;
    SprayCB*                    m_cbMapped  = nullptr;

    std::vector<Particle>       m_particles;
    std::mt19937                m_rng{ 777 };
    float                       m_spawnAccum = 0.0f;
    UINT                        m_activeVerts = 0;
};
