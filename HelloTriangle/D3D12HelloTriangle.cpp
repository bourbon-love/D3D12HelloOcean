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
        heapDesc.NumDescriptors = 1;
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
        CD3DX12_ROOT_PARAMETER rootParameters[4];
        rootParameters[0].InitAsConstantBufferView(0);
        CD3DX12_DESCRIPTOR_RANGE srvRange;
        srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 5, 0);
        rootParameters[1].InitAsDescriptorTable(1, &srvRange, D3D12_SHADER_VISIBILITY_ALL);
        rootParameters[2].InitAsConstantBufferView(1);
        rootParameters[3].InitAsConstantBufferView(2);
        CD3DX12_STATIC_SAMPLER_DESC sampler(0,
            D3D12_FILTER_MIN_MAG_MIP_LINEAR,
            D3D12_TEXTURE_ADDRESS_MODE_WRAP,
            D3D12_TEXTURE_ADDRESS_MODE_WRAP,
            D3D12_TEXTURE_ADDRESS_MODE_WRAP);
        CD3DX12_ROOT_SIGNATURE_DESC rsd;
        rsd.Init(4, rootParameters, 1, &sampler,
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
    m_floatingObject->Init(m_device, m_oceanFFT->GetHeightMap(), pFOVS, foVsLen, pFOPS, foPsLen);
    free(pFOVS); free(pFOPS);

    // FishSchool
    UINT8 *pFishVS = nullptr, *pFishPS = nullptr;
    UINT   fishVsLen = 0, fishPsLen = 0;
    ThrowIfFailed(ReadDataFromFile(GetAssetFullPath(L"fish_FishVS.cso").c_str(), &pFishVS, &fishVsLen));
    ThrowIfFailed(ReadDataFromFile(GetAssetFullPath(L"fish_FishPS.cso").c_str(), &pFishPS, &fishPsLen));
    m_fishSchool = std::make_unique<FishSchool>();
    m_fishSchool->Init(m_device, pFishVS, fishVsLen, pFishPS, fishPsLen);
    free(pFishVS); free(pFishPS);


    ThrowIfFailed(m_device->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        m_commandAllocator.Get(),
        m_renderer->GetPSO(),
        IID_PPV_ARGS(&m_commandList)));
    m_skyDome->InitResources(m_commandList);
    m_renderer->InitResources(m_commandList);
    m_floatingObject->InitBuffers(m_commandList);
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
        m_floatingObject.get());
}

// ============================================================
// OnUpdate
// ============================================================
void D3D12HelloTriangle::OnUpdate()
{
    auto currentTime = std::chrono::steady_clock::now();
    float deltaTime  = std::chrono::duration<float>(currentTime - m_lastTime).count();
    m_lastTime = currentTime;

    float scaledDt         = m_timePaused ? 0.0f : deltaTime * m_timeScale;
    float weatherIntensity = m_weatherSystem->GetWeatherIntensity();

    m_skyDome->Update(scaledDt);
    m_skyDome->SetCameraY(m_renderer->GetCameraPos().y);


    if (m_autoExposure)
    {
        float sunH    = m_skyDome->GetSunDirection().y;
        float weatherI = m_weatherSystem->GetWeatherIntensity();
        float target;
        if      (sunH > 0.25f)  target = 1.0f;
        else if (sunH > 0.0f)   target = 1.0f + (1.6f - 1.0f) * (1.0f - sunH / 0.25f);
        else if (sunH > -0.15f) target = 1.6f + (2.2f - 1.6f) * (-sunH / 0.15f);
        else                    target = 2.5f;
        target += weatherI * 0.4f;
        target = target < 0.3f ? 0.3f : (target > 5.0f ? 5.0f : target);
        float speed = (target < m_pp->exposure) ? 1.0f : 0.35f;
        float dt_clamped = speed * deltaTime < 1.0f ? speed * deltaTime : 1.0f;
        m_pp->exposure += (target - m_pp->exposure) * dt_clamped;
    }

    m_renderer->Update(scaledDt);
    m_weatherSystem->Update(scaledDt);
    m_rainSystem->Update(scaledDt, weatherIntensity,
        m_oceanFFT->windDirX, m_oceanFFT->windDirY);

    m_renderer->SetSSRMix(m_pp->ssrStrength);
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
    ImGui::Begin("Scene Controls");

    ImGui::Text("FPS: %.1f  (%.2f ms)", ImGui::GetIO().Framerate, 1000.0f / ImGui::GetIO().Framerate);

    ImGui::Separator(); ImGui::Text("--- Time ---");
    ImGui::SliderFloat("Time Scale", &m_timeScale, 0.0f, 10.0f, "%.2f x");
    ImGui::SameLine();
    if (ImGui::Button(m_timePaused ? "Resume" : "Pause"))
        m_timePaused = !m_timePaused;

    ImGui::Separator(); ImGui::Text("--- Weather ---");
    ImGui::SameLine();
    bool showcase = m_renderer->IsShowcaseMode();
    if (ImGui::Button(showcase ? "Showcase: ON" : "Showcase: OFF"))
    {
        if (showcase) { m_renderer->ToggleShowcase(); m_renderer->GetCamera().ExitShowcase(); }
        else          { m_renderer->ToggleShowcase(); m_renderer->GetCamera().EnterShowcase(); }
    }
    if (showcase)
    {
        float& h = m_renderer->GetCamera().m_showcaseHeight;
        ImGui::SliderFloat("Cam Height", &h, -15.0f, 40.0f, "%.1f m");
    }

    bool isAuto = m_weatherSystem->IsAutoWeather();
    WeatherState cur = m_weatherSystem->GetCurrentState();
    int weatherIdx = isAuto ? 0 : (cur == WeatherState::Calm ? 1 : cur == WeatherState::Windy ? 2 : 3);
    const char* weatherLabels[] = { "Auto", "Calm", "Windy", "Storm" };
    for (int i = 0; i < 4; i++)
    {
        if (i > 0) ImGui::SameLine();
        if (ImGui::RadioButton(weatherLabels[i], weatherIdx == i))
        {
            if (i == 0) { m_weatherSystem->SetAutoWeather(true); }
            else
            {
                m_weatherSystem->SetAutoWeather(false);
                WeatherState states[] = { WeatherState::Calm, WeatherState::Calm, WeatherState::Windy, WeatherState::Storm };
                m_weatherSystem->SetWeather(states[i], 3.0f);
            }
        }
    }

    ImGui::Separator(); ImGui::Text("--- Moon ---");
    float rotSpeed = m_skyDome->GetCrescentRotSpeed();
    if (ImGui::SliderFloat("Crescent Rot Speed", &rotSpeed, 0.0f, 0.5f, "%.3f"))
        m_skyDome->SetCrescentRotSpeed(rotSpeed);
    float bodyPow = m_skyDome->GetMoonBodyPow();
    if (ImGui::SliderFloat("Moon Size (exp)", &bodyPow, 300.0f, 2000.0f, "%.0f"))
        m_skyDome->SetMoonBodyPow(bodyPow);
    float occludePow = m_skyDome->GetMoonOccludePow();
    if (ImGui::SliderFloat("Occlude Size (exp)", &occludePow, 400.0f, 3000.0f, "%.0f"))
        m_skyDome->SetMoonOccludePow(occludePow);
    float offsetAmt = m_skyDome->GetCrescentOffsetAmt();
    if (ImGui::SliderFloat("Crescent Offset", &offsetAmt, 0.002f, 0.03f, "%.4f"))
        m_skyDome->SetCrescentOffsetAmt(offsetAmt);

    ImGui::Separator(); ImGui::Text("--- Bloom ---");
    ImGui::Checkbox("Auto Exposure", &m_autoExposure);
    ImGui::SameLine();
    if (m_autoExposure) ImGui::Text("EV: %.2f", m_pp->exposure);
    else                ImGui::SliderFloat("Exposure", &m_pp->exposure, 0.1f, 5.0f, "%.2f");
    ImGui::Checkbox("Enable Bloom", &m_pp->bloomEnabled);
    if (m_pp->bloomEnabled)
    {
        ImGui::SliderFloat("Threshold", &m_pp->bloomThreshold, 0.5f, 5.0f, "%.2f");
        ImGui::SliderFloat("Strength",  &m_pp->bloomStrength,  0.1f, 3.0f, "%.2f");
    }
    ImGui::Checkbox("God Rays", &m_pp->godRaysEnabled);
    if (m_pp->godRaysEnabled)
        ImGui::SliderFloat("GR Strength", &m_pp->godRayStrength, 0.1f, 3.0f, "%.2f");

    ImGui::Separator(); ImGui::Text("--- Depth of Field ---");
    ImGui::Checkbox("Enable DOF", &m_pp->dofEnabled);
    if (m_pp->dofEnabled)
    {
        ImGui::SliderFloat("Focus Depth",  &m_pp->dofFocusDepth, 0.5f, 1.0f,  "%.3f");
        ImGui::SliderFloat("Focus Range",  &m_pp->dofFocusRange, 0.02f, 0.5f, "%.3f");
        ImGui::SliderFloat("Max Blur",     &m_pp->dofMaxRadius,  0.002f, 0.025f, "%.4f");
    }

    ImGui::Separator(); ImGui::Text("--- Camera ---");
    ImGui::SliderFloat("Vignette",   &m_pp->vignetteStrength, 0.0f, 1.5f, "%.2f");
    ImGui::SliderFloat("Film Grain", &m_pp->grainStrength,    0.0f, 0.08f, "%.3f");

    ImGui::Separator(); ImGui::Text("--- SSAO ---");
    ImGui::Checkbox("Enable SSAO", &m_pp->ssaoEnabled);
    if (m_pp->ssaoEnabled)
    {
        ImGui::SliderFloat("AO Strength", &m_pp->ssaoStrength, 0.0f, 1.5f, "%.2f");
        ImGui::SliderFloat("AO Radius",   &m_pp->ssaoRadius,   0.1f, 3.0f, "%.2f");
    }

    ImGui::Separator(); ImGui::Text("--- SSR ---");
    ImGui::SliderFloat("SSR Strength", &m_pp->ssrStrength, 0.0f, 1.0f, "%.2f");

    ImGui::Separator(); ImGui::Text("--- Water ---");
    ImGui::SliderFloat("Body Strength", &m_pp->waterBodyStr,  0.0f, 3.0f,  "%.2f");
    ImGui::SliderFloat("Refraction",    &m_pp->waterRefract,  0.0f, 0.08f, "%.3f");
    ImGui::SliderFloat("Min Transmit",  &m_pp->waterMinTrans, 0.0f, 0.6f,  "%.2f");

    ImGui::Separator(); ImGui::Text("--- TAA ---");
    ImGui::Checkbox("Enable TAA", &m_pp->taaEnabled);
    if (!m_pp->taaEnabled) { m_currentJitter = { 0.0f, 0.0f }; m_renderer->SetJitter(0.0f, 0.0f); }
    if (m_pp->taaEnabled)
        ImGui::SliderFloat("History Blend", &m_pp->taaBlend, 0.5f, 0.98f, "%.2f");

    ImGui::Separator(); ImGui::Text("--- Floating Boxes ---");
    {
        int count = (int)m_floatingObject->GetBoxCount();
        ImGui::Text("Surface: %d / %d", count, FloatingObject::MAX_BOXES);
        ImGui::SameLine();
        if (ImGui::Button("Spawn Box") && count < FloatingObject::MAX_BOXES) m_floatingObject->SpawnBox();
        ImGui::SameLine();
        if (ImGui::Button("Clear")) m_floatingObject->ClearBoxes();

        int uwCount = m_floatingObject->GetUnderwaterBoxCount();
        ImGui::Text("Underwater: %d / %d", uwCount, FloatingObject::MAX_UW_BOXES);
        ImGui::SameLine();
        if (ImGui::Button("Spawn UW Box") && uwCount < FloatingObject::MAX_UW_BOXES)
            m_floatingObject->SpawnUnderwaterBox();
        ImGui::SameLine();
        if (ImGui::Button("Clear UW")) m_floatingObject->ClearUnderwaterBoxes();
    }

    ImGui::Separator(); ImGui::Text("--- Fish School ---");
    {
        int cnt = m_fishSchool->GetFishCount();
        ImGui::Text("Fish: %d / %d", cnt, FishSchool::MAX_FISH);
        ImGui::SameLine();
        if (ImGui::Button("Spawn School") && cnt == 0) m_fishSchool->SpawnSchool();
        ImGui::SameLine();
        if (ImGui::Button("Clear Fish")) m_fishSchool->ClearFish();
    }

    ImGui::Separator(); ImGui::Text("--- Volumetric Clouds ---");
    ImGui::Checkbox("Enable Clouds", &m_volumetricClouds->enabled);
    if (m_volumetricClouds->enabled)
    {
        ImGui::SliderFloat("Coverage",    &m_volumetricClouds->cloudCoverage, 0.0f, 1.0f, "%.2f");
        ImGui::SliderFloat("Density",     &m_volumetricClouds->densityMult,   0.1f, 3.0f, "%.2f");
        ImGui::SliderFloat("Scale",       &m_volumetricClouds->cloudScale,    0.3f, 3.0f, "%.2f");
        ImGui::SliderFloat("Base (m)",    &m_volumetricClouds->cloudBase,     200.0f, 2000.0f, "%.0f");
        ImGui::SliderFloat("Top (m)",     &m_volumetricClouds->cloudTop,      500.0f, 5000.0f, "%.0f");
    }

    ImGui::Separator(); ImGui::Text("--- Lens Flare ---");
    ImGui::Checkbox("Enable Lens Flare", &m_pp->lensFlareEnabled);
    if (m_pp->lensFlareEnabled)
        ImGui::SliderFloat("LF Strength", &m_pp->lensFlareStrength, 0.1f, 3.0f, "%.2f");

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

    // ---- ShadowMap ----
    PIXBeginEvent(m_commandList.Get(), PIX_COLOR(80, 80, 80), L"ShadowMap");
    m_pp->RenderShadowMap(m_commandList.Get(), m_skyDome.get(), m_renderer.get(), m_floatingObject.get());
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
    PIXBeginEvent(m_commandList.Get(), PIX_COLOR(0, 180, 160), L"Underwater");
    m_floatingObject->RenderUnderwater(ctx,
        m_skyDome->GetSunDirection(), m_skyDome->GetSunIntensity(),
        m_skyDome->GetSunColor(), m_renderer->GetCameraPos());
    m_fishSchool->Render(ctx,
        m_skyDome->GetSunDirection(), m_skyDome->GetSunIntensity(),
        m_skyDome->GetSunColor(), m_renderer->GetCameraPos(), m_renderer->GetTime());
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
    m_renderer->Render(ctx);
    // RenderWaterBox is for single-tile boundary box only; hidden in infinite ocean mode
    m_floatingObject->Render(ctx,
        m_skyDome->GetSunDirection(), m_skyDome->GetSunIntensity(),
        m_skyDome->GetSunColor(), m_renderer->GetCameraPos());
    PIXEndEvent(m_commandList.Get());

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
        ID3D12DescriptorHeap* imguiHeaps[] = { m_imguiSrvHeap.Get() };
        m_commandList->SetDescriptorHeaps(1, imguiHeaps);
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), m_commandList.Get());
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
    if (key == VK_F11) Win32Application::ToggleFullscreen();
    if (key == VK_TAB) m_renderer->ToggleWireframe();

    if (key == 'V')
    {
        if (m_renderer->IsShowcaseMode())
        { m_renderer->ToggleShowcase(); m_renderer->GetCamera().ExitShowcase(); }
        else
        { m_renderer->ToggleShowcase(); m_renderer->GetCamera().EnterShowcase(); }
    }
    if (key == '1') { m_weatherSystem->SetAutoWeather(false); m_weatherSystem->SetWeather(WeatherState::Calm,  5.0f); }
    if (key == '2') { m_weatherSystem->SetAutoWeather(false); m_weatherSystem->SetWeather(WeatherState::Windy, 5.0f); }
    if (key == '3') { m_weatherSystem->SetAutoWeather(false); m_weatherSystem->SetWeather(WeatherState::Storm, 5.0f); }
    if (key == '4')   m_weatherSystem->SetAutoWeather(true);
}
