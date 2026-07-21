#pragma once

#include <glm/glm.hpp>

// Modo Supervivencia: Hunt a Chase a Rage + finales cinemáticos.
// Solo activo con check de pausa ON. OFF es comportamiento main.

struct SurvivalRoomBounds
{
    glm::vec3 min{ 0.0f };
    glm::vec3 max{ 0.0f };
};

enum class SurvivalPhase
{
    Inactive,
    Hunt,
    Chase,
    Rage,
    Jumpscare,
    EndingKill,
    EndingEscape,
    EndingLose
};

enum class SurvivalLightWant
{
    DontCare = -1,
    Normal = 0,
    Blackout = 1,
    Admiracion = 2
};

using SurvivalCorrectMoveFn = glm::vec3 (*)(const glm::vec3& pos, const glm::vec3& delta, float radius, void* user);

// Lógica pura (testeable sin OpenGL)
// true si el monstruo está aproximadamente a la vista del jugador (frustum + delante).
bool Survival_IsMonsterVisibleToPlayer(
    const glm::vec3& monsterPos,
    const glm::vec3& playerPos,
    const glm::vec3& playerForwardFlat,
    float fovDeg = 70.0f,
    float maxSeeDist = 55.0f);

// Formato "M:SS" o "SS" en buffer del caller (min 16 chars).
void Survival_FormatTimer(float secondsLeft, char* out, int outCap);

// true en ventanas donde el SFX de máquina teclea (para sync de letras)
bool Survival_IsTypewriterKeyingAt(float audioSeconds);

// true si un punto de spawn parece libre (no embebido en colision):
// usa correctMove en 8 direcciones; testeable con mock.
bool Survival_SpaceLooksClear(
    const glm::vec3& pos,
    SurvivalCorrectMoveFn correctMove,
    void* correctUser,
    float radius = 0.85f);

void Survival_Init(const SurvivalRoomBounds& roomBounds, float monsterPosY, float monsterScale = 1.8f);
void Survival_Shutdown();

bool Survival_IsEnabled();
void Survival_SetEnabled(bool enabled);
bool Survival_IsCycleActive();
SurvivalPhase Survival_GetPhase();

void Survival_StartCycle(const glm::vec3& playerPos);
void Survival_StopCycle();
void Survival_OnEnterPlaying(const glm::vec3& playerPos);

// Probe de colision para spawn/teleport (llamar antes de StartCycle y en Update)
void Survival_SetCollisionProbe(SurvivalCorrectMoveFn fn, void* user);

// playerForward: dirección de mirada XZ (o 3D; se aplana).
void Survival_Update(
    float dt,
    const glm::vec3& playerPos,
    const glm::vec3& playerForward,
    SurvivalCorrectMoveFn correctMove,
    void* correctUser,
    bool worldPlaying);

SurvivalLightWant Survival_DesiredLight();

bool Survival_HasLiveMonster();
glm::vec3 Survival_MonsterPosition();
float Survival_MonsterYawDeg();
float Survival_MonsterScale();

bool Survival_WantsKillPrompt();
bool Survival_ConsumeKillInteract();

bool Survival_BlocksGameplayInput();
bool Survival_WantsBlackOverlay();
float Survival_JumpscareFlash();
float Survival_JumpscareProgress(); // 1 al inicio a 0 al final visual

float Survival_Timer01();
float Survival_TimerSeconds();
float Survival_TimerHudScale();
const char* Survival_PhaseLabel(); // "HUNT" / "CHASE" / "RAGE" / ...
const char* Survival_WindowStatus();
const char* Survival_PromptText();

// Cinemática 2D
bool Survival_IsCinematic();
const char* Survival_CinematicFullLine();
const char* Survival_CinematicVisibleText(); // typewriter parcial o hint
const char* Survival_CinematicBodyText();    // mensaje (parcial o completo, sin hint)
const char* Survival_CinematicFullMessage();  // mensaje COMPLETO siempre (para layout fijo)
float Survival_CinematicLineAlpha();
float Survival_CinematicGlobalAlpha();
float Survival_CinematicGlitch();
float Survival_CinematicScanlinePhase();
int Survival_CinematicStyle(); // 0 kill, 1 escape, 2 lose
bool Survival_CinematicDone();
bool Survival_CinematicIsWhiteHold(); // 1s blanco al inicio del final
float Survival_CinematicWhiteLeft();
bool Survival_TryAdvanceCinematicConfirm();
bool Survival_ConsumeReturnToMenu(); // legacy; prefer transitions via phase

bool Survival_HitCheckbox(double mx, double my, unsigned fbW, unsigned fbH);
void Survival_GetCheckboxLayout(float& outCx, float& outCy, float& outHalf, float& outLabelNy);

// Teleport debug/status (0 es no reciente)
float Survival_SecondsSinceTeleport();

// Gracia de Chase: "el monstruo despierta" (5s). Timer e IA congelados.
bool Survival_IsChaseGrace();       // true durante la gracia de Chase
float Survival_ChaseGraceSeconds(); // segundos restantes de gracia (0 si no aplica)

// Tarjeta de titulo al entrar Chase/Rage (1s)
// kind: 0 none | 1 Luces A2 | 2 Huye B1 | 3 Huye B3
int Survival_TitleCardKind();
float Survival_TitleCardAlpha(); // 0..1 para fade
bool Survival_TitleCardActive();

// DEBUG PREVIEW (quitar en el futuro)
// Bloque aislado para previsualizar jumpscare/finales sin jugar el ciclo entero.
// Para eliminarlo: poner SURVIVAL_DEBUG_PREVIEW en 0 (o borrar los bloques
// marcados con "DEBUG PREVIEW" en game_survival.cpp y main.cpp).
#ifndef SURVIVAL_DEBUG_PREVIEW
#define SURVIVAL_DEBUG_PREVIEW 1
#endif
#if SURVIVAL_DEBUG_PREVIEW
// Fuerza una fase terminal (Jumpscare / EndingKill / EndingEscape / EndingLose)
// desde cualquier punto de Playing. Habilita el modo y arranca lo necesario.
void Survival_DebugForcePhase(SurvivalPhase phase, const glm::vec3& playerPos);
#endif
// FIN DEBUG PREVIEW
