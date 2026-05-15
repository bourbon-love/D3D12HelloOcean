#include "GridMesh.h"

GridMeshData GenerateGrid(UINT rows, UINT cols, float size)
{
    GridMeshData data;

    // Vertex count = (rows+1) * (cols+1)
    // e.g., a 32x32 grid has 33x33=1089 vertices
    UINT vertexCountX = cols + 1;
    UINT vertexCountZ = rows + 1;

    float halfSize = size * 0.5f;

    // Step size per cell
    float dx = size / static_cast<float>(cols);
    float dz = size / static_cast<float>(rows);

    // UV step, uniformly distributed from (0,0) to (1,1)
    float du = 1.0f / static_cast<float>(cols);
    float dv = 1.0f / static_cast<float>(rows);

    // Generate vertices: starting from top-left corner (-halfSize, 0, -halfSize)
    // going right along X axis, forward along Z axis
    data.vertices.reserve(vertexCountX * vertexCountZ);

    for (UINT z = 0; z < vertexCountZ; ++z)
    {
        for (UINT x = 0; x < vertexCountX; ++x)
        {
            GridVertex v;
            v.position = XMFLOAT3(
                -halfSize + x * dx,   // X: left to right
                0.0f,                 // Y: completely flat; Wave Shader will displace
                -halfSize + z * dz    // Z: near to far
            );
            v.uv = XMFLOAT2(
                x * du,   // U: 0→1
                z * dv    // V: 0→1
            );
            data.vertices.push_back(v);
        }
    }

    // Generate indices: each cell is split into two triangles
    //
    //  (z,x)----(z,x+1)
    //    |    ／  |
    //    |  ／    |
    //  (z+1,x)--(z+1,x+1)
    //
    // Triangle 1: (z,x) → (z,x+1) → (z+1,x)       top triangle
    // Triangle 2: (z,x+1) → (z+1,x+1) → (z+1,x)   bottom triangle
    data.indices.reserve(rows * cols * 6);

    for (UINT z = 0; z < rows; ++z)
    {
        for (UINT x = 0; x < cols; ++x)
        {
            // Vertex indices for the four corners of the current cell
            uint32_t topLeft = z * vertexCountX + x;
            uint32_t topRight = z * vertexCountX + x + 1;
            uint32_t bottomLeft = (z + 1) * vertexCountX + x;
            uint32_t bottomRight = (z + 1) * vertexCountX + x + 1;

            // Triangle 1 (upper-left half)
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
    float h = size * 0.5f;  // Half side length
    float d = -depth;       // Bottom Y coordinate

    // Helper lambda: add a rectangular face (two triangles)
    auto addQuad = [&](XMFLOAT3 v0, XMFLOAT3 v1, XMFLOAT3 v2, XMFLOAT3 v3)
        {
            uint32_t base = static_cast<uint32_t>(data.vertices.size());
            data.vertices.push_back({ v0, XMFLOAT2(0.0f, 0.0f) });
            data.vertices.push_back({ v1, XMFLOAT2(1.0f, 0.0f) });
            data.vertices.push_back({ v2, XMFLOAT2(1.0f, 1.0f) });
            data.vertices.push_back({ v3, XMFLOAT2(0.0f, 1.0f) });

            // Triangle 1
            data.indices.push_back(base + 0);
            data.indices.push_back(base + 1);
            data.indices.push_back(base + 2);
            // Triangle 2
            data.indices.push_back(base + 0);
            data.indices.push_back(base + 2);
            data.indices.push_back(base + 3);
        };

    // Front face (z = +h)
    addQuad(
        XMFLOAT3(-h, 0.0f, +h),
        XMFLOAT3(+h, 0.0f, +h),
        XMFLOAT3(+h, d, +h),
        XMFLOAT3(-h, d, +h));

    // Back face (z = -h)
    addQuad(
        XMFLOAT3(+h, 0.0f, -h),
        XMFLOAT3(-h, 0.0f, -h),
        XMFLOAT3(-h, d, -h),
        XMFLOAT3(+h, d, -h));

    // Left face (x = -h)
    addQuad(
        XMFLOAT3(-h, 0.0f, -h),
        XMFLOAT3(-h, 0.0f, +h),
        XMFLOAT3(-h, d, +h),
        XMFLOAT3(-h, d, -h));

    // Right face (x = +h)
    addQuad(
        XMFLOAT3(+h, 0.0f, +h),
        XMFLOAT3(+h, 0.0f, -h),
        XMFLOAT3(+h, d, -h),
        XMFLOAT3(+h, d, +h));

    // Bottom face (y = d)
    addQuad(
        XMFLOAT3(-h, d, -h),
        XMFLOAT3(+h, d, -h),
        XMFLOAT3(+h, d, +h),
        XMFLOAT3(-h, d, +h));

    return data;
}
