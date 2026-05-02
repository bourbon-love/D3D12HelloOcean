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

    // ImGui初期化
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

// レンダリングパイプラインの依存関係を読み込む
void D3D12HelloTriangle::LoadPipeline()
{
    UINT dxgiFactoryFlags = 0;

#if defined(_DEBUG)
    // デバッグレイヤーを有効にする（Graphics Toolsのオプション機能が必要）
    // 注意：デバイス作成後にデバッグレイヤーを有効にすると、アクティブなデバイスが無効になる
    {
        ComPtr<ID3D12Debug> debugController;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
        {
            debugController->EnableDebugLayer();

            // 追加のデバッグレイヤーを有効にする
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

    // コマンドキューの記述と作成
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

    ThrowIfFailed(m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_commandQueue)));

    // スワップチェーンの記述と作成
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
        m_commandQueue.Get(),        // スワップチェーンはキューを使ってフラッシュを強制する
        Win32Application::GetHwnd(),
        &swapChainDesc,
        nullptr,
        nullptr,
        &swapChain
        ));

    // このサンプルはフルスクリーン遷移をサポートしない
    ThrowIfFailed(factory->MakeWindowAssociation(Win32Application::GetHwnd(), DXGI_MWA_NO_ALT_ENTER));

    ThrowIfFailed(swapChain.As(&m_swapChain));
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

    // ディスクリプタヒープの作成
    {
        // レンダーターゲットビュー（RTV）ディスクリプタヒープの記述と作成
        D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
        rtvHeapDesc.NumDescriptors = FrameCount + 9; // +2ブルーム、+1 HDR、+1ゴッドレイ、+1 DOF、+2 TAA、+2 SSAO
        rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        ThrowIfFailed(m_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_rtvHeap)));

        m_rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    }

    // フレームリソースの作成
    {
        CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(m_rtvHeap->GetCPUDescriptorHandleForHeapStart());

        // 各フレームにRTVを作成する
        for (UINT n = 0; n < FrameCount; n++)
        {
            ThrowIfFailed(m_swapChain->GetBuffer(n, IID_PPV_ARGS(&m_renderTargets[n])));
            m_device->CreateRenderTargetView(m_renderTargets[n].Get(), nullptr, rtvHandle);
            rtvHandle.Offset(1, m_rtvDescriptorSize);
        }
    }

    ThrowIfFailed(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_commandAllocator)));
}

// サンプルアセットの読み込み
void D3D12HelloTriangle::LoadAssets()
{
    // ルートシグネチャ
    {
        CD3DX12_ROOT_PARAMETER rootParameters[4];
        rootParameters[0].InitAsConstantBufferView(0); // b0 SceneCB

        CD3DX12_DESCRIPTOR_RANGE srvRange;
        srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4, 0); // t0..t3 (t3=shadowMap)
        rootParameters[1].InitAsDescriptorTable(1, &srvRange, D3D12_SHADER_VISIBILITY_ALL);
        rootParameters[2].InitAsConstantBufferView(1); // b1 RippleCB
        rootParameters[3].InitAsConstantBufferView(2); // b2 ShadowSceneCB
        CD3DX12_STATIC_SAMPLER_DESC sampler(
            0,
            D3D12_FILTER_MIN_MAG_MIP_LINEAR,
            D3D12_TEXTURE_ADDRESS_MODE_WRAP,
            D3D12_TEXTURE_ADDRESS_MODE_WRAP,
            D3D12_TEXTURE_ADDRESS_MODE_WRAP);

        CD3DX12_ROOT_SIGNATURE_DESC rootSignatureDesc;
        rootSignatureDesc.Init(4, rootParameters, 1, &sampler,
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
        ComPtr<ID3DBlob> signature, error;
        ThrowIfFailed(D3D12SerializeRootSignature(&rootSignatureDesc,
            D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error));
        ThrowIfFailed(m_device->CreateRootSignature(0,
            signature->GetBufferPointer(), signature->GetBufferSize(),
            IID_PPV_ARGS(&m_rootSignature)));
    }

    // Phillips CSを読み込む
    UINT8* pPhillipsCSData = nullptr;
    UINT   phillipsCsLen = 0;
    ThrowIfFailed(ReadDataFromFile(
        GetAssetFullPath(L"PhillipsCS.cso").c_str(),
        &pPhillipsCSData, &phillipsCsLen));

    // 時間発展CSとIFFT CSを読み込む
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

    // シェーダーバイトコードを読み込む
    UINT8* pVertexShaderData = nullptr;
    UINT8* pPixelShaderData = nullptr;
    UINT   vsLen = 0, psLen = 0;
    ThrowIfFailed(ReadDataFromFile(
        GetAssetFullPath(L"shaders_VSMain.cso").c_str(),
        &pVertexShaderData, &vsLen));
    ThrowIfFailed(ReadDataFromFile(
        GetAssetFullPath(L"shaders_PSMain.cso").c_str(),
        &pPixelShaderData, &psLen));

	// 水体バウンディングシェーダーのバイトコードを読み込む
    UINT8* pBoxVSData = nullptr;
    UINT8* pBoxPSData = nullptr;
    UINT   boxVsLen = 0, boxPsLen = 0;
    ThrowIfFailed(ReadDataFromFile(
        GetAssetFullPath(L"waterbody_VSMain.cso").c_str(),
        &pBoxVSData, &boxVsLen));
    ThrowIfFailed(ReadDataFromFile(
        GetAssetFullPath(L"waterbody_PSMain.cso").c_str(),
        &pBoxPSData, &boxPsLen));

        // PSOを作成する（この時点ではCommandListがまだない）
        m_renderer = std::make_unique<Renderer>();
        m_renderer->InitPSO(
            m_device, m_rootSignature,
            m_width, m_height,
            pVertexShaderData, vsLen,
            pPixelShaderData, psLen,
            pBoxVSData, boxVsLen,
            pBoxPSData, boxPsLen);


        // 空シェーダーのバイトコードを読み込む
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

        // 浮遊オブジェクト（木箱）のシェーダーを読み込む
        UINT8 *pFOVS = nullptr, *pFOPS = nullptr;
        UINT   foVsLen = 0,      fosPsLen = 0;
        ThrowIfFailed(ReadDataFromFile(
            GetAssetFullPath(L"floating_object_FloatObjVS.cso").c_str(), &pFOVS, &foVsLen));
        ThrowIfFailed(ReadDataFromFile(
            GetAssetFullPath(L"floating_object_FloatObjPS.cso").c_str(), &pFOPS, &fosPsLen));

        m_floatingObject = std::make_unique<FloatingObject>();
        m_floatingObject->Init(m_device, m_oceanFFT->GetHeightMap(),
            pFOVS, foVsLen, pFOPS, fosPsLen);

        free(pFOVS);
        free(pFOPS);

        // CommandListを作成する
        ThrowIfFailed(m_device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            m_commandAllocator.Get(),
            m_renderer->GetPSO(),
            IID_PPV_ARGS(&m_commandList)));
        // グリッドアップロードコマンドを記録する
        m_skyDome->InitResources(m_commandList);
        m_renderer->InitResources(m_commandList);
        m_floatingObject->InitBuffers(m_commandList);
    

    // 同期オブジェクトを作成し、アセットがGPUにアップロードされるまで待機する
    
        ThrowIfFailed(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)));
        m_fenceValue = 1;

        // Create an event handle to use for frame synchronization.
        m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (m_fenceEvent == nullptr)
        {
            ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()));
        }

        // グリッドアップロードコマンドを記録し終えたのでクローズして提出する
        ThrowIfFailed(m_commandList->Close());
        ID3D12CommandList* ppInitCmds[] = { m_commandList.Get() };
        m_commandQueue->ExecuteCommandLists(_countof(ppInitCmds), ppInitCmds);
        // コマンドリストの実行を待機する。メインループでも同じコマンドリストを再利用するが、
        // ここではセットアップが完了するまで待機するだけである
        WaitForPreviousFrame();

    InitBloom();
    InitHDR();
    InitGodRays();
    InitLensFlare();
    InitSSR();
    InitDOF();
    InitTAA();
    InitLightning();
    InitSSAO();
    InitShadowMap();
}

// フレームベースの値を更新する
void D3D12HelloTriangle::OnUpdate()
{
	auto currentTime = std::chrono::steady_clock::now();
	float deltaTime = std::chrono::duration<float>(currentTime - m_lastTime).count();
	m_lastTime = currentTime;

    float scaledDt = m_timePaused ? 0.0f : deltaTime * m_timeScale;
    float weatherIntensity = m_weatherSystem->GetWeatherIntensity();

    m_skyDome->Update(scaledDt);

    // 自動露出：太陽の高さに応じた滑らかな適応（スケールなしの実時間deltaTimeを使用）
    if (m_autoExposure)
    {
        float sunH = m_skyDome->GetSunDirection().y;
        float weatherI = m_weatherSystem->GetWeatherIntensity();

        float target;
        if      (sunH > 0.25f)  target = 1.0f;
        else if (sunH > 0.0f)   target = 1.0f + (1.6f - 1.0f) * (1.0f - sunH / 0.25f);
        else if (sunH > -0.15f) target = 1.6f + (2.2f - 1.6f) * (-sunH / 0.15f);
        else                    target = 2.5f;

        target += weatherI * 0.4f; // 嵐 → シーンが暗くなる → 露出を増やす
        target = target < 0.3f ? 0.3f : (target > 5.0f ? 5.0f : target);

        // 非対称速度：明るい方向への適応が速い（瞳孔が素早く縮む）
        float speed = (target < m_exposure) ? 1.0f : 0.35f;
        m_exposure += (target - m_exposure) * (speed * deltaTime < 1.0f ? speed * deltaTime : 1.0f);
    }

    m_renderer->Update(scaledDt);
    m_weatherSystem->Update(scaledDt);
    m_rainSystem->Update(scaledDt, weatherIntensity,
        m_oceanFFT->windDirX, m_oceanFFT->windDirY);

    m_renderer->SetSSRMix(m_ssrStrength);
    m_skyDome->SetWindDir(m_oceanFFT->windDirX, m_oceanFFT->windDirY);
    m_floatingObject->Update(scaledDt);

    // Lightning bolt: detect new strike and generate path
    float curLightning = m_skyDome->GetLightningIntensity();
    if (curLightning > 0.0f && m_prevLightningIntensity <= 0.0f)
        GenerateLightningBolt();
    m_mappedLightningCB->intensity = curLightning;
    m_prevLightningIntensity = curLightning;

    // TAAサブピクセルジッター（Halton(2,3)シーケンス、16フレームループ）
    {
        auto Halton = [](int idx, int base) {
            float f = 1.0f, r = 0.0f;
            while (idx > 0) { f /= base; r += f * (idx % base); idx /= base; }
            return r;
        };
        if (m_taaEnabled)
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

    // ImGuiフレーム
    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame(1.0f, 1.0f);
    ImGui::NewFrame();
    BuildImGuiUI();
    ImGui::Render();
}

void D3D12HelloTriangle::BuildImGuiUI()
{
    ImGui::Begin("Scene Controls");

    // ---- パフォーマンス ----
    ImGui::Text("FPS: %.1f  (%.2f ms)", ImGui::GetIO().Framerate, 1000.0f / ImGui::GetIO().Framerate);

    // ---- 時間 ----
    ImGui::Separator(); ImGui::Text("--- Time ---");
    ImGui::SliderFloat("Time Scale", &m_timeScale, 0.0f, 10.0f, "%.2f x");
    ImGui::SameLine();
    if (ImGui::Button(m_timePaused ? "Resume" : "Pause"))
        m_timePaused = !m_timePaused;

    // ---- 天気 ----
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

    // ---- 月 ----
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

    // ---- ブルーム ----
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

    // ---- 被写界深度 ----
    ImGui::Separator(); ImGui::Text("--- Depth of Field ---");
    ImGui::Checkbox("Enable DOF", &m_dofEnabled);
    if (m_dofEnabled)
    {
        ImGui::SliderFloat("Focus Depth",  &m_dofFocusDepth, 0.5f, 1.0f,  "%.3f");
        ImGui::SliderFloat("Focus Range",  &m_dofFocusRange, 0.02f, 0.5f, "%.3f");
        ImGui::SliderFloat("Max Blur",     &m_dofMaxRadius,  0.002f, 0.025f, "%.4f");
    }

    // ---- カメラ ----
    ImGui::Separator(); ImGui::Text("--- Camera ---");
    ImGui::SliderFloat("Vignette",   &m_vignetteStrength, 0.0f, 1.5f, "%.2f");
    ImGui::SliderFloat("Film Grain", &m_grainStrength,    0.0f, 0.08f, "%.3f");

    // ---- SSR（スクリーンスペース反射） ----
    ImGui::Separator(); ImGui::Text("--- SSAO ---");
    ImGui::Checkbox("Enable SSAO", &m_ssaoEnabled);
    if (m_ssaoEnabled)
    {
        ImGui::SliderFloat("AO Strength", &m_ssaoStrength, 0.0f, 1.5f, "%.2f");
        ImGui::SliderFloat("AO Radius",   &m_ssaoRadius,   0.1f, 3.0f,  "%.2f");
    }

    ImGui::Separator(); ImGui::Text("--- SSR ---");
    ImGui::SliderFloat("SSR Strength", &m_ssrStrength, 0.0f, 1.0f, "%.2f");

    ImGui::Separator(); ImGui::Text("--- TAA ---");
    ImGui::Checkbox("Enable TAA", &m_taaEnabled);
    if (!m_taaEnabled) { m_currentJitter = { 0.0f, 0.0f }; m_renderer->SetJitter(0.0f, 0.0f); }
    if (m_taaEnabled)
        ImGui::SliderFloat("History Blend", &m_taaBlend, 0.5f, 0.98f, "%.2f");

    // ---- レンズフレア ----
    ImGui::Separator(); ImGui::Text("--- Floating Boxes ---");
    {
        int count = (int)m_floatingObject->GetBoxCount();
        ImGui::Text("Boxes: %d / %d", count, FloatingObject::MAX_BOXES);
        ImGui::SameLine();
        if (ImGui::Button("Spawn Box") && count < FloatingObject::MAX_BOXES)
            m_floatingObject->SpawnBox();
        ImGui::SameLine();
        if (ImGui::Button("Clear"))
            m_floatingObject->ClearBoxes();
    }

    ImGui::Separator(); ImGui::Text("--- Lens Flare ---");
    ImGui::Checkbox("Enable Lens Flare", &m_lensFlareEnabled);
    if (m_lensFlareEnabled)
        ImGui::SliderFloat("LF Strength", &m_lensFlareStrength, 0.1f, 3.0f, "%.2f");

    ImGui::End();
}

void D3D12HelloTriangle::InitBloom()
{
    // --- ブルームRT（ブラー中にHDR値を保持するためR16Fを使用） ---
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

    // --- スロット2と3にRTVを配置 ---
    {
        CD3DX12_CPU_DESCRIPTOR_HANDLE h(
            m_rtvHeap->GetCPUDescriptorHandleForHeapStart(), 2, m_rtvDescriptorSize);
        m_device->CreateRenderTargetView(m_bloomExtractRT.Get(), nullptr, h);
        h.Offset(1, m_rtvDescriptorSize);
        m_device->CreateRenderTargetView(m_bloomBlurRT.Get(), nullptr, h);
    }

    // --- SRVヒープ：[0]=hdrRT（InitHDRで設定）、[1]=extract、[2]=blur ---
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 5; // [0]=hdrRT [1]=ブルーム抽出 [2]=ブルームブラー [3]=ゴッドレイRT [4]=SSAO
        hd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ThrowIfFailed(m_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_bloomSRVHeap)));
        m_bloomSRVIncrSize = m_device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }

    // ブルームRTの静的SRV（スロット1と2）
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

    // --- ブルームルートシグネチャ：[0] SRVテーブルt0、[1] 4ルート定数b0 ---
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

    // --- ブルーム輝度抽出とブラーPSOを読み込み作成する（R16F出力） ---
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
    // --- HDRレンダーターゲット（RTVヒープのスロット4） ---
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
            FrameCount + 2, m_rtvDescriptorSize); // スロット4
        m_device->CreateRenderTargetView(m_hdrRT.Get(), nullptr, hdrRtv);
    }

    // --- m_bloomSRVHeapのスロット0にhdrRTのSRVを設定（静的） ---
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

    // --- トーンマッピングルートシグネチャ：[0] 2-SRV(t0=hdr,t1=bloom)、[1] 2-SRV(t2=godrays,t3=ssao)、[2] 8定数 ---
    {
        CD3DX12_ROOT_PARAMETER params[3];
        CD3DX12_DESCRIPTOR_RANGE srvRange1, srvRange2;
        srvRange1.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0); // t0, t1
        srvRange2.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 2); // t2=godRay, t3=ssao
        params[0].InitAsDescriptorTable(1, &srvRange1, D3D12_SHADER_VISIBILITY_PIXEL);
        params[1].InitAsDescriptorTable(1, &srvRange2, D3D12_SHADER_VISIBILITY_PIXEL);
        params[2].InitAsConstants(8, 0); // bloom、exposure、godray、vignette、grain、time、aoStrength、pad

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

    // --- トーンマッピングPSO（LDRスワップチェーンフォーマットへ出力） ---
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

    // hdrRTはPSR状態（PopulateCommandListで遷移済み）
    if (!m_bloomEnabled) return;

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

    // === 輝度抽出パス：bloomSRVHeap[0]（hdrRTまたはtaaRT）→ extractRT ===
    float pBright[4] = { m_bloomThreshold, 0.f, 0.f, 0.f };
    cmd->SetPipelineState(m_bloomBrightPSO.Get());
    cmd->SetGraphicsRoot32BitConstants(1, 4, pBright, 0);
    cmd->SetGraphicsRootDescriptorTable(0, gpuSlot0);
    cmd->OMSetRenderTargets(1, &rtvExtract, FALSE, nullptr);
    cmd->ClearRenderTargetView(rtvExtract, kBlack, 0, nullptr);
    cmd->DrawInstanced(3, 1, 0, 0);

    // === ブラー：H+Vを2回反復（ピンポン：extract ↔ blur） ===
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
    // ループ後：hdrRT=PSR、extractRT=RT（最終ブルーム）、blurRT=PSR
}

void D3D12HelloTriangle::InitGodRays()
{
    UINT grW = m_width / 2, grH = m_height / 2;

    // --- ゴッドレイRT（半解像度、RTVヒープのスロット5） ---
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
            FrameCount + 3, m_rtvDescriptorSize); // スロット5
        m_device->CreateRenderTargetView(m_godRayRT.Get(), nullptr, rtv);
    }

    // --- m_bloomSRVHeapのスロット3にSRVを設定 ---
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

    // --- ゴッドレイルートシグネチャ：[0] 1-SRV(t0=hdrRT)、[1] 6ルート定数 ---
    {
        CD3DX12_ROOT_PARAMETER params[2];
        CD3DX12_DESCRIPTOR_RANGE srvRange;
        srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
        params[0].InitAsDescriptorTable(1, &srvRange, D3D12_SHADER_VISIBILITY_PIXEL);
        params[1].InitAsConstants(6, 0); // sunScreenXY、density、decay、weight、sunVis

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

    // --- ゴッドレイPSO ---
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

    // 太陽をスクリーンUVに投影する
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
        // 太陽の高さと画面外への距離でフェードする
        sunVis = std::clamp(sd3.y * 3.0f + 0.3f, 0.0f, 1.0f);
        float ax = fabsf(sunScreenX - 0.5f), ay = fabsf(sunScreenY - 0.5f);
        float offScreen = ax > ay ? ax : ay;
        sunVis *= std::clamp(1.0f - (offScreen - 0.5f) * 3.0f, 0.0f, 1.0f);
    }

    if (sunVis <= 0.001f) return;

    // ゴッドレイの放射状ブラーを半解像度RTに描画する
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
        m_bloomSRVHeap->GetGPUDescriptorHandleForHeapStart()); // スロット0 = hdrRT
    cmd->OMSetRenderTargets(1, &godRayRtv, FALSE, nullptr);
    cmd->DrawInstanced(3, 1, 0, 0);
}

void D3D12HelloTriangle::RenderToneMap()
{
    auto* cmd = m_commandList.Get();

    // ブルーム抽出とゴッドレイRTを読み取り用にPSRへ遷移する
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

    auto gpuSlot0 = m_bloomSRVHeap->GetGPUDescriptorHandleForHeapStart(); // hdrRT+bloom（スロット0,1）
    auto gpuSlot3 = CD3DX12_GPU_DESCRIPTOR_HANDLE(
        m_bloomSRVHeap->GetGPUDescriptorHandleForHeapStart(), 3, m_bloomSRVIncrSize); // ゴッドレイ

    cmd->SetGraphicsRootSignature(m_toneMappingRootSig.Get());
    ID3D12DescriptorHeap* heaps[] = { m_bloomSRVHeap.Get() };
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->RSSetViewports(1, &m_viewport);
    cmd->RSSetScissorRects(1, &m_scissorRect);

    float params[8] = {
        m_bloomStrength, m_exposure, m_godRayStrength,
        m_vignetteStrength, m_grainStrength, m_renderer->GetTime(),
        m_ssaoEnabled ? m_ssaoStrength : 0.0f, 0.0f
    };
    cmd->SetPipelineState(m_toneMappingPSO.Get());
    cmd->SetGraphicsRoot32BitConstants(2, 8, params, 0);
    cmd->SetGraphicsRootDescriptorTable(0, gpuSlot0);
    cmd->SetGraphicsRootDescriptorTable(1, gpuSlot3);
    cmd->OMSetRenderTargets(1, &rtvSwap, FALSE, nullptr);
    cmd->DrawInstanced(3, 1, 0, 0);

    // クリーンアップ：全ての中間RTをRENDER_TARGETに復元する
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

    // dofRTをRTに戻す
    if (m_dofEnabled)
    {
        auto bar = CD3DX12_RESOURCE_BARRIER::Transition(
            m_dofRT.Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_RENDER_TARGET);
        cmd->ResourceBarrier(1, &bar);
    }

    // taaRTをRTに戻す（次フレームの描画先として使用するため）
    if (m_taaEnabled && m_taaHistoryValid)
    {
        auto bar = CD3DX12_RESOURCE_BARRIER::Transition(
            m_taaRT.Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_RENDER_TARGET);
        cmd->ResourceBarrier(1, &bar);
    }

    // bloomSRVHeap[0]を常にhdrRTに復元する（TAA/DOFがリダイレクトした場合を含む）
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format                  = DXGI_FORMAT_R16G16B16A16_FLOAT;
        sd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels     = 1;
        m_device->CreateShaderResourceView(m_hdrRT.Get(), &sd,
            m_bloomSRVHeap->GetCPUDescriptorHandleForHeapStart()); // スロット0をhdrRTに復元
    }

    // 次フレームの空ブリット用にskySnapshotをCOPY_DESTに戻す
    if (m_skySnapshotInPSR)
    {
        auto bar = CD3DX12_RESOURCE_BARRIER::Transition(
            m_skySnapshotRT.Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_COPY_DEST);
        cmd->ResourceBarrier(1, &bar);
        m_skySnapshotInPSR = false;
    }

    // ssaoRT/ssaoBlurRTをRTに戻す（次フレームのRenderSSAO用）
    if (m_ssaoEnabled)
    {
        D3D12_RESOURCE_BARRIER bars[2] = {
            CD3DX12_RESOURCE_BARRIER::Transition(m_ssaoRT.Get(),
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_RENDER_TARGET),
            CD3DX12_RESOURCE_BARRIER::Transition(m_ssaoBlurRT.Get(),
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_RENDER_TARGET),
        };
        cmd->ResourceBarrier(2, bars);
    }

    // shadowMap を PSR → DEPTH_WRITE に戻す（次フレームの深度パス用）
    {
        auto b = CD3DX12_RESOURCE_BARRIER::Transition(m_shadowMap.Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_DEPTH_WRITE);
        cmd->ResourceBarrier(1, &b);
    }
}

void D3D12HelloTriangle::InitLensFlare()
{
    // ルートシグネチャ：6ルート定数（sunX、sunY、sunVis、strength、aspectRatio、time）
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

    // PSO：加算ブレンド、深度テストなし、LDRスワップチェーンフォーマットへ出力
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

    // 太陽をスクリーンUVに投影する（RenderGodRaysと同じロジック）
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
    // DOF出力RT（RTVヒープのスロットFrameCount+4 = 6）
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

    // DOF SRVヒープ：[0]=hdrRT  [1]=depthBuffer
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 2;
        hd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ThrowIfFailed(m_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_dofSRVHeap)));
        m_dofSRVIncrSize = m_device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        // スロット0：hdrRT（R16F）
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format                  = DXGI_FORMAT_R16G16B16A16_FLOAT;
        sd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels     = 1;
        m_device->CreateShaderResourceView(m_hdrRT.Get(), &sd,
            CD3DX12_CPU_DESCRIPTOR_HANDLE(
                m_dofSRVHeap->GetCPUDescriptorHandleForHeapStart(), 0, m_dofSRVIncrSize));

        // スロット1：デプスバッファ（R32_TYPELESSリソースのR32_FLOATビュー）
        sd.Format = DXGI_FORMAT_R32_FLOAT;
        m_device->CreateShaderResourceView(m_renderer->GetDepthBuffer(), &sd,
            CD3DX12_CPU_DESCRIPTOR_HANDLE(
                m_dofSRVHeap->GetCPUDescriptorHandleForHeapStart(), 1, m_dofSRVIncrSize));
    }

    // DOFルートシグネチャ：[0] 2-SRVテーブル、[1] 4ルート定数
    ComPtr<ID3D12RootSignature> dofRootSig;
    {
        CD3DX12_ROOT_PARAMETER params[2];
        CD3DX12_DESCRIPTOR_RANGE srvRange;
        srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0); // t0, t1
        params[0].InitAsDescriptorTable(1, &srvRange, D3D12_SHADER_VISIBILITY_PIXEL);
        params[1].InitAsConstants(4, 0); // focusDepth、focusRange、maxRadius、aspectRatio

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

        // ルートシグネチャをインラインで保存（必要に応じてヘッダーに追加する）
        // 簡略化のためローカル変数に保存してPSOをベイクする
        ComPtr<ID3D12RootSignature> rs;
        ThrowIfFailed(m_device->CreateRootSignature(0,
            sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&rs)));
        dofRootSig = rs;
    }

    // クラスメンバーとして保存する（ヘッダーでComPtr<ID3D12RootSignature> m_dofRootSigを宣言）
    // m_dofRootSigとm_dofPSOは別途ヘッダーに追加する
    // ローカル変数で保存して下記でPSOをビルドする：
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

    // ルートシグネチャとPSOを保存する（クラスメンバーとして追加が必要）
    m_dofRootSig = dofRootSig;
    ThrowIfFailed(m_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_dofPSO)));
}

void D3D12HelloTriangle::RenderDOF()
{
    auto* cmd = m_commandList.Get();

    if (!m_dofEnabled)
    {
        // パススルー：bloomSRVHeap[0]は引き続きhdrRTを指すよう維持する（既に正しい）
        return;
    }

    // hdrRTは現在PSR状態（RenderBloomから）。デプスはDEPTH_WRITE状態
    // シェーダー読み取り用にデプスをPSRへ遷移する
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

    // デプスをDEPTH_WRITEに戻す
    auto b1 = CD3DX12_RESOURCE_BARRIER::Transition(
        m_renderer->GetDepthBuffer(),
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_DEPTH_WRITE);
    cmd->ResourceBarrier(1, &b1);

    // ToneMapが読み取れるようにdofRTをPSRへ遷移する
    auto b2 = CD3DX12_RESOURCE_BARRIER::Transition(
        m_dofRT.Get(),
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmd->ResourceBarrier(1, &b2);

    // ToneMapの入力をリダイレクトする：bloomSRVHeapスロット0をdofRTに更新する
    D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format                  = DXGI_FORMAT_R16G16B16A16_FLOAT;
    sd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels     = 1;
    m_device->CreateShaderResourceView(m_dofRT.Get(), &sd,
        m_bloomSRVHeap->GetCPUDescriptorHandleForHeapStart()); // スロット0
}

void D3D12HelloTriangle::InitSSR()
{
    // 空スナップショットテクスチャ：hdrRTと同じフォーマット。COPY_DESTとして使用後PSRになる
    {
        auto heapProp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        auto rtDesc   = CD3DX12_RESOURCE_DESC::Tex2D(
            DXGI_FORMAT_R16G16B16A16_FLOAT, m_width, m_height, 1, 1);
        ThrowIfFailed(m_device->CreateCommittedResource(
            &heapProp, D3D12_HEAP_FLAG_NONE, &rtDesc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&m_skySnapshotRT)));
    }

    // 海洋SRVヒープ：[0]=heightMap  [1]=dztMap  [2]=skySnapshot  [3]=shadowMap
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 4;  // t3 はInitShadowMapで追加
        hd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ThrowIfFailed(m_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_oceanSRVHeap)));
        m_oceanSRVIncrSize = m_device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }

    // スロット0+1：FFT高さ/dztマップ（R32G32B32A32_FLOAT）
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

    // スロット2：空スナップショット（R16G16B16A16_FLOAT）
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

// シーンをレンダリングする
void D3D12HelloTriangle::OnRender()
{
    ThrowIfFailed(m_commandAllocator->Reset());
    ThrowIfFailed(m_commandList->Reset(m_commandAllocator.Get(), nullptr));

    // コンピュートパス
    m_oceanFFT->Dispatch(m_commandList,m_renderer->GetTime());

    // UAV → SRV。ウェーブシェーダーがサンプリングできるよう遷移する
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

    // シャドウマップパス（シーン描画より前に実行）
    RenderShadowMap();

    // ctxを構築する
    RenderContext ctx;
    ctx.cmd = m_commandList.Get();
    ctx.renderTarget = m_renderTargets[m_frameIndex].Get();
    // シーンをHDR RTにレンダリングする（スロットFrameCount+2 = 4）
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

    // Present → RenderTargetへ遷移
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

    // --- SSR用にm_skySnapshotRTへ空のスナップショットを取る ---
    {
        // hdrRTは現在RT状態。ブリット用にCOPY_SOURCEへ遷移する
        auto b0 = CD3DX12_RESOURCE_BARRIER::Transition(
            m_hdrRT.Get(),
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
        m_commandList->ResourceBarrier(1, &b0);

        // skySnapshotは最初のフレームはCOPY_DEST状態、それ以降はPSR状態
        if (m_skySnapshotInPSR)
        {
            auto b1 = CD3DX12_RESOURCE_BARRIER::Transition(
                m_skySnapshotRT.Get(),
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_COPY_DEST);
            m_commandList->ResourceBarrier(1, &b1);
        }

        m_commandList->CopyResource(m_skySnapshotRT.Get(), m_hdrRT.Get());

        // skySnapshot → PSRへ。海洋シェーダーが読み取れるよう遷移
        auto b2 = CD3DX12_RESOURCE_BARRIER::Transition(
            m_skySnapshotRT.Get(),
            D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        m_commandList->ResourceBarrier(1, &b2);
        m_skySnapshotInPSR = true;

        // hdrRTを海洋・雨レンダリング用にRTへ戻す
        auto b3 = CD3DX12_RESOURCE_BARRIER::Transition(
            m_hdrRT.Get(),
            D3D12_RESOURCE_STATE_COPY_SOURCE,
            D3D12_RESOURCE_STATE_RENDER_TARGET);
        m_commandList->ResourceBarrier(1, &b3);
        m_commandList->OMSetRenderTargets(1, &ctx.rtv, FALSE, &ctx.dsv);
    }

    // 海洋SRVヒープをバインドする（t0=高さ、t1=dzt、t2=skySnapshot）
    ID3D12DescriptorHeap* srvHeaps[] = { m_oceanSRVHeap.Get() };
    m_commandList->SetDescriptorHeaps(1, srvHeaps);
    m_commandList->SetGraphicsRootDescriptorTable(1,
        m_oceanSRVHeap->GetGPUDescriptorHandleForHeapStart());
    m_commandList->SetGraphicsRootConstantBufferView(
        2, m_rainSystem->GetRippleCBAddress());
    m_commandList->SetGraphicsRootConstantBufferView(
        3, m_shadowSceneCB->GetGPUVirtualAddress());

    m_renderer->Render(ctx);
    if (m_renderer->IsShowcaseMode())
        m_renderer->RenderWaterBox(ctx);

    // 浮遊木箱
    m_floatingObject->Render(ctx,
        m_skyDome->GetSunDirection(),
        m_skyDome->GetSunIntensity(),
        m_skyDome->GetSunColor(),
        m_renderer->GetCameraPos());

    m_rainSystem->Render(ctx,
        m_renderer->GetViewMatrix(),
        m_renderer->GetProjMatrix(),
        m_renderer->GetCameraPos());

    RenderLightning();
    RenderSSAO();

    // hdrRTをPSRへ遷移（全ポストプロセスパスの前提条件）
    {
        auto bar = CD3DX12_RESOURCE_BARRIER::Transition(
            m_hdrRT.Get(),
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        m_commandList->ResourceBarrier(1, &bar);
    }
    RenderTAA();
    RenderBloom();
    RenderGodRays();
    RenderDOF();
    RenderToneMap();
    RenderLensFlare();

    // ImGuiをレンダリングする（RT→Presentバリアの前）
    {
        ID3D12DescriptorHeap* imguiHeaps[] = { m_imguiSrvHeap.Get() };
        m_commandList->SetDescriptorHeaps(1, imguiHeaps);
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), m_commandList.Get());
    }

    // RenderTarget → Presentへ遷移
    auto barrierToPresent = CD3DX12_RESOURCE_BARRIER::Transition(
        ctx.renderTarget,
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_PRESENT);
    m_commandList->ResourceBarrier(1, &barrierToPresent);

    // SRV → UAVのバリアはClose前に記録する必要がある
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

    // Closeの後にExecuteする
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
    // 継続前にフレームの完了を待機するのはベストプラクティスではない。
    // ここでは簡略化のためこの実装を採用している。D3D12HelloFrameBufferingサンプルでは
    // フェンスを使用した効率的なリソース管理とGPU最大活用法を示している。

    // フェンス値をシグナルしてインクリメントする
    const UINT64 fence = m_fenceValue;
    ThrowIfFailed(m_commandQueue->Signal(m_fence.Get(), fence));
    m_fenceValue++;

    // 前のフレームが完了するまで待機する
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

void D3D12HelloTriangle::InitSSAO()
{
    UINT hw = m_width / 2, hh = m_height / 2;
    auto hp  = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    auto fmt = DXGI_FORMAT_R16_FLOAT;
    D3D12_CLEAR_VALUE cv = {};  cv.Format = fmt;

    // ssaoRT (slot FrameCount+7 = 9)
    {
        auto rd = CD3DX12_RESOURCE_DESC::Tex2D(fmt, hw, hh, 1, 1);
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        ThrowIfFailed(m_device->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_RENDER_TARGET, &cv,
            IID_PPV_ARGS(&m_ssaoRT)));
        CD3DX12_CPU_DESCRIPTOR_HANDLE rtv(
            m_rtvHeap->GetCPUDescriptorHandleForHeapStart(), FrameCount + 7, m_rtvDescriptorSize);
        m_device->CreateRenderTargetView(m_ssaoRT.Get(), nullptr, rtv);
    }

    // ssaoBlurRT (slot FrameCount+8 = 10)
    {
        auto rd = CD3DX12_RESOURCE_DESC::Tex2D(fmt, hw, hh, 1, 1);
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        ThrowIfFailed(m_device->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_RENDER_TARGET, &cv,
            IID_PPV_ARGS(&m_ssaoBlurRT)));
        CD3DX12_CPU_DESCRIPTOR_HANDLE rtv(
            m_rtvHeap->GetCPUDescriptorHandleForHeapStart(), FrameCount + 8, m_rtvDescriptorSize);
        m_device->CreateRenderTargetView(m_ssaoBlurRT.Get(), nullptr, rtv);
    }

    // SSAO SRV heap: [0]=depth [1]=ssaoRT(for blur input)
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 2;
        hd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ThrowIfFailed(m_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_ssaoSRVHeap)));
        m_ssaoSRVIncrSize = m_device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        // slot 0: depth buffer (R32_FLOAT SRV from D32_FLOAT)
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format                  = DXGI_FORMAT_R32_FLOAT;
        sd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels     = 1;
        m_device->CreateShaderResourceView(m_renderer->GetDepthBuffer(), &sd,
            CD3DX12_CPU_DESCRIPTOR_HANDLE(m_ssaoSRVHeap->GetCPUDescriptorHandleForHeapStart(),
                0, m_ssaoSRVIncrSize));

        // slot 1: ssaoRT (R16_FLOAT, blur input)
        sd.Format = DXGI_FORMAT_R16_FLOAT;
        m_device->CreateShaderResourceView(m_ssaoRT.Get(), &sd,
            CD3DX12_CPU_DESCRIPTOR_HANDLE(m_ssaoSRVHeap->GetCPUDescriptorHandleForHeapStart(),
                1, m_ssaoSRVIncrSize));
    }

    // Add ssaoBlurRT SRV to bloomSRVHeap slot 4 (t3 in ToneMap)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format                  = DXGI_FORMAT_R16_FLOAT;
        sd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels     = 1;
        m_device->CreateShaderResourceView(m_ssaoBlurRT.Get(), &sd,
            CD3DX12_CPU_DESCRIPTOR_HANDLE(m_bloomSRVHeap->GetCPUDescriptorHandleForHeapStart(),
                4, m_bloomSRVIncrSize));
    }

    // Upload constant buffer (256 bytes)
    {
        auto hp2 = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        auto buf = CD3DX12_RESOURCE_DESC::Buffer(256);
        ThrowIfFailed(m_device->CreateCommittedResource(
            &hp2, D3D12_HEAP_FLAG_NONE, &buf,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_ssaoCB)));
        CD3DX12_RANGE r(0, 0);
        m_ssaoCB->Map(0, &r, reinterpret_cast<void**>(&m_mappedSSAOCB));
        memset(m_mappedSSAOCB, 0, 256);

        // Pre-computed hemisphere kernel (8 samples, z >= 0)
        static const XMFLOAT4 kKernel[8] = {
            { 0.5381f,  0.1956f,  0.3215f, 0}, { 0.1069f,  0.1149f,  0.2749f, 0},
            {-0.4742f,  0.3518f,  0.3817f, 0}, { 0.2815f,  0.4316f,  0.1954f, 0},
            {-0.1259f, -0.2354f,  0.3926f, 0}, {-0.3134f, -0.2248f,  0.3127f, 0},
            { 0.4348f, -0.3194f,  0.1547f, 0}, {-0.2045f,  0.4285f,  0.2849f, 0}
        };
        memcpy(m_mappedSSAOCB->kernel, kKernel, sizeof(kKernel));
        m_mappedSSAOCB->screenW = (float)hw;
        m_mappedSSAOCB->screenH = (float)hh;
        m_mappedSSAOCB->nearZ   = 0.1f;
        m_mappedSSAOCB->farZ    = 2000.0f;
        m_mappedSSAOCB->radius  = m_ssaoRadius;
        m_mappedSSAOCB->bias    = 0.025f;
        // projX/projY filled each frame in RenderSSAO
    }

    // SSAO compute root sig: [0] CBV(b0), [1] SRV table(t0=depth)
    {
        CD3DX12_ROOT_PARAMETER params[2];
        params[0].InitAsConstantBufferView(0, 0, D3D12_SHADER_VISIBILITY_PIXEL);
        CD3DX12_DESCRIPTOR_RANGE sr; sr.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
        params[1].InitAsDescriptorTable(1, &sr, D3D12_SHADER_VISIBILITY_PIXEL);

        CD3DX12_STATIC_SAMPLER_DESC samplers[2];
        samplers[0].Init(0, D3D12_FILTER_MIN_MAG_MIP_POINT,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
        samplers[1].Init(1, D3D12_FILTER_MIN_MAG_MIP_LINEAR,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);

        CD3DX12_ROOT_SIGNATURE_DESC rsd;
        rsd.Init(2, params, 2, samplers, D3D12_ROOT_SIGNATURE_FLAG_NONE);
        ComPtr<ID3DBlob> sig, err;
        ThrowIfFailed(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err));
        ThrowIfFailed(m_device->CreateRootSignature(0,
            sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&m_ssaoRootSig)));
    }

    // SSAO blur root sig: [0] SRV table(t0=ssaoRT), [1] 4 constants
    {
        CD3DX12_ROOT_PARAMETER params[2];
        CD3DX12_DESCRIPTOR_RANGE sr; sr.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
        params[0].InitAsDescriptorTable(1, &sr, D3D12_SHADER_VISIBILITY_PIXEL);
        params[1].InitAsConstants(4, 0);

        CD3DX12_STATIC_SAMPLER_DESC samp(1, D3D12_FILTER_MIN_MAG_MIP_LINEAR,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);

        CD3DX12_ROOT_SIGNATURE_DESC rsd;
        rsd.Init(2, params, 1, &samp, D3D12_ROOT_SIGNATURE_FLAG_NONE);
        ComPtr<ID3DBlob> sig, err;
        ThrowIfFailed(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err));
        ThrowIfFailed(m_device->CreateRootSignature(0,
            sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&m_ssaoBlurRootSig)));
    }

    // Load shaders and create PSOs
    {
        UINT8 *pVS = nullptr, *pSSAOPS = nullptr, *pBLURPS = nullptr;
        UINT   vsLen = 0, ssaopsLen = 0, blurpsLen = 0;
        ThrowIfFailed(ReadDataFromFile(GetAssetFullPath(L"ssao_SSAOVS.cso").c_str(),    &pVS,     &vsLen));
        ThrowIfFailed(ReadDataFromFile(GetAssetFullPath(L"ssao_SSAOPS.cso").c_str(),    &pSSAOPS, &ssaopsLen));
        ThrowIfFailed(ReadDataFromFile(GetAssetFullPath(L"ssao_SSAOBLURPS.cso").c_str(),&pBLURPS, &blurpsLen));

        auto MakePSO = [&](ID3D12RootSignature* rs, const void* vs, UINT vsz,
                                                     const void* ps, UINT psz,
                           ID3D12PipelineState** outPSO) {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
            pd.pRootSignature           = rs;
            pd.VS                       = { vs, vsz };
            pd.PS                       = { ps, psz };
            pd.RasterizerState          = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
            pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            pd.BlendState               = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
            pd.DepthStencilState.DepthEnable   = FALSE;
            pd.DepthStencilState.StencilEnable = FALSE;
            pd.DSVFormat             = DXGI_FORMAT_UNKNOWN;
            pd.SampleMask            = UINT_MAX;
            pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            pd.NumRenderTargets      = 1;
            pd.RTVFormats[0]         = DXGI_FORMAT_R16_FLOAT;
            pd.SampleDesc.Count      = 1;
            ThrowIfFailed(m_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(outPSO)));
        };

        MakePSO(m_ssaoRootSig.Get(),     pVS, vsLen, pSSAOPS, ssaopsLen, &m_ssaoPSO);
        MakePSO(m_ssaoBlurRootSig.Get(), pVS, vsLen, pBLURPS, blurpsLen, &m_ssaoBlurPSO);

        free(pVS); free(pSSAOPS); free(pBLURPS);
    }
}

void D3D12HelloTriangle::RenderSSAO()
{
    if (!m_ssaoEnabled) return;

    auto* cmd = m_commandList.Get();
    UINT  hw  = m_width / 2,  hh  = m_height / 2;
    D3D12_VIEWPORT halfVP = { 0, 0, (float)hw, (float)hh, 0, 1 };
    D3D12_RECT     halfSC = { 0, 0, (LONG)hw,  (LONG)hh  };

    // Update dynamic CB fields each frame
    {
        XMMATRIX proj = m_renderer->GetCamera().GetProjMatrix();
        m_mappedSSAOCB->projX  = XMVectorGetX(proj.r[0]); // M._11
        m_mappedSSAOCB->projY  = XMVectorGetY(proj.r[1]); // M._22
        m_mappedSSAOCB->radius = m_ssaoRadius;
    }

    // Transition depth DEPTH_WRITE → PSR
    auto b0 = CD3DX12_RESOURCE_BARRIER::Transition(m_renderer->GetDepthBuffer(),
        D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmd->ResourceBarrier(1, &b0);

    auto ssaoRtv = CD3DX12_CPU_DESCRIPTOR_HANDLE(
        m_rtvHeap->GetCPUDescriptorHandleForHeapStart(), FrameCount + 7, m_rtvDescriptorSize);
    auto blurRtv = CD3DX12_CPU_DESCRIPTOR_HANDLE(
        m_rtvHeap->GetCPUDescriptorHandleForHeapStart(), FrameCount + 8, m_rtvDescriptorSize);
    const float kWhite[] = { 1, 1, 1, 1 };

    ID3D12DescriptorHeap* heaps[] = { m_ssaoSRVHeap.Get() };
    cmd->SetDescriptorHeaps(1, heaps);

    // --- Pass 1: SSAO compute ---
    cmd->SetGraphicsRootSignature(m_ssaoRootSig.Get());
    cmd->SetPipelineState(m_ssaoPSO.Get());
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->RSSetViewports(1, &halfVP);
    cmd->RSSetScissorRects(1, &halfSC);
    cmd->SetGraphicsRootConstantBufferView(0, m_ssaoCB->GetGPUVirtualAddress());
    cmd->SetGraphicsRootDescriptorTable(1,
        m_ssaoSRVHeap->GetGPUDescriptorHandleForHeapStart()); // slot 0 = depth
    cmd->OMSetRenderTargets(1, &ssaoRtv, FALSE, nullptr);
    cmd->ClearRenderTargetView(ssaoRtv, kWhite, 0, nullptr);
    cmd->DrawInstanced(3, 1, 0, 0);

    // ssaoRT RT → PSR; transition depth back
    {
        D3D12_RESOURCE_BARRIER bars[2] = {
            CD3DX12_RESOURCE_BARRIER::Transition(m_ssaoRT.Get(),
                D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(m_renderer->GetDepthBuffer(),
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE),
        };
        cmd->ResourceBarrier(2, bars);
    }

    // --- Pass 2: box blur ---
    float blurConst[4] = { 1.0f / hw, 1.0f / hh, 0, 0 };
    cmd->SetGraphicsRootSignature(m_ssaoBlurRootSig.Get());
    cmd->SetPipelineState(m_ssaoBlurPSO.Get());
    cmd->SetGraphicsRoot32BitConstants(1, 4, blurConst, 0);
    cmd->SetGraphicsRootDescriptorTable(0,
        CD3DX12_GPU_DESCRIPTOR_HANDLE(m_ssaoSRVHeap->GetGPUDescriptorHandleForHeapStart(),
            1, m_ssaoSRVIncrSize)); // slot 1 = ssaoRT
    cmd->OMSetRenderTargets(1, &blurRtv, FALSE, nullptr);
    cmd->DrawInstanced(3, 1, 0, 0);

    // ssaoBlurRT RT → PSR (tone mapping reads it); ssaoRT stays PSR until frame end
    auto b1 = CD3DX12_RESOURCE_BARRIER::Transition(m_ssaoBlurRT.Get(),
        D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmd->ResourceBarrier(1, &b1);
}

void D3D12HelloTriangle::InitShadowMap()
{
    // 2048×2048 深度テクスチャ（DSV＋SRV両方向）
    {
        auto hp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        auto rd = CD3DX12_RESOURCE_DESC::Tex2D(
            DXGI_FORMAT_R32_TYPELESS, kShadowSize, kShadowSize, 1, 1);
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE cv = {};
        cv.Format = DXGI_FORMAT_D32_FLOAT;
        cv.DepthStencil.Depth = 1.0f;
        ThrowIfFailed(m_device->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv,
            IID_PPV_ARGS(&m_shadowMap)));
    }

    // DSV heap
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        hd.NumDescriptors = 1;
        ThrowIfFailed(m_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_shadowDSVHeap)));
        D3D12_DEPTH_STENCIL_VIEW_DESC dsvd = {};
        dsvd.Format        = DXGI_FORMAT_D32_FLOAT;
        dsvd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        m_device->CreateDepthStencilView(m_shadowMap.Get(), &dsvd,
            m_shadowDSVHeap->GetCPUDescriptorHandleForHeapStart());
    }

    // shadow map SRV を oceanSRVHeap[3] に登録
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format                  = DXGI_FORMAT_R32_FLOAT;
        sd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels     = 1;
        m_device->CreateShaderResourceView(m_shadowMap.Get(), &sd,
            CD3DX12_CPU_DESCRIPTOR_HANDLE(
                m_oceanSRVHeap->GetCPUDescriptorHandleForHeapStart(),
                3, m_oceanSRVIncrSize));
    }

    // ShadowSceneCB（シェーダー側でライト行列＋バイアスを参照）
    {
        auto hp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        auto bd = CD3DX12_RESOURCE_DESC::Buffer(sizeof(ShadowSceneCB));
        ThrowIfFailed(m_device->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&m_shadowSceneCB)));
        CD3DX12_RANGE r(0, 0);
        m_shadowSceneCB->Map(0, &r, reinterpret_cast<void**>(&m_mappedShadowSceneCB));
        memset(m_mappedShadowSceneCB, 0, sizeof(ShadowSceneCB));
    }

    // 深度専用ルートシグネチャ：b0 = ShadowInstCB のみ
    {
        CD3DX12_ROOT_PARAMETER param;
        param.InitAsConstantBufferView(0);
        CD3DX12_ROOT_SIGNATURE_DESC rsd;
        rsd.Init(1, &param, 0, nullptr,
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
        ComPtr<ID3DBlob> sig, err;
        ThrowIfFailed(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err));
        ThrowIfFailed(m_device->CreateRootSignature(0,
            sig->GetBufferPointer(), sig->GetBufferSize(),
            IID_PPV_ARGS(&m_shadowRootSig)));
    }

    // 深度専用 PSO（RTなし、フロントフェースカリング＋傾きバイアスでピーターパン防止）
    {
        UINT8* pVS = nullptr; UINT vsLen = 0;
        ThrowIfFailed(ReadDataFromFile(
            GetAssetFullPath(L"shadowmap_ShadowVS.cso").c_str(), &pVS, &vsLen));

        D3D12_INPUT_ELEMENT_DESC layout[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
              D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        };
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
        pd.InputLayout            = { layout, 1 };
        pd.pRootSignature         = m_shadowRootSig.Get();
        pd.VS                     = { pVS, vsLen };
        pd.RasterizerState        = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        pd.RasterizerState.DepthBias            = 8000;
        pd.RasterizerState.SlopeScaledDepthBias = 3.0f;
        pd.RasterizerState.CullMode             = D3D12_CULL_MODE_FRONT;
        pd.BlendState             = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        pd.DepthStencilState      = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        pd.DSVFormat              = DXGI_FORMAT_D32_FLOAT;
        pd.SampleMask             = UINT_MAX;
        pd.PrimitiveTopologyType  = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets       = 0;
        pd.SampleDesc.Count       = 1;
        ThrowIfFailed(m_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_shadowPSO)));
        free(pVS);
    }

    // FloatingObject 側の影インスタンス CB を初期化
    m_floatingObject->InitShadowResources(m_device.Get());
    m_lightViewProj = XMMatrixIdentity();
}

void D3D12HelloTriangle::RenderShadowMap()
{
    auto* cmd = m_commandList.Get();
    auto  dsv = m_shadowDSVHeap->GetCPUDescriptorHandleForHeapStart();

    // 常にクリア（夜間でも有効なSRV状態を維持）
    cmd->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    float sunY     = m_skyDome->GetSunDirection().y;
    bool  doShadow = m_shadowEnabled && sunY > 0.05f;

    if (doShadow)
    {
        XMFLOAT3 sunDir = m_skyDome->GetSunDirection();
        XMFLOAT3 camPos = m_renderer->GetCameraPos();

        XMVECTOR eye = XMVectorSet(
            camPos.x - sunDir.x * 150.f,
            camPos.y - sunDir.y * 150.f + 80.f,
            camPos.z - sunDir.z * 150.f, 1.f);
        XMVECTOR at  = XMVectorSet(camPos.x, 0.f, camPos.z, 0.f);
        XMVECTOR up  = fabsf(sunDir.y) > 0.98f ?
            XMVectorSet(0, 0, 1, 0) : XMVectorSet(0, 1, 0, 0);

        XMMATRIX view = XMMatrixLookAtLH(eye, at, up);
        XMMATRIX proj = XMMatrixOrthographicLH(200.f, 200.f, 1.f, 400.f);
        m_lightViewProj = view * proj;

        D3D12_VIEWPORT vp = { 0, 0, (float)kShadowSize, (float)kShadowSize, 0, 1 };
        D3D12_RECT     sc = { 0, 0, (LONG)kShadowSize,  (LONG)kShadowSize  };
        cmd->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
        cmd->RSSetViewports(1, &vp);
        cmd->RSSetScissorRects(1, &sc);

        m_floatingObject->RenderDepth(cmd,
            m_shadowRootSig.Get(), m_shadowPSO.Get(), m_lightViewProj);
    }

    // ShadowSceneCB を更新
    {
        auto* cb = reinterpret_cast<ShadowSceneCB*>(m_mappedShadowSceneCB);
        cb->lightViewProj  = XMMatrixTranspose(m_lightViewProj);
        cb->shadowBias     = m_shadowBias;
        cb->shadowStrength = doShadow ? m_shadowStrength : 0.0f;
        cb->shadowEnabled  = doShadow ? 1.0f : 0.0f;
    }

    // DEPTH_WRITE → PSR（海洋シェーダーがt3でサンプリング）
    auto b = CD3DX12_RESOURCE_BARRIER::Transition(m_shadowMap.Get(),
        D3D12_RESOURCE_STATE_DEPTH_WRITE,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmd->ResourceBarrier(1, &b);
}

void D3D12HelloTriangle::InitLightning()
{
    // Upload CB (256 bytes)
    {
        auto hp  = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        auto buf = CD3DX12_RESOURCE_DESC::Buffer(256);
        ThrowIfFailed(m_device->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &buf,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&m_lightningCB)));
        CD3DX12_RANGE r(0, 0);
        m_lightningCB->Map(0, &r, reinterpret_cast<void**>(&m_mappedLightningCB));
        memset(m_mappedLightningCB, 0, 256);
        m_mappedLightningCB->aspect = (float)m_width / m_height;
    }

    // Root signature: [0] CBV(b0), no SRVs needed
    {
        CD3DX12_ROOT_PARAMETER params[1];
        params[0].InitAsConstantBufferView(0, 0, D3D12_SHADER_VISIBILITY_PIXEL);

        CD3DX12_ROOT_SIGNATURE_DESC rsd;
        rsd.Init(1, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE);
        ComPtr<ID3DBlob> sig, err;
        ThrowIfFailed(D3D12SerializeRootSignature(&rsd,
            D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err));
        ThrowIfFailed(m_device->CreateRootSignature(0,
            sig->GetBufferPointer(), sig->GetBufferSize(),
            IID_PPV_ARGS(&m_lightningRootSig)));
    }

    // PSO: additive blend, no depth, fullscreen triangle
    {
        UINT8 *pVS = nullptr, *pPS = nullptr;
        UINT   vsLen = 0, psLen = 0;
        ThrowIfFailed(ReadDataFromFile(
            GetAssetFullPath(L"lightning_LightningVS.cso").c_str(), &pVS, &vsLen));
        ThrowIfFailed(ReadDataFromFile(
            GetAssetFullPath(L"lightning_LightningPS.cso").c_str(), &pPS, &psLen));

        D3D12_BLEND_DESC blend = {};
        blend.RenderTarget[0].BlendEnable           = TRUE;
        blend.RenderTarget[0].SrcBlend              = D3D12_BLEND_ONE;
        blend.RenderTarget[0].DestBlend             = D3D12_BLEND_ONE;
        blend.RenderTarget[0].BlendOp               = D3D12_BLEND_OP_ADD;
        blend.RenderTarget[0].SrcBlendAlpha         = D3D12_BLEND_ONE;
        blend.RenderTarget[0].DestBlendAlpha        = D3D12_BLEND_ZERO;
        blend.RenderTarget[0].BlendOpAlpha          = D3D12_BLEND_OP_ADD;
        blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature           = m_lightningRootSig.Get();
        pd.VS                       = CD3DX12_SHADER_BYTECODE(pVS, vsLen);
        pd.PS                       = CD3DX12_SHADER_BYTECODE(pPS, psLen);
        pd.RasterizerState          = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.BlendState               = blend;
        pd.DepthStencilState.DepthEnable   = FALSE;
        pd.DepthStencilState.StencilEnable = FALSE;
        pd.DSVFormat             = DXGI_FORMAT_UNKNOWN;
        pd.SampleMask            = UINT_MAX;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets      = 1;
        pd.RTVFormats[0]         = DXGI_FORMAT_R16G16B16A16_FLOAT;
        pd.SampleDesc.Count      = 1;
        ThrowIfFailed(m_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_lightningPSO)));

        free(pVS); free(pPS);
    }
}

void D3D12HelloTriangle::GenerateLightningBolt()
{
    auto& cb = *m_mappedLightningCB;
    cb.aspect = (float)m_width / m_height;

    // Simple seeded random using current time
    auto rng = [&](float lo, float hi) -> float {
        static unsigned s = 12345;
        s = s * 1664525u + 1013904223u;
        return lo + (s >> 16) / 65535.0f * (hi - lo);
    };

    // Main bolt: upper sky → near horizon
    float sx = rng(0.25f, 0.75f),  sy = rng(0.05f, 0.25f);
    float ex = sx + rng(-0.2f, 0.2f), ey = rng(0.55f, 0.72f);

    constexpr int NB = 8;
    cb.pts[0] = { sx, sy, 0, 0 };
    for (int i = 1; i < NB - 1; i++)
    {
        float t = (float)i / (NB - 1);
        cb.pts[i] = {
            sx + (ex - sx) * t + rng(-0.055f, 0.055f),
            sy + (ey - sy) * t,
            0, 0
        };
    }
    cb.pts[NB - 1] = { ex, ey, 0, 0 };
    cb.numBolt = NB;

    // Branch: splits off from mid-bolt
    int split = 2 + (int)rng(0, 3);
    float bx0 = cb.pts[split].x, by0 = cb.pts[split].y;
    float bex = bx0 + rng(-0.12f, 0.12f), bey = by0 + rng(0.08f, 0.18f);

    constexpr int NBRANCH = 5;
    cb.pts[NB] = { bx0, by0, 0, 0 };
    for (int i = 1; i < NBRANCH - 1; i++)
    {
        float t = (float)i / (NBRANCH - 1);
        cb.pts[NB + i] = {
            bx0 + (bex - bx0) * t + rng(-0.022f, 0.022f),
            by0 + (bey - by0) * t,
            0, 0
        };
    }
    cb.pts[NB + NBRANCH - 1] = { bex, bey, 0, 0 };
    cb.numBranch = NBRANCH;
}

void D3D12HelloTriangle::RenderLightning()
{
    if (m_mappedLightningCB->intensity <= 0.001f) return;

    auto* cmd = m_commandList.Get();
    auto  hdrRtv = CD3DX12_CPU_DESCRIPTOR_HANDLE(
        m_rtvHeap->GetCPUDescriptorHandleForHeapStart(),
        FrameCount + 2, m_rtvDescriptorSize);

    cmd->SetGraphicsRootSignature(m_lightningRootSig.Get());
    cmd->SetPipelineState(m_lightningPSO.Get());
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->RSSetViewports(1, &m_viewport);
    cmd->RSSetScissorRects(1, &m_scissorRect);
    cmd->SetGraphicsRootConstantBufferView(0, m_lightningCB->GetGPUVirtualAddress());
    cmd->OMSetRenderTargets(1, &hdrRtv, FALSE, nullptr);
    cmd->DrawInstanced(3, 1, 0, 0);
}

void D3D12HelloTriangle::InitTAA()
{
    auto format   = DXGI_FORMAT_R16G16B16A16_FLOAT;
    auto heapProp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    D3D12_CLEAR_VALUE clearVal = {};
    clearVal.Format = format;

    // TAAリゾルブRT（スロット FrameCount+5 = 7）
    {
        auto rtDesc = CD3DX12_RESOURCE_DESC::Tex2D(format, m_width, m_height, 1, 1);
        rtDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        ThrowIfFailed(m_device->CreateCommittedResource(
            &heapProp, D3D12_HEAP_FLAG_NONE, &rtDesc,
            D3D12_RESOURCE_STATE_RENDER_TARGET, &clearVal,
            IID_PPV_ARGS(&m_taaRT)));
        CD3DX12_CPU_DESCRIPTOR_HANDLE rtv(
            m_rtvHeap->GetCPUDescriptorHandleForHeapStart(),
            FrameCount + 5, m_rtvDescriptorSize);
        m_device->CreateRenderTargetView(m_taaRT.Get(), nullptr, rtv);
    }

    // TAA履歴RT（スロット FrameCount+6 = 8、初回はRTとして初期化）
    {
        auto rtDesc = CD3DX12_RESOURCE_DESC::Tex2D(format, m_width, m_height, 1, 1);
        rtDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        ThrowIfFailed(m_device->CreateCommittedResource(
            &heapProp, D3D12_HEAP_FLAG_NONE, &rtDesc,
            D3D12_RESOURCE_STATE_RENDER_TARGET, &clearVal,
            IID_PPV_ARGS(&m_taaHistoryRT)));
        CD3DX12_CPU_DESCRIPTOR_HANDLE rtv(
            m_rtvHeap->GetCPUDescriptorHandleForHeapStart(),
            FrameCount + 6, m_rtvDescriptorSize);
        m_device->CreateRenderTargetView(m_taaHistoryRT.Get(), nullptr, rtv);
    }

    // TAA SRVヒープ：[0]=hdrRT  [1]=taaHistoryRT
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 2;
        hd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ThrowIfFailed(m_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_taaSRVHeap)));
        m_taaSRVIncrSize = m_device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format                  = format;
        sd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels     = 1;

        // スロット0：hdrRT（現在フレーム）
        m_device->CreateShaderResourceView(m_hdrRT.Get(), &sd,
            CD3DX12_CPU_DESCRIPTOR_HANDLE(
                m_taaSRVHeap->GetCPUDescriptorHandleForHeapStart(), 0, m_taaSRVIncrSize));
        // スロット1：taaHistoryRT（履歴フレーム）
        m_device->CreateShaderResourceView(m_taaHistoryRT.Get(), &sd,
            CD3DX12_CPU_DESCRIPTOR_HANDLE(
                m_taaSRVHeap->GetCPUDescriptorHandleForHeapStart(), 1, m_taaSRVIncrSize));
    }

    // TAAルートシグネチャ：[0] 2-SRVテーブル（t0=current, t1=history）、[1] 4ルート定数
    {
        CD3DX12_ROOT_PARAMETER params[2];
        CD3DX12_DESCRIPTOR_RANGE srvRange;
        srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0);
        params[0].InitAsDescriptorTable(1, &srvRange, D3D12_SHADER_VISIBILITY_PIXEL);
        params[1].InitAsConstants(4, 0); // texelSize.xy, blendFactor, pad

        CD3DX12_STATIC_SAMPLER_DESC samplers[2] = {};
        // s0: ポイントサンプラー（現在フレーム用、ジッターアーティファクトを避ける）
        samplers[0].Init(0, D3D12_FILTER_MIN_MAG_MIP_POINT,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
        // s1: バイリニアサンプラー（履歴フレーム用、スムーズな補間）
        samplers[1].Init(1, D3D12_FILTER_MIN_MAG_MIP_LINEAR,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP);

        CD3DX12_ROOT_SIGNATURE_DESC rsd;
        rsd.Init(2, params, 2, samplers, D3D12_ROOT_SIGNATURE_FLAG_NONE);
        ComPtr<ID3DBlob> sig, err;
        ThrowIfFailed(D3D12SerializeRootSignature(&rsd,
            D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err));
        ThrowIfFailed(m_device->CreateRootSignature(0,
            sig->GetBufferPointer(), sig->GetBufferSize(),
            IID_PPV_ARGS(&m_taaRootSig)));
    }

    // TAA PSO
    {
        UINT8 *pVS = nullptr, *pPS = nullptr;
        UINT   vsLen = 0,      psLen = 0;
        ThrowIfFailed(ReadDataFromFile(
            GetAssetFullPath(L"taa_TAAVS.cso").c_str(), &pVS, &vsLen));
        ThrowIfFailed(ReadDataFromFile(
            GetAssetFullPath(L"taa_TAAPS.cso").c_str(), &pPS, &psLen));

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature           = m_taaRootSig.Get();
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
        pd.RTVFormats[0]         = format;
        pd.SampleDesc.Count      = 1;
        ThrowIfFailed(m_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_taaPSO)));

        delete[] pVS;
        delete[] pPS;
    }
}

void D3D12HelloTriangle::RenderTAA()
{
    if (!m_taaEnabled) return;

    auto* cmd = m_commandList.Get();

    // 初回：履歴RTをRT→PSRへ遷移する
    if (!m_taaHistoryInPSR)
    {
        auto bar = CD3DX12_RESOURCE_BARRIER::Transition(
            m_taaHistoryRT.Get(),
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cmd->ResourceBarrier(1, &bar);
        m_taaHistoryInPSR = true;
    }

    auto taaRtv = CD3DX12_CPU_DESCRIPTOR_HANDLE(
        m_rtvHeap->GetCPUDescriptorHandleForHeapStart(),
        FrameCount + 5, m_rtvDescriptorSize);

    // 初回フレームは履歴なし（blend=0でゴーストを防ぐ）
    struct TAACBData { float texW, texH, blend, pad; } cb = {
        1.0f / m_width,
        1.0f / m_height,
        m_taaHistoryValid ? m_taaBlend : 0.0f,
        0.0f
    };

    cmd->SetGraphicsRootSignature(m_taaRootSig.Get());
    ID3D12DescriptorHeap* heaps[] = { m_taaSRVHeap.Get() };
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->RSSetViewports(1, &m_viewport);
    cmd->RSSetScissorRects(1, &m_scissorRect);
    cmd->SetPipelineState(m_taaPSO.Get());
    cmd->SetGraphicsRoot32BitConstants(1, 4, &cb, 0);
    cmd->SetGraphicsRootDescriptorTable(0, m_taaSRVHeap->GetGPUDescriptorHandleForHeapStart());
    cmd->OMSetRenderTargets(1, &taaRtv, FALSE, nullptr);
    cmd->DrawInstanced(3, 1, 0, 0);

    m_taaHistoryValid = true;

    // taaRT(RT) → COPY_SOURCE、taaHistoryRT(PSR) → COPY_DEST
    {
        D3D12_RESOURCE_BARRIER bars[2] = {
            CD3DX12_RESOURCE_BARRIER::Transition(m_taaRT.Get(),
                D3D12_RESOURCE_STATE_RENDER_TARGET,
                D3D12_RESOURCE_STATE_COPY_SOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(m_taaHistoryRT.Get(),
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_COPY_DEST),
        };
        cmd->ResourceBarrier(2, bars);
    }
    cmd->CopyResource(m_taaHistoryRT.Get(), m_taaRT.Get());

    // 両バッファをPSRへ復元
    {
        D3D12_RESOURCE_BARRIER bars[2] = {
            CD3DX12_RESOURCE_BARRIER::Transition(m_taaRT.Get(),
                D3D12_RESOURCE_STATE_COPY_SOURCE,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(m_taaHistoryRT.Get(),
                D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
        };
        cmd->ResourceBarrier(2, bars);
    }

    // bloomSRVHeap[0]をtaaRTにリダイレクト（ブルーム/ゴッドレイ/DOFがTAA後の画像を使用）
    D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format                  = DXGI_FORMAT_R16G16B16A16_FLOAT;
    sd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels     = 1;
    m_device->CreateShaderResourceView(m_taaRT.Get(), &sd,
        m_bloomSRVHeap->GetCPUDescriptorHandleForHeapStart()); // スロット0 → taaRT
}