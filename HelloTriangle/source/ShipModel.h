// ============================================================
// ShipModel.h
// Loads a GLTF ship model (hull/rigging/sails groups) and
// renders it on the ocean surface by sampling the FFT heightmap.
// ============================================================
#pragma once
#include <d3d12.h>
#include <wrl.h>
#include <DirectXMath.h>
#include <array>
#include <string>
#include <vector>
#include "../DXSampleHelper.h"
#include "renderer/RendererContext.h"

using Microsoft::WRL::ComPtr;
using namespace DirectX;

struct ShipCloudParams
{
    float time, coverage, scale, base, top, windX, windZ, densityMult, enabled;
};

class ShipModel
{
public:
    // gltfDir: absolute path to the folder containing the .gltf file,
    //          ending with a backslash e.g. "E:\\Assets\\dutch_ship_large_02_1k.gltf\\"
    void Init(
        ComPtr<ID3D12Device>  device,
        ID3D12Resource*       heightMap,
        const std::string&    gltfDir,
        const UINT8* vsData,       UINT vsLen,
        const UINT8* psData,       UINT psLen,
        const UINT8* shadowVsData, UINT shadowVsLen);

    void InitBuffers(ComPtr<ID3D12GraphicsCommandList> cmdList);

    // Advance spring-damper physics for pitch/roll (call before Render each frame)
    void Update(float dt, float windSpeed, float phillipsA, float windDirX, float windDirY);

    void Render(
        RenderContext& ctx,
        XMFLOAT3 sunDir,    float sunIntensity,
        XMFLOAT3 sunColor,  XMFLOAT3 cameraPos,
        XMFLOAT3 moonDir,   float moonIntensity,
        XMFLOAT3 moonColor, float lightningIntensity,
        const ShipCloudParams& cloud);

    void RenderDepth(
        ID3D12GraphicsCommandList* cmd,
        const XMMATRIX&            lightViewProj);

    float     scale    = 5.0f;
    float     yaw      = 0.0f;
    XMFLOAT3  worldPos = { 0.0f, 0.0f, 80.0f };

private:
    struct Vertex { XMFLOAT3 pos; XMFLOAT3 normal; XMFLOAT2 uv; };

    struct MeshGroup
    {
        ComPtr<ID3D12Resource>       vb, vbUpload;
        ComPtr<ID3D12Resource>       ib, ibUpload;
        D3D12_VERTEX_BUFFER_VIEW     vbView{};
        D3D12_INDEX_BUFFER_VIEW      ibView{};
        UINT                         indexCount = 0;
        ComPtr<ID3D12Resource>       diffuseTex,  diffuseUpload;
        ComPtr<ID3D12Resource>       normalTex,   normalUpload;
        ComPtr<ID3D12Resource>       armTex,      armUpload;
        ComPtr<ID3D12DescriptorHeap> srvHeap; // 4 slots: diff/norm/arm/heightmap
    };

    struct alignas(256) ShipCB
    {
        XMMATRIX viewProj;                            // 64
        XMFLOAT3 worldPos;      float scale;          // 16
        XMFLOAT3 sunDir;        float sunIntensity;   // 16
        XMFLOAT3 sunColor;      float yaw;            // 16
        XMFLOAT3 cameraPos;     float pad0;           // 16
        XMFLOAT3 moonDir;       float moonIntensity;  // 16
        XMFLOAT3 moonColor;     float lightningIntensity; // 16
        float    time;          float cloudCoverage;  //  8
        float    cloudScale;    float cloudBase;      //  8
        float    cloudTop;      float cloudWindX;     //  8
        float    cloudWindZ;    float cloudDensityMult; // 8
        float    cloudEnabled;  float pitch;          //  8
        float    roll;          float pad1[13];       // 56 -> total 256
    };
    static_assert(sizeof(ShipCB) == 256);

    struct alignas(256) ShadowCB
    {
        XMMATRIX lightViewProj;                  // 64
        XMFLOAT3 worldPos;  float scale;         // 16
        float    yaw;       float pad[43];       // 4 + 172 -> total 256
    };
    static_assert(sizeof(ShadowCB) == 256);

    ComPtr<ID3D12Device>         m_device;
    std::array<MeshGroup, 3>     m_groups;
    ComPtr<ID3D12Resource>       m_cb;
    ShipCB*                      m_mappedCB       = nullptr;
    ComPtr<ID3D12Resource>       m_shadowCB;
    ShadowCB*                    m_mappedShadowCB = nullptr;
    ComPtr<ID3D12RootSignature>  m_rootSig;
    ComPtr<ID3D12PipelineState>  m_pso;           // hull: CCW front, cull back
    ComPtr<ID3D12PipelineState>  m_psoDoubleSided; // rigging/sails: no culling
    ComPtr<ID3D12RootSignature>  m_shadowRootSig;
    ComPtr<ID3D12PipelineState>  m_shadowPSO;
    ID3D12Resource*              m_heightMap    = nullptr;
    UINT                         m_srvIncr      = 0;
    std::string                  m_gltfDir;

    // Spring-damper state for wave-induced pitch/roll
    float m_pitch    = 0.0f, m_pitchVel = 0.0f;
    float m_roll     = 0.0f, m_rollVel  = 0.0f;
    float m_simTime  = 0.0f;

    // Temporary CPU-side storage between LoadGLTF and InitBuffers
    std::vector<Vertex>   m_cpuVerts[3];
    std::vector<uint32_t> m_cpuIndices[3];
    std::string           m_texPaths[3];
    std::string           m_normalPaths[3];
    std::string           m_armPaths[3];

    void LoadGLTF(const std::string& gltfDir);
    void LoadTexture(ComPtr<ID3D12Resource>& tex, ComPtr<ID3D12Resource>& upload,
                     const std::string& jpgPath,
                     ComPtr<ID3D12GraphicsCommandList> uploadCmd);
    void UploadBuffer(ComPtr<ID3D12GraphicsCommandList> cmd,
                      ComPtr<ID3D12Resource>& dest, ComPtr<ID3D12Resource>& upload,
                      const void* data, UINT64 byteSize,
                      D3D12_RESOURCE_STATES finalState);
};
