// FishSchool.h - CPU Boids simulation + GPU instanced rendering
#pragma once
#include <d3d12.h>
#include <wrl.h>
#include <DirectXMath.h>
#include <vector>
#include "../DXSampleHelper.h"
#include "renderer/RendererContext.h"

using Microsoft::WRL::ComPtr;
using namespace DirectX;

class FishSchool
{
public:
    static constexpr int MAX_FISH = 150;

    void Init(
        ComPtr<ID3D12Device> device,
        const UINT8* vsData, UINT vsLen,
        const UINT8* psData, UINT psLen);

    void InitBuffers(ComPtr<ID3D12GraphicsCommandList> cmdList);

    void Update(float dt, float time);

    void Render(
        RenderContext& ctx,
        XMFLOAT3 sunDir, float sunIntensity, XMFLOAT3 sunColor,
        XMFLOAT3 cameraPos, float time);

    void SpawnSchool(int n = MAX_FISH);
    void ClearFish()         { m_fish.clear(); }
    int  GetFishCount() const { return (int)m_fish.size(); }

private:
    struct Fish
    {
        XMFLOAT3 pos;
        XMFLOAT3 vel;
        float    phase;
        XMFLOAT4 color;
    };

    struct Vertex { XMFLOAT3 pos; XMFLOAT3 normal; };

    struct alignas(256) SceneCB
    {
        XMMATRIX viewProj;                       //  64
        XMFLOAT3 sunDir;     float sunIntensity; //  16
        XMFLOAT3 sunColor;   float time;         //  16
        XMFLOAT3 cameraPos;  float pad_cb;       //  16
        float    pad[36];                        // 144 → total 256
    };
    static_assert(sizeof(SceneCB) == 256);

    struct FishInstance
    {
        XMFLOAT3 pos;     float phase;  // 16
        XMFLOAT3 forward; float speed;  // 16
        XMFLOAT3 up;      float pad;    // 16
        XMFLOAT4 color;                 // 16
    };
    static_assert(sizeof(FishInstance) == 64);

    std::vector<Fish>            m_fish;

    ComPtr<ID3D12Device>         m_device;
    ComPtr<ID3D12Resource>       m_vb, m_vbUpload;
    ComPtr<ID3D12Resource>       m_ib, m_ibUpload;
    D3D12_VERTEX_BUFFER_VIEW     m_vbView  = {};
    D3D12_INDEX_BUFFER_VIEW      m_ibView  = {};
    UINT                         m_indexCount = 0;

    ComPtr<ID3D12Resource>       m_instanceBuf;        // UPLOAD heap, persistent map
    FishInstance*                m_mappedInstances = nullptr;

    ComPtr<ID3D12Resource>       m_sceneCB;
    SceneCB*                     m_mappedSceneCB = nullptr;

    ComPtr<ID3D12DescriptorHeap> m_srvHeap;    // [0] = instance StructuredBuffer SRV
    ComPtr<ID3D12RootSignature>  m_rootSig;
    ComPtr<ID3D12PipelineState>  m_pso;

    void Boids(float dt);
};
