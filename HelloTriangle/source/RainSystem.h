// ============================================================
// RainSystem.h
// Rain particle system class. Manages up to 2000 particles and
// up to 200 ripples.
// ============================================================
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

struct RainDrop
{
    XMFLOAT3 position;
    float    speed;      // fall speed
    float    length;     // raindrop line length
    
};

struct RainVertex
{
    XMFLOAT3 position;
    float    alpha;     // opacity
};

struct Ripple
{
    XMFLOAT2 position; // water surface XZ coordinates
    float    radius;   // current radius
    float    maxRadius; // maximum radius
    float    age;      // current age
    float    lifetime; // total lifetime
};

class RainSystem
{
public:
    void Init(ComPtr<ID3D12Device> device,
        ComPtr<ID3D12RootSignature> rootSignature,
        const UINT8* vsData, UINT vsSize,
        const UINT8* psData, UINT psSize);
    void InitResources(ComPtr<ID3D12GraphicsCommandList> cmdList);
    void Update(float deltaTime, float intensity, float windDirX, float windDirY); // intensity: 0=no rain, 1=heavy storm
    void Render(RenderContext& ctx,
        const XMMATRIX& view, const XMMATRIX& proj,
        const XMFLOAT3& cameraPos);
	ID3D12Resource* GetRippleCB() const { return m_rippleCB.Get(); }
    D3D12_GPU_VIRTUAL_ADDRESS GetRippleCBAddress() const
    {
        return m_rippleCB->GetGPUVirtualAddress();
    }
private:
    void CreatePSO(const UINT8* vsData, UINT vsSize,
        const UINT8* psData, UINT psSize);
    void SpawnRainDrop(const XMFLOAT3& cameraPos);

    static const UINT MAX_RAINDROPS = 2000;
    static const UINT VERTS_PER_DROP = 2; // two vertices per raindrop (line segment)
	static const UINT MAX_RIPPLES = 200;
	std::vector<Ripple> m_ripples;

    // Ripple data CB sampled by the shader
    struct RippleData
    {
		XMFLOAT2 positions;
		float    radius;
		float    strength;
	};
    ComPtr<ID3D12Device>            m_device;
    ComPtr<ID3D12PipelineState>     m_rainPSO;
    ComPtr<ID3D12RootSignature>     m_rainRootSig;
    // Dynamic VB, updated every frame
    ComPtr<ID3D12Resource>          m_rainVB;
    D3D12_VERTEX_BUFFER_VIEW        m_rainVBView = {};
    RainVertex* m_rainVBMapped = nullptr;

    // Rain CB
    struct __declspec(align(256)) RainCB
    {
        XMMATRIX viewProj;
        float    alpha;    // overall opacity, varies with weather intensity
        float    pad[3];
    };
    ComPtr<ID3D12Resource>  m_rainCB;
    RainCB* m_rainCBMapped = nullptr;

    //rippleCB
    struct __declspec(align(256)) RippleCB
    {
		RippleData ripples[MAX_RIPPLES];
        UINT       rippleCount;
		float      pad[3];
    };
	ComPtr<ID3D12Resource>  m_rippleCB;
	RippleCB*               m_rippleCBMapped = nullptr;

    std::vector<RainDrop>   m_drops;
    std::mt19937            m_rng{ 42 };
    float                   m_intensity = 0.0f;
    UINT                    m_activeDrops = 0;

	// Wind direction, unit vector; rain lines tilt during storms
    float m_windDirX = 1.0f;
    float m_windDirY = 0.0f;
};