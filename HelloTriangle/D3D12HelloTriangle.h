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

#pragma once

#include "DXSample.h"
#include <wrl/client.h>
#include "source/Renderer.h"
#include <memory>
#include <DirectXMath.h>
#include <d3dx12_core.h>
#include <dxgi1_6.h>
#include <chrono>
#include "source/SkyDome.h"
#include "source/OceanFFT.h"
#include "source/WeatherSystem.h"
#include "source/RainSystem.h"

using namespace DirectX;

// Note that while ComPtr is used to manage the lifetime of resources on the CPU,
// it has no understanding of the lifetime of resources on the GPU. Apps must account
// for the GPU lifetime of resources to avoid destroying objects that may still be
// referenced by the GPU.
// An example of this can be found in the class method: OnDestroy().
using Microsoft::WRL::ComPtr;


class D3D12HelloTriangle : public DXSample
{
public:
    D3D12HelloTriangle(UINT width, UINT height, std::wstring name);

    virtual void OnInit();
    virtual void OnUpdate();
    virtual void OnRender();
    virtual void OnDestroy();
    virtual void OnMouseMove(float dx, float dy) override;
    
    void OnKeyDown(UINT8 key) override;
private:
    static const UINT FrameCount = 2;
    std::unique_ptr<Renderer> m_renderer;
    std::unique_ptr<SkyDome>  m_skyDome;
    std::unique_ptr<OceanFFT> m_oceanFFT;
    std::unique_ptr<WeatherSystem> m_weatherSystem;
    std::unique_ptr<RainSystem> m_rainSystem;

    std::chrono::steady_clock::time_point m_lastTime;
   

    struct SceneCB
    {
        XMMATRIX view;
        XMMATRIX proj;
    };

    // Pipeline objects.
    CD3DX12_VIEWPORT m_viewport;
    CD3DX12_RECT m_scissorRect;
    ComPtr<IDXGISwapChain3> m_swapChain;
    ComPtr<ID3D12Device> m_device;
    ComPtr<ID3D12Resource> m_renderTargets[FrameCount];
    ComPtr<ID3D12CommandAllocator> m_commandAllocator;
    ComPtr<ID3D12CommandQueue> m_commandQueue;
    ComPtr<ID3D12RootSignature> m_rootSignature;
    ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    ComPtr<ID3D12GraphicsCommandList> m_commandList;
    UINT m_rtvDescriptorSize;

 
    //ConstantBuffer m_cbData;
    // Synchronization objects.
    UINT m_frameIndex;
    HANDLE m_fenceEvent;
    ComPtr<ID3D12Fence> m_fence;
    UINT64 m_fenceValue;


    void LoadPipeline();
    void LoadAssets();
    void PopulateCommandList();
    void WaitForPreviousFrame();
};
