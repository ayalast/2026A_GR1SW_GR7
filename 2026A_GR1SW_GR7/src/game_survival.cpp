#include "game_survival.h"
#include "audio_bgm.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <random>
#include <string>
#include <vector>

// Balance
static constexpr float kHuntSeconds = 60.0f;
static constexpr float kChaseSeconds = 45.0f;
static constexpr float kRageSeconds = 30.0f;

static constexpr float kMonsterFleeSpeed = 4.6f;
static constexpr float kMonsterChaseSpeed = 16.9f;
static constexpr float kMonsterRageSpeed = 18.8f;
static constexpr float kTurnSlowMul = 0.58f;
static constexpr float kWallSlowMul = 0.68f;
static constexpr float kCatchRadius = 1.70f;
static constexpr float kInteractRadius = 4.2f;
static constexpr float kSpawnMinDistFromPlayer = 20.0f; // Hunt: cerca medio, encontrable
static constexpr float kMonsterRadius = 0.85f;
static constexpr float kTrailInterval = 0.07f;
static constexpr int   kTrailMax = 100;

// Spawn al iniciar la persecución (banda de distancia al jugador).
// Chase (luces apagadas): más lejos + periodo de gracia para reaccionar.
// Rage (admiración): ya despierta, aparece cerca y persigue de inmediato.
static constexpr float kChaseSpawnMin = 20.0f;
static constexpr float kChaseSpawnMax = 26.0f;
static constexpr float kRageSpawnMin  = 12.0f;
static constexpr float kRageSpawnMax  = 16.0f;

// Gracia solo en Chase. El monstruo despierta. Timer e IA congelados mientras
// el jugador ve la cuenta y el aviso de correr. Rage no tiene gracia.
static constexpr float kChaseGraceSeconds = 5.0f;

// Red de seguridad anti atasco durante la persecucion. Si el monstruo no logra
// acercarse en este tiempo, se reubica cerca para garantizar que pueda atrapar.
static constexpr float kChaseStuckSeconds = 3.5f;
static constexpr float kChaseStuckSpawnMin = 10.0f;
static constexpr float kChaseStuckSpawnMax = 14.0f;

static constexpr float kJumpscareDuration = 3.0f;
static constexpr float kTitleCardSeconds = 1.05f;
// Teleport SOLO en Hunt (luces encendidas), UNA sola vez, salto corto:
// dificulta encontrarlo sin dejarlo inalcanzable.
static constexpr int   kHuntMaxTeleports = 1;
static constexpr float kTeleportCooldownMin = 8.0f;
static constexpr float kTeleportCooldownMax = 14.0f;
static constexpr float kTeleportChancePerSec = 0.35f; // while off screen & cooldown ready
static constexpr float kTeleportMinJump = 10.0f;
static constexpr float kTeleportMaxJump = 20.0f;

// Cinemática final: 1s blanco + máquina de escribir a ritmo natural constante.
// La escritura NO se sincroniza al audio; primero manda la animación y luego
// se editará el audio para que sus silencios cuadren con este tecleo.
static constexpr float kCineWhiteSeconds = 1.0f;
// Velocidad de tecleo fija (caracteres por segundo). Ritmo de máquina real.
static constexpr float kTypeCharsPerSec = 11.0f;
static constexpr float kTypewriterAudioDur = 34.97f;
static constexpr float kTypeDoneHold = 2.5f;

static bool g_enabled = false;
static SurvivalPhase g_phase = SurvivalPhase::Inactive;
static SurvivalRoomBounds g_room{};
static float g_monsterPosY = 0.0f;
static float g_monsterScale = 1.8f;
static bool g_inited = false;

static glm::vec3 g_monsterPos(0.0f);
static float g_monsterYaw = 0.0f;
static float g_timer = 0.0f;
static float g_timerMax = 1.0f;
static float g_phaseElapsed = 0.0f;

static float g_jumpscareT = 0.0f;
static float g_jumpscareFlash = 0.0f;
static float g_titleCardT = 0.0f;
// 0 none | 1 Luces Apagadas (A2) | 2 ¡Huye! B1 | 3 ¡Huye! B3 (aleatorio en Rage)
static int   g_titleCardKind = 0;

struct TrailPt { glm::vec3 p; };
static std::vector<TrailPt> g_trail;
static float g_trailAcc = 0.0f;
static glm::vec3 g_lastMoveDir(0.0f, 0.0f, 1.0f);
static bool g_inInteractRange = false;

static float g_teleportCd = 8.0f;
static float g_teleportReadyAcc = 0.0f;
static float g_sinceTeleport = 999.0f;
static int   g_huntTeleports = 0;
static float g_chaseGraceT = 0.0f;
static float g_chaseStuckAcc = 0.0f;
static float g_chaseBestDist = 1e9f;

// Cinemática
static std::vector<const char*> g_lines;
static std::string g_fullMessage;   // todas las lineas unidas
static int g_lineIndex = 0;         // legacy (lineas)
static float g_lineT = 0.0f;
static float g_cineGlobalT = 0.0f;  // desde inicio de cinemática
static float g_cineWhiteLeft = 0.0f;
static bool g_typewriterAudioStarted = false;
static float g_typeAccum = 0.0f;    // segundos efectivos de escritura
static float g_typeDoneHold = 0.0f;
static int   g_fullCodepoints = 0;
static bool g_cineDone = false;
static int g_cineStyle = 0;
static char g_visibleBuf[768] = {};
static float g_scanPhase = 0.0f;

// Cuenta codepoints UTF 8 (no bytes)
static int utf8CodepointCount(const char* s)
{
    if (!s) return 0;
    int n = 0;
    while (*s)
    {
        unsigned char c = static_cast<unsigned char>(*s);
        if ((c & 0xC0) != 0x80)
            ++n;
        ++s;
    }
    return n;
}

// Avanza n codepoints UTF 8 y devuelve offset en bytes
static int utf8ByteOffsetForCodepoints(const char* s, int codepoints)
{
    if (!s || codepoints <= 0) return 0;
    int i = 0;
    int cp = 0;
    while (s[i] && cp < codepoints)
    {
        unsigned char c = static_cast<unsigned char>(s[i]);
        int len = 1;
        if ((c & 0x80) == 0) len = 1;
        else if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        i += len;
        ++cp;
    }
    return i;
}

static std::mt19937 g_rng{ std::random_device{}() };

static float randRange(float a, float b)
{
    std::uniform_real_distribution<float> d(a, b);
    return d(g_rng);
}

static float distXZ(const glm::vec3& a, const glm::vec3& b)
{
    const float dx = a.x - b.x;
    const float dz = a.z - b.z;
    return std::sqrt(dx * dx + dz * dz);
}

static float yawFromDir(const glm::vec3& d)
{
    return glm::degrees(std::atan2(d.z, d.x));
}

static glm::vec3 flatNorm(const glm::vec3& v)
{
    glm::vec3 f(v.x, 0.0f, v.z);
    float len = glm::length(f);
    if (len < 1e-5f)
        return glm::vec3(0.0f, 0.0f, -1.0f);
    return f / len;
}

// Pure logic
bool Survival_IsMonsterVisibleToPlayer(
    const glm::vec3& monsterPos,
    const glm::vec3& playerPos,
    const glm::vec3& playerForwardFlat,
    float fovDeg,
    float maxSeeDist)
{
    const float d = distXZ(monsterPos, playerPos);
    if (d > maxSeeDist)
        return false;
    if (d < 1.2f)
        return true; // encima del jugador

    glm::vec3 toM = flatNorm(monsterPos - playerPos);
    glm::vec3 fwd = flatNorm(playerForwardFlat);
    float cosA = glm::dot(fwd, toM);
    // half FOV with small margin (more conservative es harder to "see" for teleport block)
    float halfFov = glm::radians(fovDeg * 0.5f);
    float cosHalf = std::cos(halfFov);
    return cosA >= cosHalf;
}

void Survival_FormatTimer(float secondsLeft, char* out, int outCap)
{
    if (!out || outCap < 4)
        return;
    if (secondsLeft < 0.0f)
        secondsLeft = 0.0f;
    int total = static_cast<int>(std::ceil(secondsLeft));
    int m = total / 60;
    int s = total % 60;
    if (m > 0)
        std::snprintf(out, outCap, "%d:%02d", m, s);
    else
        std::snprintf(out, outCap, "%d", s);
}

bool Survival_SpaceLooksClear(
    const glm::vec3& pos,
    SurvivalCorrectMoveFn correctMove,
    void* correctUser,
    float radius)
{
    if (!correctMove)
        return true;
    glm::vec3 p = pos;
    p.y = 0.0f;
    // 8 direcciones: si casi no se puede mover, estamos dentro de geometria
    const float step = 1.15f;
    int freeDirs = 0;
    for (int i = 0; i < 8; ++i)
    {
        const float ang = (float)i * 0.78539816f; // 45 deg
        glm::vec3 dir(std::cos(ang), 0.0f, std::sin(ang));
        glm::vec3 moved = correctMove(p, dir * step, radius, correctUser);
        moved.y = 0.0f;
        if (glm::length(moved) > step * 0.35f)
            ++freeDirs;
    }
    // Al menos la mitad de las direcciones deben permitir desplazamiento
    return freeDirs >= 4;
}

static void rebuildFullMessage()
{
    g_fullMessage.clear();
    for (size_t i = 0; i < g_lines.size(); ++i)
    {
        if (i) g_fullMessage += "\n\n";
        if (g_lines[i]) g_fullMessage += g_lines[i];
    }
}

static void resetCinematicState(int style)
{
    g_lineIndex = 0;
    g_lineT = 0.0f;
    g_cineGlobalT = 0.0f;
    g_cineWhiteLeft = kCineWhiteSeconds;
    g_typewriterAudioStarted = false;
    g_typeAccum = 0.0f;
    g_typeDoneHold = 0.0f;
    g_cineDone = false;
    g_cineStyle = style;
    g_visibleBuf[0] = '\0';
    rebuildFullMessage();
    g_fullCodepoints = utf8CodepointCount(g_fullMessage.c_str());
    AudioBgm_TypewriterStop();
}

static void setLinesKill()
{
    // Final "mataste al monstruo al comienzo" (interacción E en Hunt).
    g_lines = {
        "La sensación de las luces se siente mejor sabiendo que no hay nadie persiguiéndote.",
        "No te importa si las luces son falsas, no te importa si las paredes son falsas.",
        "Una nueva esperanza ha despertado en ti."
    };
    resetCinematicState(0);
}

static void setLinesEscape()
{
    // Final tras sobrevivir al modo admiracion (escapaste).
    g_lines = {
        "Corre, corre tan rápido como puedas.",
        "Gracias a un error ahora eres libre.",
        "La tierra en tus pies está fría y el césped te hace cosquillas.",
        "Lo sientes, es real.",
        "Has escapado."
    };
    resetCinematicState(1);
}

static void setLinesLose()
{
    g_lines = {
        "Te encontró.",
        "No hay salida.",
        "Corriste en círculos por el laberinto.",
        "Y el laberinto te devoró."
    };
    resetCinematicState(2);
}

// Ventanas donde el audio de máquina escribe (aprox. al SFX real ~35s).
// Fuera de estas ventanas: pausa (rodillo / silencio) , no se escriben letras.
static bool isTypewriterKeying(float audioT)
{
    if (audioT < 0.0f)
        return false;
    // Intro: rodillo / ajuste (sin teclas)
    if (audioT < 1.85f)
        return false;
    // Primer bloque de escritura
    if (audioT < 12.2f)
        return true;
    // Pausa larga central del clip
    if (audioT < 17.95f)
        return false;
    // Segundo bloque
    if (audioT < 32.6f)
        return true;
    // Cierre
    return false;
}

bool Survival_IsTypewriterKeyingAt(float audioSeconds)
{
    return isTypewriterKeying(audioSeconds);
}

static SurvivalCorrectMoveFn g_spawnCorrect = nullptr;
static void* g_spawnUser = nullptr;

void Survival_SetCollisionProbe(SurvivalCorrectMoveFn fn, void* user)
{
    g_spawnCorrect = fn;
    g_spawnUser = user;
}

static bool trySpawnFar(const glm::vec3& playerPos, float minDist, float minFromOld = 0.0f)
{
    std::uniform_real_distribution<float> ux(g_room.min.x + 10.0f, g_room.max.x - 10.0f);
    std::uniform_real_distribution<float> uz(g_room.min.z + 10.0f, g_room.max.z - 10.0f);
    std::uniform_real_distribution<float> uyaw(0.0f, 360.0f);
    glm::vec3 old = g_monsterPos;

    for (int i = 0; i < 160; ++i)
    {
        glm::vec3 p(ux(g_rng), g_monsterPosY, uz(g_rng));
        if (distXZ(p, playerPos) < minDist)
            continue;
        if (minFromOld > 0.0f && distXZ(p, old) < minFromOld)
            continue;
        if (!Survival_SpaceLooksClear(p, g_spawnCorrect, g_spawnUser, kMonsterRadius))
            continue;
        g_monsterPos = p;
        g_monsterYaw = uyaw(g_rng);
        return true;
    }
    // Fallback: ancla lejana + snap por colision
    glm::vec3 p(
        (g_room.min.x + g_room.max.x) * 0.5f - 42.0f,
        g_monsterPosY,
        (g_room.min.z + g_room.max.z) * 0.5f - 22.0f);
    if (distXZ(p, playerPos) < minDist * 0.5f)
        p.x = playerPos.x + minDist;
    // Empujar fuera de paredes con micro pasos
    if (g_spawnCorrect)
    {
        glm::vec3 probe = p; probe.y = 0.0f;
        for (int k = 0; k < 12; ++k)
        {
            glm::vec3 push(randRange(-1.f, 1.f), 0.f, randRange(-1.f, 1.f));
            if (glm::length(push) < 0.1f) continue;
            push = glm::normalize(push) * 2.0f;
            glm::vec3 m = g_spawnCorrect(probe, push, kMonsterRadius, g_spawnUser);
            probe += m;
        }
        p.x = probe.x;
        p.z = probe.z;
        p.y = g_monsterPosY;
    }
    g_monsterPos = p;
    g_monsterYaw = 45.0f;
    return true;
}

// Spawn cercano en una banda [minDist, maxDist] alrededor del jugador, en espacio
// libre. Se usa al iniciar Chase/Rage para que el monstruo aparezca cerca y persiga.
static bool trySpawnNear(const glm::vec3& playerPos, float minDist, float maxDist)
{
    std::uniform_real_distribution<float> uang(0.0f, 6.2831853f);
    std::uniform_real_distribution<float> urad(minDist, maxDist);
    // Barrido: varios ángulos/radios buscando un punto libre dentro de la sala.
    for (int i = 0; i < 200; ++i)
    {
        const float ang = uang(g_rng);
        const float rad = urad(g_rng);
        glm::vec3 p(
            playerPos.x + std::cos(ang) * rad,
            g_monsterPosY,
            playerPos.z + std::sin(ang) * rad);
        p.x = std::clamp(p.x, g_room.min.x + 3.0f, g_room.max.x - 3.0f);
        p.z = std::clamp(p.z, g_room.min.z + 3.0f, g_room.max.z - 3.0f);
        if (distXZ(p, playerPos) < minDist * 0.75f)
            continue;
        if (!Survival_SpaceLooksClear(p, g_spawnCorrect, g_spawnUser, kMonsterRadius))
            continue;
        g_monsterPos = p;
        g_monsterYaw = yawFromDir(flatNorm(playerPos - p));
        return true;
    }
    // Fallback: a maxDist justo detrás del jugador, empujado fuera de paredes.
    glm::vec3 p(playerPos.x, g_monsterPosY, playerPos.z + maxDist);
    p.x = std::clamp(p.x, g_room.min.x + 3.0f, g_room.max.x - 3.0f);
    p.z = std::clamp(p.z, g_room.min.z + 3.0f, g_room.max.z - 3.0f);
    if (g_spawnCorrect)
    {
        glm::vec3 probe = p; probe.y = 0.0f;
        for (int k = 0; k < 12; ++k)
        {
            glm::vec3 push(randRange(-1.f, 1.f), 0.f, randRange(-1.f, 1.f));
            if (glm::length(push) < 0.1f) continue;
            push = glm::normalize(push) * 1.5f;
            probe += g_spawnCorrect(probe, push, kMonsterRadius, g_spawnUser);
        }
        p.x = probe.x; p.z = probe.z; p.y = g_monsterPosY;
    }
    g_monsterPos = p;
    g_monsterYaw = yawFromDir(flatNorm(playerPos - p));
    return true;
}

static void beginPhase(SurvivalPhase p, const glm::vec3& playerPos)
{
    g_phase = p;
    g_phaseElapsed = 0.0f;
    g_trail.clear();
    g_trailAcc = 0.0f;
    g_inInteractRange = false;

    switch (p)
    {
    case SurvivalPhase::Hunt:
        g_timerMax = kHuntSeconds;
        g_timer = kHuntSeconds;
        trySpawnFar(playerPos, kSpawnMinDistFromPlayer);
        g_teleportCd = randRange(kTeleportCooldownMin, kTeleportCooldownMax);
        g_teleportReadyAcc = 0.0f;
        g_sinceTeleport = 999.0f;
        g_huntTeleports = 0; // solo Hunt teleporta, y máx kHuntMaxTeleports veces
        g_chaseGraceT = 0.0f;
        g_titleCardT = 0.0f;
        g_titleCardKind = 0;
        AudioBgm_TimerTickStop();
        std::cout << "[Survival] HUNT " << kHuntSeconds << "s — E cerca para matar; monstruo huye (1 teleport)\n";
        break;
    case SurvivalPhase::Chase:
        g_timerMax = kChaseSeconds;
        g_timer = kChaseSeconds;
        // Aparece a media distancia para perseguir, sin teleport en este modo.
        trySpawnNear(playerPos, kChaseSpawnMin, kChaseSpawnMax);
        g_chaseGraceT = kChaseGraceSeconds;
        g_chaseStuckAcc = 0.0f;
        g_chaseBestDist = distXZ(g_monsterPos, playerPos);
        g_titleCardT = kTitleCardSeconds;
        g_titleCardKind = 1;
        AudioBgm_TimerTickStop();
        AudioBgm_PlaySfx("sounds/flashlight.mp3", 0.55f);
        std::cout << "[Survival] CHASE " << kChaseSeconds << "s luces apagadas, gracia "
                  << kChaseGraceSeconds << "s y persigue\n";
        break;
    case SurvivalPhase::Rage:
        g_timerMax = kRageSeconds;
        g_timer = kRageSeconds;
        // Reposiciona cerca para persecucion agresiva inmediata, sin gracia ni teleport.
        trySpawnNear(playerPos, kRageSpawnMin, kRageSpawnMax);
        g_chaseGraceT = 0.0f;
        g_chaseStuckAcc = 0.0f;
        g_chaseBestDist = distXZ(g_monsterPos, playerPos);
        g_titleCardT = kTitleCardSeconds;
        g_titleCardKind = (std::rand() % 2 == 0) ? 2 : 3;
        AudioBgm_TimerTickStop();
        AudioBgm_PlaySfx("sounds/admiracion_alarm.wav", 0.45f);
        std::cout << "[Survival] RAGE " << kRageSeconds << "s admiracion, monstruo persigue B"
                  << (g_titleCardKind == 2 ? "1" : "3") << "\n";
        break;
    case SurvivalPhase::Jumpscare:
        g_jumpscareT = kJumpscareDuration;
        g_jumpscareFlash = 1.0f;
        g_titleCardT = 0.0f;
        g_titleCardKind = 0;
        AudioBgm_TimerTickStop();
        AudioBgm_PlaySfx("sounds/entity_jumpscare_scream.mp3", 1.0f);
        AudioBgm_PlaySfx("sounds/human_caught_scream.mp3", 0.95f);
        std::cout << "[Survival] JUMPSCARE (entity+human screams)\n";
        break;
    case SurvivalPhase::EndingKill:
        setLinesKill();
        AudioBgm_ProximitySetVolume(0.0f);
        AudioBgm_TimerTickStop();
        AudioBgm_PlaySfx("sounds/flashlight.mp3", 0.35f);
        std::cout << "[Survival] ENDING KILL\n";
        break;
    case SurvivalPhase::EndingEscape:
        setLinesEscape();
        AudioBgm_ProximitySetVolume(0.0f);
        AudioBgm_TimerTickStop();
        std::cout << "[Survival] ENDING ESCAPE\n";
        break;
    case SurvivalPhase::EndingLose:
        setLinesLose();
        AudioBgm_ProximitySetVolume(0.0f);
        AudioBgm_TimerTickStop();
        std::cout << "[Survival] ENDING LOSE\n";
        break;
    default:
        break;
    }
}

static glm::vec3 moveWithCollision(
    SurvivalCorrectMoveFn correctMove, void* user, glm::vec3 pos, glm::vec3 delta)
{
    delta.y = 0.0f;
    glm::vec3 probePos = pos;
    probePos.y = 0.0f;
    glm::vec3 corrected = delta;
    if (correctMove)
        corrected = correctMove(probePos, delta, kMonsterRadius, user);
    corrected.y = 0.0f;
    pos.x += corrected.x;
    pos.z += corrected.z;
    pos.y = g_monsterPosY;
    pos.x = std::clamp(pos.x, g_room.min.x + 2.0f, g_room.max.x - 2.0f);
    pos.z = std::clamp(pos.z, g_room.min.z + 2.0f, g_room.max.z - 2.0f);
    return pos;
}

static bool wallAhead(
    SurvivalCorrectMoveFn correctMove, void* user, const glm::vec3& pos, const glm::vec3& dir, float dist)
{
    if (!correctMove) return false;
    glm::vec3 d = flatNorm(dir);
    glm::vec3 probe = d * dist;
    glm::vec3 probePos = pos;
    probePos.y = 0.0f;
    glm::vec3 corrected = correctMove(probePos, probe, kMonsterRadius, user);
    float moved = glm::length(glm::vec3(corrected.x, 0.0f, corrected.z));
    return moved < dist * 0.45f;
}

static void updateFlee(
    float dt, const glm::vec3& playerPos, SurvivalCorrectMoveFn correctMove, void* user)
{
    glm::vec3 away = g_monsterPos - playerPos;
    away.y = 0.0f;
    float len = glm::length(away);
    if (len < 0.01f)
    {
        away = glm::vec3(1.0f, 0.0f, 0.0f);
        len = 1.0f;
    }
    away /= len;

    if (wallAhead(correctMove, user, g_monsterPos, away, 2.2f))
    {
        glm::vec3 side(-away.z, 0.0f, away.x);
        if (wallAhead(correctMove, user, g_monsterPos, side, 1.8f))
            side = -side;
        away = glm::normalize(away * 0.35f + side * 0.65f);
    }

    float speed = kMonsterFleeSpeed;
    if (len < 12.0f) speed *= 1.25f;

    glm::vec3 prev = g_monsterPos;
    g_monsterPos = moveWithCollision(correctMove, user, g_monsterPos, away * speed * dt);
    glm::vec3 moved = g_monsterPos - prev;
    moved.y = 0.0f;
    if (glm::length(moved) > 1e-4f)
    {
        g_lastMoveDir = glm::normalize(moved);
        g_monsterYaw = yawFromDir(g_lastMoveDir);
    }
}

static void pushTrail(const glm::vec3& playerPos)
{
    TrailPt t;
    t.p = playerPos;
    t.p.y = 0.0f;
    g_trail.push_back(t);
    if ((int)g_trail.size() > kTrailMax)
        g_trail.erase(g_trail.begin());
}

static void updateChase(
    float dt, const glm::vec3& playerPos, SurvivalCorrectMoveFn correctMove, void* user, float baseSpeed)
{
    g_trailAcc += dt;
    if (g_trailAcc >= kTrailInterval)
    {
        g_trailAcc = 0.0f;
        pushTrail(playerPos);
    }

    glm::vec3 target = playerPos;
    target.y = 0.0f;
    if (g_trail.size() >= 4)
    {
        size_t back = std::min<size_t>(g_trail.size() - 1, 14);
        target = g_trail[g_trail.size() - 1 - back].p;
        if (distXZ(g_monsterPos, target) < 1.2f && g_trail.size() > 2)
            target = g_trail.back().p;
    }

    glm::vec3 to = target - g_monsterPos;
    to.y = 0.0f;
    float len = glm::length(to);
    if (len < 1e-3f)
        to = playerPos - g_monsterPos;
    to.y = 0.0f;
    len = glm::length(to);
    if (len < 1e-3f)
        return;
    glm::vec3 dir = to / len;

    glm::vec3 toPlayer = playerPos - g_monsterPos;
    toPlayer.y = 0.0f;
    float lp = glm::length(toPlayer);
    if (lp > 1e-3f)
    {
        toPlayer /= lp;
        dir = glm::normalize(dir * 0.52f + toPlayer * 0.48f);
    }

    float speed = baseSpeed;
    if (glm::length(g_lastMoveDir) > 0.1f)
    {
        float turn = glm::dot(glm::normalize(g_lastMoveDir), dir);
        if (turn < 0.55f)
            speed *= kTurnSlowMul;
        else if (turn < 0.85f)
            speed *= 0.85f;
    }
    if (wallAhead(correctMove, user, g_monsterPos, dir, 1.6f))
    {
        speed *= kWallSlowMul;
        glm::vec3 side(-dir.z, 0.0f, dir.x);
        if (!wallAhead(correctMove, user, g_monsterPos, side, 1.2f))
            dir = glm::normalize(dir * 0.3f + side * 0.7f);
        else if (!wallAhead(correctMove, user, g_monsterPos, -side, 1.2f))
            dir = glm::normalize(dir * 0.3f + (-side) * 0.7f);
    }

    glm::vec3 prev = g_monsterPos;
    g_monsterPos = moveWithCollision(correctMove, user, g_monsterPos, dir * speed * dt);
    glm::vec3 moved = g_monsterPos - prev;
    moved.y = 0.0f;
    if (glm::length(moved) > 1e-4f)
    {
        g_lastMoveDir = glm::normalize(moved);
        g_monsterYaw = yawFromDir(g_lastMoveDir);
    }
    else
    {
        glm::vec3 jitter(randRange(-1.f, 1.f), 0.0f, randRange(-1.f, 1.f));
        if (glm::length(jitter) > 0.1f)
            g_monsterPos = moveWithCollision(
                correctMove, user, g_monsterPos, glm::normalize(jitter) * baseSpeed * 0.4f * dt);
    }
}

// Red de seguridad de la persecucion. Si el monstruo no reduce su mejor distancia
// al jugador durante kChaseStuckSeconds, se reubica cerca para no quedar atascado
// entre paredes y asegurar que la persecucion pueda terminar en captura.
static void updateChaseAntiStuck(float dt, const glm::vec3& playerPos)
{
    const float d = distXZ(g_monsterPos, playerPos);
    if (d < g_chaseBestDist - 0.25f)
    {
        g_chaseBestDist = d;
        g_chaseStuckAcc = 0.0f;
        return;
    }
    g_chaseStuckAcc += dt;
    if (g_chaseStuckAcc >= kChaseStuckSeconds)
    {
        trySpawnNear(playerPos, kChaseStuckSpawnMin, kChaseStuckSpawnMax);
        g_chaseStuckAcc = 0.0f;
        g_chaseBestDist = distXZ(g_monsterPos, playerPos);
        g_trail.clear();
        std::cout << "[Survival] Persecucion reubicada, el monstruo estaba atascado\n";
    }
}

// Teleport solo en Hunt, una unica vez por ciclo y a un salto corto acotado,
// para que sea algo mas dificil de encontrar pero nunca inalcanzable.
static void tryHuntTeleport(float dt, const glm::vec3& playerPos, const glm::vec3& playerForward)
{
    g_sinceTeleport += dt;
    if (g_huntTeleports >= kHuntMaxTeleports)
        return;

    g_teleportCd -= dt;
    if (g_teleportCd > 0.0f)
    {
        g_teleportReadyAcc = 0.0f;
        return;
    }

    // Solo si NO está a la vista del jugador (frustum + delante)
    if (Survival_IsMonsterVisibleToPlayer(g_monsterPos, playerPos, playerForward, 72.0f, 52.0f))
    {
        g_teleportReadyAcc = 0.0f;
        return;
    }

    // Acumula mientras sigue oculto; salta cuando llega a ~1 (≈ 1/kChance segundos)
    g_teleportReadyAcc += dt * kTeleportChancePerSec;
    if (g_teleportReadyAcc < 1.0f)
        return;

    // Salto corto acotado: reaparece a una banda media, no en la otra punta.
    glm::vec3 before = g_monsterPos;
    if (!trySpawnNear(playerPos, kTeleportMinJump, kTeleportMaxJump))
    {
        g_teleportReadyAcc = 0.6f; // reintentar pronto
        return;
    }
    if (distXZ(before, g_monsterPos) < kTeleportMinJump * 0.5f)
    {
        g_teleportReadyAcc = 0.6f;
        return;
    }

    g_huntTeleports++;
    g_teleportReadyAcc = 0.0f;
    g_sinceTeleport = 0.0f;
    AudioBgm_PlaySfx("sounds/footstep.wav", 0.08f);
    std::cout << "[Survival] TELEPORT Hunt (única vez) -> ("
              << g_monsterPos.x << ", " << g_monsterPos.z << ")\n";
}

static float typewriterCharsPerSec()
{
    // Ritmo natural constante; la escritura manda y el audio se edita luego.
    return kTypeCharsPerSec;
}

static void rebuildVisibleText()
{
    g_visibleBuf[0] = '\0';
    if (g_cineDone)
    {
        // El body se toma de g_fullMessage; visible hint aparte en main
        if (!g_fullMessage.empty())
        {
            const int n = std::min((int)g_fullMessage.size(), (int)sizeof(g_visibleBuf) - 1);
            std::memcpy(g_visibleBuf, g_fullMessage.data(), static_cast<size_t>(n));
            g_visibleBuf[n] = '\0';
        }
        return;
    }
    // Pantalla blanca inicial: sin texto
    if (g_cineWhiteLeft > 0.0f)
        return;
    if (g_fullMessage.empty())
        return;

    const int wantCp = static_cast<int>(g_typeAccum * typewriterCharsPerSec());
    int cp = std::max(0, std::min(wantCp, g_fullCodepoints));
    int n = utf8ByteOffsetForCodepoints(g_fullMessage.c_str(), cp);
    if (n >= (int)sizeof(g_visibleBuf))
        n = (int)sizeof(g_visibleBuf) - 1;
    std::memcpy(g_visibleBuf, g_fullMessage.data(), static_cast<size_t>(n));
    g_visibleBuf[n] = '\0';
}

static void updateCinematic(float dt)
{
    g_cineGlobalT += dt;
    g_scanPhase += dt * 0.5f;

    if (g_cineDone)
    {
        rebuildVisibleText();
        return;
    }
    if (g_fullMessage.empty() && g_lines.empty())
    {
        g_cineDone = true;
        rebuildVisibleText();
        return;
    }

    // 1) Blanco puro 1 segundo completo
    if (g_cineWhiteLeft > 0.0f)
    {
        g_cineWhiteLeft -= dt;
        if (g_cineWhiteLeft > 0.0f)
        {
            g_visibleBuf[0] = '\0';
            return;
        }
        g_cineWhiteLeft = 0.0f;
        if (!g_typewriterAudioStarted)
        {
            // Audio de tecleo dedicado por final: mismo SFX pero recortado para
            // que el tecleo continuo cuadre exactamente con la escritura.
            const char* twPath =
                g_cineStyle == 0 ? "sounds/typewriter_kill.mp3" :
                g_cineStyle == 1 ? "sounds/typewriter_escape.mp3" :
                                   "sounds/typewriter_lose.mp3";
            AudioBgm_TypewriterStart(twPath, 0.78f);
            g_typewriterAudioStarted = true;
            std::cout << "[Survival] Typewriter audio start: " << twPath << "\n";
        }
    }

    // 2) La escritura avanza SIEMPRE a ritmo natural constante (no gated por audio).
    //    El audio de cada final ya está recortado a esta misma duración.
    g_typeAccum += dt;

    rebuildVisibleText();

    const int shownCp = static_cast<int>(g_typeAccum * typewriterCharsPerSec());
    if (shownCp >= g_fullCodepoints && g_fullCodepoints > 0)
    {
        g_typeDoneHold += dt;
        // Tras mensaje completo: hold corto
        if (g_typeDoneHold >= kTypeDoneHold)
        {
            g_cineDone = true;
            rebuildVisibleText();
        }
    }
    else
    {
        g_typeDoneHold = 0.0f;
    }
}

void Survival_Init(const SurvivalRoomBounds& roomBounds, float monsterPosY, float monsterScale)
{
    g_room = roomBounds;
    g_monsterPosY = monsterPosY;
    g_monsterScale = monsterScale;
    g_inited = true;
    g_enabled = false;
    g_phase = SurvivalPhase::Inactive;
    g_trail.reserve(kTrailMax);
    std::cout << "[Survival] Init monstruoY=" << monsterPosY << " scale=" << monsterScale << "\n";
}

void Survival_Shutdown()
{
    g_inited = false;
    g_enabled = false;
    g_phase = SurvivalPhase::Inactive;
    g_trail.clear();
}

bool Survival_IsEnabled() { return g_enabled; }

void Survival_SetEnabled(bool enabled)
{
    if (!g_inited)
        return;
    if (g_enabled == enabled)
        return;
    g_enabled = enabled;
    std::cout << "[Survival] Check = " << (enabled ? "ON" : "OFF") << "\n";
    if (!enabled)
        Survival_StopCycle();
}

bool Survival_IsCycleActive()
{
    return g_phase != SurvivalPhase::Inactive;
}

SurvivalPhase Survival_GetPhase() { return g_phase; }

void Survival_StartCycle(const glm::vec3& playerPos)
{
    if (!g_inited || !g_enabled)
        return;
    beginPhase(SurvivalPhase::Hunt, playerPos);
}

void Survival_StopCycle()
{
    g_phase = SurvivalPhase::Inactive;
    g_timer = 0.0f;
    g_trail.clear();
    g_jumpscareFlash = 0.0f;
    g_lines.clear();
    g_cineDone = false;
    g_inInteractRange = false;
    g_visibleBuf[0] = '\0';
    g_titleCardT = 0.0f;
    g_titleCardKind = 0;
    AudioBgm_TimerTickStop();
    AudioBgm_TypewriterStop();
    std::cout << "[Survival] Ciclo detenido\n";
}

void Survival_OnEnterPlaying(const glm::vec3& playerPos)
{
    if (g_enabled)
        Survival_StartCycle(playerPos);
    else
        Survival_StopCycle();
}

void Survival_Update(
    float dt,
    const glm::vec3& playerPos,
    const glm::vec3& playerForward,
    SurvivalCorrectMoveFn correctMove,
    void* correctUser,
    bool worldPlaying)
{
    Survival_SetCollisionProbe(correctMove, correctUser);
    if (!g_inited || !g_enabled)
        return;
    if (g_phase == SurvivalPhase::Inactive)
        return;

    if (g_phase == SurvivalPhase::Jumpscare)
    {
        g_jumpscareT -= dt;
        g_jumpscareFlash = std::max(0.0f, g_jumpscareT / kJumpscareDuration);
        g_monsterPos = glm::vec3(playerPos.x, g_monsterPosY, playerPos.z);
        if (g_jumpscareT <= 0.0f)
        {
            beginPhase(SurvivalPhase::EndingLose, playerPos);
            std::cout << "[Survival] Jumpscare -> EndingLose\n";
        }
        return;
    }

    if (g_phase == SurvivalPhase::EndingKill
        || g_phase == SurvivalPhase::EndingEscape
        || g_phase == SurvivalPhase::EndingLose)
    {
        updateCinematic(dt);
        return;
    }

    if (!worldPlaying)
        return;

    // Anuncio de titulo (~1s): congela timer e IA; al terminar arranca tick
    if (g_titleCardT > 0.0f && (g_phase == SurvivalPhase::Chase || g_phase == SurvivalPhase::Rage))
    {
        g_titleCardT -= dt;
        if (g_titleCardT <= 0.0f)
        {
            g_titleCardT = 0.0f;
            if (g_phase == SurvivalPhase::Chase)
                AudioBgm_TimerTickSetMode(1);
            else if (g_phase == SurvivalPhase::Rage)
                AudioBgm_TimerTickSetMode(2);
            g_titleCardKind = 0;
        }
        return;
    }

    // Gracia de Chase: "el monstruo despierta". Timer de fase e IA congelados;
    // el jugador oye/ve el aviso y puede empezar a correr. Rage no entra aquí.
    if (g_phase == SurvivalPhase::Chase && g_chaseGraceT > 0.0f)
    {
        g_chaseGraceT -= dt;
        if (g_chaseGraceT <= 0.0f)
        {
            g_chaseGraceT = 0.0f;
            std::cout << "[Survival] Chase: el monstruo despertó, empieza la persecución\n";
        }
        return;
    }

    g_phaseElapsed += dt;
    g_timer -= dt;
    if (g_timer < 0.0f) g_timer = 0.0f;

    if (g_phase == SurvivalPhase::Hunt)
    {
        updateFlee(dt, playerPos, correctMove, correctUser);
        tryHuntTeleport(dt, playerPos, playerForward);
        g_inInteractRange = (distXZ(g_monsterPos, playerPos) <= kInteractRadius);
        if (g_timer <= 0.0f)
            beginPhase(SurvivalPhase::Chase, playerPos);
        return;
    }

    g_inInteractRange = false;

    if (g_phase == SurvivalPhase::Chase)
    {
        updateChase(dt, playerPos, correctMove, correctUser, kMonsterChaseSpeed);
        updateChaseAntiStuck(dt, playerPos);
        if (distXZ(g_monsterPos, playerPos) <= kCatchRadius)
        {
            beginPhase(SurvivalPhase::Jumpscare, playerPos);
            return;
        }
        if (g_timer <= 0.0f)
            beginPhase(SurvivalPhase::Rage, playerPos);
        return;
    }

    if (g_phase == SurvivalPhase::Rage)
    {
        updateChase(dt, playerPos, correctMove, correctUser, kMonsterRageSpeed);
        updateChaseAntiStuck(dt, playerPos);
        if (distXZ(g_monsterPos, playerPos) <= kCatchRadius)
        {
            beginPhase(SurvivalPhase::Jumpscare, playerPos);
            return;
        }
        if (g_timer <= 0.0f)
            beginPhase(SurvivalPhase::EndingEscape, playerPos);
        return;
    }
}

SurvivalLightWant Survival_DesiredLight()
{
    if (!g_enabled || g_phase == SurvivalPhase::Inactive)
        return SurvivalLightWant::DontCare;
    switch (g_phase)
    {
    case SurvivalPhase::Hunt: return SurvivalLightWant::Normal;
    case SurvivalPhase::Chase: return SurvivalLightWant::Blackout;
    case SurvivalPhase::Rage: return SurvivalLightWant::Admiracion;
    case SurvivalPhase::Jumpscare: return SurvivalLightWant::Blackout;
    case SurvivalPhase::EndingKill:
    case SurvivalPhase::EndingEscape:
    case SurvivalPhase::EndingLose: return SurvivalLightWant::Blackout;
    default: return SurvivalLightWant::DontCare;
    }
}

bool Survival_HasLiveMonster()
{
    if (!g_enabled) return false;
    return g_phase == SurvivalPhase::Hunt
        || g_phase == SurvivalPhase::Chase
        || g_phase == SurvivalPhase::Rage
        || g_phase == SurvivalPhase::Jumpscare;
}

glm::vec3 Survival_MonsterPosition() { return g_monsterPos; }
float Survival_MonsterYawDeg() { return g_monsterYaw; }
float Survival_MonsterScale() { return g_monsterScale; }

bool Survival_WantsKillPrompt()
{
    return g_phase == SurvivalPhase::Hunt && g_inInteractRange;
}

bool Survival_ConsumeKillInteract()
{
    if (g_phase != SurvivalPhase::Hunt || !g_inInteractRange)
        return false;
    beginPhase(SurvivalPhase::EndingKill, g_monsterPos);
    return true;
}

bool Survival_BlocksGameplayInput()
{
    return g_phase == SurvivalPhase::Jumpscare
        || g_phase == SurvivalPhase::EndingKill
        || g_phase == SurvivalPhase::EndingEscape
        || g_phase == SurvivalPhase::EndingLose
        || Survival_TitleCardActive();
}

bool Survival_WantsBlackOverlay()
{
    return Survival_IsCinematic() || g_phase == SurvivalPhase::Jumpscare;
}

float Survival_JumpscareFlash()
{
    return g_phase == SurvivalPhase::Jumpscare ? g_jumpscareFlash : 0.0f;
}

float Survival_JumpscareProgress()
{
    if (g_phase != SurvivalPhase::Jumpscare) return 0.0f;
    return std::clamp(g_jumpscareT / kJumpscareDuration, 0.0f, 1.0f);
}

float Survival_Timer01()
{
    if (g_phase != SurvivalPhase::Hunt && g_phase != SurvivalPhase::Chase && g_phase != SurvivalPhase::Rage)
        return 0.0f;
    if (g_timerMax <= 1e-4f) return 0.0f;
    return std::clamp(g_timer / g_timerMax, 0.0f, 1.0f);
}

float Survival_TimerSeconds()
{
    if (g_phase != SurvivalPhase::Hunt && g_phase != SurvivalPhase::Chase && g_phase != SurvivalPhase::Rage)
        return 0.0f;
    return g_timer;
}

float Survival_TimerHudScale()
{
    if (g_phase != SurvivalPhase::Rage) return 1.0f;
    float t = g_phaseElapsed;
    return 1.18f + 0.014f * t + 0.05f * std::sin(t * 2.4f);
}

const char* Survival_PhaseLabel()
{
    switch (g_phase)
    {
    case SurvivalPhase::Hunt: return "BUSCA";
    case SurvivalPhase::Chase: return "HUYE";
    case SurvivalPhase::Rage: return "SOBREVIVE";
    case SurvivalPhase::Jumpscare: return "JUMPSCARE";
    case SurvivalPhase::EndingKill: return "VICTORIA";
    case SurvivalPhase::EndingEscape: return "ESCAPASTE";
    case SurvivalPhase::EndingLose: return "PERDISTE";
    default: return "";
    }
}

const char* Survival_WindowStatus()
{
    static char buf[160];
    if (!g_enabled)
    {
        std::snprintf(buf, sizeof(buf), "Supervivencia OFF");
        return buf;
    }
    if (g_phase == SurvivalPhase::Hunt)
    {
        char t[16];
        Survival_FormatTimer(g_timer, t, sizeof(t));
        std::snprintf(buf, sizeof(buf), "HUNT %s%s", t, g_inInteractRange ? " | E: matar" : "");
        return buf;
    }
    if (g_phase == SurvivalPhase::Chase)
    {
        char t[16];
        Survival_FormatTimer(g_timer, t, sizeof(t));
        std::snprintf(buf, sizeof(buf), "CHASE %s — HUYE", t);
        return buf;
    }
    if (g_phase == SurvivalPhase::Rage)
    {
        char t[16];
        Survival_FormatTimer(g_timer, t, sizeof(t));
        std::snprintf(buf, sizeof(buf), "RAGE %s — SOBREVIVE", t);
        return buf;
    }
    return Survival_PhaseLabel();
}

const char* Survival_PromptText()
{
    if (Survival_WantsKillPrompt())
        return "Presiona E para matar al monstruo";
    return nullptr;
}

bool Survival_IsCinematic()
{
    return g_phase == SurvivalPhase::EndingKill
        || g_phase == SurvivalPhase::EndingEscape
        || g_phase == SurvivalPhase::EndingLose;
}

const char* Survival_CinematicFullLine()
{
    if (!Survival_IsCinematic()) return nullptr;
    if (g_cineDone) return "Enter / Click para continuar";
    if (g_lines.empty() || g_lineIndex < 0 || g_lineIndex >= (int)g_lines.size())
        return nullptr;
    return g_lines[g_lineIndex];
}

const char* Survival_CinematicVisibleText()
{
    if (!Survival_IsCinematic()) return nullptr;
    return g_visibleBuf[0] ? g_visibleBuf : nullptr;
}

const char* Survival_CinematicBodyText()
{
    if (!Survival_IsCinematic() || g_cineWhiteLeft > 0.0f)
        return nullptr;
    if (g_cineDone)
        return g_fullMessage.empty() ? nullptr : g_fullMessage.c_str();
    return g_visibleBuf[0] ? g_visibleBuf : nullptr;
}

const char* Survival_CinematicFullMessage()
{
    if (!Survival_IsCinematic())
        return nullptr;
    return g_fullMessage.empty() ? nullptr : g_fullMessage.c_str();
}

float Survival_CinematicLineAlpha()
{
    if (!Survival_IsCinematic()) return 0.0f;
    if (g_cineDone) return 1.0f;
    // soft fade in first chars
    return std::clamp(0.35f + g_lineT * 2.0f, 0.0f, 1.0f);
}

float Survival_CinematicGlobalAlpha()
{
    if (!Survival_IsCinematic()) return 0.0f;
    // Blanco/escritura: siempre opaco; sin fade raro
    return 1.0f;
}

float Survival_CinematicGlitch()
{
    // Sin glitch/vibración en finales de texto (pedido del usuario)
    return 0.0f;
}

float Survival_CinematicWhiteLeft()
{
    return g_cineWhiteLeft;
}

bool Survival_CinematicIsWhiteHold()
{
    return Survival_IsCinematic() && g_cineWhiteLeft > 0.0f;
}

float Survival_CinematicScanlinePhase()
{
    return g_scanPhase;
}

int Survival_CinematicStyle() { return g_cineStyle; }

bool Survival_CinematicDone()
{
    return g_cineDone && Survival_IsCinematic();
}

bool Survival_TryAdvanceCinematicConfirm()
{
    if (!Survival_CinematicDone())
        return false;
    Survival_StopCycle();
    return true;
}

bool Survival_ConsumeReturnToMenu()
{
    // Ya no usamos auto menu tras jumpscare; va a EndingLose.
    return false;
}

float Survival_SecondsSinceTeleport() { return g_sinceTeleport; }

bool Survival_HitCheckbox(double mx, double my, unsigned fbW, unsigned fbH)
{
    if (fbW == 0 || fbH == 0) return false;
    float cx, cy, half, labelNy;
    Survival_GetCheckboxLayout(cx, cy, half, labelNy);
    (void)labelNy;
    double nx = mx / (double)fbW;
    double ny = my / (double)fbH;
    return nx > (cx - half - 0.02) && nx < (cx + 0.28)
        && ny > (cy - half - 0.02) && ny < (cy + half + 0.02);
}

void Survival_GetCheckboxLayout(float& outCx, float& outCy, float& outHalf, float& outLabelNy)
{
    // Caja a la izquierda bajo SALIR; hitbox un poco mas grande que el dibujo
    outCx = 0.20f;
    outCy = 0.74f;
    outHalf = 0.022f;
    outLabelNy = 0.74f;
}

int Survival_TitleCardKind()
{
    if (g_titleCardT <= 0.0f)
        return 0;
    return g_titleCardKind;
}

bool Survival_TitleCardActive()
{
    return g_titleCardT > 0.0f && g_titleCardKind != 0;
}

bool Survival_IsChaseGrace()
{
    return g_phase == SurvivalPhase::Chase && g_chaseGraceT > 0.0f;
}

float Survival_ChaseGraceSeconds()
{
    return Survival_IsChaseGrace() ? g_chaseGraceT : 0.0f;
}

float Survival_TitleCardAlpha()
{
    if (!Survival_TitleCardActive())
        return 0.0f;
    // Fade in 0.15s, hold, fade out 0.2s
    const float t = g_titleCardT; // remaining
    const float elapsed = kTitleCardSeconds - t;
    float a = 1.0f;
    if (elapsed < 0.18f)
        a = elapsed / 0.18f;
    if (t < 0.22f)
        a = std::min(a, t / 0.22f);
    return std::clamp(a, 0.0f, 1.0f);
}

// DEBUG PREVIEW (quitar en el futuro)
#if SURVIVAL_DEBUG_PREVIEW
void Survival_DebugForcePhase(SurvivalPhase phase, const glm::vec3& playerPos)
{
    if (!g_inited)
        return;
    // Habilitar el modo aunque el check de UI este OFF, para poder previsualizar.
    g_enabled = true;
    // Ancla el monstruo al jugador para que el jumpscare tenga posicion valida.
    g_monsterPos = glm::vec3(playerPos.x, g_monsterPosY, playerPos.z);
    beginPhase(phase, playerPos);
    std::cout << "[Survival][DEBUG] Fase forzada = " << Survival_PhaseLabel() << "\n";
}
#endif
// FIN DEBUG PREVIEW
