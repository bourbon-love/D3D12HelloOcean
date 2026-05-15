// ============================================================
// Camera.h
// Camera class. Provides WASD + mouse control and automatic showcase orbit mode.
// ============================================================
#pragma once
#include <DirectXMath.h>

using namespace DirectX;

struct Camera
{
    XMFLOAT3 position = { 0.0f, 50.0f, -200.0f };
    XMFLOAT3 m_savedPosition = { 0.0f, 60.0f, -50.0f };
    float    m_savedPitch = -0.15f;
    float    m_savedYaw = 0.0f;

    float m_pitch = -0.0f; // Look up/down
    float m_yaw = 0.0f;   // Look left/right
    XMFLOAT2 m_jitter = { 0.0f, 0.0f }; // TAA sub-pixel jitter (NDC space)
    float sensitivity = 0.002f;
    float fov = XM_PIDIV4;
    float aspect = 1.0f;
    float nearZ = 0.1f;
    float farZ = 2000.0f;

	// Showcase mode parameters
    bool  m_showcaseMode = false;  // Showcase mode toggle
    float m_showcaseAngle = 0.0f;  // Auto-rotation angle
    float m_showcaseRadius = 120.0f; // Orbit radius
    float m_showcaseHeight = 5.0f;  // Center height for vertical oscillation

    XMMATRIX GetViewMatrix() const;
    XMMATRIX GetProjMatrix() const;
    void ProcessMouse(float deltaX, float deltaY);
    void Move(float forwardDelta, float rightDelta);
	void UpdateShowcase(float deltaTime);

    void EnterShowcase()
    {
        // Save current position
        m_savedPosition = position;
        m_savedPitch = m_pitch;
        m_savedYaw = m_yaw;
        m_showcaseMode = true;
    }

    void ExitShowcase()
    {
        // Restore saved position
        position = m_savedPosition;
        m_pitch = m_savedPitch;
        m_yaw = m_savedYaw;
        m_showcaseMode = false;
    }
};