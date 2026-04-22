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
    float    speed;      // 下落速度
    float    length;     // 雨线长度
    
};

struct RainVertex
{
    XMFLOAT3 position;
    float    alpha;     // 透明度
};

struct Ripple
{
    XMFLOAT2 position; // 水面XZ坐标
    float    radius;   // 当前半径
    float    maxRadius; // 最大半径
    float    age;      // 当前存活时间
    float    lifetime; // 总存活时间
};

class RainSystem
{
public:
    void Init(ComPtr<ID3D12Device> device,
        ComPtr<ID3D12RootSignature> rootSignature,
        const UINT8* vsData, UINT vsSize,
        const UINT8* psData, UINT psSize);
    void InitResources(ComPtr<ID3D12GraphicsCommandList> cmdList);
    void Update(float deltaTime, float intensity, float windDirX, float windDirY); // intensity: 0=无雨, 1=暴风雨
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
    static const UINT VERTS_PER_DROP = 2; // 每个雨滴两个顶点（线段）
	static const UINT MAX_RIPPLES = 200;
	std::vector<Ripple> m_ripples;

    // 供 shader 采样的涟漪数据 CB
    struct RippleData
    {
		XMFLOAT2 positions;
		float    radius;
		float    strength;
	};
    ComPtr<ID3D12Device>            m_device;
    ComPtr<ID3D12PipelineState>     m_rainPSO;
    ComPtr<ID3D12RootSignature>     m_rainRootSig;
    // 动态 VB，每帧更新
    ComPtr<ID3D12Resource>          m_rainVB;
    D3D12_VERTEX_BUFFER_VIEW        m_rainVBView = {};
    RainVertex* m_rainVBMapped = nullptr;

    // Rain CB
    struct __declspec(align(256)) RainCB
    {
        XMMATRIX viewProj;
        float    alpha;    // 整体透明度，随天气强度变化
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

	// 风向，单位向量，暴风雨时雨线倾斜
    float m_windDirX = 1.0f;
    float m_windDirY = 0.0f;
};