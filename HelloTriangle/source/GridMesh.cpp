#include "GridMesh.h"

GridMeshData GenerateGrid(UINT rows, UINT cols, float size)
{
    GridMeshData data;

    // 顶点数 = (rows+1) * (cols+1)
    // 比如32x32的网格，有33x33=1089个顶点
    UINT vertexCountX = cols + 1;
    UINT vertexCountZ = rows + 1;

    float halfSize = size * 0.5f;

    // 每个格子的步长
    float dx = size / static_cast<float>(cols);
    float dz = size / static_cast<float>(rows);

    // UV步长，从(0,0)到(1,1)均匀分布
    float du = 1.0f / static_cast<float>(cols);
    float dv = 1.0f / static_cast<float>(rows);

    // 生成顶点：从左上角(-halfSize, 0, -halfSize)开始
    // 沿X轴向右，沿Z轴向前
    data.vertices.reserve(vertexCountX * vertexCountZ);

    for (UINT z = 0; z < vertexCountZ; ++z)
    {
        for (UINT x = 0; x < vertexCountX; ++x)
        {
            GridVertex v;
            v.position = XMFLOAT3(
                -halfSize + x * dx,   // X：从左到右
                0.0f,                 // Y：完全平坦，Wave Shader再做位移
                -halfSize + z * dz    // Z：从近到远
            );
            v.uv = XMFLOAT2(
                x * du,   // U：0→1
                z * dv    // V：0→1
            );
            data.vertices.push_back(v);
        }
    }

    // 生成索引：每个格子拆成两个三角形
    //
    //  (z,x)----(z,x+1)
    //    |    ／  |
    //    |  ／    |
    //  (z+1,x)--(z+1,x+1)
    //
    // 三角形1：(z,x) → (z,x+1) → (z+1,x)       顶部三角
    // 三角形2：(z,x+1) → (z+1,x+1) → (z+1,x)   底部三角
    data.indices.reserve(rows * cols * 6);

    for (UINT z = 0; z < rows; ++z)
    {
        for (UINT x = 0; x < cols; ++x)
        {
            // 当前格子四个角的顶点索引
            uint32_t topLeft = z * vertexCountX + x;
            uint32_t topRight = z * vertexCountX + x + 1;
            uint32_t bottomLeft = (z + 1) * vertexCountX + x;
            uint32_t bottomRight = (z + 1) * vertexCountX + x + 1;

            // 三角形1（左上半）
            data.indices.push_back(topLeft);
            data.indices.push_back(bottomLeft);
            data.indices.push_back(topRight);

            data.indices.push_back(topRight);
            data.indices.push_back(bottomLeft);
            data.indices.push_back(bottomRight);
        }
    }

    return data;
}

GridMeshData GenerateWaterBox(float size, float depth)
{
    GridMeshData data;
    float h = size * 0.5f;  // 半边长
    float d = -depth;       // 底部Y坐标

    // 辅助lambda：添加一个矩形面（两个三角形）
    auto addQuad = [&](XMFLOAT3 v0, XMFLOAT3 v1, XMFLOAT3 v2, XMFLOAT3 v3)
        {
            uint32_t base = static_cast<uint32_t>(data.vertices.size());
            data.vertices.push_back({ v0, XMFLOAT2(0.0f, 0.0f) });
            data.vertices.push_back({ v1, XMFLOAT2(1.0f, 0.0f) });
            data.vertices.push_back({ v2, XMFLOAT2(1.0f, 1.0f) });
            data.vertices.push_back({ v3, XMFLOAT2(0.0f, 1.0f) });

            // 三角形1
            data.indices.push_back(base + 0);
            data.indices.push_back(base + 1);
            data.indices.push_back(base + 2);
            // 三角形2
            data.indices.push_back(base + 0);
            data.indices.push_back(base + 2);
            data.indices.push_back(base + 3);
        };

    // 前面 (z = +h)
    addQuad(
        XMFLOAT3(-h, 0.0f, +h),
        XMFLOAT3(+h, 0.0f, +h),
        XMFLOAT3(+h, d, +h),
        XMFLOAT3(-h, d, +h));

    // 后面 (z = -h)
    addQuad(
        XMFLOAT3(+h, 0.0f, -h),
        XMFLOAT3(-h, 0.0f, -h),
        XMFLOAT3(-h, d, -h),
        XMFLOAT3(+h, d, -h));

    // 左面 (x = -h)
    addQuad(
        XMFLOAT3(-h, 0.0f, -h),
        XMFLOAT3(-h, 0.0f, +h),
        XMFLOAT3(-h, d, +h),
        XMFLOAT3(-h, d, -h));

    // 右面 (x = +h)
    addQuad(
        XMFLOAT3(+h, 0.0f, +h),
        XMFLOAT3(+h, 0.0f, -h),
        XMFLOAT3(+h, d, -h),
        XMFLOAT3(+h, d, +h));

    // 底面 (y = d)
    addQuad(
        XMFLOAT3(-h, d, -h),
        XMFLOAT3(+h, d, -h),
        XMFLOAT3(+h, d, +h),
        XMFLOAT3(-h, d, +h));

    return data;
}
