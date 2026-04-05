#include "Camera.h"
#include <algorithm>

XMMATRIX Camera::GetViewMatrix() const
{
    XMVECTOR pos = XMLoadFloat3(&position);

    XMVECTOR forward = XMVectorSet(
        cosf(m_pitch) * sinf(m_yaw),
        sinf(m_pitch),
        cosf(m_pitch) * cosf(m_yaw),
        0.0f
    );

    forward = XMVector3Normalize(forward);

    XMVECTOR up = XMVectorSet(0, 1, 0, 0);

    return XMMatrixLookToLH(pos, forward, up);
}

XMMATRIX Camera::GetProjMatrix() const
{
    return XMMatrixPerspectiveFovLH(
        fov,
        aspect,
        nearZ,
        farZ
    );
}

void Camera::ProcessMouse(float deltaX, float deltaY)
{
    m_yaw += deltaX * sensitivity;
    m_pitch -= deltaY * sensitivity; 

    m_pitch = std::clamp(m_pitch, -XM_PIDIV2 + 0.01f, XM_PIDIV2 - 0.01f);
}

void Camera::Move(float forwardDelta, float rightDelta)
{
    // 1️⃣ 前方向（只在水平面移动！）
    XMVECTOR forward = XMVectorSet(
        cosf(m_pitch) * sinf(m_yaw),
        0.0f, // 👈 锁地面（关键）
        cosf(m_pitch) * cosf(m_yaw),
        0.0f
    );

    forward = XMVector3Normalize(forward);

    // 2️D 右方向
    XMVECTOR right = XMVector3Cross(
        XMVectorSet(0, 1, 0, 0), // up
        forward
    );

    right = XMVector3Normalize(right);

    // 3️D 位置更新
    XMVECTOR pos = XMLoadFloat3(&position);

    pos += forward * forwardDelta;
    pos += right * rightDelta;

    XMStoreFloat3(&position, pos);
}