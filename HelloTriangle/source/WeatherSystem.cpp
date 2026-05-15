#include "WeatherSystem.h"
#include <algorithm>

WeatherParams WeatherSystem::GetPreset(WeatherState state)
{
    switch (state)
    {
    case WeatherState::Calm:
        return { 10.0f, 0.1f, 1.0f, 0.0f, 0.4f, 0.85f, 0.0f };
    case WeatherState::Windy:
        return { 40.0f, 0.3f, 1.0f, 0.2f, 0.6f, 0.75f, 0.3f };
    case WeatherState::Storm:
        return { 75.0f, 0.8f, 1.0f, 0.4f, 1.2f, 0.65f, 1.0f };
    default:
        return { 10.0f, 0.1f, 1.0f, 0.0f, 0.4f, 0.85f, 0.0f };
    }
}

void WeatherSystem::Init(OceanFFT* ocean,SkyDome* sky)
{
    m_ocean = ocean;
    m_sky   = sky;
    m_currentParams = GetPreset(WeatherState::Calm);
    m_fromParams = m_currentParams;
    m_targetParams = m_currentParams;

    m_ocean->windSpeed = m_currentParams.windSpeed;
    m_ocean->phillipsA = m_currentParams.phillipsA;
    m_ocean->windDirX = m_currentParams.windDirX;
    m_ocean->windDirY = m_currentParams.windDirY;
}

void WeatherSystem::SetWeather(WeatherState state, float transitionTime)
{
    if (state == m_targetState) return;

    m_targetState = state;
    m_fromParams = m_currentParams;
    m_targetParams = GetPreset(state);
    m_transitionTime = transitionTime;
    m_transitionElapsed = 0.0f;
    m_inTransition = true;
}

void WeatherSystem::Update(float deltaTime)
{
    // Automatic weather linkage
    if (m_autoWeather && m_sky)
    {
        float sunY = m_sky->GetSunDirection().y;

        WeatherState autoState;
        if (sunY > 0.4f)
            autoState = WeatherState::Calm;
        else if (sunY > 0.0f)
            autoState = WeatherState::Windy;
        else
            autoState = WeatherState::Storm;

        // Only trigger a transition when the state changes; transition time is 10 seconds.
        if (autoState != m_targetState)
            SetWeather(autoState, 10.0f);
    }

    if (m_inTransition)
    {
        m_transitionElapsed += deltaTime;
        float t = m_transitionElapsed / m_transitionTime;
        t = std::clamp(t, 0.0f, 1.0f);

        // smoothstep makes the transition feel more natural
        float s = t * t * (3.0f - 2.0f * t);

        m_currentParams.windSpeed    = m_fromParams.windSpeed + s * (m_targetParams.windSpeed - m_fromParams.windSpeed);
        m_currentParams.phillipsA    = m_fromParams.phillipsA + s * (m_targetParams.phillipsA - m_fromParams.phillipsA);
        m_currentParams.windDirX     = m_fromParams.windDirX + s * (m_targetParams.windDirX - m_fromParams.windDirX);
        m_currentParams.windDirY     = m_fromParams.windDirY + s * (m_targetParams.windDirY - m_fromParams.windDirY);
        m_currentParams.cloudDensity = m_fromParams.cloudDensity + s * (m_targetParams.cloudDensity - m_fromParams.cloudDensity);
        m_currentParams.cloudScale   = m_fromParams.cloudScale + s * (m_targetParams.cloudScale - m_fromParams.cloudScale);
        m_currentParams.weatherIntensity = m_fromParams.weatherIntensity + s * (m_targetParams.weatherIntensity - m_fromParams.weatherIntensity);

        if (t >= 1.0f)
            m_inTransition = false;
    }
    // Wind direction dynamic drift
    m_time += deltaTime;

    // Base wind direction angle changes slowly
    float baseAngle = m_time * 0.05f; // slow rotation

    // During storms add turbulence so wind direction changes more dramatically
    float turbulence = m_currentParams.weatherIntensity * sinf(m_time * 0.3f) * 0.5f;
    float angle = baseAngle + turbulence;


    m_ocean->windSpeed = m_currentParams.windSpeed;
    m_ocean->phillipsA = m_currentParams.phillipsA;
    m_ocean->windDirX = cosf(angle);
    m_ocean->windDirY = sinf(angle);
    m_sky->SetCloudParams(
        m_currentParams.cloudDensity,
        m_currentParams.cloudScale,
        0.6f);
    //m_sky->SetWeatherIntensity(1.0f);
    m_sky->SetWeatherIntensity(m_currentParams.weatherIntensity);

}