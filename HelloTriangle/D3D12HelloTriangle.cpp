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
        rtvHeapDesc.NumDescriptors = FrameCount;
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
        srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0);//t0
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
    
}

// Update frame-based values.
void D3D12HelloTriangle::OnUpdate()
{
	// Update the scene.
	auto currentTime = std::chrono::steady_clock::now();
	float deltaTime = std::chrono::duration<float>(currentTime - m_lastTime).count();
    float weatherIntensity = m_weatherSystem->GetWeatherIntensity();
	m_lastTime = currentTime;
    m_skyDome->Update(deltaTime);
    m_renderer->Update(deltaTime);
    m_weatherSystem->Update(deltaTime);
    m_rainSystem->Update(deltaTime,weatherIntensity,
        m_oceanFFT->windDirX, m_oceanFFT->windDirY);
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
    ctx.rtv = CD3DX12_CPU_DESCRIPTOR_HANDLE(
        m_rtvHeap->GetCPUDescriptorHandleForHeapStart(),
        m_frameIndex, m_rtvDescriptorSize);
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

    m_skyDome->Render(ctx);

    ID3D12DescriptorHeap* srvHeaps[] = { m_oceanFFT->GetSRVHeap() };
    m_commandList->SetDescriptorHeaps(1, srvHeaps);
    m_commandList->SetGraphicsRootDescriptorTable(1,
    m_oceanFFT->GetSRVHeap()->GetGPUDescriptorHandleForHeapStart());
    m_commandList->SetGraphicsRootConstantBufferView(
        2, m_rainSystem->GetRippleCBAddress());

    m_renderer->Render(ctx);
    if (m_renderer->IsShowcaseMode())
        m_renderer->RenderWaterBox(ctx);
    m_rainSystem->Render(ctx,
        m_renderer->GetViewMatrix(),
        m_renderer->GetProjMatrix(),
        m_renderer->GetCameraPos());

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
    // Ensure that the GPU is no longer referencing resources that are about to be
    // cleaned up by the destructor.
    WaitForPreviousFrame();

    CloseHandle(m_fenceEvent);
}

void D3D12HelloTriangle::OnMouseMove(float dx, float dy)
{
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