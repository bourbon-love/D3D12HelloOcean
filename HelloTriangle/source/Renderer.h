// ============================================================
// Renderer.h
// Ocean mesh rendering class. Manages 512x512 grid, FFT height map sampling,
// Gerstner waves, scene constant buffer, and camera control.
// ============================================================
#pragma once
#include <d3d12.h>
#include <wrl.h>
#include "renderer/RendererContext.h"
#include "Camera.h"
#include <vector>
#include "SkyDome.h"
#include "WeatherSystem.h"
using Microsoft::WRL::ComPtr;


static const UINT  GRID_SIZE = 512;   // Subdivision count
static const float GRID_WORLD_SIZE = 400.0f; // World-space size

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
    void ToggleShipOrbit() { m_shipOrbitMode = !m_shipOrbitMode; }
    bool IsShipOrbitMode() const { return m_shipOrbitMode; }
	// For SkyDome to read sun and sky parameters
    void SetSkyDome(SkyDome* skyDome) { m_skyDome = skyDome; }
    void SetWeatherSystem(WeatherSystem* ws) { m_weatherSystem = ws; }
    void SetSSRMix(float v) { m_ssrMix = v; }
    void SetCloudShadowParams(float coverage, float scale, float base, float top,
                              float densityMult, float windX, float windZ, float enabled)
    {
        m_cloudCoverage    = coverage;
        m_cloudScale       = scale;
        m_cloudBase        = base;
        m_cloudTop         = top;
        m_cloudDensityMult = densityMult;
        m_cloudWindX       = windX;
        m_cloudWindZ       = windZ;
        m_cloudEnabled     = enabled;
    }
    void SetJitter(float x, float y) { m_camera.m_jitter = { x, y }; }
    float GetCloudCoverage()    const { return m_cloudCoverage; }
    float GetCloudScale()       const { return m_cloudScale; }
    float GetCloudBase()        const { return m_cloudBase; }
    float GetCloudTop()         const { return m_cloudTop; }
    float GetCloudWindX()       const { return m_cloudWindX; }
    float GetCloudWindZ()       const { return m_cloudWindZ; }
    float GetCloudDensityMult() const { return m_cloudDensityMult; }
    float GetCloudEnabled()     const { return m_cloudEnabled; }
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
		float amplitude;            // Wave amplitude
		float waveLength;           // Wave wavelength
		float speed;                // Wave speed
		float steepness;            // Wave steepness (0-1, higher = steeper)
		float pad0, pad1;           // Padding for 16-byte alignment
  
    };

    struct SceneCB
    {
        XMMATRIX view;
        XMMATRIX proj;
        float    time;
        XMFLOAT3 cameraPos;

        XMFLOAT3 sunDir;
        float    sunIntensity;
        XMFLOAT3 sunColor;
        float    padSun;
        XMFLOAT3 skyColor;
        float    padSky;
        float fogStart;
        float fogEnd;
        float foamIntensity;
        float ssrMix;
        WaveParam waves[4];

        XMFLOAT2 tileOffset;  // Tile XZ offset (world space)
        XMFLOAT2 tilePad;

        // Cloud shadow: ocean surface occlusion from cloud density above
        float cloudCoverage;
        float cloudScale;
        float cloudBase;
        float cloudTop;
        float cloudWindX;
        float cloudWindZ;
        float cloudDensityMult;
        float cloudEnabled;   // 0=off, 1=on
    };

    // Constant buffer slots: 256-byte alignment required
    static const UINT  CB_SLOT_SIZE = 512;          // >= sizeof(SceneCB) and a multiple of 256
    static const UINT  CB_MAX_TILES = 100;          // Maximum number of tiles drawn
    static const int   TILE_RADIUS  = 4;            // ±4 tiles around camera (9x9 = 81 tiles max)
    static constexpr float TILE_SIZE = GRID_WORLD_SIZE;  // Tile world-space size (= 400m)


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
    bool  m_shipOrbitMode = false;
    float m_ssrMix = 1.0f;
    float m_time   = 0.0f;
    float m_cloudCoverage    = 0.45f;
    float m_cloudScale       = 1.0f;
    float m_cloudBase        = 600.0f;
    float m_cloudTop         = 2200.0f;
    float m_cloudDensityMult = 1.0f;
    float m_cloudWindX       = 0.0f;
    float m_cloudWindZ       = 0.0f;
    float m_cloudEnabled     = 0.0f;

    SceneCB m_lastSceneCB = {};  // Used as base for tile CBs in Render()

    // LOD grid (64x64): for distant tiles
    ComPtr<ID3D12Resource>   m_lodVB, m_lodVBUpload;
    ComPtr<ID3D12Resource>   m_lodIB, m_lodIBUpload;
    D3D12_VERTEX_BUFFER_VIEW m_lodVBView = {};
    D3D12_INDEX_BUFFER_VIEW  m_lodIBView = {};
    UINT                     m_lodIndexCount = 0;

    // Frustum planes (for view frustum culling)
    XMFLOAT4 m_frustumPlanes[6] = {};
    void ExtractFrustumPlanes();
    bool IsTileVisible(int tx, int tz) const;
    void CreateLodGridBuffers(ComPtr<ID3D12GraphicsCommandList> cmdList);

    ComPtr<ID3D12Device> m_device;
    ComPtr<ID3D12GraphicsCommandList> m_commandList;
    ComPtr<ID3D12RootSignature> m_rootSignature;
    ComPtr<ID3D12Resource> m_constantBuffer;
    ComPtr<ID3D12PipelineState>       m_pipelineState;       // Solid PSO
    ComPtr<ID3D12PipelineState>       m_wireframePSO;        // Wireframe PSO
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

    // Water box
    ComPtr<ID3D12Resource> m_boxVB;
    ComPtr<ID3D12Resource> m_boxVBUpload;
    ComPtr<ID3D12Resource> m_boxIB;
    ComPtr<ID3D12Resource> m_boxIBUpload;
    D3D12_VERTEX_BUFFER_VIEW m_boxVBView = {};
    D3D12_INDEX_BUFFER_VIEW  m_boxIBView = {};
    UINT                     m_boxIndexCount = 0;

    // Translucent PSO
    ComPtr<ID3D12PipelineState> m_waterBoxPSO;
};