/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

/** \file
    \ingroup world
*/

#include "Weather.h"
#include "GameTime.h"
#include "Log.h"
#include "Map.h"
#include "MiscPackets.h"
#include "Player.h"
#include "Random.h"
#include "ScriptMgr.h"
#include "Util.h"
#include "World.h"
// @tswow-begin
#include "Map.h"
#include "TSEvents.h"
#include "TSMap.h"
#include "TSWeather.h"
// @tswow-end

/// Create the Weather object
Weather::Weather(Map* map, uint32 zoneId, WeatherData const* weatherChances)
    : m_map(map), m_zone(zoneId), m_weatherChances(weatherChances)
{
    m_timer.SetInterval(sWorld->getIntConfig(CONFIG_INTERVAL_CHANGEWEATHER));
    m_type = WEATHER_TYPE_FINE;
    m_intensity = 0;

    TC_LOG_INFO("misc", "WORLD: Starting weather system for zone {} (change every {} minutes).", m_zone, (uint32)(m_timer.GetInterval() / (MINUTE*IN_MILLISECONDS)));
}

/// Launch a weather update
bool Weather::Update(uint32 diff)
{
    if (m_timer.GetCurrent() >= 0)
        m_timer.Update(diff);
    else
        m_timer.SetCurrent(0);

    ///- If the timer has passed, ReGenerate the weather
    if (m_timer.Passed())
    {
        m_timer.Reset();
        // update only if Regenerate has changed the weather
        if (ReGenerate())
        {
            ///- Weather will be removed if not updated (no players in zone anymore)
            if (!UpdateWeather())
                return false;
        }
    }

    sScriptMgr->OnWeatherUpdate(this, diff);

    // @tswow-begin
    FIRE_ID(m_map->GetId(),Map,OnWeatherUpdate,TSMap(m_map), TSWeather(this));
    // @tswow-end

    return true;
}

/// Calculate the new weather
bool Weather::ReGenerate()
{
    if (!m_weatherChances)
    {
        m_type = WEATHER_TYPE_FINE;
        m_intensity = 0.0f;
        return false;
    }

    /// Weather statistics:
    ///- 30% - no change
    ///- 30% - weather gets better (if not fine) or change weather type
    ///- 30% - weather worsens (if not fine)
    ///- 10% - radical change (if not fine)
    uint32 u = urand(0, 99);

    if (u < 30)
        return false;

    // remember old values
    WeatherType old_type = m_type;
    float old_intensity = m_intensity;

    //78 days between January 1st and March 20nd; 365/4=91 days by season
    // season source http://aa.usno.navy.mil/data/docs/EarthSeasons.html
    time_t gtime = GameTime::GetGameTime();
    struct tm ltime;
    localtime_r(&gtime, &ltime);
    uint32 season = ((ltime.tm_yday - 78 + 365)/91)%4;

    static char const* seasonName[WEATHER_SEASONS] = { "spring", "summer", "fall", "winter" };

    TC_LOG_INFO("misc", "Generating a change in {} weather for zone {}.", seasonName[season], m_zone);

    if ((u < 60) && (m_intensity < 0.33333334f))                // Get fair
    {
        m_type = WEATHER_TYPE_FINE;
        m_intensity = 0.0f;
    }

    if ((u < 60) && (m_type != WEATHER_TYPE_FINE))          // Get better
    {
        m_intensity -= 0.33333334f;
        return true;
    }

    if ((u < 90) && (m_type != WEATHER_TYPE_FINE))          // Get worse
    {
        m_intensity += 0.33333334f;
        return true;
    }

    if (m_type != WEATHER_TYPE_FINE)
    {
        /// Radical change:
        ///- if light -> heavy
        ///- if medium -> change weather type
        ///- if heavy -> 50% light, 50% change weather type

        if (m_intensity < 0.33333334f)
        {
            m_intensity = 0.9999f;                              // go nuts
            return true;
        }
        else
        {
            if (m_intensity > 0.6666667f)
            {
                                                            // Severe change, but how severe?
                uint32 rnd = urand(0, 99);
                if (rnd < 50)
                {
                    m_intensity -= 0.6666667f;
                    return true;
                }
            }
            m_type = WEATHER_TYPE_FINE;                     // clear up
            m_intensity = 0;
        }
    }

    // At this point, only weather that isn't doing anything remains but that have weather data
    uint32 chance1 = m_weatherChances->data[season].rainChance;
    uint32 chance2 = chance1+ m_weatherChances->data[season].snowChance;
    uint32 chance3 = chance2+ m_weatherChances->data[season].stormChance;

    uint32 rnd = urand(1, 100);
    if (rnd <= chance1)
        m_type = WEATHER_TYPE_RAIN;
    else if (rnd <= chance2)
        m_type = WEATHER_TYPE_SNOW;
    else if (rnd <= chance3)
        m_type = WEATHER_TYPE_STORM;
    else
        m_type = WEATHER_TYPE_FINE;

    /// New weather statistics (if not fine):
    ///- 85% light
    ///- 7% medium
    ///- 7% heavy
    /// If fine 100% sun (no fog)

    if (m_type == WEATHER_TYPE_FINE)
    {
        m_intensity = 0.0f;
    }
    else if (u < 90)
    {
        m_intensity = (float)rand_norm() * 0.3333f;
    }
    else
    {
        // Severe change, but how severe?
        rnd = urand(0, 99);
        if (rnd < 50)
            m_intensity = (float)rand_norm() * 0.3333f + 0.3334f;
        else
            m_intensity = (float)rand_norm() * 0.3333f + 0.6667f;
    }

    // return true only in case weather changes
    return m_type != old_type || m_intensity != old_intensity;
}

void Weather::SendWeatherUpdateToPlayer(Player* player)
{
    WorldPackets::Misc::Weather weather(GetWeatherState(), m_intensity);
    player->SendDirectMessage(weather.Write());
}

void Weather::SendFineWeatherUpdateToPlayer(Player* player)
{
    WorldPackets::Misc::Weather weather(WEATHER_STATE_FINE);
    player->SendDirectMessage(weather.Write());
}

/// Send the new weather to all players in the zone
// @tswow-begin triggerScripts parameter
bool Weather::UpdateWeather(bool triggerScripts)
// @tswow-end
{
    ///- Send the weather packet to all players in this zone
    if (m_intensity >= 1)
        m_intensity = 0.9999f;
    else if (m_intensity < 0)
        m_intensity = 0.0001f;

    WeatherState state = GetWeatherState();

    WorldPackets::Misc::Weather weather(state, m_intensity);

    //- Returns false if there were no players found to update
    if (!m_map->SendZoneMessage(m_zone, weather.Write()))
        return false;

    ///- Log the event
    char const* wthstr;
    switch (state)
    {
        case WEATHER_STATE_FOG:
            wthstr = "fog";
            break;
        case WEATHER_STATE_LIGHT_RAIN:
            wthstr = "light rain";
            break;
        case WEATHER_STATE_MEDIUM_RAIN:
            wthstr = "medium rain";
            break;
        case WEATHER_STATE_HEAVY_RAIN:
            wthstr = "heavy rain";
            break;
        case WEATHER_STATE_LIGHT_SNOW:
            wthstr = "light snow";
            break;
        case WEATHER_STATE_MEDIUM_SNOW:
            wthstr = "medium snow";
            break;
        case WEATHER_STATE_HEAVY_SNOW:
            wthstr = "heavy snow";
            break;
        case WEATHER_STATE_LIGHT_SANDSTORM:
            wthstr = "light sandstorm";
            break;
        case WEATHER_STATE_MEDIUM_SANDSTORM:
            wthstr = "medium sandstorm";
            break;
        case WEATHER_STATE_HEAVY_SANDSTORM:
            wthstr = "heavy sandstorm";
            break;
        case WEATHER_STATE_THUNDERS:
            wthstr = "thunders";
            break;
        case WEATHER_STATE_BLACKRAIN:
            wthstr = "blackrain";
            break;
        case WEATHER_STATE_FINE:
        default:
            wthstr = "fine";
            break;
    }

    TC_LOG_INFO("misc", "Change the weather of zone {} to {}.", m_zone, wthstr);
    sScriptMgr->OnWeatherChange(this, state, m_intensity);

    // @tswow-begin
    if (triggerScripts)
    {
        FIRE_ID(m_map->GetId(), Map, OnWeatherChange, TSMap(m_map), TSWeather(this));
    }
    // @tswow-end

    return true;
}

/// Set the weather
// @tswow-begin triggerScripts parameter
void Weather::SetWeather(WeatherType type, float intensity, bool triggerScripts)
// @tswow-end
{
    if (m_type == type && m_intensity == intensity)
        return;

    m_type = type;
    m_intensity = intensity;
    // @tswow-begin triggerScripts argument
    UpdateWeather(triggerScripts);
    // @tswow-end
}

/// Get the sound number associated with the current weather
WeatherState Weather::GetWeatherState() const
{
    if (m_intensity < 0.27f)
        return WEATHER_STATE_FINE;

    switch (m_type)
    {
        case WEATHER_TYPE_RAIN:
            if (m_intensity < 0.40f)
                return WEATHER_STATE_LIGHT_RAIN;
            else if (m_intensity < 0.70f)
                return WEATHER_STATE_MEDIUM_RAIN;
            else
                return WEATHER_STATE_HEAVY_RAIN;
        case WEATHER_TYPE_SNOW:
            if (m_intensity < 0.40f)
                return WEATHER_STATE_LIGHT_SNOW;
            else if (m_intensity < 0.70f)
                return WEATHER_STATE_MEDIUM_SNOW;
            else
                return WEATHER_STATE_HEAVY_SNOW;
        case WEATHER_TYPE_STORM:
            if (m_intensity < 0.40f)
                return WEATHER_STATE_LIGHT_SANDSTORM;
            else if (m_intensity < 0.70f)
                return WEATHER_STATE_MEDIUM_SANDSTORM;
            else
                return WEATHER_STATE_HEAVY_SANDSTORM;
        case WEATHER_TYPE_BLACKRAIN:
            return WEATHER_STATE_BLACKRAIN;
        case WEATHER_TYPE_THUNDERS:
            return WEATHER_STATE_THUNDERS;
        case WEATHER_TYPE_FINE:
        default:
            return WEATHER_STATE_FINE;
    }
}
