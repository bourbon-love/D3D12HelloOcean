#pragma once
#include <DirectXMath.h>

using namespace DirectX;

struct Camera
{
    XMFLOAT3 position = { 0.0f, 55.0f, -200.0f };

    float m_pitch = -0.2f; // è„â∫ä≈
    float m_yaw = 0.0f;   // ç∂âEä≈
    float sensitivity = 0.002f;
    float fov = XM_PIDIV4;
    float aspect = 1.0f;
    float nearZ = 0.1f;
    float farZ = 2000.0f;

    XMMATRIX GetViewMatrix() const;
    XMMATRIX GetProjMatrix() const;
    void ProcessMouse(float deltaX, float deltaY);
    void Move(float forwardDelta, float rightDelta);
};