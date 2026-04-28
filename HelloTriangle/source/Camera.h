#pragma once
#include <DirectXMath.h>

using namespace DirectX;

struct Camera
{
    XMFLOAT3 position = { 0.0f, 50.0f, -200.0f };
    XMFLOAT3 m_savedPosition = { 0.0f, 60.0f, -50.0f };
    float    m_savedPitch = -0.15f;
    float    m_savedYaw = 0.0f;

    float m_pitch = -0.0f; // 上下看
    float m_yaw = 0.0f;   // 左右看
    float sensitivity = 0.002f;
    float fov = XM_PIDIV4;
    float aspect = 1.0f;
    float nearZ = 0.1f;
    float farZ = 2000.0f;

	// 展示模式相关参数
    bool  m_showcaseMode = false;  // 展示模式开关
    float m_showcaseAngle = 0.0f;  // 自动旋转角度
    float m_showcaseRadius = 1000.0f; // 环绕半径
    float m_showcaseHeight = 200.0f; // 环绕高度

    XMMATRIX GetViewMatrix() const;
    XMMATRIX GetProjMatrix() const;
    void ProcessMouse(float deltaX, float deltaY);
    void Move(float forwardDelta, float rightDelta);
	void UpdateShowcase(float deltaTime);

    void EnterShowcase()
    {
        // 保存当前位置
        m_savedPosition = position;
        m_savedPitch = m_pitch;
        m_savedYaw = m_yaw;
        m_showcaseMode = true;
    }

    void ExitShowcase()
    {
        // 恢复保存的位置
        position = m_savedPosition;
        m_pitch = m_savedPitch;
        m_yaw = m_savedYaw;
        m_showcaseMode = false;
    }
};