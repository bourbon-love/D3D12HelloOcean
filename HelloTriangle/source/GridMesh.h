#pragma once
#include <vector>
#include <DirectXMath.h>
#include <wrl.h>

using namespace DirectX;

// 顶点格式：位置 + UV
// 去掉了Color，改成UV，为后续法线贴图和波浪采样做准备
struct GridVertex
{
    XMFLOAT3 position;
    XMFLOAT2 uv;
};

struct GridMeshData
{
    std::vector<GridVertex> vertices;
    std::vector<uint32_t>   indices;   // 改成uint32，32x32以上顶点数会超uint16上限
};

// 生成平坦网格
// rows/cols : 分段数，实际顶点数是(rows+1)*(cols+1)
// size      : 网格总尺寸，以世界坐标为单位，网格居中于原点
GridMeshData GenerateGrid(UINT rows, UINT cols, float size);
// 生成水箱侧面和底面
GridMeshData GenerateWaterBox(float size, float depth);