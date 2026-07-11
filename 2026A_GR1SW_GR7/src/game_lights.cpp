#include "game_lights.h"

#include <iostream>
#include <random>
#include <unordered_map>
#include <cmath>
#include <utility>
#include <functional>

void applyCeilingLightMode(std::vector<CeilingLight>& lights, bool blackout)
{
    int on = 0, off = 0;
    for (CeilingLight& cl : lights)
    {
        cl.isOn = blackout ? cl.onTotalBlack : cl.onClear;
        if (cl.isOn) ++on; else ++off;
    }
    std::cout << "[Light] Modo " << (blackout ? "NEGRO TOTAL" : "CLARO")
              << " -> ON=" << on << " OFF=" << off << "\n";
}

void findCeilingLights(
    const glm::vec3& worldOffset,
    std::vector<CeilingLight>& lights,
    bool blackout)
{
    std::mt19937 rng(20260710u);
    std::uniform_real_distribution<float> roll(0.0f, 1.0f);

    float offsetX = 2.9f;
    float offsetZ = -2.3f;
    float spacingX = 6.0f;
    float spacingZ = 8.0f;

    float xMin = -182.4f + worldOffset.x;
    float xMax = 194.8f + worldOffset.x;
    float zMin = -185.7f + worldOffset.z;
    float zMax = 191.5f + worldOffset.z;

    float ceilY = 8.565f + worldOffset.y - 1.2f;

    const float zoneSize = 28.0f;
    using ZoneKey = std::pair<int, int>;
    struct ZoneKeyHash
    {
        size_t operator()(const ZoneKey& k) const noexcept
        {
            return std::hash<int>{}(k.first) ^ (std::hash<int>{}(k.second) << 1);
        }
    };
    auto zoneKey = [zoneSize](float x, float z) -> ZoneKey {
        int ix = static_cast<int>(std::floor(x / zoneSize));
        int iz = static_cast<int>(std::floor(z / zoneSize));
        return { ix, iz };
    };

    std::unordered_map<ZoneKey, bool, ZoneKeyHash> zoneIsDark;
    for (float x = xMin + spacingX * 0.5f + offsetX; x < xMax; x += spacingX)
    {
        for (float z = zMin + spacingZ * 0.5f + offsetZ; z < zMax; z += spacingZ)
        {
            ZoneKey k = zoneKey(x, z);
            if (zoneIsDark.find(k) != zoneIsDark.end())
                continue;
            zoneIsDark[k] = roll(rng) < 0.38f;
        }
    }

    std::unordered_map<ZoneKey, bool, ZoneKeyHash> zoneDarkSmoothed = zoneIsDark;
    for (const auto& kv : zoneIsDark)
    {
        const int ix = kv.first.first;
        const int iz = kv.first.second;
        int darkNeighbors = 0;
        for (int dx = -1; dx <= 1; ++dx)
        {
            for (int dz = -1; dz <= 1; ++dz)
            {
                if (dx == 0 && dz == 0) continue;
                ZoneKey nk{ ix + dx, iz + dz };
                auto it = zoneIsDark.find(nk);
                if (it != zoneIsDark.end() && it->second)
                    darkNeighbors++;
            }
        }
        if (darkNeighbors >= 2)
            zoneDarkSmoothed[kv.first] = true;
        else if (darkNeighbors == 0 && kv.second && roll(rng) < 0.45f)
            zoneDarkSmoothed[kv.first] = false;
    }

    lights.clear();
    int onCount = 0;
    int offCount = 0;
    for (float x = xMin + spacingX * 0.5f + offsetX; x < xMax; x += spacingX)
    {
        for (float z = zMin + spacingZ * 0.5f + offsetZ; z < zMax; z += spacingZ)
        {
            CeilingLight cl;
            cl.position = glm::vec3(x, ceilY, z);

            ZoneKey k = zoneKey(x, z);
            bool darkZone = zoneDarkSmoothed.count(k) ? zoneDarkSmoothed[k] : false;

            if (darkZone)
                cl.onClear = roll(rng) < 0.04f;
            else
                cl.onClear = roll(rng) > 0.08f;

            cl.onTotalBlack = false;
            cl.isOn = blackout ? cl.onTotalBlack : cl.onClear;
            if (cl.onClear) onCount++;
            else offCount++;
            lights.push_back(cl);
        }
    }

    std::cout << "Ceiling lights: total=" << lights.size()
              << " clear-ON=" << onCount << " clear-OFF=" << offCount
              << " zone=" << zoneSize << "u"
              << " | actual=" << (blackout ? "NEGRO" : "CLARO") << "\n";
    std::cout << "[Light] ENCENDER = zonas claras/oscuras. APAGAR = mapa negro + linterna.\n";
}
