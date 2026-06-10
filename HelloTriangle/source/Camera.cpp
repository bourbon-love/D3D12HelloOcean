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
    XMMATRIX proj = XMMatrixPerspectiveFovLH(fov, aspect, nearZ, farZ);
    // TAA jitter: add NDC offset to x/y of row[2] (clip.x += jitter.x * pos.z)
    proj.r[2] = XMVectorSetX(proj.r[2], XMVectorGetX(proj.r[2]) + m_jitter.x);
    proj.r[2] = XMVectorSetY(proj.r[2], XMVectorGetY(proj.r[2]) + m_jitter.y);
    return proj;
}

void Camera::ProcessMouse(float deltaX, float deltaY)
{
    m_yaw += deltaX * sensitivity;
    m_pitch -= deltaY * sensitivity; 

    m_pitch = std::clamp(m_pitch, -XM_PIDIV2 + 0.01f, XM_PIDIV2 - 0.01f);
}

void Camera::Move(float forwardDelta, float rightDelta)
{
    // 1. Forward direction (horizontal movement only)
    XMVECTOR forward = XMVectorSet(
        cosf(m_pitch) * sinf(m_yaw),
        0.0f, // Lock to ground plane (key)
        cosf(m_pitch) * cosf(m_yaw),
        0.0f
    );

    forward = XMVector3Normalize(forward);

    // 2. Right direction
    XMVECTOR right = XMVector3Cross(
        XMVectorSet(0, 1, 0, 0), // up
        forward
    );

    right = XMVector3Normalize(right);

    // 3. Update position
    XMVECTOR pos = XMLoadFloat3(&position);

    pos += forward * forwardDelta;
    pos += right * rightDelta;

    XMStoreFloat3(&position, pos);
}

void Camera::UpdateShowcase(float deltaTime)
{
    if (!m_showcaseMode) return;
    m_showcaseAngle += deltaTime * m_showcaseSpeed;

    position.x = sinf(m_showcaseAngle) * m_showcaseRadius;
    position.z = cosf(m_showcaseAngle) * m_showcaseRadius;

    // Compound vertical oscillation
    //   slow dive  (~60s cycle): large amplitude, occasionally dips below surface
    //   med swell  (~19s cycle): mid ocean swell
    //   small chop  (~7s cycle): surface chop feel
    float slowDive  = sinf(m_showcaseAngle * 0.35f) *  8.0f;
    float medSwell  = sinf(m_showcaseAngle * 1.10f) *  2.5f;
    float smallChop = sinf(m_showcaseAngle * 2.80f) *  0.8f;
    position.y = m_showcaseHeight + slowDive + medSwell + smallChop;
    // range: [5-8-2.5-0.8, 5+8+2.5+0.8] = [-6.3, 16.3] m

    m_yaw   = m_showcaseAngle + XM_PI;
    m_pitch = -atanf(position.y / m_showcaseRadius); // look toward origin
}

void Camera::UpdateShipOrbit(float deltaTime, const XMFLOAT3& target)
{
    if (!m_shipOrbitMode) return;
    m_shipOrbitAngle += deltaTime * m_showcaseSpeed; // reuse "Orbit Speed" slider

    position.x = target.x + sinf(m_shipOrbitAngle) * m_shipOrbitRadius;
    position.z = target.z + cosf(m_shipOrbitAngle) * m_shipOrbitRadius;
    position.y = m_showcaseHeight;                   // reuse "Cam Height" slider

    // Look toward the ship (target), not the world origin
    float dx    = target.x - position.x;
    float dy    = target.y - position.y;
    float dz    = target.z - position.z;
    float horiz = sqrtf(dx * dx + dz * dz);
    m_yaw   = atan2f(dx, dz);
    m_pitch = atan2f(dy, std::max(horiz, 0.001f));
}
