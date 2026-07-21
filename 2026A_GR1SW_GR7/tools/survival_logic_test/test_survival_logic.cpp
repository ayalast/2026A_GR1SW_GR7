#include <iostream>
#include <cstring>
#include <cmath>
#include <glm/glm.hpp>
#include "game_survival.h"

static int g_fail = 0;
static void expect(bool cond, const char* msg) {
    if (!cond) { std::cout << "FAIL: " << msg << "\n"; ++g_fail; }
    else std::cout << "PASS: " << msg << "\n";
}

// Mock: wall slab |x| < 2 blocks movement into it
static glm::vec3 mockCorrect(const glm::vec3& pos, const glm::vec3& delta, float /*radius*/, void* /*user*/) {
    glm::vec3 np = pos + delta;
    // solid wall region -4..4 in X for |z| anything near origin band
    if (std::abs(np.x) < 2.0f && std::abs(pos.x) < 2.0f) {
        // fully blocked if trying to stay inside wall
        return glm::vec3(0.0f);
    }
    if (std::abs(np.x) < 2.0f && std::abs(pos.x) >= 2.0f) {
        // clip at wall
        glm::vec3 m = delta;
        if (pos.x >= 2.0f && np.x < 2.0f) m.x = 2.0f - pos.x;
        if (pos.x <= -2.0f && np.x > -2.0f) m.x = -2.0f - pos.x;
        return m;
    }
    return delta;
}

int main() {
    char buf[32];
    Survival_FormatTimer(65.2f, buf, 32);
    expect(std::strcmp(buf, "1:06") == 0 || std::strcmp(buf, "1:05") == 0, "FormatTimer ~65s -> M:SS");
    Survival_FormatTimer(0.f, buf, 32);
    expect(std::strcmp(buf, "0") == 0, "FormatTimer 0");

    glm::vec3 player(0,0,0);
    glm::vec3 forward(0,0,-1);
    expect(Survival_IsMonsterVisibleToPlayer(glm::vec3(0,0,-10), player, forward, 70.f, 55.f),
           "monster ahead is visible");
    expect(!Survival_IsMonsterVisibleToPlayer(glm::vec3(0,0,10), player, forward, 70.f, 55.f),
           "monster behind is NOT visible");
    expect(!Survival_IsMonsterVisibleToPlayer(glm::vec3(0,0,-10), player, forward, 70.f, 55.f) == false,
           "visible => teleport blocked");
    bool canTpBehind = !Survival_IsMonsterVisibleToPlayer(glm::vec3(0,0,10), player, forward, 72.f, 52.f);
    expect(canTpBehind, "teleport allowed when behind");

    // Space clear: open area far from wall
    expect(Survival_SpaceLooksClear(glm::vec3(10,0,10), mockCorrect, nullptr, 0.85f),
           "open space looks clear");
    // Inside wall x=0: most moves blocked
    expect(!Survival_SpaceLooksClear(glm::vec3(0,0,0), mockCorrect, nullptr, 0.85f),
           "inside wall slab is NOT clear");

    // Typewriter sync windows (shipped isTypewriterKeying schedule)
    expect(!Survival_IsTypewriterKeyingAt(0.5f), "typewriter roller intro no keys");
    expect(Survival_IsTypewriterKeyingAt(5.0f), "typewriter first typing block");
    expect(!Survival_IsTypewriterKeyingAt(15.0f), "typewriter mid pause");
    expect(Survival_IsTypewriterKeyingAt(22.0f), "typewriter second typing block");
    expect(!Survival_IsTypewriterKeyingAt(34.0f), "typewriter end silence");

    std::cout << "TOTAL_FAILS=" << g_fail << "\n";
    return g_fail == 0 ? 0 : 1;
}
