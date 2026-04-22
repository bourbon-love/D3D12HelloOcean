#pragma once
#include "OceanFFT.h"
#include "SkyDome.h"

enum class WeatherState
{
    Calm,    // 平静
    Windy,   // 有风
    Storm,   // 暴风
};

struct WeatherParams
{
    float windSpeed;
    float phillipsA;
    float windDirX;
    float windDirY;
    float cloudDensity;
    float cloudScale;
    float weatherIntensity;
};

class WeatherSystem
{
public:
    void Init(OceanFFT* ocean,SkyDome* sky);
    void SetWeather(WeatherState state, float transitionTime = 5.0f);
    void Update(float deltaTime);

    WeatherState GetCurrentState() const { return m_targetState; }
	void SetAutoWeather(bool autoWeather) { m_autoWeather = autoWeather; }
    float GetWeatherIntensity() const { return m_currentParams.weatherIntensity; }

private:
    OceanFFT* m_ocean = nullptr;
    SkyDome* m_sky    = nullptr;

    WeatherState m_targetState = WeatherState::Calm;

    WeatherParams m_currentParams;
    WeatherParams m_fromParams;
    WeatherParams m_targetParams;

    float m_transitionTime = 0.0f;
    float m_transitionElapsed = 0.0f;
    bool  m_inTransition = false;
    float m_time = 0.0f;
	bool  m_autoWeather = true; // 是否自动循环天气状态
    static WeatherParams GetPreset(WeatherState state);
};