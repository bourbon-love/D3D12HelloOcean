#pragma once
#include "OceanFFT.h"
#include "SkyDome.h"
#include <algorithm>

enum class WeatherState
{
    Calm,    // calm
    Windy,   // windy
    Storm,   // storm
    Tsunami, // extreme waves, manual only
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
    float tsunamiIntensity;  // 0 for Calm/Windy/Storm, 1 for Tsunami
};

class WeatherSystem
{
public:
    void Init(OceanFFT* ocean,SkyDome* sky);
    void SetWeather(WeatherState state, float transitionTime = 5.0f);
    void Update(float deltaTime);

    WeatherState GetCurrentState() const { return m_targetState; }
	void SetAutoWeather(bool autoWeather) { m_autoWeather = autoWeather; }
    bool IsAutoWeather() const { return m_autoWeather; }
    float GetWeatherIntensity() const { return m_currentParams.weatherIntensity; }

    // Current cloud coverage [0,1] driven by weather state, used to set VolumetricClouds.
    float GetCloudCoverage() const { return std::clamp(m_currentParams.cloudDensity, 0.0f, 1.0f); }

    // Rainbow appears during rain (Storm/Windy). Returns 0..1.
    float GetRainbowIntensity() const
    {
        return std::clamp((m_currentParams.weatherIntensity - 0.15f) / 0.50f, 0.0f, 1.0f);
    }

    float GetTsunamiIntensity() const { return m_currentParams.tsunamiIntensity; }

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
	bool  m_autoWeather = true; // Whether to automatically cycle through weather states
    static WeatherParams GetPreset(WeatherState state);
};