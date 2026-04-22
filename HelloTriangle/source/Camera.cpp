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

void Camera::UpdateShowcase(float deltaTime)
{
    if (!m_showcaseMode) return;
    m_showcaseAngle += deltaTime * 0.3f; // 旋转速度
    position.x = sinf(m_showcaseAngle) * m_showcaseRadius;
    position.z = cosf(m_showcaseAngle) * m_showcaseRadius;
    position.y = m_showcaseHeight;

    // 始终看向原点（水族箱中心）
    // 通过 pitch 和 yaw 控制方向
    m_yaw = m_showcaseAngle + XM_PI; // 朝向原点
    m_pitch = -atanf(m_showcaseHeight / m_showcaseRadius); // 俯角
}
