//*********************************************************
//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
//*********************************************************

#include "D3D12HelloTriangle.h"
#include <d3dx12_root_signature.h>
#include <d3dx12_barriers.h>
#include <string>

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

    // ImGui 初始化
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

// Load the rendering pipeline dependencies.
void D3D12HelloTriangle::LoadPipeline()
{
    UINT dxgiFactoryFlags = 0;

#if defined(_DEBUG)
    // Enable the debug layer (requires the Graphics Tools "optional feature").
    // NOTE: Enabling the debug layer after device creation will invalidate the active device.
    {
        ComPtr<ID3D12Debug> debugController;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
        {
            debugController->EnableDebugLayer();

            // Enable additional debug layers.
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

        ThrowIfFailed(D3D12CreateDevice(
            warpAdapter.Get(),
            D3D_FEATURE_LEVEL_11_0,
            IID_PPV_ARGS(&m_device)
            ));
    }
    else
    {
        ComPtr<IDXGIAdapter1> hardwareAdapter;
        GetHardwareAdapter(factory.Get(), &hardwareAdapter);

        ThrowIfFailed(D3D12CreateDevice(
            hardwareAdapter.Get(),
            D3D_FEATURE_LEVEL_11_0,
            IID_PPV_ARGS(&m_device)
            ));
    }

    // Describe and create the command queue.
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

    ThrowIfFailed(m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_commandQueue)));

    // Describe and create the swap chain.
    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
    swapChainDesc.BufferCount = FrameCount;
    swapChainDesc.Width = m_width;
    swapChainDesc.Height = m_height;
    swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.SampleDesc.Count = 1;

    ComPtr<IDXGISwapChain1> swapChain;
    ThrowIfFailed(factory->CreateSwapChainForHwnd(
        m_commandQueue.Get(),        // Swap chain needs the queue so that it can force a flush on it.
        Win32Application::GetHwnd(),
        &swapChainDesc,
        nullptr,
        nullptr,
        &swapChain
        ));

    // This sample does not support fullscreen transitions.
    ThrowIfFailed(factory->MakeWindowAssociation(Win32Application::GetHwnd(), DXGI_MWA_NO_ALT_ENTER));

    ThrowIfFailed(swapChain.As(&m_swapChain));
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

    // Create descriptor heaps.
    {
        // Describe and create a render target view (RTV) descriptor heap.
        D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
        rtvHeapDesc.NumDescriptors = FrameCount + 5; // +2 bloom, +1 HDR, +1 god ray, +1 DOF
        rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        ThrowIfFailed(m_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_rtvHeap)));

        m_rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    }

    // Create frame resources.
    {
        CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(m_rtvHeap->GetCPUDescriptorHandleForHeapStart());

        // Create a RTV for each frame.
        for (UINT n = 0; n < FrameCount; n++)
        {
            ThrowIfFailed(m_swapChain->GetBuffer(n, IID_PPV_ARGS(&m_renderTargets[n])));
            m_device->CreateRenderTargetView(m_renderTargets[n].Get(), nullptr, rtvHandle);
            rtvHandle.Offset(1, m_rtvDescriptorSize);
        }
    }

    ThrowIfFailed(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_commandAllocator)));
}

// Load the sample assets.
void D3D12HelloTriangle::LoadAssets()
{
    // Root Signature
    {
        CD3DX12_ROOT_PARAMETER rootParameters[3];
        rootParameters[0].InitAsConstantBufferView(0);//b0 cbv

        CD3DX12_DESCRIPTOR_RANGE srvRange;
        srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0); // t0, t1, t2(skySnapshot)
        rootParameters[1].InitAsDescriptorTable(1, &srvRange, D3D12_SHADER_VISIBILITY_ALL);
        rootParameters[2].InitAsConstantBufferView(1); // b1 涟漪CB
        CD3DX12_STATIC_SAMPLER_DESC sampler(
            0, 
            D3D12_FILTER_MIN_MAG_MIP_LINEAR,
            D3D12_TEXTURE_ADDRESS_MODE_WRAP, //u
            D3D12_TEXTURE_ADDRESS_MODE_WRAP, //v
            D3D12_TEXTURE_ADDRESS_MODE_WRAP  //w
            );

        CD3DX12_ROOT_SIGNATURE_DESC rootSignatureDesc;
        rootSignatureDesc.Init(3, rootParameters, 1, &sampler,
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
        ComPtr<ID3DBlob> signature, error;
        ThrowIfFailed(D3D12SerializeRootSignature(&rootSignatureDesc,
            D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error));
        ThrowIfFailed(m_device->CreateRootSignature(0,
            signature->GetBufferPointer(), signature->GetBufferSize(),
            IID_PPV_ARGS(&m_rootSignature)));
    }

    // 读取Phillips CS
    UINT8* pPhillipsCSData = nullptr;
    UINT   phillipsCsLen = 0;
    ThrowIfFailed(ReadDataFromFile(
        GetAssetFullPath(L"PhillipsCS.cso").c_str(),
        &pPhillipsCSData, &phillipsCsLen));

    // 读取时域演化CS和IFFT CS
    UINT8* pTimeEvoCSData = nullptr;
    UINT8* pIFFTCSData = nullptr;
    UINT   timeEvoCsLen = 0, ifftCsLen = 0;

    ThrowIfFailed(ReadDataFromFile(
        GetAssetFullPath(L"TimeEvolutionCS.cso").c_str(),
        &pTimeEvoCSData, &timeEvoCsLen));
    ThrowIfFailed(ReadDataFromFile(
        GetAssetFullPath(L"IFFTCS.cso").c_str(),
        &pIFFTCSData, &ifftCsLen));
    m_oceanFFT = std::make_unique<OceanFFT>();
    m_oceanFFT->Init(
        m_device, m_commandQueue, 256,
        pPhillipsCSData, phillipsCsLen,
        pTimeEvoCSData, timeEvoCsLen,   
        pIFFTCSData, ifftCsLen);     

    // 读取Shader字节码
    UINT8* pVertexShaderData = nullptr;
    UINT8* pPixelShaderData = nullptr;
    UINT   vsLen = 0, psLen = 0;
    ThrowIfFailed(ReadDataFromFile(
        GetAssetFullPath(L"shaders_VSMain.cso").c_str(),
        &pVertexShaderData, &vsLen));
    ThrowIfFailed(ReadDataFromFile(
        GetAssetFullPath(L"shaders_PSMain.cso").c_str(),
        &pPixelShaderData, &psLen));

	// 读取水体边界Shader字节码
    UINT8* pBoxVSData = nullptr;
    UINT8* pBoxPSData = nullptr;
    UINT   boxVsLen = 0, boxPsLen = 0;
    ThrowIfFailed(ReadDataFromFile(
        GetAssetFullPath(L"waterbody_VSMain.cso").c_str(),
        &pBoxVSData, &boxVsLen));
    ThrowIfFailed(ReadDataFromFile(
        GetAssetFullPath(L"waterbody_PSMain.cso").c_str(),
        &pBoxPSData, &boxPsLen));

        //创建PSO（此时还没有CommandList）
        m_renderer = std::make_unique<Renderer>();
        m_renderer->InitPSO(
            m_device, m_rootSignature,
            m_width, m_height,
            pVertexShaderData, vsLen,
            pPixelShaderData, psLen,
            pBoxVSData, boxVsLen,
            pBoxPSData, boxPsLen);


        // 读取天空Shader字节码
        UINT8* pSkyVSData = nullptr;
        UINT8* pSkyPSData = nullptr;
        UINT   skyVsLen = 0, skyPsLen = 0;
        ThrowIfFailed(ReadDataFromFile(
            GetAssetFullPath(L"skyshaders_VSMain.cso").c_str(),
            &pSkyVSData, &skyVsLen));
        ThrowIfFailed(ReadDataFromFile(
            GetAssetFullPath(L"skyshaders_PSMain.cso").c_str(),
            &pSkyPSData, &skyPsLen));

        m_skyDome = std::make_unique<SkyDome>();
        m_skyDome->InitPSO(
            m_device, m_rootSignature,
            m_width, m_height,
            pSkyVSData, skyVsLen,
            pSkyPSData, skyPsLen);

        UINT8* pRainVSData = nullptr;
        UINT8* pRainPSData = nullptr;
        UINT   rainVsLen = 0, rainPsLen = 0;
        ThrowIfFailed(ReadDataFromFile(
            GetAssetFullPath(L"rain_VSMain.cso").c_str(),
            &pRainVSData, &rainVsLen));
        ThrowIfFailed(ReadDataFromFile(
            GetAssetFullPath(L"rain_PSMain.cso").c_str(),
            &pRainPSData, &rainPsLen));

        m_rainSystem = std::make_unique<RainSystem>();
        m_rainSystem->Init(m_device, m_rootSignature,
            pRainVSData, rainVsLen,
            pRainPSData, rainPsLen);
        m_rainSystem->InitResources(m_commandList);


        m_weatherSystem = std::make_unique<WeatherSystem>();
        m_weatherSystem->Init(m_oceanFFT.get(),m_skyDome.get());

        // PSO创建CommandList）
        ThrowIfFailed(m_device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            m_commandAllocator.Get(),
            m_renderer->GetPSO(),   
            IID_PPV_ARGS(&m_commandList)));
        //录制Grid上传命令
        m_skyDome->InitResources(m_commandList);
        m_renderer->InitResources(m_commandList);
    

    // Create synchronization objects and wait until assets have been uploaded to the GPU.
    
        ThrowIfFailed(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)));
        m_fenceValue = 1;

        // Create an event handle to use for frame synchronization.
        m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (m_fenceEvent == nullptr)
        {
            ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()));
        }

        // Grid上传命令已录制完，关闭并提交
        ThrowIfFailed(m_commandList->Close());
        ID3D12CommandList* ppInitCmds[] = { m_commandList.Get() };
        m_commandQueue->ExecuteCommandLists(_countof(ppInitCmds), ppInitCmds);
        // Wait for the command list to execute; we are reusing the same command
        // list in our main loop but for now, we just want to wait for setup to
        // complete before continuing.
        WaitForPreviousFrame();

    InitBloom();
    InitHDR();
    InitGodRays();
    InitLensFlare();
    InitSSR();
    InitDOF();
}

// Update frame-based values.
void D3D12HelloTriangle::OnUpdate()
{
	auto currentTime = std::chrono::steady_clock::now();
	float deltaTime = std::chrono::duration<float>(currentTime - m_lastTime).count();
	m_lastTime = currentTime;

    float scaledDt = m_timePaused ? 0.0f : deltaTime * m_timeScale;
    float weatherIntensity = m_weatherSystem->GetWeatherIntensity();

    m_skyDome->Update(scaledDt);

    // Auto exposure: smooth adaptation to sun height (uses real deltaTime, not scaled)
    if (m_autoExposure)
    {
        float sunH = m_skyDome->GetSunDirection().y;
        float weatherI = m_weatherSystem->GetWeatherIntensity();

        float target;
        if      (sunH > 0.25f)  target = 1.0f;
        else if (sunH > 0.0f)   target = 1.0f + (1.6f - 1.0f) * (1.0f - sunH / 0.25f);
        else if (sunH > -0.15f) target = 1.6f + (2.2f - 1.6f) * (-sunH / 0.15f);
        else                    target = 2.5f;

        target += weatherI * 0.4f; // storm → darker scene → more exposure
        target = target < 0.3f ? 0.3f : (target > 5.0f ? 5.0f : target);

        // Asymmetric speed: faster to adapt to bright (pupil constricts fast)
        float speed = (target < m_exposure) ? 1.0f : 0.35f;
        m_exposure += (target - m_exposure) * (speed * deltaTime < 1.0f ? speed * deltaTime : 1.0f);
    }

    m_renderer->Update(scaledDt);
    m_weatherSystem->Update(scaledDt);
    m_rainSystem->Update(scaledDt, weatherIntensity,
        m_oceanFFT->windDirX, m_oceanFFT->windDirY);

    m_renderer->SetSSRMix(m_ssrStrength);

    // ImGui 帧
    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame(1.0f, 1.0f);
    ImGui::NewFrame();
    BuildImGuiUI();
    ImGui::Render();
}

void D3D12HelloTriangle::BuildImGuiUI()
{
    ImGui::Begin("Scene Controls");

    // ---- Performance ----
    ImGui::Text("FPS: %.1f  (%.2f ms)", ImGui::GetIO().Framerate, 1000.0f / ImGui::GetIO().Framerate);

    // ---- Time ----
    ImGui::Separator(); ImGui::Text("--- Time ---");
    ImGui::SliderFloat("Time Scale", &m_timeScale, 0.0f, 10.0f, "%.2f x");
    ImGui::SameLine();
    if (ImGui::Button(m_timePaused ? "Resume" : "Pause"))
        m_timePaused = !m_timePaused;

    // ---- Weather ----
    ImGui::Separator(); ImGui::Text("--- Weather ---");
    ImGui::SameLine();
    bool showcase = m_renderer->IsShowcaseMode();
    if (ImGui::Button(showcase ? "Showcase: ON" : "Showcase: OFF"))
    {
        if (showcase)
        {
            m_renderer->ToggleShowcase();
            m_renderer->GetCamera().ExitShowcase();
        }
        else
        {
            m_renderer->ToggleShowcase();
            m_renderer->GetCamera().EnterShowcase();
        }
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
            if (i == 0)
            {
                m_weatherSystem->SetAutoWeather(true);
            }
            else
            {
                m_weatherSystem->SetAutoWeather(false);
                WeatherState states[] = { WeatherState::Calm, WeatherState::Calm, WeatherState::Windy, WeatherState::Storm };
                m_weatherSystem->SetWeather(states[i], 3.0f);
            }
        }
    }

    // ---- Moon ----
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

    // ---- Bloom ----
    ImGui::Separator(); ImGui::Text("--- Bloom ---");
    ImGui::Checkbox("Auto Exposure", &m_autoExposure);
    ImGui::SameLine();
    if (m_autoExposure)
        ImGui::Text("EV: %.2f", m_exposure);
    else
        ImGui::SliderFloat("Exposure", &m_exposure, 0.1f, 5.0f, "%.2f");
    ImGui::Checkbox("Enable Bloom", &m_bloomEnabled);
    if (m_bloomEnabled)
    {
        ImGui::SliderFloat("Threshold", &m_bloomThreshold, 0.5f, 5.0f,  "%.2f");
        ImGui::SliderFloat("Strength",  &m_bloomStrength,  0.1f, 3.0f,  "%.2f");
    }
    ImGui::Checkbox("God Rays", &m_godRaysEnabled);
    if (m_godRaysEnabled)
        ImGui::SliderFloat("GR Strength", &m_godRayStrength, 0.1f, 3.0f, "%.2f");

    // ---- Depth of Field ----
    ImGui::Separator(); ImGui::Text("--- Depth of Field ---");
    ImGui::Checkbox("Enable DOF", &m_dofEnabled);
    if (m_dofEnabled)
    {
        ImGui::SliderFloat("Focus Depth",  &m_dofFocusDepth, 0.5f, 1.0f,  "%.3f");
        ImGui::SliderFloat("Focus Range",  &m_dofFocusRange, 0.02f, 0.5f, "%.3f");
        ImGui::SliderFloat("Max Blur",     &m_dofMaxRadius,  0.002f, 0.025f, "%.4f");
    }

    // ---- Camera ----
    ImGui::Separator(); ImGui::Text("--- Camera ---");
    ImGui::SliderFloat("Vignette",   &m_vignetteStrength, 0.0f, 1.5f, "%.2f");
    ImGui::SliderFloat("Film Grain", &m_grainStrength,    0.0f, 0.08f, "%.3f");

    // ---- SSR ----
    ImGui::Separator(); ImGui::Text("--- SSR ---");
    ImGui::SliderFloat("SSR Strength", &m_ssrStrength, 0.0f, 1.0f, "%.2f");

    // ---- Lens Flare ----
    ImGui::Separator(); ImGui::Text("--- Lens Flare ---");
    ImGui::Checkbox("Enable Lens Flare", &m_lensFlareEnabled);
    if (m_lensFlareEnabled)
        ImGui::SliderFloat("LF Strength", &m_lensFlareStrength, 0.1f, 3.0f, "%.2f");

    ImGui::End();
}

void D3D12HelloTriangle::InitBloom()
{
    // --- Bloom RTs (R16F to preserve HDR values through blur) ---
    {
        auto heapProp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        auto rtDesc   = CD3DX12_RESOURCE_DESC::Tex2D(
            DXGI_FORMAT_R16G16B16A16_FLOAT, m_width, m_height, 1, 1);
        rtDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE clearVal = {};
        clearVal.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;

        ThrowIfFailed(m_device->CreateCommittedResource(
            &heapProp, D3D12_HEAP_FLAG_NONE, &rtDesc,
            D3D12_RESOURCE_STATE_RENDER_TARGET, &clearVal,
            IID_PPV_ARGS(&m_bloomExtractRT)));
        ThrowIfFailed(m_device->CreateCommittedResource(
            &heapProp, D3D12_HEAP_FLAG_NONE, &rtDesc,
            D3D12_RESOURCE_STATE_RENDER_TARGET, &clearVal,
            IID_PPV_ARGS(&m_bloomBlurRT)));
    }

    // --- RTVs in slots 2 and 3 ---
    {
        CD3DX12_CPU_DESCRIPTOR_HANDLE h(
            m_rtvHeap->GetCPUDescriptorHandleForHeapStart(), 2, m_rtvDescriptorSize);
        m_device->CreateRenderTargetView(m_bloomExtractRT.Get(), nullptr, h);
        h.Offset(1, m_rtvDescriptorSize);
        m_device->CreateRenderTargetView(m_bloomBlurRT.Get(), nullptr, h);
    }

    // --- SRV heap: [0]=hdrRT (set by InitHDR), [1]=extract, [2]=blur ---
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 4; // [0]=hdrRT [1]=bloomExtract [2]=bloomBlur [3]=godRayRT
        hd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ThrowIfFailed(m_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_bloomSRVHeap)));
        m_bloomSRVIncrSize = m_device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }

    // Static SRVs for bloom RTs (slots 1 and 2)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format                  = DXGI_FORMAT_R16G16B16A16_FLOAT;
        sd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels     = 1;

        m_device->CreateShaderResourceView(m_bloomExtractRT.Get(), &sd,
            CD3DX12_CPU_DESCRIPTOR_HANDLE(
                m_bloomSRVHeap->GetCPUDescriptorHandleForHeapStart(), 1, m_bloomSRVIncrSize));
        m_device->CreateShaderResourceView(m_bloomBlurRT.Get(), &sd,
            CD3DX12_CPU_DESCRIPTOR_HANDLE(
                m_bloomSRVHeap->GetCPUDescriptorHandleForHeapStart(), 2, m_bloomSRVIncrSize));
    }

    // --- Bloom root signature: [0] SRV table t0, [1] 4 root constants b0 ---
    {
        CD3DX12_ROOT_PARAMETER params[2];
        CD3DX12_DESCRIPTOR_RANGE srvRange;
        srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
        params[0].InitAsDescriptorTable(1, &srvRange, D3D12_SHADER_VISIBILITY_PIXEL);
        params[1].InitAsConstants(4, 0);

        CD3DX12_STATIC_SAMPLER_DESC sampler(0,
            D3D12_FILTER_MIN_MAG_MIP_LINEAR,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP);

        CD3DX12_ROOT_SIGNATURE_DESC rsd;
        rsd.Init(2, params, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE);
        ComPtr<ID3DBlob> sig, err;
        ThrowIfFailed(D3D12SerializeRootSignature(&rsd,
            D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err));
        ThrowIfFailed(m_device->CreateRootSignature(0,
            sig->GetBufferPointer(), sig->GetBufferSize(),
            IID_PPV_ARGS(&m_bloomRootSig)));
    }

    // --- Load and create bloom bright-pass + blur PSOs (R16F output) ---
    UINT8 *pVS = nullptr, *pBright = nullptr, *pBlur = nullptr;
    UINT   vsLen = 0,      brightLen = 0,      blurLen = 0;
    ThrowIfFailed(ReadDataFromFile(GetAssetFullPath(L"bloom_BloomVS.cso").c_str(),      &pVS,     &vsLen));
    ThrowIfFailed(ReadDataFromFile(GetAssetFullPath(L"bloom_BrightPassPS.cso").c_str(), &pBright, &brightLen));
    ThrowIfFailed(ReadDataFromFile(GetAssetFullPath(L"bloom_BlurPS.cso").c_str(),       &pBlur,   &blurLen));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature           = m_bloomRootSig.Get();
    pd.VS                       = CD3DX12_SHADER_BYTECODE(pVS, vsLen);
    pd.RasterizerState          = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.BlendState               = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    pd.DepthStencilState.DepthEnable   = FALSE;
    pd.DepthStencilState.StencilEnable = FALSE;
    pd.DSVFormat             = DXGI_FORMAT_UNKNOWN;
    pd.SampleMask            = UINT_MAX;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets      = 1;
    pd.RTVFormats[0]         = DXGI_FORMAT_R16G16B16A16_FLOAT;
    pd.SampleDesc.Count      = 1;

    pd.PS = CD3DX12_SHADER_BYTECODE(pBright, brightLen);
    ThrowIfFailed(m_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_bloomBrightPSO)));

    pd.PS = CD3DX12_SHADER_BYTECODE(pBlur, blurLen);
    ThrowIfFailed(m_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_bloomBlurPSO)));
}

void D3D12HelloTriangle::InitHDR()
{
    // --- HDR render target (slot 4 in RTV heap) ---
    {
        auto heapProp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        auto rtDesc   = CD3DX12_RESOURCE_DESC::Tex2D(
            DXGI_FORMAT_R16G16B16A16_FLOAT, m_width, m_height, 1, 1);
        rtDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE clearVal = {};
        clearVal.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        ThrowIfFailed(m_device->CreateCommittedResource(
            &heapProp, D3D12_HEAP_FLAG_NONE, &rtDesc,
            D3D12_RESOURCE_STATE_RENDER_TARGET, &clearVal,
            IID_PPV_ARGS(&m_hdrRT)));

        CD3DX12_CPU_DESCRIPTOR_HANDLE hdrRtv(
            m_rtvHeap->GetCPUDescriptorHandleForHeapStart(),
            FrameCount + 2, m_rtvDescriptorSize); // slot 4
        m_device->CreateRenderTargetView(m_hdrRT.Get(), nullptr, hdrRtv);
    }

    // --- SRV for hdrRT in slot 0 of m_bloomSRVHeap (static) ---
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format                  = DXGI_FORMAT_R16G16B16A16_FLOAT;
        sd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels     = 1;
        m_device->CreateShaderResourceView(m_hdrRT.Get(), &sd,
            CD3DX12_CPU_DESCRIPTOR_HANDLE(
                m_bloomSRVHeap->GetCPUDescriptorHandleForHeapStart(), 0, m_bloomSRVIncrSize));
    }

    // --- Tone mapping root signature: [0] 2-SRV (t0=hdr,t1=bloom), [1] 1-SRV (t2=godrays), [2] 3 constants ---
    {
        CD3DX12_ROOT_PARAMETER params[3];
        CD3DX12_DESCRIPTOR_RANGE srvRange1, srvRange2;
        srvRange1.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0); // t0, t1
        srvRange2.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 2); // t2
        params[0].InitAsDescriptorTable(1, &srvRange1, D3D12_SHADER_VISIBILITY_PIXEL);
        params[1].InitAsDescriptorTable(1, &srvRange2, D3D12_SHADER_VISIBILITY_PIXEL);
        params[2].InitAsConstants(8, 0); // bloom, exposure, godray, vignette, grain, time, pad×2

        CD3DX12_STATIC_SAMPLER_DESC sampler(0,
            D3D12_FILTER_MIN_MAG_MIP_LINEAR,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP);

        CD3DX12_ROOT_SIGNATURE_DESC rsd;
        rsd.Init(3, params, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE);
        ComPtr<ID3DBlob> sig, err;
        ThrowIfFailed(D3D12SerializeRootSignature(&rsd,
            D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err));
        ThrowIfFailed(m_device->CreateRootSignature(0,
            sig->GetBufferPointer(), sig->GetBufferSize(),
            IID_PPV_ARGS(&m_toneMappingRootSig)));
    }

    // --- Tone mapping PSO (output to LDR swap chain format) ---
    {
        UINT8 *pVS = nullptr, *pPS = nullptr;
        UINT   vsLen = 0,      psLen = 0;
        ThrowIfFailed(ReadDataFromFile(
            GetAssetFullPath(L"tonemapping_ToneMapVS.cso").c_str(), &pVS, &vsLen));
        ThrowIfFailed(ReadDataFromFile(
            GetAssetFullPath(L"tonemapping_ToneMapPS.cso").c_str(), &pPS, &psLen));

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature           = m_toneMappingRootSig.Get();
        pd.VS                       = CD3DX12_SHADER_BYTECODE(pVS, vsLen);
        pd.PS                       = CD3DX12_SHADER_BYTECODE(pPS, psLen);
        pd.RasterizerState          = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.BlendState               = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        pd.DepthStencilState.DepthEnable   = FALSE;
        pd.DepthStencilState.StencilEnable = FALSE;
        pd.DSVFormat             = DXGI_FORMAT_UNKNOWN;
        pd.SampleMask            = UINT_MAX;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets      = 1;
        pd.RTVFormats[0]         = DXGI_FORMAT_R8G8B8A8_UNORM;
        pd.SampleDesc.Count      = 1;
        ThrowIfFailed(m_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_toneMappingPSO)));
    }
}

void D3D12HelloTriangle::RenderBloom()
{
    auto* cmd = m_commandList.Get();

    // When bloom is disabled we still need hdrRT in PSR for tone mapping
    if (!m_bloomEnabled)
    {
        auto bar = CD3DX12_RESOURCE_BARRIER::Transition(
            m_hdrRT.Get(),
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cmd->ResourceBarrier(1, &bar);
        return;
    }

    const float kBlack[] = { 0, 0, 0, 0 };
    auto rtvExtract = CD3DX12_CPU_DESCRIPTOR_HANDLE(
        m_rtvHeap->GetCPUDescriptorHandleForHeapStart(), 2, m_rtvDescriptorSize);
    auto rtvBlur = CD3DX12_CPU_DESCRIPTOR_HANDLE(
        m_rtvHeap->GetCPUDescriptorHandleForHeapStart(), 3, m_rtvDescriptorSize);
    auto gpuSlot0 = CD3DX12_GPU_DESCRIPTOR_HANDLE(
        m_bloomSRVHeap->GetGPUDescriptorHandleForHeapStart(), 0, m_bloomSRVIncrSize);
    auto gpuSlot1 = CD3DX12_GPU_DESCRIPTOR_HANDLE(
        m_bloomSRVHeap->GetGPUDescriptorHandleForHeapStart(), 1, m_bloomSRVIncrSize);
    auto gpuSlot2 = CD3DX12_GPU_DESCRIPTOR_HANDLE(
        m_bloomSRVHeap->GetGPUDescriptorHandleForHeapStart(), 2, m_bloomSRVIncrSize);

    cmd->SetGraphicsRootSignature(m_bloomRootSig.Get());
    ID3D12DescriptorHeap* heaps[] = { m_bloomSRVHeap.Get() };
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->RSSetViewports(1, &m_viewport);
    cmd->RSSetScissorRects(1, &m_scissorRect);

    // === BRIGHT PASS: hdrRT(RT→PSR) → extractRT ===
    {
        auto bar = CD3DX12_RESOURCE_BARRIER::Transition(
            m_hdrRT.Get(),
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cmd->ResourceBarrier(1, &bar);
    }
    float pBright[4] = { m_bloomThreshold, 0.f, 0.f, 0.f };
    cmd->SetPipelineState(m_bloomBrightPSO.Get());
    cmd->SetGraphicsRoot32BitConstants(1, 4, pBright, 0);
    cmd->SetGraphicsRootDescriptorTable(0, gpuSlot0);
    cmd->OMSetRenderTargets(1, &rtvExtract, FALSE, nullptr);
    cmd->ClearRenderTargetView(rtvExtract, kBlack, 0, nullptr);
    cmd->DrawInstanced(3, 1, 0, 0);

    // === BLUR: 2 H+V iterations (ping-pong extract ↔ blur) ===
    float pBlurH[4] = { 0.f, 0.f, 1.f, 0.f };
    float pBlurV[4] = { 0.f, 0.f, 0.f, 1.f };
    for (int iter = 0; iter < 2; ++iter)
    {
        {
            auto bar = CD3DX12_RESOURCE_BARRIER::Transition(
                m_bloomExtractRT.Get(),
                D3D12_RESOURCE_STATE_RENDER_TARGET,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            cmd->ResourceBarrier(1, &bar);
        }
        if (iter > 0)
        {
            auto bar = CD3DX12_RESOURCE_BARRIER::Transition(
                m_bloomBlurRT.Get(),
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_RENDER_TARGET);
            cmd->ResourceBarrier(1, &bar);
        }
        cmd->SetPipelineState(m_bloomBlurPSO.Get());
        cmd->SetGraphicsRoot32BitConstants(1, 4, pBlurH, 0);
        cmd->SetGraphicsRootDescriptorTable(0, gpuSlot1);
        cmd->OMSetRenderTargets(1, &rtvBlur, FALSE, nullptr);
        cmd->ClearRenderTargetView(rtvBlur, kBlack, 0, nullptr);
        cmd->DrawInstanced(3, 1, 0, 0);

        {
            D3D12_RESOURCE_BARRIER bars[2] = {
                CD3DX12_RESOURCE_BARRIER::Transition(m_bloomBlurRT.Get(),
                    D3D12_RESOURCE_STATE_RENDER_TARGET,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
                CD3DX12_RESOURCE_BARRIER::Transition(m_bloomExtractRT.Get(),
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_RENDER_TARGET),
            };
            cmd->ResourceBarrier(2, bars);
        }
        cmd->SetGraphicsRoot32BitConstants(1, 4, pBlurV, 0);
        cmd->SetGraphicsRootDescriptorTable(0, gpuSlot2);
        cmd->OMSetRenderTargets(1, &rtvExtract, FALSE, nullptr);
        cmd->ClearRenderTargetView(rtvExtract, kBlack, 0, nullptr);
        cmd->DrawInstanced(3, 1, 0, 0);
    }
    // After loop: hdrRT=PSR, extractRT=RT (final bloom), blurRT=PSR
}

void D3D12HelloTriangle::InitGodRays()
{
    UINT grW = m_width / 2, grH = m_height / 2;

    // --- God ray RT (half resolution, slot 5 in RTV heap) ---
    {
        auto heapProp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        auto rtDesc   = CD3DX12_RESOURCE_DESC::Tex2D(
            DXGI_FORMAT_R16G16B16A16_FLOAT, grW, grH, 1, 1);
        rtDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE clearVal = {};
        clearVal.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        ThrowIfFailed(m_device->CreateCommittedResource(
            &heapProp, D3D12_HEAP_FLAG_NONE, &rtDesc,
            D3D12_RESOURCE_STATE_RENDER_TARGET, &clearVal,
            IID_PPV_ARGS(&m_godRayRT)));

        CD3DX12_CPU_DESCRIPTOR_HANDLE rtv(
            m_rtvHeap->GetCPUDescriptorHandleForHeapStart(),
            FrameCount + 3, m_rtvDescriptorSize); // slot 5
        m_device->CreateRenderTargetView(m_godRayRT.Get(), nullptr, rtv);
    }

    // --- SRV in slot 3 of m_bloomSRVHeap ---
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format                  = DXGI_FORMAT_R16G16B16A16_FLOAT;
        sd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels     = 1;
        m_device->CreateShaderResourceView(m_godRayRT.Get(), &sd,
            CD3DX12_CPU_DESCRIPTOR_HANDLE(
                m_bloomSRVHeap->GetCPUDescriptorHandleForHeapStart(),
                3, m_bloomSRVIncrSize));
    }

    // --- God ray root signature: [0] 1-SRV (t0=hdrRT), [1] 6 root constants ---
    {
        CD3DX12_ROOT_PARAMETER params[2];
        CD3DX12_DESCRIPTOR_RANGE srvRange;
        srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
        params[0].InitAsDescriptorTable(1, &srvRange, D3D12_SHADER_VISIBILITY_PIXEL);
        params[1].InitAsConstants(6, 0); // sunScreenXY, density, decay, weight, sunVis

        CD3DX12_STATIC_SAMPLER_DESC sampler(0,
            D3D12_FILTER_MIN_MAG_MIP_LINEAR,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP);

        CD3DX12_ROOT_SIGNATURE_DESC rsd;
        rsd.Init(2, params, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE);
        ComPtr<ID3DBlob> sig, err;
        ThrowIfFailed(D3D12SerializeRootSignature(&rsd,
            D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err));
        ThrowIfFailed(m_device->CreateRootSignature(0,
            sig->GetBufferPointer(), sig->GetBufferSize(),
            IID_PPV_ARGS(&m_godRayRootSig)));
    }

    // --- God ray PSO ---
    {
        UINT8 *pVS = nullptr, *pPS = nullptr;
        UINT   vsLen = 0,      psLen = 0;
        ThrowIfFailed(ReadDataFromFile(
            GetAssetFullPath(L"godrays_GodRayVS.cso").c_str(), &pVS, &vsLen));
        ThrowIfFailed(ReadDataFromFile(
            GetAssetFullPath(L"godrays_GodRayPS.cso").c_str(), &pPS, &psLen));

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature           = m_godRayRootSig.Get();
        pd.VS                       = CD3DX12_SHADER_BYTECODE(pVS, vsLen);
        pd.PS                       = CD3DX12_SHADER_BYTECODE(pPS, psLen);
        pd.RasterizerState          = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.BlendState               = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        pd.DepthStencilState.DepthEnable   = FALSE;
        pd.DepthStencilState.StencilEnable = FALSE;
        pd.DSVFormat             = DXGI_FORMAT_UNKNOWN;
        pd.SampleMask            = UINT_MAX;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets      = 1;
        pd.RTVFormats[0]         = DXGI_FORMAT_R16G16B16A16_FLOAT;
        pd.SampleDesc.Count      = 1;
        ThrowIfFailed(m_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_godRayPSO)));
    }
}

void D3D12HelloTriangle::RenderGodRays()
{
    auto* cmd = m_commandList.Get();

    auto godRayRtv = CD3DX12_CPU_DESCRIPTOR_HANDLE(
        m_rtvHeap->GetCPUDescriptorHandleForHeapStart(),
        FrameCount + 3, m_rtvDescriptorSize);

    const float kBlack[] = { 0, 0, 0, 0 };
    cmd->ClearRenderTargetView(godRayRtv, kBlack, 0, nullptr);

    if (!m_godRaysEnabled) return;

    // Project sun to screen UV
    XMFLOAT3 sd3 = m_skyDome->GetSunDirection();
    XMVECTOR sunWorld = XMVector3Normalize(XMLoadFloat3(&sd3));
    sunWorld = XMVectorSetW(XMVectorScale(sunWorld, 999.0f), 1.0f);
    XMMATRIX vp = XMMatrixMultiply(m_renderer->GetViewMatrix(), m_renderer->GetProjMatrix());
    XMVECTOR clip = XMVector4Transform(sunWorld, vp);

    float w = XMVectorGetW(clip);
    float sunScreenX = 0.5f, sunScreenY = 0.5f, sunVis = 0.0f;
    if (w > 0.0f)
    {
        sunScreenX = XMVectorGetX(clip) / w * 0.5f + 0.5f;
        sunScreenY = -XMVectorGetY(clip) / w * 0.5f + 0.5f;
        // Fade by sun height and how far off-screen the sun is
        sunVis = std::clamp(sd3.y * 3.0f + 0.3f, 0.0f, 1.0f);
        float ax = fabsf(sunScreenX - 0.5f), ay = fabsf(sunScreenY - 0.5f);
        float offScreen = ax > ay ? ax : ay;
        sunVis *= std::clamp(1.0f - (offScreen - 0.5f) * 3.0f, 0.0f, 1.0f);
    }

    if (sunVis <= 0.001f) return;

    // Draw god ray radial blur into half-res RT
    UINT grW = m_width / 2, grH = m_height / 2;
    D3D12_VIEWPORT grVP = { 0, 0, (float)grW, (float)grH, 0, 1 };
    D3D12_RECT     grSC = { 0, 0, (LONG)grW,  (LONG)grH };

    struct GRParams { float sx, sy, density, decay, weight, vis; } p = {
        sunScreenX, sunScreenY, 0.96f, 0.97f, 0.04f, sunVis
    };

    cmd->SetGraphicsRootSignature(m_godRayRootSig.Get());
    ID3D12DescriptorHeap* heaps[] = { m_bloomSRVHeap.Get() };
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->RSSetViewports(1, &grVP);
    cmd->RSSetScissorRects(1, &grSC);
    cmd->SetPipelineState(m_godRayPSO.Get());
    cmd->SetGraphicsRoot32BitConstants(1, 6, &p, 0);
    cmd->SetGraphicsRootDescriptorTable(0,
        m_bloomSRVHeap->GetGPUDescriptorHandleForHeapStart()); // slot 0 = hdrRT
    cmd->OMSetRenderTargets(1, &godRayRtv, FALSE, nullptr);
    cmd->DrawInstanced(3, 1, 0, 0);
}

void D3D12HelloTriangle::RenderToneMap()
{
    auto* cmd = m_commandList.Get();

    // Transition bloom extract and god ray RT to PSR for reading
    {
        D3D12_RESOURCE_BARRIER bars[2] = {
            CD3DX12_RESOURCE_BARRIER::Transition(m_bloomExtractRT.Get(),
                D3D12_RESOURCE_STATE_RENDER_TARGET,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(m_godRayRT.Get(),
                D3D12_RESOURCE_STATE_RENDER_TARGET,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
        };
        cmd->ResourceBarrier(2, bars);
    }

    auto rtvSwap = CD3DX12_CPU_DESCRIPTOR_HANDLE(
        m_rtvHeap->GetCPUDescriptorHandleForHeapStart(),
        m_frameIndex, m_rtvDescriptorSize);

    auto gpuSlot0 = m_bloomSRVHeap->GetGPUDescriptorHandleForHeapStart(); // hdrRT+bloom (slots 0,1)
    auto gpuSlot3 = CD3DX12_GPU_DESCRIPTOR_HANDLE(
        m_bloomSRVHeap->GetGPUDescriptorHandleForHeapStart(), 3, m_bloomSRVIncrSize); // god ray

    cmd->SetGraphicsRootSignature(m_toneMappingRootSig.Get());
    ID3D12DescriptorHeap* heaps[] = { m_bloomSRVHeap.Get() };
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->RSSetViewports(1, &m_viewport);
    cmd->RSSetScissorRects(1, &m_scissorRect);

    float params[8] = {
        m_bloomStrength, m_exposure, m_godRayStrength,
        m_vignetteStrength, m_grainStrength, m_renderer->GetTime(),
        0.0f, 0.0f
    };
    cmd->SetPipelineState(m_toneMappingPSO.Get());
    cmd->SetGraphicsRoot32BitConstants(2, 8, params, 0);
    cmd->SetGraphicsRootDescriptorTable(0, gpuSlot0);
    cmd->SetGraphicsRootDescriptorTable(1, gpuSlot3);
    cmd->OMSetRenderTargets(1, &rtvSwap, FALSE, nullptr);
    cmd->DrawInstanced(3, 1, 0, 0);

    // Cleanup: restore all intermediate RTs to RENDER_TARGET
    D3D12_RESOURCE_BARRIER cleanup[3] = {
        CD3DX12_RESOURCE_BARRIER::Transition(m_hdrRT.Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_RENDER_TARGET),
        CD3DX12_RESOURCE_BARRIER::Transition(m_bloomExtractRT.Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_RENDER_TARGET),
        CD3DX12_RESOURCE_BARRIER::Transition(m_godRayRT.Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_RENDER_TARGET),
    };
    cmd->ResourceBarrier(3, cleanup);

    if (m_bloomEnabled)
    {
        auto bar = CD3DX12_RESOURCE_BARRIER::Transition(
            m_bloomBlurRT.Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_RENDER_TARGET);
        cmd->ResourceBarrier(1, &bar);
    }

    // Restore dofRT to RT and redirect bloomSRVHeap[0] back to hdrRT
    if (m_dofEnabled)
    {
        auto bar = CD3DX12_RESOURCE_BARRIER::Transition(
            m_dofRT.Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_RENDER_TARGET);
        cmd->ResourceBarrier(1, &bar);

        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format                  = DXGI_FORMAT_R16G16B16A16_FLOAT;
        sd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels     = 1;
        m_device->CreateShaderResourceView(m_hdrRT.Get(), &sd,
            m_bloomSRVHeap->GetCPUDescriptorHandleForHeapStart()); // restore slot 0 → hdrRT
    }

    // Restore skySnapshot to COPY_DEST ready for next frame's sky blit
    if (m_skySnapshotInPSR)
    {
        auto bar = CD3DX12_RESOURCE_BARRIER::Transition(
            m_skySnapshotRT.Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_COPY_DEST);
        cmd->ResourceBarrier(1, &bar);
        m_skySnapshotInPSR = false;
    }
}

void D3D12HelloTriangle::InitLensFlare()
{
    // Root signature: 6 root constants (sunX, sunY, sunVis, strength, aspectRatio, time)
    {
        CD3DX12_ROOT_PARAMETER params[1];
        params[0].InitAsConstants(6, 0);

        CD3DX12_ROOT_SIGNATURE_DESC rsd;
        rsd.Init(1, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE);
        ComPtr<ID3DBlob> sig, err;
        ThrowIfFailed(D3D12SerializeRootSignature(&rsd,
            D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err));
        ThrowIfFailed(m_device->CreateRootSignature(0,
            sig->GetBufferPointer(), sig->GetBufferSize(),
            IID_PPV_ARGS(&m_lensFlareRootSig)));
    }

    // PSO: additive blend, no depth test, outputs to LDR swap chain format
    {
        UINT8 *pVS = nullptr, *pPS = nullptr;
        UINT   vsLen = 0,      psLen = 0;
        ThrowIfFailed(ReadDataFromFile(
            GetAssetFullPath(L"lensflare_LensFlareVS.cso").c_str(), &pVS, &vsLen));
        ThrowIfFailed(ReadDataFromFile(
            GetAssetFullPath(L"lensflare_LensFlarePS.cso").c_str(), &pPS, &psLen));

        D3D12_BLEND_DESC blendDesc = {};
        blendDesc.RenderTarget[0].BlendEnable    = TRUE;
        blendDesc.RenderTarget[0].SrcBlend       = D3D12_BLEND_ONE;
        blendDesc.RenderTarget[0].DestBlend      = D3D12_BLEND_ONE;
        blendDesc.RenderTarget[0].BlendOp        = D3D12_BLEND_OP_ADD;
        blendDesc.RenderTarget[0].SrcBlendAlpha  = D3D12_BLEND_ONE;
        blendDesc.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ONE;
        blendDesc.RenderTarget[0].BlendOpAlpha   = D3D12_BLEND_OP_ADD;
        blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature           = m_lensFlareRootSig.Get();
        pd.VS                       = CD3DX12_SHADER_BYTECODE(pVS, vsLen);
        pd.PS                       = CD3DX12_SHADER_BYTECODE(pPS, psLen);
        pd.RasterizerState          = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.BlendState               = blendDesc;
        pd.DepthStencilState.DepthEnable   = FALSE;
        pd.DepthStencilState.StencilEnable = FALSE;
        pd.DSVFormat             = DXGI_FORMAT_UNKNOWN;
        pd.SampleMask            = UINT_MAX;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets      = 1;
        pd.RTVFormats[0]         = DXGI_FORMAT_R8G8B8A8_UNORM;
        pd.SampleDesc.Count      = 1;
        ThrowIfFailed(m_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_lensFlarePSO)));
    }
}

void D3D12HelloTriangle::RenderLensFlare()
{
    if (!m_lensFlareEnabled) return;

    auto* cmd = m_commandList.Get();

    // Project sun to screen UV (same logic as RenderGodRays)
    XMFLOAT3 sd3 = m_skyDome->GetSunDirection();
    XMVECTOR sunWorld = XMVector3Normalize(XMLoadFloat3(&sd3));
    sunWorld = XMVectorSetW(XMVectorScale(sunWorld, 999.0f), 1.0f);
    XMMATRIX vp = XMMatrixMultiply(m_renderer->GetViewMatrix(), m_renderer->GetProjMatrix());
    XMVECTOR clip = XMVector4Transform(sunWorld, vp);

    float w = XMVectorGetW(clip);
    float sunScreenX = 0.5f, sunScreenY = 0.5f, sunVis = 0.0f;
    if (w > 0.0f)
    {
        sunScreenX = XMVectorGetX(clip) / w * 0.5f + 0.5f;
        sunScreenY = -XMVectorGetY(clip) / w * 0.5f + 0.5f;
        sunVis = std::clamp(sd3.y * 3.0f + 0.3f, 0.0f, 1.0f);
        float ax = fabsf(sunScreenX - 0.5f), ay = fabsf(sunScreenY - 0.5f);
        float offScreen = ax > ay ? ax : ay;
        sunVis *= std::clamp(1.0f - (offScreen - 0.5f) * 3.0f, 0.0f, 1.0f);
    }

    if (sunVis <= 0.001f) return;

    auto rtvSwap = CD3DX12_CPU_DESCRIPTOR_HANDLE(
        m_rtvHeap->GetCPUDescriptorHandleForHeapStart(),
        m_frameIndex, m_rtvDescriptorSize);

    struct LFParams { float sx, sy, vis, strength, aspect, time; } p = {
        sunScreenX, sunScreenY, sunVis, m_lensFlareStrength,
        (float)m_width / (float)m_height, m_renderer->GetTime()
    };

    cmd->SetGraphicsRootSignature(m_lensFlareRootSig.Get());
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->RSSetViewports(1, &m_viewport);
    cmd->RSSetScissorRects(1, &m_scissorRect);
    cmd->SetPipelineState(m_lensFlarePSO.Get());
    cmd->SetGraphicsRoot32BitConstants(0, 6, &p, 0);
    cmd->OMSetRenderTargets(1, &rtvSwap, FALSE, nullptr);
    cmd->DrawInstanced(3, 1, 0, 0);
}

void D3D12HelloTriangle::InitDOF()
{
    // DOF output RT (slot FrameCount+4 = 6 in RTV heap)
    {
        auto heapProp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        auto rtDesc   = CD3DX12_RESOURCE_DESC::Tex2D(
            DXGI_FORMAT_R16G16B16A16_FLOAT, m_width, m_height, 1, 1);
        rtDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE clearVal = {};
        clearVal.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        ThrowIfFailed(m_device->CreateCommittedResource(
            &heapProp, D3D12_HEAP_FLAG_NONE, &rtDesc,
            D3D12_RESOURCE_STATE_RENDER_TARGET, &clearVal,
            IID_PPV_ARGS(&m_dofRT)));

        CD3DX12_CPU_DESCRIPTOR_HANDLE rtv(
            m_rtvHeap->GetCPUDescriptorHandleForHeapStart(),
            FrameCount + 4, m_rtvDescriptorSize);
        m_device->CreateRenderTargetView(m_dofRT.Get(), nullptr, rtv);
    }

    // DOF SRV heap: [0]=hdrRT  [1]=depthBuffer
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 2;
        hd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ThrowIfFailed(m_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_dofSRVHeap)));
        m_dofSRVIncrSize = m_device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        // Slot 0: hdrRT (R16F)
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format                  = DXGI_FORMAT_R16G16B16A16_FLOAT;
        sd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels     = 1;
        m_device->CreateShaderResourceView(m_hdrRT.Get(), &sd,
            CD3DX12_CPU_DESCRIPTOR_HANDLE(
                m_dofSRVHeap->GetCPUDescriptorHandleForHeapStart(), 0, m_dofSRVIncrSize));

        // Slot 1: depth buffer (R32_FLOAT view of R32_TYPELESS resource)
        sd.Format = DXGI_FORMAT_R32_FLOAT;
        m_device->CreateShaderResourceView(m_renderer->GetDepthBuffer(), &sd,
            CD3DX12_CPU_DESCRIPTOR_HANDLE(
                m_dofSRVHeap->GetCPUDescriptorHandleForHeapStart(), 1, m_dofSRVIncrSize));
    }

    // DOF root signature: [0] 2-SRV table, [1] 4 root constants
    ComPtr<ID3D12RootSignature> dofRootSig;
    {
        CD3DX12_ROOT_PARAMETER params[2];
        CD3DX12_DESCRIPTOR_RANGE srvRange;
        srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0); // t0, t1
        params[0].InitAsDescriptorTable(1, &srvRange, D3D12_SHADER_VISIBILITY_PIXEL);
        params[1].InitAsConstants(4, 0); // focusDepth, focusRange, maxRadius, aspectRatio

        CD3DX12_STATIC_SAMPLER_DESC sampler(0,
            D3D12_FILTER_MIN_MAG_MIP_LINEAR,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP);

        CD3DX12_ROOT_SIGNATURE_DESC rsd;
        rsd.Init(2, params, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE);
        ComPtr<ID3DBlob> sig, err;
        ThrowIfFailed(D3D12SerializeRootSignature(&rsd,
            D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err));

        // Store root sig inline (reuse m_lensFlareRootSig pattern — add to header if needed)
        // For simplicity, store in a local and bake the PSO:
        ComPtr<ID3D12RootSignature> rs;
        ThrowIfFailed(m_device->CreateRootSignature(0,
            sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&rs)));
        dofRootSig = rs;
    }

    // Store as a class member — declare ComPtr<ID3D12RootSignature> m_dofRootSig in header
    // (We'll forward-declare it via a local trick: assign to m_lensFlarePSO's sibling)
    // Actually: add m_dofRootSig and m_dofPSO to the header separately.
    // For now store via local variable and build PSO below:
    UINT8 *pVS = nullptr, *pPS = nullptr;
    UINT   vsLen = 0,      psLen = 0;
    ThrowIfFailed(ReadDataFromFile(
        GetAssetFullPath(L"dof_DOFVS.cso").c_str(), &pVS, &vsLen));
    ThrowIfFailed(ReadDataFromFile(
        GetAssetFullPath(L"dof_DOFPS.cso").c_str(), &pPS, &psLen));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature           = dofRootSig.Get();
    pd.VS                       = CD3DX12_SHADER_BYTECODE(pVS, vsLen);
    pd.PS                       = CD3DX12_SHADER_BYTECODE(pPS, psLen);
    pd.RasterizerState          = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.BlendState               = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    pd.DepthStencilState.DepthEnable   = FALSE;
    pd.DepthStencilState.StencilEnable = FALSE;
    pd.DSVFormat             = DXGI_FORMAT_UNKNOWN;
    pd.SampleMask            = UINT_MAX;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets      = 1;
    pd.RTVFormats[0]         = DXGI_FORMAT_R16G16B16A16_FLOAT;
    pd.SampleDesc.Count      = 1;

    // Store root sig + PSO — need to add these as class members
    m_dofRootSig = dofRootSig;
    ThrowIfFailed(m_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_dofPSO)));
}

void D3D12HelloTriangle::RenderDOF()
{
    auto* cmd = m_commandList.Get();

    if (!m_dofEnabled)
    {
        // Pass-through: update bloomSRVHeap[0] to still point to hdrRT (already correct)
        return;
    }

    // hdrRT is currently in PSR (from RenderBloom). Depth is in DEPTH_WRITE.
    // Transition depth to PSR for shader read
    auto b0 = CD3DX12_RESOURCE_BARRIER::Transition(
        m_renderer->GetDepthBuffer(),
        D3D12_RESOURCE_STATE_DEPTH_WRITE,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmd->ResourceBarrier(1, &b0);

    auto dofRtv = CD3DX12_CPU_DESCRIPTOR_HANDLE(
        m_rtvHeap->GetCPUDescriptorHandleForHeapStart(),
        FrameCount + 4, m_rtvDescriptorSize);

    const float kBlack[] = { 0, 0, 0, 0 };
    struct DOFParams { float focusDepth, focusRange, maxRadius, aspect; } p = {
        m_dofFocusDepth, m_dofFocusRange, m_dofMaxRadius,
        (float)m_width / (float)m_height
    };

    cmd->SetGraphicsRootSignature(m_dofRootSig.Get());
    ID3D12DescriptorHeap* heaps[] = { m_dofSRVHeap.Get() };
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->RSSetViewports(1, &m_viewport);
    cmd->RSSetScissorRects(1, &m_scissorRect);
    cmd->SetPipelineState(m_dofPSO.Get());
    cmd->SetGraphicsRoot32BitConstants(1, 4, &p, 0);
    cmd->SetGraphicsRootDescriptorTable(0,
        m_dofSRVHeap->GetGPUDescriptorHandleForHeapStart());
    cmd->OMSetRenderTargets(1, &dofRtv, FALSE, nullptr);
    cmd->ClearRenderTargetView(dofRtv, kBlack, 0, nullptr);
    cmd->DrawInstanced(3, 1, 0, 0);

    // Restore depth to DEPTH_WRITE
    auto b1 = CD3DX12_RESOURCE_BARRIER::Transition(
        m_renderer->GetDepthBuffer(),
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_DEPTH_WRITE);
    cmd->ResourceBarrier(1, &b1);

    // Transition dofRT to PSR so ToneMap can read it
    auto b2 = CD3DX12_RESOURCE_BARRIER::Transition(
        m_dofRT.Get(),
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmd->ResourceBarrier(1, &b2);

    // Redirect ToneMap input: update bloomSRVHeap slot 0 to point to dofRT
    D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format                  = DXGI_FORMAT_R16G16B16A16_FLOAT;
    sd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels     = 1;
    m_device->CreateShaderResourceView(m_dofRT.Get(), &sd,
        m_bloomSRVHeap->GetCPUDescriptorHandleForHeapStart()); // slot 0
}

void D3D12HelloTriangle::InitSSR()
{
    // Sky snapshot texture: same format as hdrRT, used as COPY_DEST then PSR
    {
        auto heapProp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        auto rtDesc   = CD3DX12_RESOURCE_DESC::Tex2D(
            DXGI_FORMAT_R16G16B16A16_FLOAT, m_width, m_height, 1, 1);
        ThrowIfFailed(m_device->CreateCommittedResource(
            &heapProp, D3D12_HEAP_FLAG_NONE, &rtDesc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&m_skySnapshotRT)));
    }

    // Ocean SRV heap: [0]=heightMap  [1]=dztMap  [2]=skySnapshot
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 3;
        hd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ThrowIfFailed(m_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_oceanSRVHeap)));
        m_oceanSRVIncrSize = m_device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }

    // Slots 0+1: FFT height/dzt maps (R32G32B32A32_FLOAT)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format                  = DXGI_FORMAT_R32G32B32A32_FLOAT;
        sd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels     = 1;

        m_device->CreateShaderResourceView(m_oceanFFT->GetHeightMap(), &sd,
            CD3DX12_CPU_DESCRIPTOR_HANDLE(
                m_oceanSRVHeap->GetCPUDescriptorHandleForHeapStart(), 0, m_oceanSRVIncrSize));
        m_device->CreateShaderResourceView(m_oceanFFT->GetDztMap(), &sd,
            CD3DX12_CPU_DESCRIPTOR_HANDLE(
                m_oceanSRVHeap->GetCPUDescriptorHandleForHeapStart(), 1, m_oceanSRVIncrSize));
    }

    // Slot 2: sky snapshot (R16G16B16A16_FLOAT)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format                  = DXGI_FORMAT_R16G16B16A16_FLOAT;
        sd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels     = 1;
        m_device->CreateShaderResourceView(m_skySnapshotRT.Get(), &sd,
            CD3DX12_CPU_DESCRIPTOR_HANDLE(
                m_oceanSRVHeap->GetCPUDescriptorHandleForHeapStart(), 2, m_oceanSRVIncrSize));
    }
}

// Render the scene.
void D3D12HelloTriangle::OnRender()
{
    ThrowIfFailed(m_commandAllocator->Reset());
    ThrowIfFailed(m_commandList->Reset(m_commandAllocator.Get(), nullptr));

    // Compute Pass
    m_oceanFFT->Dispatch(m_commandList,m_renderer->GetTime());

    // UAV → SRV，让Wave Shader能采样
    D3D12_RESOURCE_BARRIER toSRV[2] = {
        CD3DX12_RESOURCE_BARRIER::Transition(
            m_oceanFFT->GetHeightMap(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        CD3DX12_RESOURCE_BARRIER::Transition(
            m_oceanFFT->GetDztMap(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
    };
    m_commandList->ResourceBarrier(2, toSRV);

    // 构建ctx
    RenderContext ctx;
    ctx.cmd = m_commandList.Get();
    ctx.renderTarget = m_renderTargets[m_frameIndex].Get();
    // Scene renders into HDR RT (slot FrameCount+2 = 4)
    ctx.rtv = CD3DX12_CPU_DESCRIPTOR_HANDLE(
        m_rtvHeap->GetCPUDescriptorHandleForHeapStart(),
        FrameCount + 2, m_rtvDescriptorSize);
    ctx.viewport = m_viewport;
    ctx.scissor = m_scissorRect;
    ctx.dsv = m_renderer->GetDSVHeap()
        ->GetCPUDescriptorHandleForHeapStart();
    ctx.vb = m_renderer->GetGridVBView();
    ctx.ib = m_renderer->GetGridIBView();
    ctx.indexCount = m_renderer->GetGridIndexCount();
    ctx.view = m_renderer->GetViewMatrix();
    ctx.proj = m_renderer->GetProjMatrix();

    // Present → RenderTarget
    auto barrierToRT = CD3DX12_RESOURCE_BARRIER::Transition(
        ctx.renderTarget,
        D3D12_RESOURCE_STATE_PRESENT,
        D3D12_RESOURCE_STATE_RENDER_TARGET);
    m_commandList->ResourceBarrier(1, &barrierToRT);

    m_commandList->OMSetRenderTargets(1, &ctx.rtv, FALSE, &ctx.dsv);
    const float clearColor[] = { 0.0f, 0.2f, 0.4f, 1.0f };
    m_commandList->ClearRenderTargetView(ctx.rtv, clearColor, 0, nullptr);
    m_commandList->ClearDepthStencilView(
        ctx.dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    m_skyDome->SetShowcaseMode(m_renderer->IsShowcaseMode());
    m_skyDome->Render(ctx);

    // --- Snapshot sky into m_skySnapshotRT for SSR ---
    {
        // hdrRT is currently RT; transition to COPY_SOURCE for the blit
        auto b0 = CD3DX12_RESOURCE_BARRIER::Transition(
            m_hdrRT.Get(),
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
        m_commandList->ResourceBarrier(1, &b0);

        // skySnapshot starts in COPY_DEST (first frame) or PSR (subsequent)
        if (m_skySnapshotInPSR)
        {
            auto b1 = CD3DX12_RESOURCE_BARRIER::Transition(
                m_skySnapshotRT.Get(),
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_COPY_DEST);
            m_commandList->ResourceBarrier(1, &b1);
        }

        m_commandList->CopyResource(m_skySnapshotRT.Get(), m_hdrRT.Get());

        // skySnapshot → PSR for ocean shader to read
        auto b2 = CD3DX12_RESOURCE_BARRIER::Transition(
            m_skySnapshotRT.Get(),
            D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        m_commandList->ResourceBarrier(1, &b2);
        m_skySnapshotInPSR = true;

        // Restore hdrRT to RT for ocean/rain rendering
        auto b3 = CD3DX12_RESOURCE_BARRIER::Transition(
            m_hdrRT.Get(),
            D3D12_RESOURCE_STATE_COPY_SOURCE,
            D3D12_RESOURCE_STATE_RENDER_TARGET);
        m_commandList->ResourceBarrier(1, &b3);
        m_commandList->OMSetRenderTargets(1, &ctx.rtv, FALSE, &ctx.dsv);
    }

    // Bind ocean SRV heap (t0=height, t1=dzt, t2=skySnapshot)
    ID3D12DescriptorHeap* srvHeaps[] = { m_oceanSRVHeap.Get() };
    m_commandList->SetDescriptorHeaps(1, srvHeaps);
    m_commandList->SetGraphicsRootDescriptorTable(1,
        m_oceanSRVHeap->GetGPUDescriptorHandleForHeapStart());
    m_commandList->SetGraphicsRootConstantBufferView(
        2, m_rainSystem->GetRippleCBAddress());

    m_renderer->Render(ctx);
    if (m_renderer->IsShowcaseMode())
        m_renderer->RenderWaterBox(ctx);
    m_rainSystem->Render(ctx,
        m_renderer->GetViewMatrix(),
        m_renderer->GetProjMatrix(),
        m_renderer->GetCameraPos());

    RenderBloom();
    RenderGodRays();
    RenderDOF();
    RenderToneMap();
    RenderLensFlare();

    // ImGui 渲染（在 RT→Present barrier 之前）
    {
        ID3D12DescriptorHeap* imguiHeaps[] = { m_imguiSrvHeap.Get() };
        m_commandList->SetDescriptorHeaps(1, imguiHeaps);
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), m_commandList.Get());
    }

    // RenderTarget → Present
    auto barrierToPresent = CD3DX12_RESOURCE_BARRIER::Transition(
        ctx.renderTarget,
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_PRESENT);
    m_commandList->ResourceBarrier(1, &barrierToPresent);

    // SRV → UAV必须在Close之前录制
    D3D12_RESOURCE_BARRIER toUAV[2] = {
        CD3DX12_RESOURCE_BARRIER::Transition(
            m_oceanFFT->GetHeightMap(),
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        CD3DX12_RESOURCE_BARRIER::Transition(
            m_oceanFFT->GetDztMap(),
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
    };
    m_commandList->ResourceBarrier(2, toUAV);

    // Close之后才Execute
    ThrowIfFailed(m_commandList->Close());
    ID3D12CommandList* ppCommandLists[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(
        _countof(ppCommandLists), ppCommandLists);

    ThrowIfFailed(m_swapChain->Present(1, 0));
    WaitForPreviousFrame();
}


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
    // WAITING FOR THE FRAME TO COMPLETE BEFORE CONTINUING IS NOT BEST PRACTICE.
    // This is code implemented as such for simplicity. The D3D12HelloFrameBuffering
    // sample illustrates how to use fences for efficient resource usage and to
    // maximize GPU utilization.

    // Signal and increment the fence value.
    const UINT64 fence = m_fenceValue;
    ThrowIfFailed(m_commandQueue->Signal(m_fence.Get(), fence));
    m_fenceValue++;

    // Wait until the previous frame is finished.
    if (m_fence->GetCompletedValue() < fence)
    {
        ThrowIfFailed(m_fence->SetEventOnCompletion(fence, m_fenceEvent));
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }

    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
}
void D3D12HelloTriangle::OnKeyDown(UINT8 key)
{
    if (key == VK_TAB)
    {
        m_renderer->ToggleWireframe();
    }

    if (key == 'V')
    {
        if (m_renderer->IsShowcaseMode())
        {
            m_renderer->ToggleShowcase();
            m_renderer->GetCamera().ExitShowcase();
        }
        else
        {
            m_renderer->ToggleShowcase();
            m_renderer->GetCamera().EnterShowcase();
        }
    }
    if (key == '1')
    {
        m_weatherSystem->SetAutoWeather(false);
        m_weatherSystem->SetWeather(WeatherState::Calm, 5.0f);
    }
    if (key == '2')
    {
        m_weatherSystem->SetAutoWeather(false);
        m_weatherSystem->SetWeather(WeatherState::Windy, 5.0f);
    }
    if (key == '3')
    {
        m_weatherSystem->SetAutoWeather(false);
        m_weatherSystem->SetWeather(WeatherState::Storm, 5.0f);
    }
    if (key == '4') 
        m_weatherSystem->SetAutoWeather(true);
}