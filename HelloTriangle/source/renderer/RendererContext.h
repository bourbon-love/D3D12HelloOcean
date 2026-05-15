#pragma once
#include <d3d12.h>
#include <DirectXMath.h>

struct RenderContext
{
    ID3D12GraphicsCommandList*  cmd;

    // Current frame
    ID3D12Resource*             renderTarget;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv;

    // Viewport
    D3D12_VIEWPORT              viewport;
    D3D12_RECT                  scissor;

    // Geometry
    D3D12_VERTEX_BUFFER_VIEW    vb;
    D3D12_INDEX_BUFFER_VIEW     ib;

    // Constants
    D3D12_GPU_VIRTUAL_ADDRESS   cbAddress;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv;
    UINT                        indexCount;

    DirectX::XMMATRIX           view;   
    DirectX::XMMATRIX           proj;   
};