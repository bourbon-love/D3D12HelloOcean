#pragma once
#include <d3d12.h>
#include <wrl.h>
#include "renderer/RendererContext.h"
#include "Camera.h"
#include <vector>
#include "SkyDome.h"
#include "WeatherSystem.h"
using Microsoft::WRL::ComPtr;


static const UINT  GRID_SIZE = 512;   // 分段数
static const float GRID_WORLD_SIZE = 400.0f; // 世界空间尺寸

class Renderer
{
public:
    // Initializes PSO, CBV, depth buffer, and wireframe PSO
    // Must be called before InitResources
    void InitPSO(
        ComPtr<ID3D12Device> device,
        ComPtr<ID3D12RootSignature> rootSignature,
        UINT width,
        UINT height,
        const UINT8* vsData,UINT vsSize,
        const UINT8* psData,UINT psSize,
		const UINT8* waterBoxVSData, UINT waterBoxVSSize,
		const UINT8* waterBoxPSData, UINT waterBoxPSSize
        );
    // Records grid mesh upload commands into the command list
    // Call before closing and executing the init command list    
    void InitResources(ComPtr<ID3D12GraphicsCommandList> commandList);

    void Render(RenderContext& ctx);
    void Update(float deltaTime);
    void OnMouseMove(float dx, float dy);

	void RenderWaterBox(RenderContext& ctx);
    void ToggleWireframe();

    ID3D12DescriptorHeap* GetDSVHeap() const { return m_dsvHeap.Get(); }
    ID3D12Resource* GetDepthBuffer() const { return m_depthBuffer.Get(); }
    ID3D12PipelineState* GetPSO() const { return m_pipelineState.Get(); }
    D3D12_VERTEX_BUFFER_VIEW    GetGridVBView() const { return m_gridVBView; }
    D3D12_INDEX_BUFFER_VIEW     GetGridIBView() const { return m_gridIBView; }
    UINT                     GetGridIndexCount() const { return m_gridIndexCount; }
    float GetTime() const { return m_time; }
    XMFLOAT3 GetCameraPos() const { return m_camera.position; }
	Camera& GetCamera() { return m_camera; }
    void ToggleShowcase() { m_showcaseMode = !m_showcaseMode; }
    bool IsShowcaseMode() const { return m_showcaseMode; }
	// 供SkyDome读取太阳和天空参数
    void SetSkyDome(SkyDome* skyDome) { m_skyDome = skyDome; }
    void SetWeatherSystem(WeatherSystem* ws) { m_weatherSystem = ws; }
    XMMATRIX GetViewMatrix() const
    {
        return m_camera.GetViewMatrix();
    }
    XMMATRIX GetProjMatrix() const
    {
        return m_camera.GetProjMatrix();
    }

    struct WaveParam
    {
        XMFLOAT2 direction; 
		float amplitude;            // 波浪振幅
		float waveLength;           // 波浪波长
		float speed;                // 波浪速度 
		float steepness;            // 波浪陡峭度(0-1之间，值越大波浪越陡峭)
		float pad0, pad1;           // 填充以满足16字节对齐
  
    };

    struct SceneCB
    {

        XMMATRIX view;
        XMMATRIX proj;
        float    time;              //累计时间，驱动波浪动画
		XMFLOAT3 cameraPos;          //摄像机位置，用于计算视角依赖的波浪效果

        XMFLOAT3 sunDir;
        float             sunIntensity;
        XMFLOAT3 sunColor;
        float             padSun;
        XMFLOAT3 skyColor;
        float             padSky;
        float fogStart;   // 雾开始距离
        float fogEnd;     // 雾结束距离
        XMFLOAT2 padFog;
        WaveParam waves[4];

    };


private:

    void CreateDepthBuffer(UINT width, UINT height);
    void CreateGridBuffers(ComPtr<ID3D12GraphicsCommandList> cmdList);
    void CreateWaterBoxBuffers(ComPtr<ID3D12GraphicsCommandList> cmdList);
    void CreateWireframePSO();

    Camera m_camera;
	SkyDome* m_skyDome = nullptr;
    WeatherSystem* m_weatherSystem = nullptr;
    SceneCB* m_mappedCB = nullptr;
    UINT8* m_pCbvDataBegin;
    bool  m_wireframe = false;
    bool  m_showcaseMode = false;

	float m_time = 0.0f;

    ComPtr<ID3D12Device> m_device;
    ComPtr<ID3D12GraphicsCommandList> m_commandList;
    ComPtr<ID3D12RootSignature> m_rootSignature;
    ComPtr<ID3D12Resource> m_constantBuffer;
    ComPtr<ID3D12PipelineState>       m_pipelineState;       // 实体PSO
    ComPtr<ID3D12PipelineState>       m_wireframePSO;        // 线框PSO
    //depth
    ComPtr<ID3D12DescriptorHeap> m_dsvHeap;
    ComPtr<ID3D12Resource>       m_depthBuffer;

    //Grid Mesh
    ComPtr<ID3D12Resource> m_gridVB;
    ComPtr<ID3D12Resource> m_gridVBUpload; 
    ComPtr<ID3D12Resource> m_gridIB;
    ComPtr<ID3D12Resource> m_gridIBUpload; 
    D3D12_VERTEX_BUFFER_VIEW m_gridVBView = {};
    D3D12_INDEX_BUFFER_VIEW  m_gridIBView = {};
    UINT m_gridIndexCount = 0;

    //shaderdata
    std::vector<UINT8> m_vertexShaderData;
    std::vector<UINT8> m_pixelShaderData;

    std::vector<UINT8> m_waterBoxVSData;
    std::vector<UINT8> m_waterBoxPSData;

    // 水箱
    ComPtr<ID3D12Resource> m_boxVB;
    ComPtr<ID3D12Resource> m_boxVBUpload;
    ComPtr<ID3D12Resource> m_boxIB;
    ComPtr<ID3D12Resource> m_boxIBUpload;
    D3D12_VERTEX_BUFFER_VIEW m_boxVBView = {};
    D3D12_INDEX_BUFFER_VIEW  m_boxIBView = {};
    UINT                     m_boxIndexCount = 0;

    // 半透明 PSO
    ComPtr<ID3D12PipelineState> m_waterBoxPSO;
};