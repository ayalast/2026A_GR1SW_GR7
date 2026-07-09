#pragma once

#include <vector>
#include <glm/glm.hpp>

// Luces de techo del nivel (grid + zonas lit/dark)
struct CeilingLight
{
    glm::vec3 position;
    bool onClear = true;       // modo normal
    bool onTotalBlack = false; // blackout
    bool isOn = true;
};

// blackout=true -> onTotalBlack; si no, onClear
void applyCeilingLightMode(std::vector<CeilingLight>& lights, bool blackout);

// Genera la grilla de lamparas y decide zonas oscuras
void findCeilingLights(
    const glm::vec3& worldOffset,
    std::vector<CeilingLight>& lights,
    bool blackout);
