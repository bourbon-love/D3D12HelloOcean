#pragma once
#include <vector>
#include <DirectXMath.h>
#include <wrl.h>

using namespace DirectX;

// Vertex format: position + UV
// Removed Color, replaced with UV, in preparation for normal mapping and wave sampling
struct GridVertex
{
    XMFLOAT3 position;
    XMFLOAT2 uv;
};

struct GridMeshData
{
    std::vector<GridVertex> vertices;
    std::vector<uint32_t>   indices;   // Changed to uint32; vertex count above 32x32 exceeds uint16 limit
};

// Generate a flat grid
// rows/cols : subdivision count; actual vertex count is (rows+1)*(cols+1)
// size      : total grid size in world units, centered at origin
GridMeshData GenerateGrid(UINT rows, UINT cols, float size);
// Generate water box sides and bottom face
GridMeshData GenerateWaterBox(float size, float depth);