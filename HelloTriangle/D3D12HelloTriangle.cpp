#include "D3D12HelloTriangle.h"
#include <d3dx12_root_signature.h>
#include <d3dx12_barriers.h>
#include <string>
#include "GpuMarkers.h"

extern "C" { __declspec(dllexport) extern const UINT D3D12SDKVersion = 618; }
extern "C" { __declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\"; }

D3D12HelloTriangle::D3D12HelloTriangle(UINT width, UINT height, std::wstring name) :
    DXSample(width, height, name),
    m_frameIndex(0),
    m_viewport(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)),
    m_scissorRect(0, 0, static_cast<LONG>(width), static_cast<LONG>(height)),
    m_rtvDescriptorSize(0)
{
}

void D3D12HelloTriangle::OnInit()
{
    LoadPipeline();
    LoadAssets();
    m_lastTime = std::chrono::steady_clock::now();
    m_renderer->SetSkyDome(m_skyDome.get());


    {
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.NumDescriptors = 8;   // slot0=font, slots1-6=cubemap faces, slot7=BRDF LUT
        heapDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ThrowIfFailed(m_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_imguiSrvHeap)));
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(Win32Application::GetHwnd());
    ImGui_ImplDX12_Init(m_device.Get(), FrameCount,
        DXGI_FORMAT_R8G8B8A8_UNORM,
        m_imguiSrvHeap.Get(),
        m_imguiSrvHeap->GetCPUDescriptorHandleForHeapStart(),
        m_imguiSrvHeap->GetGPUDescriptorHandleForHeapStart());

    // Register IBL debug SRVs into the ImGui heap (must be after m_imguiSrvHeap creation).
    // slots 1-6: sky cubemap faces, slot 7: BRDF LUT.
    {
        UINT descSize = m_device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        auto cpuBase = m_imguiSrvHeap->GetCPUDescriptorHandleForHeapStart();

        for (UINT face = 0; face < 6; ++face)
        {
            D3D12_CPU_DESCRIPTOR_HANDLE cpu = { cpuBase.ptr + (face + 1) * descSize };
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Format                         = DXGI_FORMAT_R16G16B16A16_FLOAT;
            srvDesc.ViewDimension                  = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            srvDesc.Shader4ComponentMapping        = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Texture2DArray.MipLevels       = 1;
            srvDesc.Texture2DArray.FirstArraySlice = face;
            srvDesc.Texture2DArray.ArraySize       = 1;
            m_device->CreateShaderResourceView(
                m_iblSystem->GetCaptureCubemap(), &srvDesc, cpu);
        }

        D3D12_CPU_DESCRIPTOR_HANDLE cpuLut = { cpuBase.ptr + 7 * descSize };
        D3D12_SHADER_RESOURCE_VIEW_DESC lutSrv = {};
        lutSrv.Format                    = DXGI_FORMAT_R16G16_FLOAT;
        lutSrv.ViewDimension             = D3D12_SRV_DIMENSION_TEXTURE2D;
        lutSrv.Shader4ComponentMapping   = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        lutSrv.Texture2D.MipLevels       = 1;
        m_device->CreateShaderResourceView(m_iblSystem->GetBRDFLut(), &lutSrv, cpuLut);
    }
}

// ============================================================
// LoadPipeline
// ============================================================
void D3D12HelloTriangle::LoadPipeline()
{
    UINT dxgiFactoryFlags = 0;

#if defined(_DEBUG)
    {
        ComPtr<ID3D12Debug> debugController;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
        {
            debugController->EnableDebugLayer();
            dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
        }
    }
#endif

    ComPtr<IDXGIFactory4> factory;
    ThrowIfFailed(CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(&factory)));

    if (m_useWarpDevice)
    {
        ComPtr<IDXGIAdapter> warpAdapter;
        ThrowIfFailed(factory->EnumWarpAdapter(IID_PPV_ARGS(&warpAdapter)));
        ThrowIfFailed(D3D12CreateDevice(warpAdapter.Get(),
            D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_device)));
    }
    else
    {
        ComPtr<IDXGIAdapter1> hardwareAdapter;
        GetHardwareAdapter(factory.Get(), &hardwareAdapter);
        ThrowIfFailed(D3D12CreateDevice(hardwareAdapter.Get(),
            D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_device)));
    }

    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queueDesc.Type  = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ThrowIfFailed(m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_commandQueue)));

    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
    swapChainDesc.BufferCount  = FrameCount;
    swapChainDesc.Width        = m_width;
    swapChainDesc.Height       = m_height;
    swapChainDesc.Format       = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.BufferUsage  = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.SwapEffect   = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.SampleDesc.Count = 1;

    ComPtr<IDXGISwapChain1> swapChain;
    ThrowIfFailed(factory->CreateSwapChainForHwnd(
        m_commandQueue.Get(), Win32Application::GetHwnd(),
        &swapChainDesc, nullptr, nullptr, &swapChain));
    ThrowIfFailed(factory->MakeWindowAssociation(Win32Application::GetHwnd(), DXGI_MWA_NO_ALT_ENTER));
    ThrowIfFailed(swapChain.As(&m_swapChain));
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();


    {
        D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
        rtvHeapDesc.NumDescriptors = FrameCount + 9;
        rtvHeapDesc.Type  = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        ThrowIfFailed(m_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_rtvHeap)));
        m_rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    }

    {
        CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(m_rtvHeap->GetCPUDescriptorHandleForHeapStart());
        for (UINT n = 0; n < FrameCount; n++)
        {
            ThrowIfFailed(m_swapChain->GetBuffer(n, IID_PPV_ARGS(&m_renderTargets[n])));
            m_device->CreateRenderTargetView(m_renderTargets[n].Get(), nullptr, rtvHandle);
            rtvHandle.Offset(1, m_rtvDescriptorSize);
        }
    }

    ThrowIfFailed(m_device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_commandAllocator)));
}

// ============================================================
// LoadAssets
// ============================================================
void D3D12HelloTriangle::LoadAssets()
{

    {
        CD3DX12_ROOT_PARAMETER rootParameters[5];
        rootParameters[0].InitAsConstantBufferView(0);
        CD3DX12_DESCRIPTOR_RANGE srvRange;
        srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 7, 0);  // t0..t6: FFT maps + sky + shadow + refraction + IBL prefilter + IBL LUT
        rootParameters[1].InitAsDescriptorTable(1, &srvRange, D3D12_SHADER_VISIBILITY_ALL);
        rootParameters[2].InitAsConstantBufferView(1);
        rootParameters[3].InitAsConstantBufferView(2);
        rootParameters[4].InitAsConstantBufferView(3);  // b3 SHCB for EvalSH in ocean PS
        CD3DX12_STATIC_SAMPLER_DESC sampler(0,
            D3D12_FILTER_MIN_MAG_MIP_LINEAR,
            D3D12_TEXTURE_ADDRESS_MODE_WRAP,
            D3D12_TEXTURE_ADDRESS_MODE_WRAP,
            D3D12_TEXTURE_ADDRESS_MODE_WRAP);
        CD3DX12_ROOT_SIGNATURE_DESC rsd;
        rsd.Init(5, rootParameters, 1, &sampler,
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
        ComPtr<ID3DBlob> signature, error;
        ThrowIfFailed(D3D12SerializeRootSignature(&rsd,
            D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error));
        ThrowIfFailed(m_device->CreateRootSignature(0,
            signature->GetBufferPointer(), signature->GetBufferSize(),
            IID_PPV_ARGS(&m_rootSignature)));
    }

    // OceanFFT
    UINT8 *pPhillips = nullptr, *pTimeEvo = nullptr, *pIFFT = nullptr;
    UINT   phillipsLen = 0, timeEvoLen = 0, ifftLen = 0;
    ThrowIfFailed(ReadDataFromFile(GetAssetFullPath(L"PhillipsCS.cso").c_str(),       &pPhillips, &phillipsLen));
    ThrowIfFailed(ReadDataFromFile(GetAssetFullPath(L"TimeEvolutionCS.cso").c_str(), &pTimeEvo,  &timeEvoLen));
    ThrowIfFailed(ReadDataFromFile(GetAssetFullPath(L"IFFTCS.cso").c_str(),          &pIFFT,     &ifftLen));
    m_oceanFFT = std::make_unique<OceanFFT>();
    m_oceanFFT->Init(m_device, m_commandQueue, 256,
        pPhillips, phillipsLen, pTimeEvo, timeEvoLen, pIFFT, ifftLen);

    UINT8 *pSkyCaptureCS = nullptr, *pBRDFLutCS = nullptr, *pIrradCS = nullptr, *pPrefilterCS = nullptr;
    UINT   skyCaptureLen = 0, brdfLutLen = 0, irradLen = 0, prefilterLen = 0;
    ThrowIfFailed(ReadDataFromFile(GetAssetFullPath(L"SkyCaptureCS.cso").c_str(),           &pSkyCaptureCS, &skyCaptureLen));
    ThrowIfFailed(ReadDataFromFile(GetAssetFullPath(L"BRDFLutCS.cso").c_str(),              &pBRDFLutCS,    &brdfLutLen));
    ThrowIfFailed(ReadDataFromFile(GetAssetFullPath(L"IrradianceConvolveCS.cso").c_str(),   &pIrradCS,      &irradLen));
    ThrowIfFailed(ReadDataFromFile(GetAssetFullPath(L"PrefilterSpecularCS.cso").c_str(),    &pPrefilterCS,  &prefilterLen));
    m_iblSystem = std::make_unique<IBLSystem>();
    m_iblSystem->Init(m_device, m_commandQueue, 64, 128,
        pSkyCaptureCS, skyCaptureLen, pBRDFLutCS, brdfLutLen,
        pIrradCS, irradLen, pPrefilterCS, prefilterLen);

    UINT8 *pVS = nullptr, *pPS = nullptr, *pBoxVS = nullptr, *pBoxPS = nullptr;
    UINT   vsLen = 0, psLen = 0, boxVsLen = 0, boxPsLen = 0;
    ThrowIfFailed(ReadDataFromFile(GetAssetFullPath(L"shaders_VSMain.cso").c_str(),      &pVS,    &vsLen));
    ThrowIfFailed(ReadDataFromFile(GetAssetFullPath(L"shaders_PSMain.cso").c_str(),      &pPS,    &psLen));
    ThrowIfFailed(ReadDataFromFile(GetAssetFullPath(L"waterbody_VSMain.cso").c_str(),    &pBoxVS, &boxVsLen));
    ThrowIfFailed(ReadDataFromFile(GetAssetFullPath(L"waterbody_PSMain.cso").c_str(),    &pBoxPS, &boxPsLen));
    m_renderer = std::make_unique<Renderer>();
    m_renderer->InitPSO(m_device, m_rootSignature, m_width, m_height,
        pVS, vsLen, pPS, psLen, pBoxVS, boxVsLen, pBoxPS, boxPsLen);

    // SkyDome
    UINT8 *pSkyVS = nullptr, *pSkyPS = nullptr;
    UINT   skyVsLen = 0, skyPsLen = 0;
    ThrowIfFailed(ReadDataFromFile(GetAssetFullPath(L"skyshaders_VSMain.cso").c_str(), &pSkyVS, &skyVsLen));
    ThrowIfFailed(ReadDataFromFile(GetAssetFullPath(L"skyshaders_PSMain.cso").c_str(), &pSkyPS, &skyPsLen));
    m_skyDome = std::make_unique<SkyDome>();
    m_skyDome->InitPSO(m_device, m_rootSignature, m_width, m_height,
        pSkyVS, skyVsLen, pSkyPS, skyPsLen);

    // VolumetricClouds
    UINT8 *pCloudVS = nullptr, *pCloudPS = nullptr, *pCloudCompositePS = nullptr;
    UINT   cloudVsLen = 0, cloudPsLen = 0, cloudCompositePsLen = 0;
    ThrowIfFailed(ReadDataFromFile(GetAssetFullPath(L"clouds_CloudVS.cso").c_str(),     &pCloudVS,          &cloudVsLen));
    ThrowIfFailed(ReadDataFromFile(GetAssetFullPath(L"clouds_CloudPS.cso").c_str(),     &pCloudPS,          &cloudPsLen));
    ThrowIfFailed(ReadDataFromFile(GetAssetFullPath(L"clouds_CompositePS.cso").c_str(), &pCloudCompositePS, &cloudCompositePsLen));
    m_volumetricClouds = std::make_unique<VolumetricClouds>();
    m_volumetricClouds->Init(m_device, m_width, m_height,
        pCloudVS, cloudVsLen, pCloudPS, cloudPsLen,
        pCloudCompositePS, cloudCompositePsLen);
    free(pCloudVS); free(pCloudPS); free(pCloudCompositePS);

    // RainbowEffect
    UINT8 *pRainbowVS = nullptr, *pRainbowPS = nullptr;
    UINT   rainbowVsLen = 0, rainbowPsLen = 0;
    ThrowIfFailed(ReadDataFromFile(GetAssetFullPath(L"rainbow_RainbowVS.cso").c_str(), &pRainbowVS, &rainbowVsLen));
    ThrowIfFailed(ReadDataFromFile(GetAssetFullPath(L"rainbow_RainbowPS.cso").c_str(), &pRainbowPS, &rainbowPsLen));
    m_rainbowEffect = std::make_unique<RainbowEffect>();
    m_rainbowEffect->Init(m_device, m_width, m_height,
        pRainbowVS, rainbowVsLen, pRainbowPS, rainbowPsLen);
    free(pRainbowVS); free(pRainbowPS);

    // RainSystem
    UINT8 *pRainVS = nullptr, *pRainPS = nullptr;
    UINT   rainVsLen = 0, rainPsLen = 0;
    ThrowIfFailed(ReadDataFromFile(GetAssetFullPath(L"rain_VSMain.cso").c_str(),  &pRainVS, &rainVsLen));
    ThrowIfFailed(ReadDataFromFile(GetAssetFullPath(L"rain_PSMain.cso").c_str(),  &pRainPS, &rainPsLen));
    m_rainSystem = std::make_unique<RainSystem>();
    m_rainSystem->Init(m_device, m_rootSignature, pRainVS, rainVsLen, pRainPS, rainPsLen);
    m_rainSystem->InitResources(m_commandList);

    m_weatherSystem = std::make_unique<WeatherSystem>();
    m_weatherSystem->Init(m_oceanFFT.get(), m_skyDome.get());

    // FloatingObject
    UINT8 *pFOVS = nullptr, *pFOPS = nullptr;
    UINT   foVsLen = 0, foPsLen = 0;
    ThrowIfFailed(ReadDataFromFile(GetAssetFullPath(L"floating_object_FloatObjVS.cso").c_str(), &pFOVS, &foVsLen));
    ThrowIfFailed(ReadDataFromFile(GetAssetFullPath(L"floating_object_FloatObjPS.cso").c_str(), &pFOPS, &foPsLen));
    m_floatingObject = std::make_unique<FloatingObject>();
    m_floatingObject->Init(m_device, m_iblSystem.get(), m_oceanFFT->GetHeightMap(), pFOVS, foVsLen, pFOPS, foPsLen);
    free(pFOVS); free(pFOPS);

    // ShipModel
    UINT8 *pShipVS = nullptr, *pShipPS = nullptr, *pShipShadowVS = nullptr;
    UINT   shipVsLen = 0, shipPsLen = 0, shipShadowVsLen = 0;
    ThrowIfFailed(ReadDataFromFile(GetAssetFullPath(L"ship_ShipVS.cso").c_str(),
        &pShipVS, &shipVsLen));
    ThrowIfFailed(ReadDataFromFile(GetAssetFullPath(L"ship_ShipPS.cso").c_str(),
        &pShipPS, &shipPsLen));
    ThrowIfFailed(ReadDataFromFile(GetAssetFullPath(L"ship_ShipShadowVS.cso").c_str(),
        &pShipShadowVS, &shipShadowVsLen));
    // GetAssetFullPath(L"") is the executable's directory; the GLTF asset lives at
    // <repo root>\Assets\, four levels up from <repo root>\HelloTriangle\bin\x64\Debug\.
    std::wstring wAssetsPath = GetAssetFullPath(L"");
    std::string assetsPathNarrow(wAssetsPath.size(), '\0');
    for (size_t i = 0; i < wAssetsPath.size(); ++i)
        assetsPathNarrow[i] = static_cast<char>(wAssetsPath[i]);
    std::string shipGltfDir = assetsPathNarrow + "..\\..\\..\\..\\Assets\\dutch_ship_large_02_1k.gltf\\";

    m_shipModel = std::make_unique<ShipModel>();
    m_shipModel->Init(
        m_device,
        m_iblSystem.get(),
        m_oceanFFT->GetHeightMap(),
        shipGltfDir,
        pShipVS, shipVsLen,
        pShipPS, shipPsLen,
        pShipShadowVS, shipShadowVsLen);
    free(pShipVS); free(pShipPS); free(pShipShadowVS);

    // FishSchool
    UINT8 *pFishVS = nullptr, *pFishPS = nullptr;
    UINT   fishVsLen = 0, fishPsLen = 0;
    ThrowIfFailed(ReadDataFromFile(GetAssetFullPath(L"fish_FishVS.cso").c_str(), &pFishVS, &fishVsLen));
    ThrowIfFailed(ReadDataFromFile(GetAssetFullPath(L"fish_FishPS.cso").c_str(), &pFishPS, &fishPsLen));
    m_fishSchool = std::make_unique<FishSchool>();
    m_fishSchool->Init(m_device, m_iblSystem.get(), pFishVS, fishVsLen, pFishPS, fishPsLen);
    free(pFishVS); free(pFishPS);


    ThrowIfFailed(m_device->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        m_commandAllocator.Get(),
        m_renderer->GetPSO(),
        IID_PPV_ARGS(&m_commandList)));
    m_skyDome->InitResources(m_commandList);
    m_renderer->InitResources(m_commandList);
    m_floatingObject->InitBuffers(m_commandList);
    m_shipModel->InitBuffers(m_commandList);
    m_fishSchool->InitBuffers(m_commandList);
    m_fishSchool->SpawnSchool();


    ThrowIfFailed(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)));
    m_fenceValue = 1;
    m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!m_fenceEvent) ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()));

    ThrowIfFailed(m_commandList->Close());
    ID3D12CommandList* ppInitCmds[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(_countof(ppInitCmds), ppInitCmds);
    WaitForPreviousFrame();


    m_pp = std::make_unique<PostProcessPipeline>();
    m_pp->Init(m_device, m_rtvHeap.Get(), m_rtvDescriptorSize,
               FrameCount, m_width, m_height, GetAssetFullPath(L""));
    m_pp->InitSceneResources(
        m_renderer->GetDepthBuffer(),
        m_oceanFFT->GetHeightMap(),
        m_oceanFFT->GetDztMap(),
        m_floatingObject.get(),
        m_iblSystem.get());
}

// ============================================================
// OnUpdate
// ============================================================
void D3D12HelloTriangle::OnUpdate()
{
    // Copy staged SH coefficients into SHCB — safe here because WaitForPreviousFrame()
    // was called at the end of the previous OnRender, so GPU writes are complete.
    m_iblSystem->ReadbackSH();

    auto currentTime = std::chrono::steady_clock::now();
    float deltaTime  = std::chrono::duration<float>(currentTime - m_lastTime).count();
    m_lastTime = currentTime;

    float scaledDt         = m_timePaused ? 0.0f : deltaTime * m_timeScale;
    float weatherIntensity = m_weatherSystem->GetWeatherIntensity();

    m_skyDome->Update(scaledDt);
    m_skyDome->SmoothLighting(deltaTime); // real-time filter: prevents abrupt sunset snapping
    m_skyDome->SetCameraY(m_renderer->GetCameraPos().y);


    if (m_autoExposure)
    {
        float sunH     = m_skyDome->GetSunDirection().y;
        float weatherI = m_weatherSystem->GetWeatherIntensity();
        float target;
        // Single continuous ramp: 1.0 at noon, 2.5 at deep night — no formula jump.
        // Old code had a discontinuous slope change at h=-0.15 (jump from 2.2 to 2.5)
        // that caused a visible brightness snap during twilight.
        if      (sunH > 0.25f)  target = 1.0f;
        else if (sunH > 0.0f)   target = 1.0f + 0.6f * (1.0f - sunH / 0.25f);
        else if (sunH > -0.30f) target = 1.6f + 0.9f * (-sunH / 0.30f);
        else                    target = 2.5f;
        target += weatherI * 0.4f;
        target = target < 0.3f ? 0.3f : (target > 5.0f ? 5.0f : target);
        if (!m_autoExposureInitialized)
        {
            // Snap to target on the first frame so the startup exposure is
            // already correct.  Without this, exposure starts at 1.0 and
            // rises toward 1.6 over ~1.5s before falling back to 1.0 as the
            // sun rises past 0.25 — the whole screen flashes briefly.
            m_pp->exposure = target;
            m_autoExposureInitialized = true;
        }
        else
        {
            float dt_clamped = deltaTime < 1.0f ? deltaTime : 1.0f;
            m_pp->exposure += (target - m_pp->exposure) * dt_clamped;
        }
    }

    m_renderer->Update(scaledDt);
    m_weatherSystem->Update(scaledDt);
    {
        OceanFFT::HeightSamples hs = m_oceanFFT->HasHeightSamples()
            ? m_oceanFFT->ReadHeightSamples()
            : OceanFFT::HeightSamples{};
        m_shipModel->Update(scaledDt, hs.h0, hs.hBow, hs.hSide);
    }

    // Auto-orbit around the ship in ship-orbit mode
    if (m_renderer->IsShipOrbitMode())
        m_renderer->GetCamera().UpdateShipOrbit(scaledDt, m_shipModel->worldPos);

    // Moisture accumulates during rain, decays ~50s after rain stops
    {
        constexpr float kAccum = 0.5f;
        constexpr float kDecay = 1.0f / 20.0f; // ~20s to fully dissipate after rain stops
        if (weatherIntensity > 0.2f)
            m_rainMoisture += scaledDt * kAccum * ((weatherIntensity - 0.2f) / 0.8f);
        else
            m_rainMoisture -= scaledDt * kDecay;
        m_rainMoisture = std::clamp(m_rainMoisture, 0.0f, 1.0f);
    }

    // Weather drives VolumetricClouds coverage when auto mode is on
    if (m_autoCloudCoverage)
        m_volumetricClouds->cloudCoverage = m_weatherSystem->GetCloudCoverage();

    // Rainbow cloud clear factor: coverage > 0.70 suppresses rainbow entirely
    {
        float cov = m_volumetricClouds->cloudCoverage;
        m_rainbowCloudFactor = std::clamp((0.70f - cov) / 0.40f, 0.0f, 1.0f);
    }

    m_rainSystem->Update(scaledDt, weatherIntensity,
        m_oceanFFT->windDirX, m_oceanFFT->windDirY,
        m_renderer->GetCameraPos());

    m_renderer->SetSSRMix(m_pp->ssrStrength);
    m_renderer->SetCloudShadowParams(
        m_volumetricClouds->cloudCoverage,
        m_volumetricClouds->cloudScale,
        m_volumetricClouds->cloudBase,
        m_volumetricClouds->cloudTop,
        m_volumetricClouds->densityMult,
        m_oceanFFT->windDirX,
        m_oceanFFT->windDirY,
        m_volumetricClouds->enabled ? 1.0f : 0.0f);
    m_skyDome->SetWindDir(m_oceanFFT->windDirX, m_oceanFFT->windDirY);
    m_floatingObject->Update(scaledDt);
    m_fishSchool->Update(scaledDt, m_renderer->GetTime());


    float curLightning = m_skyDome->GetLightningIntensity();
    bool  newStrike    = (curLightning > 0.0f && m_prevLightningIntensity <= 0.0f);
    m_pp->UpdateLightning(curLightning, newStrike);
    m_prevLightningIntensity = curLightning;


    {
        auto Halton = [](int idx, int base) {
            float f = 1.0f, r = 0.0f;
            while (idx > 0) { f /= base; r += f * (idx % base); idx /= base; }
            return r;
        };
        if (m_pp->taaEnabled)
        {
            int jIdx = (m_jitterIndex % 16) + 1;
            m_currentJitter.x = (Halton(jIdx, 2) - 0.5f) * 2.0f / m_width;
            m_currentJitter.y = (Halton(jIdx, 3) - 0.5f) * 2.0f / m_height;
            m_jitterIndex++;
        }
        else
        {
            m_currentJitter = { 0.0f, 0.0f };
        }
        m_renderer->SetJitter(m_currentJitter.x, m_currentJitter.y);
    }


    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame(1.0f, 1.0f);
    ImGui::NewFrame();
    BuildImGuiUI();
    ImGui::Render();
}

// ============================================================
// BuildImGuiUI
// ============================================================
void D3D12HelloTriangle::BuildImGuiUI()
{
    ImGuiIO& io = ImGui::GetIO();
    const float W          = io.DisplaySize.x;
    const float H          = io.DisplaySize.y;
    const float kLeftW     = 265.0f;
    const float kRightW    = 275.0f;
    const float kBottomH   = 62.0f;
    const float kPanelH    = H - kBottomH;
    const ImGuiWindowFlags kPanelFlags =
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse;

    // ============================================================
    // LEFT PANEL — scene / sky / cloud parameters
    // ============================================================
    ImGui::SetNextWindowPos (ImVec2(0, 0),      ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(kLeftW, kPanelH), ImGuiCond_Always);
    ImGui::Begin("Scene", nullptr, kPanelFlags);

    ImGui::Text("FPS: %.1f  (%.2f ms)", io.Framerate, 1000.0f / io.Framerate);

    // --- Time ---
    ImGui::Separator(); ImGui::TextColored({0.4f,0.8f,1.0f,1.0f}, "Time");
    ImGui::SliderFloat("Scale##time", &m_timeScale, 0.0f, 10.0f, "%.2f x");

    // --- Moon ---
    ImGui::Separator(); ImGui::TextColored({0.4f,0.8f,1.0f,1.0f}, "Moon");
    float rotSpeed = m_skyDome->GetCrescentRotSpeed();
    if (ImGui::SliderFloat("Crescent Spd", &rotSpeed, 0.0f, 0.5f, "%.3f"))
        m_skyDome->SetCrescentRotSpeed(rotSpeed);
    float bodyPow = m_skyDome->GetMoonBodyPow();
    if (ImGui::SliderFloat("Moon Size",    &bodyPow,   300.0f,  2000.0f, "%.0f"))
        m_skyDome->SetMoonBodyPow(bodyPow);
    float occludePow = m_skyDome->GetMoonOccludePow();
    if (ImGui::SliderFloat("Occlude Size", &occludePow, 400.0f, 3000.0f, "%.0f"))
        m_skyDome->SetMoonOccludePow(occludePow);
    float offsetAmt = m_skyDome->GetCrescentOffsetAmt();
    if (ImGui::SliderFloat("Crescent Ofs", &offsetAmt, 0.002f, 0.03f, "%.4f"))
        m_skyDome->SetCrescentOffsetAmt(offsetAmt);

    // --- Volumetric Clouds ---
    ImGui::Separator(); ImGui::TextColored({0.4f,0.8f,1.0f,1.0f}, "Clouds");
    ImGui::Checkbox("Enable Clouds", &m_volumetricClouds->enabled);
    if (m_volumetricClouds->enabled)
    {
        ImGui::Checkbox("Auto Coverage", &m_autoCloudCoverage);
        if (m_autoCloudCoverage)
        {
            ImGui::SameLine();
            ImGui::TextDisabled("(weather driven)");
            ImGui::Text("Coverage: %.2f  Clear: %.2f",
                m_volumetricClouds->cloudCoverage, m_rainbowCloudFactor);
        }
        else
            ImGui::SliderFloat("Coverage", &m_volumetricClouds->cloudCoverage, 0.0f, 1.0f, "%.2f");
        ImGui::SliderFloat("Density",  &m_volumetricClouds->densityMult,   0.1f,    3.0f,    "%.2f");
        ImGui::SliderFloat("Scale",    &m_volumetricClouds->cloudScale,    0.3f,    3.0f,    "%.2f");
        ImGui::SliderFloat("Base (m)", &m_volumetricClouds->cloudBase,     200.0f,  2000.0f, "%.0f");
        ImGui::SliderFloat("Top (m)",  &m_volumetricClouds->cloudTop,      500.0f,  5000.0f, "%.0f");
    }

    // --- Water ---
    ImGui::Separator(); ImGui::TextColored({0.4f,0.8f,1.0f,1.0f}, "Water");
    ImGui::SliderFloat("Body Str",    &m_pp->waterBodyStr,  0.0f, 3.0f,  "%.2f");
    ImGui::SliderFloat("Refraction",  &m_pp->waterRefract,  0.0f, 0.08f, "%.3f");
    ImGui::SliderFloat("Min Transmit",&m_pp->waterMinTrans, 0.0f, 0.6f,  "%.2f");

    // --- Camera / Showcase ---
    ImGui::Separator(); ImGui::TextColored({0.4f,0.8f,1.0f,1.0f}, "Camera");
    ImGui::SliderFloat("Vignette",   &m_pp->vignetteStrength, 0.0f, 1.5f,  "%.2f");
    ImGui::SliderFloat("Film Grain", &m_pp->grainStrength,    0.0f, 0.08f, "%.3f");
    if (m_renderer->IsShowcaseMode() || m_renderer->IsShipOrbitMode())
    {
        // Shared by both orbit modes (Showcase / Ship Orbit are mutually exclusive)
        float& h = m_renderer->GetCamera().m_showcaseHeight;
        float& s = m_renderer->GetCamera().m_showcaseSpeed;
        ImGui::SliderFloat("Cam Height", &h, -15.0f, 40.0f,  "%.1f m");
        ImGui::SliderFloat("Orbit Speed",&s,   0.0f,  1.5f,  "%.2f");
    }

    ImGui::End();

    // ============================================================
    // RIGHT PANEL — post-process parameters
    // ============================================================
    ImGui::SetNextWindowPos (ImVec2(W - kRightW, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(kRightW, kPanelH), ImGuiCond_Always);
    ImGui::Begin("Post Process", nullptr, kPanelFlags);

    // --- Exposure / Bloom ---
    ImGui::TextColored({1.0f,0.8f,0.4f,1.0f}, "Bloom");
    ImGui::Checkbox("Auto Exposure", &m_autoExposure);
    if (m_autoExposure) { ImGui::SameLine(); ImGui::Text("EV: %.2f", m_pp->exposure); }
    else                  ImGui::SliderFloat("Exposure",  &m_pp->exposure,       0.1f, 5.0f,  "%.2f");
    ImGui::Checkbox("Enable Bloom", &m_pp->bloomEnabled);
    if (m_pp->bloomEnabled)
    {
        ImGui::SliderFloat("Threshold", &m_pp->bloomThreshold, 0.5f, 5.0f, "%.2f");
        ImGui::SliderFloat("Strength",  &m_pp->bloomStrength,  0.1f, 3.0f, "%.2f");
    }

    // --- God Rays ---
    ImGui::Separator(); ImGui::TextColored({1.0f,0.8f,0.4f,1.0f}, "God Rays");
    ImGui::Checkbox("Enable##GR", &m_pp->godRaysEnabled);
    if (m_pp->godRaysEnabled)
        ImGui::SliderFloat("GR Strength", &m_pp->godRayStrength, 0.1f, 3.0f, "%.2f");

    // --- Lens Flare ---
    ImGui::Separator(); ImGui::TextColored({1.0f,0.8f,0.4f,1.0f}, "Lens Flare");
    ImGui::Checkbox("Enable##LF", &m_pp->lensFlareEnabled);
    if (m_pp->lensFlareEnabled)
        ImGui::SliderFloat("LF Strength", &m_pp->lensFlareStrength, 0.1f, 3.0f, "%.2f");

    // --- Depth of Field ---
    ImGui::Separator(); ImGui::TextColored({1.0f,0.8f,0.4f,1.0f}, "Depth of Field");
    ImGui::Checkbox("Enable DOF", &m_pp->dofEnabled);
    if (m_pp->dofEnabled)
    {
        ImGui::SliderFloat("Focus Depth", &m_pp->dofFocusDepth, 0.5f,  1.0f,   "%.3f");
        ImGui::SliderFloat("Focus Range", &m_pp->dofFocusRange, 0.02f, 0.5f,   "%.3f");
        ImGui::SliderFloat("Max Blur",    &m_pp->dofMaxRadius,  0.002f,0.025f, "%.4f");
    }

    // --- SSAO ---
    ImGui::Separator(); ImGui::TextColored({1.0f,0.8f,0.4f,1.0f}, "SSAO");
    ImGui::Checkbox("Enable SSAO", &m_pp->ssaoEnabled);
    if (m_pp->ssaoEnabled)
    {
        ImGui::SliderFloat("AO Strength", &m_pp->ssaoStrength, 0.0f, 1.5f, "%.2f");
        ImGui::SliderFloat("AO Radius",   &m_pp->ssaoRadius,   0.1f, 3.0f, "%.2f");
    }

    // --- SSR ---
    ImGui::Separator(); ImGui::TextColored({1.0f,0.8f,0.4f,1.0f}, "SSR");
    ImGui::SliderFloat("SSR Strength", &m_pp->ssrStrength, 0.0f, 1.0f, "%.2f");

    // --- TAA ---
    ImGui::Separator(); ImGui::TextColored({1.0f,0.8f,0.4f,1.0f}, "TAA");
    ImGui::Checkbox("Enable TAA", &m_pp->taaEnabled);
    if (!m_pp->taaEnabled) { m_currentJitter = { 0.0f, 0.0f }; m_renderer->SetJitter(0.0f, 0.0f); }
    if (m_pp->taaEnabled)
        ImGui::SliderFloat("History Blend", &m_pp->taaBlend, 0.5f, 0.98f, "%.2f");

    // --- Rainbow ---
    ImGui::Separator(); ImGui::TextColored({1.0f,0.8f,0.4f,1.0f}, "Rainbow");
    ImGui::Checkbox("Enable Rainbow", &m_rainbowEffect->enabled);
    if (m_rainbowEffect->enabled)
    {
        ImGui::SliderFloat("RB Intensity", &m_rainbowEffect->intensity,  0.0f, 3.0f, "%.2f");
        ImGui::SliderFloat("Band Width",   &m_rainbowEffect->bandWidth,   0.5f, 6.0f, "%.1f");
        ImGui::Checkbox("Secondary Bow",  &m_rainbowEffect->secondaryBow);
        ImGui::Text("Moisture: %.2f", m_rainMoisture);
    }

    // --- IBL ---
    ImGui::Separator(); ImGui::TextColored({0.4f,0.8f,1.0f,1.0f}, "IBL");
    ImGui::Checkbox("IBL Debug Preview", &m_showIBLDebug);

    ImGui::End();

    // ============================================================
    // BOTTOM BAR — buttons only
    // ============================================================
    ImGui::SetNextWindowPos (ImVec2(0, H - kBottomH), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(W, kBottomH),     ImGuiCond_Always);
    ImGui::Begin("##BottomBar", nullptr,
        kPanelFlags | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar);

    // 5 groups, evenly spaced across the full bar width
    const float kBarInner  = W - 20.0f;
    const float kGroupW    = kBarInner / 5.0f;
    const float kBarStartX = 10.0f;
    const float kBarY      = ImGui::GetCursorPosY();  // lock Y so all groups sit on the same line

    auto NextGroup = [&](int idx)
    {
        ImGui::SetCursorPos(ImVec2(kBarStartX + idx * kGroupW, kBarY));
    };

    // Group 0 — Time
    NextGroup(0);
    ImGui::TextDisabled("Time"); ImGui::SameLine();
    if (ImGui::Button(m_timePaused ? "Resume" : " Pause "))
        m_timePaused = !m_timePaused;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80.0f);
    ImGui::SliderFloat("##ts", &m_timeScale, 0.0f, 10.0f, "%.1fx");

    // Group 1 — Showcase / Ship Orbit (mutually exclusive)
    NextGroup(1);
    bool showcase  = m_renderer->IsShowcaseMode();
    bool shipOrbit = m_renderer->IsShipOrbitMode();
    ImGui::TextDisabled("View"); ImGui::SameLine();
    if (ImGui::Button(showcase ? "Showcase ON " : "Showcase OFF"))
    {
        if (showcase) { m_renderer->ToggleShowcase(); m_renderer->GetCamera().ExitShowcase(); }
        else
        {
            if (shipOrbit) { m_renderer->ToggleShipOrbit(); m_renderer->GetCamera().ExitShipOrbit(); }
            m_renderer->ToggleShowcase(); m_renderer->GetCamera().EnterShowcase();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(shipOrbit ? "Ship Orbit ON " : "Ship Orbit OFF"))
    {
        if (shipOrbit) { m_renderer->ToggleShipOrbit(); m_renderer->GetCamera().ExitShipOrbit(); }
        else
        {
            if (showcase) { m_renderer->ToggleShowcase(); m_renderer->GetCamera().ExitShowcase(); }
            m_renderer->ToggleShipOrbit(); m_renderer->GetCamera().EnterShipOrbit();
        }
    }

    // Group 2 — Weather
    NextGroup(2);
    bool isAuto = m_weatherSystem->IsAutoWeather();
    WeatherState cur = m_weatherSystem->GetCurrentState();
    int weatherIdx = isAuto ? 0
        : cur == WeatherState::Calm    ? 1
        : cur == WeatherState::Windy   ? 2
        : cur == WeatherState::Storm   ? 3
        : 4;
    const char* weatherLabels[] = { "Auto", "Calm", "Windy", "Storm", "Tsunami" };
    WeatherState weatherStates[] = { WeatherState::Calm, WeatherState::Calm,
                                     WeatherState::Windy, WeatherState::Storm,
                                     WeatherState::Tsunami };
    auto clickWeather = [&](int i)
    {
        if (i == 0) m_weatherSystem->SetAutoWeather(true);
        else
        {
            m_weatherSystem->SetAutoWeather(false);
            float ttime = (weatherStates[i] == WeatherState::Tsunami) ? 20.0f : 10.0f;
            m_weatherSystem->SetWeather(weatherStates[i], ttime);
        }
    };
    // Row 1: label + mild weathers
    ImGui::TextDisabled("Weather"); ImGui::SameLine();
    for (int i = 0; i <= 2; i++)
    {
        if (i > 0) ImGui::SameLine();
        if (ImGui::RadioButton(weatherLabels[i], weatherIdx == i)) clickWeather(i);
    }
    // Row 2: severe weathers — re-anchor to same X, one line down
    ImGui::SetCursorPos(ImVec2(kBarStartX + 2 * kGroupW, kBarY + ImGui::GetTextLineHeightWithSpacing()));
    for (int i = 3; i < 5; i++)
    {
        if (i > 3) ImGui::SameLine();
        if (ImGui::RadioButton(weatherLabels[i], weatherIdx == i)) clickWeather(i);
    }

    // Group 3 — Floating Boxes
    NextGroup(3);
    {
        int cnt   = (int)m_floatingObject->GetBoxCount();
        int uwCnt = m_floatingObject->GetUnderwaterBoxCount();
        ImGui::TextDisabled("Boxes"); ImGui::SameLine();
        ImGui::Text("%d/%d", cnt, FloatingObject::MAX_BOXES); ImGui::SameLine();
        if (ImGui::Button("Spawn##box") && cnt < FloatingObject::MAX_BOXES) m_floatingObject->SpawnBox();
        ImGui::SameLine();
        if (ImGui::Button("Clear##box")) m_floatingObject->ClearBoxes();
        ImGui::SameLine();
        ImGui::Text("UW %d/%d", uwCnt, FloatingObject::MAX_UW_BOXES); ImGui::SameLine();
        if (ImGui::Button("Spawn##uw") && uwCnt < FloatingObject::MAX_UW_BOXES) m_floatingObject->SpawnUnderwaterBox();
        ImGui::SameLine();
        if (ImGui::Button("Clear##uw")) m_floatingObject->ClearUnderwaterBoxes();
    }

    // Group 4 — Fish
    NextGroup(4);
    {
        int cnt = m_fishSchool->GetFishCount();
        ImGui::TextDisabled("Fish"); ImGui::SameLine();
        ImGui::Text("%d/%d", cnt, FishSchool::MAX_FISH); ImGui::SameLine();
        if (ImGui::Button("Spawn School") && cnt == 0) m_fishSchool->SpawnSchool();
        ImGui::SameLine();
        if (ImGui::Button("Clear Fish")) m_fishSchool->ClearFish();
    }

    ImGui::End();

    // ---- IBL Debug Preview (gated) ----
    if (!m_showIBLDebug) return;
    ImGui::SetNextWindowSize(ImVec2(340, 310), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(10, 280), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("IBL Debug", nullptr))
    {
        UINT descSize = m_device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        auto gpuBase = m_imguiSrvHeap->GetGPUDescriptorHandleForHeapStart();

        // 6 cubemap faces in a 2×3 grid
        const float sz = 90.0f;
        const char* faceNames[6] = { "+X", "-X", "+Y", "-Y", "+Z", "-Z" };
        for (int face = 0; face < 6; ++face)
        {
            D3D12_GPU_DESCRIPTOR_HANDLE h = { gpuBase.ptr + (UINT64)(face + 1) * descSize };
            ImTextureID texID = (ImTextureID)(void*)h.ptr;
            if (face % 2 != 0) ImGui::SameLine();
            ImGui::BeginGroup();
            ImGui::Text("%s", faceNames[face]);
            ImGui::Image(texID, ImVec2(sz, sz));
            ImGui::EndGroup();
            if (face % 2 == 0) ImGui::SameLine(sz + 16.0f);
        }

        ImGui::Separator();
        ImGui::Text("BRDF LUT (R=scale, G=bias)");
        D3D12_GPU_DESCRIPTOR_HANDLE lutH = { gpuBase.ptr + 7ull * descSize };
        ImGui::Image((ImTextureID)(void*)lutH.ptr, ImVec2(128, 128));
    }
    ImGui::End();
}

// ============================================================
// OnRender
// ============================================================
void D3D12HelloTriangle::OnRender()
{
    ThrowIfFailed(m_commandAllocator->Reset());
    ThrowIfFailed(m_commandList->Reset(m_commandAllocator.Get(), nullptr));

    PIXBeginEvent(m_commandList.Get(), PIX_COLOR(30, 144, 255), L"Frame");

    // ---- Compute: OceanFFT ----
    m_oceanFFT->Dispatch(m_commandList, m_renderer->GetTime());
    D3D12_RESOURCE_BARRIER toSRV[2] = {
        CD3DX12_RESOURCE_BARRIER::Transition(m_oceanFFT->GetHeightMap(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        CD3DX12_RESOURCE_BARRIER::Transition(m_oceanFFT->GetDztMap(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
    };
    m_commandList->ResourceBarrier(2, toSRV);

    // ---- Compute: IBL sky capture (rolling 6-frame cycle) ----
    PIXBeginEvent(m_commandList.Get(), PIX_COLOR(100, 180, 255), L"IBL Capture");
    m_iblSystem->Dispatch(m_commandList, m_skyDome.get(), m_renderer->GetTime());
    PIXEndEvent(m_commandList.Get());

    // ---- ShadowMap ----
    PIXBeginEvent(m_commandList.Get(), PIX_COLOR(80, 80, 80), L"ShadowMap");
    m_pp->RenderShadowMap(m_commandList.Get(), m_skyDome.get(), m_renderer.get(),
                          m_floatingObject.get(), m_shipModel.get());
    PIXEndEvent(m_commandList.Get());

    RenderContext ctx;
    ctx.cmd          = m_commandList.Get();
    ctx.renderTarget = m_renderTargets[m_frameIndex].Get();
    ctx.rtv          = m_pp->GetHDRRTV();
    ctx.viewport     = m_viewport;
    ctx.scissor      = m_scissorRect;
    ctx.dsv          = m_renderer->GetDSVHeap()->GetCPUDescriptorHandleForHeapStart();
    ctx.vb           = m_renderer->GetGridVBView();
    ctx.ib           = m_renderer->GetGridIBView();
    ctx.indexCount   = m_renderer->GetGridIndexCount();
    ctx.view         = m_renderer->GetViewMatrix();
    ctx.proj         = m_renderer->GetProjMatrix();

    auto barrierToRT = CD3DX12_RESOURCE_BARRIER::Transition(
        ctx.renderTarget,
        D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    m_commandList->ResourceBarrier(1, &barrierToRT);

    m_commandList->OMSetRenderTargets(1, &ctx.rtv, FALSE, &ctx.dsv);
    const float clearColor[] = { 0.0f, 0.2f, 0.4f, 1.0f };
    m_commandList->ClearRenderTargetView(ctx.rtv, clearColor, 0, nullptr);
    m_commandList->ClearDepthStencilView(ctx.dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    // ---- SkyDome ----
    PIXBeginEvent(m_commandList.Get(), PIX_COLOR(100, 180, 255), L"SkyDome");
    m_skyDome->SetShowcaseMode(m_renderer->IsShowcaseMode());
    m_skyDome->Render(ctx);
    PIXEndEvent(m_commandList.Get());

    // ---- Rainbow ----
    // Rendered before volumetric clouds so cloud alpha-blend naturally occludes it.
    PIXBeginEvent(m_commandList.Get(), PIX_COLOR(255, 180, 80), L"Rainbow");
    {
        RainbowEffect::Params rp;
        rp.cameraPos    = m_renderer->GetCameraPos();
        rp.sunDir       = m_skyDome->GetSunDirection();
        rp.rainFactor   = m_rainMoisture * m_rainbowCloudFactor;
        rp.sunIntensity = m_skyDome->GetSunIntensity();
        rp.view         = m_renderer->GetViewMatrix();
        rp.proj         = m_renderer->GetProjMatrix();
        m_rainbowEffect->Render(m_commandList.Get(), ctx.rtv, m_viewport, m_scissorRect, rp);
    }
    PIXEndEvent(m_commandList.Get());

    // ---- Volumetric Clouds ----
    PIXBeginEvent(m_commandList.Get(), PIX_COLOR(200, 220, 255), L"VolumetricClouds");
    {
        VolumetricClouds::Params cp;
        cp.cameraPos        = m_renderer->GetCameraPos();
        cp.time             = m_renderer->GetTime();
        cp.sunDir           = m_skyDome->GetSunDirection();
        cp.sunIntensity     = m_skyDome->GetSunIntensity();
        cp.sunColor         = m_skyDome->GetSunColor();
        cp.weatherIntensity = m_weatherSystem->GetWeatherIntensity();
        cp.windX            = m_oceanFFT->windDirX;
        cp.windZ            = m_oceanFFT->windDirY;
        float sunH          = m_skyDome->GetSunDirection().y;
        cp.nightFactor      = std::clamp(-sunH * 3.0f, 0.0f, 1.0f);
        cp.view             = m_renderer->GetViewMatrix();
        cp.proj             = m_renderer->GetProjMatrix();
        m_volumetricClouds->Render(
            m_commandList.Get(), ctx.rtv, m_viewport, m_scissorRect, cp);
    }
    PIXEndEvent(m_commandList.Get());

    m_pp->TakeSkySnapshot(m_commandList.Get());
    m_commandList->OMSetRenderTargets(1, &ctx.rtv, FALSE, &ctx.dsv);

    // ---- Underwater (FloatingObject + Fish) ----
    // Both read lighting through SkyDome's unified sun/moon-blended light (see
    // SkyDome::GetActiveLight*) instead of raw sun-only values:
    //  - FloatingObject previously went fully dark at night (sunIntensity -> 0, no
    //    moonlight contribution); now it stays moonlit like everything else.
    //  - FishSchool's bioluminescence tuning (fish.hlsl:90-92) was already written
    //    assuming the incoming intensity bottoms out at the moon's 0.15 ("nightBlend
    //    = 0.55 at night"), but was actually receiving raw GetSunIntensity() (bottoms
    //    out at 0, giving nightBlend = 1.0) — passing the blended intensity makes the
    //    effect match its own tuning comment.
    PIXBeginEvent(m_commandList.Get(), PIX_COLOR(0, 180, 160), L"Underwater");
    m_floatingObject->RenderUnderwater(ctx,
        m_skyDome->GetActiveLightDirection(), m_skyDome->GetActiveLightIntensity(),
        m_skyDome->GetActiveLightColor(), m_renderer->GetCameraPos(),
        m_iblSystem->GetSHCB());
    m_fishSchool->Render(ctx,
        m_skyDome->GetActiveLightDirection(), m_skyDome->GetActiveLightIntensity(),
        m_skyDome->GetActiveLightColor(), m_renderer->GetCameraPos(), m_renderer->GetTime(),
        m_iblSystem->GetSHCB());
    PIXEndEvent(m_commandList.Get());

    m_pp->TakeRefractionSnapshot(m_commandList.Get());
    m_commandList->OMSetRenderTargets(1, &ctx.rtv, FALSE, &ctx.dsv);

    m_commandList->SetGraphicsRootSignature(m_rootSignature.Get());

    auto* oceanHeap = m_pp->GetOceanSRVHeap();
    ID3D12DescriptorHeap* srvHeaps[] = { oceanHeap };
    m_commandList->SetDescriptorHeaps(1, srvHeaps);
    m_commandList->SetGraphicsRootDescriptorTable(1, oceanHeap->GetGPUDescriptorHandleForHeapStart());
    m_commandList->SetGraphicsRootConstantBufferView(2, m_rainSystem->GetRippleCBAddress());
    m_commandList->SetGraphicsRootConstantBufferView(3, m_pp->GetShadowSceneCBAddr());

    // ---- Ocean ----
    PIXBeginEvent(m_commandList.Get(), PIX_COLOR(0, 100, 200), L"Ocean");
    // Bind SHCB at slot 4 (b3) before Render(); Renderer calls SetGraphicsRootSignature
    // with the same m_rootSignature, which preserves already-bound root parameters.
    m_commandList->SetGraphicsRootConstantBufferView(4, m_iblSystem->GetSHCB());
    m_renderer->Render(ctx);
    // RenderWaterBox is for single-tile boundary box only; hidden in infinite ocean mode
    m_floatingObject->Render(ctx,
        m_skyDome->GetActiveLightDirection(), m_skyDome->GetActiveLightIntensity(),
        m_skyDome->GetActiveLightColor(), m_renderer->GetCameraPos(),
        m_iblSystem->GetSHCB());
    PIXEndEvent(m_commandList.Get());

    // ---- Ship ----
    PIXBeginEvent(m_commandList.Get(), PIX_COLOR(139, 90, 43), L"Ship");
    ShipCloudParams shipCloud {
        m_renderer->GetTime(),
        m_renderer->GetCloudCoverage(),    m_renderer->GetCloudScale(),
        m_renderer->GetCloudBase(),        m_renderer->GetCloudTop(),
        m_renderer->GetCloudWindX(),       m_renderer->GetCloudWindZ(),
        m_renderer->GetCloudDensityMult(), m_renderer->GetCloudEnabled()
    };
    // Single light source, smoothly blended between sun and moon by SkyDome
    // (see SkyDome::GetActiveLight* — keeps the ship's lighting in sync with the ocean
    // and prevents two simultaneously-active lights from opposite directions, which used
    // to leak "moonlight" onto the sunlit side of the hull at full daylight).
    m_shipModel->Render(ctx,
        m_skyDome->GetActiveLightDirection(), m_skyDome->GetActiveLightIntensity(),
        m_skyDome->GetActiveLightColor(),     m_renderer->GetCameraPos(),
        m_skyDome->GetLightningIntensity(),
        shipCloud,
        m_iblSystem->GetSHCB());
    // Restore ocean root signature and SRV heap after ship changes them
    m_commandList->SetGraphicsRootSignature(m_rootSignature.Get());
    {
        auto* shipRestoreHeap = m_pp->GetOceanSRVHeap();
        ID3D12DescriptorHeap* shipRestoreHeaps[] = { shipRestoreHeap };
        m_commandList->SetDescriptorHeaps(1, shipRestoreHeaps);
        m_commandList->SetGraphicsRootDescriptorTable(1, shipRestoreHeap->GetGPUDescriptorHandleForHeapStart());
        m_commandList->SetGraphicsRootConstantBufferView(2, m_rainSystem->GetRippleCBAddress());
        m_commandList->SetGraphicsRootConstantBufferView(3, m_pp->GetShadowSceneCBAddr());
    }
    PIXEndEvent(m_commandList.Get());

    // ---- Height readback for ship physics (1-frame latency) ----
    {
        constexpr float STEP = 20.0f;
        float sx  = m_shipModel->worldPos.x;
        float sz  = m_shipModel->worldPos.z;
        float yaw = m_shipModel->yaw;
        float bx  = -sinf(yaw), bz = cosf(yaw);   // bow direction
        float rx  =  cosf(yaw), rz = sinf(yaw);   // starboard direction
        m_oceanFFT->RecordHeightSamples(m_commandList.Get(),
            sx,                sz,
            sx + bx * STEP,    sz + bz * STEP,
            sx + rx * STEP,    sz + rz * STEP);
    }

    // ---- Rain ----
    PIXBeginEvent(m_commandList.Get(), PIX_COLOR(150, 210, 255), L"Rain");
    m_rainSystem->Render(ctx,
        m_renderer->GetViewMatrix(), m_renderer->GetProjMatrix(),
        m_renderer->GetCameraPos());
    PIXEndEvent(m_commandList.Get());

    // ---- Effects (Lightning, SSAO) ----
    PIXBeginEvent(m_commandList.Get(), PIX_COLOR(255, 255, 100), L"Effects");
    m_pp->RenderLightning(m_commandList.Get());
    m_pp->RenderSSAO(m_commandList.Get(), m_renderer.get());
    PIXEndEvent(m_commandList.Get());

    // ---- PostProcess ----
    PIXBeginEvent(m_commandList.Get(), PIX_COLOR(200, 100, 255), L"PostProcess");
    auto swapRTV = CD3DX12_CPU_DESCRIPTOR_HANDLE(
        m_rtvHeap->GetCPUDescriptorHandleForHeapStart(),
        m_frameIndex, m_rtvDescriptorSize);
    m_pp->RenderPostProcess(
        m_commandList.Get(), m_frameIndex, swapRTV,
        m_renderer.get(), m_skyDome.get());
    PIXEndEvent(m_commandList.Get());

    // ---- ImGui ----
    PIXBeginEvent(m_commandList.Get(), PIX_COLOR(180, 180, 180), L"ImGui");
    {
        // Capture cubemap lives in UAV between frames; transition to PSR so ImGui::Image() works.
        // BRDF LUT is permanently in PSR|NPSR (set by RunBRDFLutOnce) — no transition needed.
        auto iblIn = CD3DX12_RESOURCE_BARRIER::Transition(
            m_iblSystem->GetCaptureCubemap(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        m_commandList->ResourceBarrier(1, &iblIn);

        ID3D12DescriptorHeap* imguiHeaps[] = { m_imguiSrvHeap.Get() };
        m_commandList->SetDescriptorHeaps(1, imguiHeaps);
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), m_commandList.Get());

        // Restore capture cubemap to UAV for next frame's sky capture.
        auto iblOut = CD3DX12_RESOURCE_BARRIER::Transition(
            m_iblSystem->GetCaptureCubemap(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m_commandList->ResourceBarrier(1, &iblOut);
    }
    PIXEndEvent(m_commandList.Get());


    PIXEndEvent(m_commandList.Get()); // Frame

    auto barrierToPresent = CD3DX12_RESOURCE_BARRIER::Transition(
        ctx.renderTarget,
        D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    m_commandList->ResourceBarrier(1, &barrierToPresent);


    D3D12_RESOURCE_BARRIER toUAV[2] = {
        CD3DX12_RESOURCE_BARRIER::Transition(m_oceanFFT->GetHeightMap(),
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        CD3DX12_RESOURCE_BARRIER::Transition(m_oceanFFT->GetDztMap(),
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
    };
    m_commandList->ResourceBarrier(2, toUAV);

    ThrowIfFailed(m_commandList->Close());
    ID3D12CommandList* ppCommandLists[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);
    ThrowIfFailed(m_swapChain->Present(1, 0));
    WaitForPreviousFrame();
}

// ============================================================
// OnDestroy / OnMouseMove / WaitForPreviousFrame / OnKeyDown
// ============================================================
void D3D12HelloTriangle::OnDestroy()
{
    WaitForPreviousFrame();
    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CloseHandle(m_fenceEvent);
}

void D3D12HelloTriangle::OnMouseMove(float dx, float dy)
{
    if (ImGui::GetIO().WantCaptureMouse) return;
    m_renderer->OnMouseMove(dx, dy);
}

void D3D12HelloTriangle::WaitForPreviousFrame()
{
    const UINT64 fence = m_fenceValue;
    ThrowIfFailed(m_commandQueue->Signal(m_fence.Get(), fence));
    m_fenceValue++;
    if (m_fence->GetCompletedValue() < fence)
    {
        ThrowIfFailed(m_fence->SetEventOnCompletion(fence, m_fenceEvent));
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
}

void D3D12HelloTriangle::OnKeyDown(UINT8 key)
{
    if (key == VK_F11)    Win32Application::ToggleFullscreen();
    if (key == VK_TAB)    m_renderer->ToggleWireframe();
    if (key == VK_SPACE)  m_timePaused = !m_timePaused;

    if (key == 'V')
    {
        if (m_renderer->IsShowcaseMode())
        { m_renderer->ToggleShowcase(); m_renderer->GetCamera().ExitShowcase(); }
        else
        {
            if (m_renderer->IsShipOrbitMode())
            { m_renderer->ToggleShipOrbit(); m_renderer->GetCamera().ExitShipOrbit(); }
            m_renderer->ToggleShowcase(); m_renderer->GetCamera().EnterShowcase();
        }
    }
    if (key == '1') { m_weatherSystem->SetAutoWeather(false); m_weatherSystem->SetWeather(WeatherState::Calm,    10.0f); }
    if (key == '2') { m_weatherSystem->SetAutoWeather(false); m_weatherSystem->SetWeather(WeatherState::Windy,   10.0f); }
    if (key == '3') { m_weatherSystem->SetAutoWeather(false); m_weatherSystem->SetWeather(WeatherState::Storm,   10.0f); }
    if (key == '4') { m_weatherSystem->SetAutoWeather(false); m_weatherSystem->SetWeather(WeatherState::Tsunami, 20.0f); }
    if (key == '0')   m_weatherSystem->SetAutoWeather(true);
}
