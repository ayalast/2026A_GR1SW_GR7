#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <iostream>
#include <vector>
#include <random>
#include <cfloat>
#include <cmath>
#include <algorithm>
#include <unordered_map>
#include <fstream>
#include <memory>
#include <string>
#include <cstdio>
#include <cstring>
#include <unordered_map>

// Incluimos Assimp SOLO para probar que el vinculador (Linker) no de errores.
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

#include <shader.h>
#include <camera.h>
#include <model.h>
#include <collisions.h>

#include "audio_bgm.h"
#include "game_lights.h"
#include "game_ui.h"
#include "game_survival.h"
#include "game_text.h"

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

// Volumen bajo de la musica de fondo
static const float BGM_DISTANT_VOLUME = 0.045f;

// Callbacks
void framebuffer_size_callback(GLFWwindow* window, int width, int height);
void mouse_callback(GLFWwindow* window, double xpos, double ypos);
void scroll_callback(GLFWwindow* window, double xoffset, double yoffset);
void processInput(GLFWwindow* window);
void mouse_button_callback(GLFWwindow* window, int button, int action, int mods);

// Resolucion (se actualiza con el resize)
unsigned int SCR_WIDTH = 1280;
unsigned int SCR_HEIGHT = 720;

// AppState esta en game_ui.h
AppState appState = AppState::Menu;
static float fadeBlack = 1.0f;
static const float FADE_IN_SECONDS = 0.65f;
static bool uiClickLatch = false;

// Radios de dibujo / luces / sombras
static const float STREAM_DRAW_RADIUS   = 500.0f;
static const float STREAM_SHADOW_RADIUS = 80.0f;
static const float STREAM_LIGHT_RADIUS  = 100.0f;
static const int   STREAM_MAX_POINT_LIGHTS = 48;

// true es carga rapida solo mapa+luces (debug); false es demo completa
static const bool LIGHTING_FAST_LOAD = false;

// Camara
Camera camera(glm::vec3(0.0f, 0.0f, 3.0f));
float lastX = 640.0f;
float lastY = 360.0f;
bool firstMouse = true;

// Timing
float deltaTime = 0.0f;
float lastFrame = 0.0f;

// Posicion del nivel
glm::vec3 backroomsPos(0.0f, -3.0f, 0.0f);

// Colisiones
CollisionManager colManager;
// AABBs de monstruos estaticos (se quitan en survival para no dejar cajas invisibles)
static std::vector<AABB> g_demonStaticBoxes;
static size_t g_demonColStart = 0;
static size_t g_demonColCount = 0;
static bool g_demonColsActive = true;
// Bounds locales del monstruo (jumpscare: anclar cara a camara)
static AABB g_demonBoundsLocal{};

static glm::vec3 survivalCorrectMove(const glm::vec3& pos, const glm::vec3& delta, float radius, void* user)
{
    auto* cm = static_cast<CollisionManager*>(user);
    return cm->correctMovement(pos, delta, radius);
}

static void survivalRemoveDemonCollisions()
{
    if (!g_demonColsActive || g_demonColCount == 0)
        return;
    if (g_demonColStart + g_demonColCount <= colManager.staticBoxCount())
        colManager.eraseStaticBoxes(g_demonColStart, g_demonColCount);
    g_demonColsActive = false;
    std::cout << "[Survival] AABBs monstruo estatico desactivados (" << g_demonColCount << ")\n";
}

static void survivalRestoreDemonCollisions()
{
    if (g_demonColsActive || g_demonStaticBoxes.empty())
        return;
    g_demonColStart = colManager.staticBoxCount();
    for (const AABB& b : g_demonStaticBoxes)
        colManager.addStaticBox(b);
    g_demonColCount = g_demonStaticBoxes.size();
    g_demonColsActive = true;
    std::cout << "[Survival] AABBs monstruo estatico restaurados (" << g_demonColCount << ")\n";
}

bool flashlightOn = false;

// Balanceo de camara al caminar
static float headBobTimer = 0.0f;
static float headBobAmount = 0.0f;
static glm::vec3 headBobOffset(0.0f);
static bool playerIsWalking = false;

// UI hitboxes: game_ui.cpp
// Modos de iluminacion (menu de pausa)
enum class LightPresentation
{
    Normal,
    Blackout,
    Admiracion
};
static LightPresentation lightPresentation = LightPresentation::Normal;
static bool lightsBlackoutMode = false;

// Apagones aleatorios dentro del modo admiracion
static float admBlackoutTimer = 0.0f;
static float admBlackoutNext  = 16.0f;
static float g_admFlicker     = 1.0f;

// Arrastre de sliders de volumen (0 es ninguno)
static int g_volDrag = 0;

static float distXZ(const glm::vec3& a, const glm::vec3& b)
{
    float dx = a.x - b.x;
    float dz = a.z - b.z;
    return std::sqrt(dx * dx + dz * dz);
}

static bool nearAnyXZ(const glm::vec3& cam, const std::vector<glm::vec3>& anchors, float radius)
{
    for (const glm::vec3& a : anchors)
    {
        if (distXZ(cam, a) <= radius)
            return true;
    }
    return false;
}

// structs base (instancias, anchors y demas)

// Estructura base para instancias
struct Instance
{
    glm::vec3 position;
    glm::vec3 rotationDeg;
    float scale;
};

// Estructura para cámaras de vigilancia
struct CameraInstance
{
    glm::vec3 position;
    float yawDeg;
    float pitchDeg;
};

// Estructura para props montados en pared
struct WallMountedInstance
{
    glm::vec3 position;
    float yawDeg;
    glm::vec3 scale;
};

// Estructura para anchors de pared
struct WallAnchor
{
    glm::vec3 min;
    glm::vec3 max;
    bool thinX;
};

// Configuración para colocación de cámaras
struct CameraPlacementConfig
{
    int targetCount = 200;
    int maxAttempts = 5000;
    float minCamDistance = 6.0f;
    float nearTopMin = 0.80f;
    float nearTopMax = 1.2f;
    float wallClearance = 0.02f;
    float pitchDownMin = -28.0f;
    float pitchDownMax = -12.0f;
    float yawJitterMin = -10.0f;
    float yawJitterMax = 10.0f;
};

// utilidades varias

float randomRange(std::mt19937& rng, float minValue, float maxValue)
{
    std::uniform_real_distribution<float> dist(minValue, maxValue);
    return dist(rng);
}

float distanceXZ(const glm::vec3& a, const glm::vec3& b)
{
    const float dx = a.x - b.x;
    const float dz = a.z - b.z;
    return std::sqrt(dx * dx + dz * dz);
}

float computeFootprintRadiusXZ(const AABB& localBounds, float scale)
{
    const float maxX = std::max(std::abs(localBounds.min.x), std::abs(localBounds.max.x));
    const float maxZ = std::max(std::abs(localBounds.min.z), std::abs(localBounds.max.z));
    return std::max(maxX, maxZ) * scale;
}

// calculo de bounds

AABB computeMeshBounds(const Mesh& mesh)
{
    AABB bounds;
    bounds.min = glm::vec3(FLT_MAX, FLT_MAX, FLT_MAX);
    bounds.max = glm::vec3(-FLT_MAX, -FLT_MAX, -FLT_MAX);

    for (const Vertex& vertex : mesh.vertices)
    {
        bounds.min = glm::min(bounds.min, vertex.Position);
        bounds.max = glm::max(bounds.max, vertex.Position);
    }

    return bounds;
}

AABB computeModelBounds(const Model& model, bool firstMeshOnly = false)
{
    if (model.meshes.empty())
        return { glm::vec3(0.0f), glm::vec3(0.0f) };

    AABB bounds;
    bounds.min = glm::vec3(FLT_MAX, FLT_MAX, FLT_MAX);
    bounds.max = glm::vec3(-FLT_MAX, -FLT_MAX, -FLT_MAX);

    const size_t meshCount = firstMeshOnly ? 1u : model.meshes.size();
    for (size_t i = 0; i < meshCount; ++i)
    {
        const AABB meshBounds = computeMeshBounds(model.meshes[i]);
        bounds.min = glm::min(bounds.min, meshBounds.min);
        bounds.max = glm::max(bounds.max, meshBounds.max);
    }

    return bounds;
}

// matrices y transformaciones

glm::mat4 buildInstanceMatrix(const Instance& instance)
{
    glm::mat4 model = glm::mat4(1.0f);
    model = glm::translate(model, instance.position);
    model = glm::rotate(model, glm::radians(instance.rotationDeg.y), glm::vec3(0.0f, 1.0f, 0.0f));
    model = glm::rotate(model, glm::radians(instance.rotationDeg.x), glm::vec3(1.0f, 0.0f, 0.0f));
    model = glm::rotate(model, glm::radians(instance.rotationDeg.z), glm::vec3(0.0f, 0.0f, 1.0f));
    model = glm::scale(model, glm::vec3(instance.scale));
    return model;
}

AABB transformBounds(const AABB& localBounds, const glm::mat4& transform)
{
    const glm::vec3 corners[8] = {
        glm::vec3(localBounds.min.x, localBounds.min.y, localBounds.min.z),
        glm::vec3(localBounds.max.x, localBounds.min.y, localBounds.min.z),
        glm::vec3(localBounds.min.x, localBounds.max.y, localBounds.min.z),
        glm::vec3(localBounds.max.x, localBounds.max.y, localBounds.min.z),
        glm::vec3(localBounds.min.x, localBounds.min.y, localBounds.max.z),
        glm::vec3(localBounds.max.x, localBounds.min.y, localBounds.max.z),
        glm::vec3(localBounds.min.x, localBounds.max.y, localBounds.max.z),
        glm::vec3(localBounds.max.x, localBounds.max.y, localBounds.max.z)
    };

    AABB outBounds;
    outBounds.min = glm::vec3(FLT_MAX, FLT_MAX, FLT_MAX);
    outBounds.max = glm::vec3(-FLT_MAX, -FLT_MAX, -FLT_MAX);

    for (const glm::vec3& corner : corners)
    {
        const glm::vec3 p = glm::vec3(transform * glm::vec4(corner, 1.0f));
        outBounds.min = glm::min(outBounds.min, p);
        outBounds.max = glm::max(outBounds.max, p);
    }

    return outBounds;
}

AABB buildScaledBounds(const AABB& localBounds, const glm::vec3& worldPosition, const glm::vec3& scale)
{
    AABB worldBounds;
    worldBounds.min = worldPosition + localBounds.min * scale;
    worldBounds.max = worldPosition + localBounds.max * scale;
    return worldBounds;
}

// aqui se colocan y generan las instancias

glm::vec3 keepInstanceInsideRoomXZ(
    const glm::vec3& desiredPosition,
    const AABB& roomBounds,
    const AABB& localBounds,
    const glm::vec3& scale,
    float wallClearance)
{
    glm::vec3 corrected = desiredPosition;
    const AABB instanceBounds = buildScaledBounds(localBounds, desiredPosition, scale);

    const float minAllowedX = roomBounds.min.x + wallClearance;
    const float maxAllowedX = roomBounds.max.x - wallClearance;
    const float minAllowedZ = roomBounds.min.z + wallClearance;
    const float maxAllowedZ = roomBounds.max.z - wallClearance;

    if (instanceBounds.min.x < minAllowedX)
        corrected.x += (minAllowedX - instanceBounds.min.x);
    else if (instanceBounds.max.x > maxAllowedX)
        corrected.x -= (instanceBounds.max.x - maxAllowedX);

    if (instanceBounds.min.z < minAllowedZ)
        corrected.z += (minAllowedZ - instanceBounds.min.z);
    else if (instanceBounds.max.z > maxAllowedZ)
        corrected.z -= (instanceBounds.max.z - maxAllowedZ);

    return corrected;
}

std::vector<WallAnchor> collectWallAnchors(const Model& collisionsModel, const glm::vec3& worldOffset)
{
    std::vector<WallAnchor> wallAnchors;
    wallAnchors.reserve(collisionsModel.meshes.size());

    for (const Mesh& mesh : collisionsModel.meshes)
    {
        AABB meshBounds = computeMeshBounds(mesh);
        meshBounds.min += worldOffset;
        meshBounds.max += worldOffset;

        const glm::vec3 size = meshBounds.max - meshBounds.min;
        const bool tallEnough = size.y > 2.0f;
        const bool thinX = size.x < 1.2f && size.z > 1.5f;
        const bool thinZ = size.z < 1.2f && size.x > 1.5f;

        if (tallEnough && (thinX || thinZ))
            wallAnchors.push_back({ meshBounds.min, meshBounds.max, thinX });
    }

    return wallAnchors;
}

// camaras de seguridad

std::vector<CameraInstance> generateCameraInstances(
    const std::vector<WallAnchor>& wallAnchors,
    const glm::vec3& roomCenter,
    float wallAttachOffset,
    const CameraPlacementConfig& cfg)
{
    std::vector<CameraInstance> instances;
    instances.reserve(cfg.targetCount);

    if (wallAnchors.empty())
        return instances;

    std::random_device rd;
    std::mt19937 rng(rd());
    std::uniform_real_distribution<float> nearTop(cfg.nearTopMin, cfg.nearTopMax);
    std::uniform_real_distribution<float> pitchDown(cfg.pitchDownMin, cfg.pitchDownMax);
    std::uniform_real_distribution<float> yawJitter(cfg.yawJitterMin, cfg.yawJitterMax);
    std::uniform_int_distribution<size_t> anchorPicker(0, wallAnchors.size() - 1);

    for (int attempt = 0; attempt < cfg.maxAttempts && (int)instances.size() < cfg.targetCount; attempt++)
    {
        const WallAnchor& anchor = wallAnchors[anchorPicker(rng)];
        CameraInstance instance{};
        float y = anchor.max.y - nearTop(rng);

        if (anchor.thinX)
        {
            const float zMin = anchor.min.z + 0.2f;
            const float zMax = std::max(zMin, anchor.max.z - 0.2f);
            std::uniform_real_distribution<float> along(zMin, zMax);
            const float anchorCenterX = (anchor.min.x + anchor.max.x) * 0.5f;
            const bool plusXFace = roomCenter.x >= anchorCenterX;
            const float wallX = plusXFace ? anchor.max.x : anchor.min.x;

            instance.position = glm::vec3(
                plusXFace ? (wallX + wallAttachOffset) : (wallX - wallAttachOffset),
                y,
                along(rng)
            );

            float baseYaw = plusXFace ? 90.0f : -90.0f;
            instance.yawDeg = baseYaw + yawJitter(rng);
        }
        else
        {
            const float xMin = anchor.min.x + 0.2f;
            const float xMax = std::max(xMin, anchor.max.x - 0.2f);
            std::uniform_real_distribution<float> along(xMin, xMax);
            const float anchorCenterZ = (anchor.min.z + anchor.max.z) * 0.5f;
            const bool plusZFace = roomCenter.z >= anchorCenterZ;
            const float wallZ = plusZFace ? anchor.max.z : anchor.min.z;

            instance.position = glm::vec3(
                along(rng),
                y,
                plusZFace ? (wallZ + wallAttachOffset) : (wallZ - wallAttachOffset)
            );

            float baseYaw = plusZFace ? 0.0f : 180.0f;
            instance.yawDeg = baseYaw + yawJitter(rng);
        }

        instance.pitchDeg = pitchDown(rng);

        bool tooClose = false;
        for (const CameraInstance& placed : instances)
        {
            if (glm::distance(placed.position, instance.position) < cfg.minCamDistance)
            {
                tooClose = true;
                break;
            }
        }

        if (!tooClose)
            instances.push_back(instance);
    }

    return instances;
}

// instancias sobre el piso

std::vector<Instance> createFloorInstances(
    const std::vector<glm::vec3>& anchorsXZ,
    const AABB& roomBounds,
    const AABB& localBounds,
    float floorY,
    float scale,
    const std::vector<float>& yaws,
    float wallClearance)
{
    std::vector<Instance> out;
    out.reserve(anchorsXZ.size());

    const float footprintRadius = computeFootprintRadiusXZ(localBounds, scale);
    const float minX = roomBounds.min.x + footprintRadius + wallClearance;
    const float maxX = roomBounds.max.x - footprintRadius - wallClearance;
    const float minZ = roomBounds.min.z + footprintRadius + wallClearance;
    const float maxZ = roomBounds.max.z - footprintRadius - wallClearance;

    if (minX >= maxX || minZ >= maxZ)
        return out;

    for (size_t i = 0; i < anchorsXZ.size(); ++i)
    {
        Instance instance{};
        instance.scale = scale;
        instance.rotationDeg = glm::vec3(0.0f, yaws.empty() ? 0.0f : yaws[i % yaws.size()], 0.0f);

        instance.position.x = glm::clamp(anchorsXZ[i].x, minX, maxX);
        instance.position.z = glm::clamp(anchorsXZ[i].z, minZ, maxZ);
        instance.position.y = floorY - (localBounds.min.y * scale);

        out.push_back(instance);
    }

    return out;
}

// cabinas telefonicas

std::vector<Instance> generatePhoneBooths(const AABB& roomBounds, const AABB& boothBounds, float floorY, int targetCount)
{
    std::vector<Instance> out;
    out.reserve(targetCount);

    std::random_device rd;
    std::mt19937 rng(rd());

    const int maxAttempts = 30000;
    // El modelo del telefono es muy grande; se escala para que quepa bajo el techo.
    const float boothHeight = std::max(0.001f, boothBounds.max.y - boothBounds.min.y);
    const float scaleMin = 2.4f / boothHeight;
    const float scaleMax = 3.0f / boothHeight;
    const float minDistance = 3.5f;
    const float wallClearance = 0.25f;

    for (int attempt = 0; attempt < maxAttempts && (int)out.size() < targetCount; ++attempt)
    {
        Instance instance{};
        instance.scale = randomRange(rng, scaleMin, scaleMax);
        instance.rotationDeg = glm::vec3(0.0f, randomRange(rng, 0.0f, 360.0f), 0.0f);

        const float footprintRadius = computeFootprintRadiusXZ(boothBounds, instance.scale);
        const float minX = roomBounds.min.x + footprintRadius + wallClearance;
        const float maxX = roomBounds.max.x - footprintRadius - wallClearance;
        const float minZ = roomBounds.min.z + footprintRadius + wallClearance;
        const float maxZ = roomBounds.max.z - footprintRadius - wallClearance;

        if (minX >= maxX || minZ >= maxZ)
            break;

        instance.position.x = randomRange(rng, minX, maxX);
        instance.position.z = randomRange(rng, minZ, maxZ);
        instance.position.y = floorY - (boothBounds.min.y * instance.scale);

        bool tooClose = false;
        for (const Instance& other : out)
        {
            if (distanceXZ(other.position, instance.position) < minDistance)
            {
                tooClose = true;
                break;
            }
        }

        if (!tooClose)
            out.push_back(instance);
    }

    return out;
}

// dibujado

void drawPlanarShadows(Shader& shadowShader, Model& model, const std::vector<Instance>& instances, float floorY, glm::vec3 lightDir, bool firstMeshOnly = false, float maxDist = 1e9f)
{
    // Crear la matriz de proyección de sombra plana para GLM (Column Major)
    glm::mat4 shadowMat(1.0f);

    // Evitar la división por cero si la luz apunta completamente horizontal
    if (std::abs(lightDir.y) < 0.001f) lightDir.y = -0.001f;

    // Columna 1 de la matriz: Proyecta la altura (Y) hacia los lados dependiendo de la inclinación de la luz
    shadowMat[1][0] = -lightDir.x / lightDir.y;
    shadowMat[1][1] = 0.0f;
    shadowMat[1][2] = -lightDir.z / lightDir.y;

    // Columna 3 de la matriz: Traslada la sombra proyectada al nivel exacto del suelo
    shadowMat[3][0] = floorY * (lightDir.x / lightDir.y);
    shadowMat[3][1] = floorY + 0.002f; // Offset milimétrico para evitar Z Fighting (parpadeo de texturas)
    shadowMat[3][2] = floorY * (lightDir.z / lightDir.y);

    for (const Instance& instance : instances)
    {
        if (distXZ(camera.Position, instance.position) > maxDist)
            continue;

        // Matriz de transformación original de la instancia de objeto
        glm::mat4 modelMat = buildInstanceMatrix(instance);

        // La matriz final combina la proyección de sombra con la posición del objeto
        // El fade por linterna es por fragmento en basico.fs (FragPos en el suelo)
        shadowShader.setMat4("model", shadowMat * modelMat);

        if (firstMeshOnly)
        {
            if (!model.meshes.empty())
                model.meshes[0].Draw(shadowShader);
        }
        else
        {
            model.Draw(shadowShader);
        }
    }
}

void drawInstances(Shader& shader, Model& model, const std::vector<Instance>& instances, bool firstMeshOnly = false, float maxDist = 1e9f)
{
    for (const Instance& instance : instances)
    {
        if (distXZ(camera.Position, instance.position) > maxDist)
            continue;

        shader.setMat4("model", buildInstanceMatrix(instance));

        if (firstMeshOnly)
        {
            if (!model.meshes.empty())
                model.meshes[0].Draw(shader);
        }
        else
        {
            
            model.Draw(shader);
        }
    }
}

void drawCameraInstances(
    Shader& shader,
    Model& cameraModel,
    const std::vector<CameraInstance>& instances,
    const glm::vec3& cameraScale,
    float maxDist = 1e9f)
{
    for (const CameraInstance& instance : instances)
    {
        if (distXZ(camera.Position, instance.position) > maxDist)
            continue;

        glm::mat4 model = glm::mat4(1.0f);
        model = glm::translate(model, instance.position);
        model = glm::rotate(model, glm::radians(instance.yawDeg), glm::vec3(0.0f, 1.0f, 0.0f));
        model = glm::rotate(model, glm::radians(instance.pitchDeg), glm::vec3(1.0f, 0.0f, 0.0f));
        model = glm::scale(model, cameraScale);
        shader.setMat4("model", model);
        cameraModel.Draw(shader);
    }
}

// colisiones

// params: usar solo la primera malla, cuanto inflar en XZ y el techo minimo del AABB.
// el minTopY es clave: si el objeto queda bajo el ojo (Y 0) la colision no frena y se atraviesa.
void addInstancesCollision(
    CollisionManager& manager,
    const Model& model,
    const std::vector<Instance>& instances,
    bool firstMeshOnly = false,
    float xzPad = 0.0f,
    float minTopY = -1.0e9f)
{
    const AABB localBounds = computeModelBounds(model, firstMeshOnly);

    for (const Instance& instance : instances)
    {
        AABB worldBounds = transformBounds(localBounds, buildInstanceMatrix(instance));
        if (xzPad > 0.0f)
        {
            worldBounds.min.x -= xzPad;
            worldBounds.max.x += xzPad;
            worldBounds.min.z -= xzPad;
            worldBounds.max.z += xzPad;
        }
        if (worldBounds.max.y < minTopY)
            worldBounds.max.y = minTopY;
        manager.addStaticBox(worldBounds);
    }
}

void addFurnitureCollisions(
    CollisionManager& manager,
    const AABB& localBounds,
    const std::vector<CameraInstance>& instances,
    const glm::vec3& uniformScale)
{
    for (const CameraInstance& instance : instances)
        manager.addStaticBox(buildScaledBounds(localBounds, instance.position, uniformScale));
}

static std::vector<CeilingLight>* g_ceilingLightsPtr = nullptr;

static void applyLightPresentation()
{
    lightsBlackoutMode = (lightPresentation == LightPresentation::Blackout);
    if (g_ceilingLightsPtr)
        applyCeilingLightMode(*g_ceilingLightsPtr, lightsBlackoutMode);
    AudioBgm_SetBlackoutMute(lightsBlackoutMode);
    const bool adm = (lightPresentation == LightPresentation::Admiracion);
    AudioBgm_AdmiracionAlarmSetActive(adm);
    AudioBgm_AmbientBuzzSetActive(lightPresentation == LightPresentation::Normal);
    if (!adm)
    {
        admBlackoutTimer = 0.0f;
        admBlackoutNext  = 4.0f;
        g_admFlicker     = 1.0f;
    }
    else
    {
        admBlackoutTimer = 0.0f;
        admBlackoutNext  = 3.0f + static_cast<float>(std::rand() % 3001) / 1000.0f;
    }

    const char* name = "NORMAL";
    if (lightPresentation == LightPresentation::Blackout) name = "BLACKOUT";
    else if (lightPresentation == LightPresentation::Admiracion) name = "ADMIRACION";
    std::cout << "[UI] Presentacion luces -> " << name << "\n";
}

static void toggleLightsBlackoutMode()
{
    if (Survival_IsEnabled() && Survival_IsCycleActive())
    {
        std::cout << "[Survival] Luces controladas por el ciclo (L ignorado)\n";
        return;
    }
    if (lightPresentation == LightPresentation::Blackout)
        lightPresentation = LightPresentation::Normal;
    else
        lightPresentation = LightPresentation::Blackout;
    applyLightPresentation();
}

static void toggleAdmiracionMode()
{
    if (Survival_IsEnabled() && Survival_IsCycleActive())
    {
        std::cout << "[Survival] Luces controladas por el ciclo (M ignorado)\n";
        return;
    }
    if (lightPresentation == LightPresentation::Admiracion)
        lightPresentation = LightPresentation::Normal;
    else
        lightPresentation = LightPresentation::Admiracion; // sale de blackout al entrar
    applyLightPresentation();
    std::cout << "[UI] MODO ADMIRACION "
              << (lightPresentation == LightPresentation::Admiracion ? "ON (rojo + alarma + monstruo)" : "OFF")
              << "\n";
}

static void survivalSyncLights()
{
    SurvivalLightWant w = Survival_DesiredLight();
    if (w == SurvivalLightWant::DontCare)
        return;
    LightPresentation want = LightPresentation::Normal;
    if (w == SurvivalLightWant::Blackout) want = LightPresentation::Blackout;
    else if (w == SurvivalLightWant::Admiracion) want = LightPresentation::Admiracion;
    if (lightPresentation != want)
    {
        lightPresentation = want;
        applyLightPresentation();
    }
}

static void survivalToggleFromUi(const glm::vec3& playerPos)
{
    const bool next = !Survival_IsEnabled();
    Survival_SetEnabled(next);
    Survival_SetCollisionProbe(survivalCorrectMove, &colManager);
    if (next)
    {
        survivalRemoveDemonCollisions();
        // El check se activa casi siempre desde PAUSA: hay que arrancar Hunt aqui.
        // En pausa el timer no descuenta (worldPlaying false); al CONTINUAR sigue desde 60s.
        if (appState == AppState::Playing || appState == AppState::FadeIn
            || appState == AppState::Paused)
        {
            Survival_StartCycle(playerPos);
            survivalSyncLights();
            std::cout << "[Survival] ON + ciclo Hunt iniciado (timer pausado hasta CONTINUAR)\n";
        }
        else
        {
            std::cout << "[Survival] ON — el ciclo Hunt arrancara al COMENZAR\n";
        }
    }
    else
    {
        Survival_StopCycle();
        survivalRestoreDemonCollisions();
        lightPresentation = LightPresentation::Normal;
        applyLightPresentation();
        std::cout << "[Survival] OFF — estado main (sin timers/IA)\n";
    }
}

// Luces de techo: ver game_lights.cpp (findCeilingLights / applyCeilingLightMode)

// devuelve el centro local de la lampara
glm::vec3 findLocalLampCenter(const Model& model)
{
    glm::vec3 sum(0.0f);
    int count = 0;
    for (const Mesh& mesh : model.meshes)
    {
        bool isLampMesh = false;
        for (const Texture& tex : mesh.textures)
        {
            if (tex.path.find("oillamp") != std::string::npos || tex.path.find("lantern") != std::string::npos)
            {
                isLampMesh = true;
                break;
            }
        }
        if (!isLampMesh)
            continue;
        for (const Vertex& v : mesh.vertices)
        {
            sum += v.Position;
            count++;
        }
    }
    if (count > 0)
    {
        return sum / (float)count;
    }
    return glm::vec3(0.0f);
}

// main: carga, splash y loop

int main() {
    // 1. Inicializar GLFW
    glfwInit();
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_STENCIL_BITS, 8);

    // 2. Ventana maximizada (NO fullscreen exclusivo: no "roba" el monitor)
    //    Se ve al maximo, pero sigue siendo una ventana normal (Alt+Tab, barra de tareas, etc.).
    GLFWmonitor* monitor = glfwGetPrimaryMonitor();
    int winW = 1280, winH = 720, winX = 100, winY = 100;
    if (monitor)
    {
        int mx = 0, my = 0, mw = 0, mh = 0;
        // Area util del monitor (sin barra de tareas, si el SO lo reporta)
        glfwGetMonitorWorkarea(monitor, &mx, &my, &mw, &mh);
        if (mw > 0 && mh > 0)
        {
            winW = mw;
            winH = mh;
            winX = mx;
            winY = my;
        }
        else
        {
            const GLFWvidmode* mode = glfwGetVideoMode(monitor);
            if (mode)
            {
                winW = mode->width;
                winH = mode->height;
            }
        }
    }

    SCR_WIDTH = static_cast<unsigned int>(winW);
    SCR_HEIGHT = static_cast<unsigned int>(winH);

    // monitor es NULL a modo ventana (no exclusive fullscreen)
    GLFWwindow* window = glfwCreateWindow(winW, winH, "Backrooms - Grupo 7", NULL, NULL);
    if (window == NULL) {
        std::cout << "Error al crear la ventana GLFW. Revisa la DLL." << std::endl;
        glfwTerminate();
        return -1;
    }
    glfwSetWindowPos(window, winX, winY);
    glfwMaximizeWindow(window); // maximizada al maximo, sin bloquear el escritorio

    glfwMakeContextCurrent(window);
    glfwSetFramebufferSizeCallback(window, framebuffer_size_callback);
    glfwSetCursorPosCallback(window, mouse_callback);
    glfwSetScrollCallback(window, scroll_callback);
    glfwSetMouseButtonCallback(window, mouse_button_callback);

    int fbW = 0, fbH = 0;
    glfwGetFramebufferSize(window, &fbW, &fbH);
    if (fbW > 0 && fbH > 0)
    {
        SCR_WIDTH = static_cast<unsigned int>(fbW);
        SCR_HEIGHT = static_cast<unsigned int>(fbH);
    }
    lastX = SCR_WIDTH / 2.0f;
    lastY = SCR_HEIGHT / 2.0f;
    std::cout << "[Video] Ventana maximizada " << SCR_WIDTH << "x" << SCR_HEIGHT
              << " (no fullscreen exclusivo)\n";

    // Cursor libre mientras carga (se captura al empezar a jugar)
    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);

    // 3. Inicializar GLAD (rapido: ya se ve la ventana)
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        std::cout << "Error al inicializar GLAD." << std::endl;
        return -1;
    }

    glEnable(GL_DEPTH_TEST);
    glClearColor(0.04f, 0.04f, 0.06f, 1.0f);

    // Splash 2D (pantalla de carga real, no solo titulo de ventana)
    std::unique_ptr<Shader> splashShader = std::make_unique<Shader>("shaders/splash.vs", "shaders/splash.fs");
    unsigned int splashVAO = 0, splashVBO = 0, splashTex = 0;
    {
        // Fullscreen quad NDC: pos.xy + uv
        float quad[] = {
            // pos      // uv
            -1.0f, -1.0f,  0.0f, 0.0f,
             1.0f, -1.0f,  1.0f, 0.0f,
             1.0f,  1.0f,  1.0f, 1.0f,
            -1.0f, -1.0f,  0.0f, 0.0f,
             1.0f,  1.0f,  1.0f, 1.0f,
            -1.0f,  1.0f,  0.0f, 1.0f,
        };
        glGenVertexArrays(1, &splashVAO);
        glGenBuffers(1, &splashVBO);
        glBindVertexArray(splashVAO);
        glBindBuffer(GL_ARRAY_BUFFER, splashVBO);
        glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
        glBindVertexArray(0);

        glGenTextures(1, &splashTex);
        glBindTexture(GL_TEXTURE_2D, splashTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        int tw = 0, th = 0, tn = 0;
        stbi_set_flip_vertically_on_load(true);
        unsigned char* pixels = stbi_load("textures/splash.png", &tw, &th, &tn, 0);
        if (pixels)
        {
            GLenum fmt = (tn == 4) ? GL_RGBA : GL_RGB;
            glTexImage2D(GL_TEXTURE_2D, 0, fmt, tw, th, 0, fmt, GL_UNSIGNED_BYTE, pixels);
            stbi_image_free(pixels);
            std::cout << "[Load] Splash texture OK (" << tw << "x" << th << ")\n";
        }
        else
        {
            // Fallback 1x1 oscuro
            unsigned char fallback[] = { 12, 12, 18, 255 };
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, fallback);
            std::cout << "[Load] No se encontro textures/splash.png, usando color solido.\n";
        }
        glBindTexture(GL_TEXTURE_2D, 0);
        // No dejar flip true: modelos usan Assimp FlipUVs + stbi flip false
        stbi_set_flip_vertically_on_load(false);
    }

    auto loadUiTexture = [&](const char* path) -> unsigned int {
        unsigned int tex = 0;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        int tw = 0, th = 0, tn = 0;
        stbi_set_flip_vertically_on_load(true);
        unsigned char* pixels = stbi_load(path, &tw, &th, &tn, 0);
        if (pixels)
        {
            GLenum fmt = (tn == 4) ? GL_RGBA : GL_RGB;
            glTexImage2D(GL_TEXTURE_2D, 0, fmt, tw, th, 0, fmt, GL_UNSIGNED_BYTE, pixels);
            stbi_image_free(pixels);
            std::cout << "[UI] Texture " << path << " OK\n";
        }
        else
        {
            unsigned char fallback[] = { 12, 12, 18, 255 };
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, fallback);
            std::cout << "[UI] Falta " << path << "\n";
        }
        glBindTexture(GL_TEXTURE_2D, 0);
        stbi_set_flip_vertically_on_load(false);
        return tex;
    };

    unsigned int menuStartTex = 0;
    unsigned int menuPauseTex = 0;          // boton "APAGAR LUCES" (modo lit actual)
    unsigned int menuPauseLightsOffTex = 0; // boton "ENCENDER LUCES" (modo blackout actual)
    unsigned int digitsTex = loadUiTexture("textures/digits.png");
    unsigned int barBgTex = 0;
    unsigned int barFillTex = 0;
    unsigned int barGlowTex = 0;
    unsigned int barKnobTex = 0;
    unsigned int barKnobEdgeTex = 0;
    {
        // texturas 1x1 para barra de progreso / sliders pausa
        auto solidTex = [](unsigned char r, unsigned char g, unsigned char b, unsigned char a = 255) {
            unsigned int t = 0;
            glGenTextures(1, &t);
            glBindTexture(GL_TEXTURE_2D, t);
            unsigned char px[] = { r, g, b, a };
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glBindTexture(GL_TEXTURE_2D, 0);
            return t;
        };
        barBgTex = solidTex(40, 44, 52);
        barFillTex = solidTex(235, 205, 105);
        barGlowTex = solidTex(255, 230, 140, 255);
        barKnobTex = solidTex(250, 230, 150);
        barKnobEdgeTex = solidTex(170, 140, 60);
    }

    float loadProgress = 0.0f; // 0..1 realista por etapas ponderadas

    auto drawFullscreenTex = [&](unsigned int tex, float dim = 0.0f, float alpha = 1.0f, bool clearFirst = true) {
        if (clearFirst)
        {
            glDisable(GL_DEPTH_TEST);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        }
        else
        {
            glDisable(GL_DEPTH_TEST);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        }
        splashShader->use();
        splashShader->setFloat("uDim", dim);
        splashShader->setFloat("uAlpha", alpha);
        splashShader->setInt("uTex", 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex);
        glBindVertexArray(splashVAO);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glBindVertexArray(0);
        glDisable(GL_BLEND);
        glEnable(GL_DEPTH_TEST);
    };

    // Dibuja un quad NDC con UVs [0,1]
    auto drawNdcTexturedQuad = [&](float x0, float y0, float x1, float y1,
                                   unsigned int tex, float u0, float v0, float u1, float v1, float alpha = 1.0f) {
        float q[] = {
            x0, y0, u0, v0,
            x1, y0, u1, v0,
            x1, y1, u1, v1,
            x0, y0, u0, v0,
            x1, y1, u1, v1,
            x0, y1, u0, v1,
        };
        glBindBuffer(GL_ARRAY_BUFFER, splashVBO);
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(q), q);
        splashShader->use();
        splashShader->setFloat("uDim", 0.0f);
        splashShader->setFloat("uAlpha", alpha);
        splashShader->setInt("uTex", 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex);
        glDisable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glBindVertexArray(splashVAO);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glBindVertexArray(0);
        glDisable(GL_BLEND);
        glEnable(GL_DEPTH_TEST);
        // restaurar quad fullscreen en VBO para el splash
        float full[] = {
            -1.f, -1.f, 0.f, 0.f,
             1.f, -1.f, 1.f, 0.f,
             1.f,  1.f, 1.f, 1.f,
            -1.f, -1.f, 0.f, 0.f,
             1.f,  1.f, 1.f, 1.f,
            -1.f,  1.f, 0.f, 1.f,
        };
        glBindBuffer(GL_ARRAY_BUFFER, splashVBO);
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(full), full);
    };

    auto drawPercentNumber = [&](int percent, float centerX, float centerY, float digitW, float digitH) {
        percent = std::max(0, std::min(100, percent));
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%d%%", percent);
        size_t n = std::strlen(buf);
        float totalW = digitW * (float)n;
        float x = centerX - totalW * 0.5f;
        for (size_t i = 0; i < n; ++i)
        {
            int idx = (buf[i] == '%') ? 10 : (buf[i] - '0');
            if (idx < 0 || idx > 10) idx = 0;
            float u0 = (float)idx / 11.0f;
            float u1 = (float)(idx + 1) / 11.0f;
            drawNdcTexturedQuad(x, centerY - digitH * 0.5f, x + digitW, centerY + digitH * 0.5f,
                                digitsTex, u0, 0.0f, u1, 1.0f, 1.0f);
            x += digitW;
        }
    };

    auto drawSplashFrame = [&](const char* statusTitle, float progress01, float dim = 0.0f) {
        progress01 = std::max(0.0f, std::min(1.0f, progress01));
        loadProgress = progress01;
        int pct = (int)std::round(progress01 * 100.0f);
        if (statusTitle)
        {
            char title[256];
            std::snprintf(title, sizeof(title), "%s  [%d%%]", statusTitle, pct);
            glfwSetWindowTitle(window, title);
        }
        glfwPollEvents();
        if (glfwWindowShouldClose(window))
            return false;
        int w = 0, h = 0;
        glfwGetFramebufferSize(window, &w, &h);
        if (w > 0 && h > 0)
        {
            SCR_WIDTH = static_cast<unsigned int>(w);
            SCR_HEIGHT = static_cast<unsigned int>(h);
            glViewport(0, 0, w, h);
        }
        drawFullscreenTex(splashTex, dim, 1.0f, true);

        // Barra de progreso realista (NDC, zona inferior centro)
        const float barX0 = -0.22f, barX1 = 0.22f;
        const float barY0 = -0.28f, barY1 = -0.24f;
        drawNdcTexturedQuad(barX0, barY0, barX1, barY1, barBgTex, 0, 0, 1, 1, 1.0f);
        float fillX1 = barX0 + (barX1 - barX0) * progress01;
        if (progress01 > 0.001f)
            drawNdcTexturedQuad(barX0, barY0, fillX1, barY1, barFillTex, 0, 0, 1, 1, 1.0f);

        // Porcentaje (dígitos monoespaciados, sin gaps raros)
        drawPercentNumber(pct, 0.0f, -0.38f, 0.038f, 0.065f);

        glfwSwapBuffers(window);
        return true;
    };

    // Pesos relativos por etapa (incluyen props: se cargan AQUI, no al caminar)
    // Suma es 1.0
    const float W_SHADERS = 0.03f;
    const float W_COLLISION = 0.10f;
    const float W_LEVEL = 0.16f;
    const float W_BORDER = 0.07f;
    const float W_LIGHTS = 0.09f;
    const float W_AUDIO = 0.04f;
    const float W_CAMS = 0.12f;
    const float W_OFFICE = 0.14f;
    const float W_BOXES = 0.04f;
    const float W_DEMON = 0.04f;
    const float W_COMP = 0.05f;
    const float W_BOOTHS = 0.07f;
    const float W_SPAWN = 0.02f;
    const float W_UI = 0.02f;
    const float W_SETTLE = 0.01f;
    float progressBase = 0.0f;

    static unsigned int blackTex = 0;
    auto ensureBlackTex = [&]() {
        if (blackTex != 0) return;
        glGenTextures(1, &blackTex);
        glBindTexture(GL_TEXTURE_2D, blackTex);
        unsigned char px[] = { 0, 0, 0, 255 };
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glBindTexture(GL_TEXTURE_2D, 0);
    };
    auto drawBlackOverlay = [&](float alpha) {
        if (alpha <= 0.001f) return;
        ensureBlackTex();
        drawFullscreenTex(blackTex, 0.0f, alpha, false);
    };

    if (!drawSplashFrame("Backrooms - Cargando...", 0.0f))
    {
        glfwTerminate();
        return 0;
    }
    std::cout << "[Load] Splash visible. Carga por etapas con % real.\n";

    // Modelos (se llenan por etapas)
    std::unique_ptr<Model> backroomsModel;
    std::unique_ptr<Model> backroomsCollisionsModel;
    std::unique_ptr<Model> border;
    std::unique_ptr<Model> surveillanceCameraModel;
    std::unique_ptr<Model> officeFurnitureModel;
    std::unique_ptr<Model> oldPaperBoxesModel;
    std::unique_ptr<Model> monsterAlienModel;
    std::unique_ptr<Model> sciFiComputerModel;
    std::unique_ptr<Model> publicPhoneBoothModel;

    // Shaders en heap para poder crearlos despues del primer frame
    std::unique_ptr<Shader> backroomsShader;
    std::unique_ptr<Shader> cubeShader;

    camera.MovementSpeed = 10;

    // Placeholders de bounds (se rellenan al cargar colisiones)
    AABB roomLocalBounds{};
    AABB roomWorldBounds{};
    glm::vec3 roomCenter(0.0f);
    float floorY = 0.0f;
    std::vector<WallAnchor> wallAnchors;
    const float cameraScale = 1.0f;
    const float cameraWallGap = 0.02f;

    // ETAPA 1: shaders de juego
    if (!drawSplashFrame("Backrooms - Cargando shaders...", progressBase + W_SHADERS * 0.3f)) { glfwTerminate(); return 0; }
    backroomsShader = std::make_unique<Shader>("shaders/backrooms.vs", "shaders/backrooms.fs");
    cubeShader = std::make_unique<Shader>("shaders/basico.vs", "shaders/basico.fs");
    progressBase += W_SHADERS;
    if (!drawSplashFrame("Backrooms - Shaders listos", progressBase)) { glfwTerminate(); return 0; }
    std::cout << "[Load] Shaders OK\n";

    // ETAPA 2: colisiones del mapa
    if (!drawSplashFrame("Backrooms - Cargando colisiones del mapa...", progressBase + W_COLLISION * 0.2f)) { glfwTerminate(); return 0; }
    backroomsCollisionsModel = std::make_unique<Model>("models/backrooms_level_0_collisions/backrooms.obj");
    roomLocalBounds = computeModelBounds(*backroomsCollisionsModel);
    roomWorldBounds = { roomLocalBounds.min + backroomsPos, roomLocalBounds.max + backroomsPos };
    roomCenter = (roomWorldBounds.min + roomWorldBounds.max) * 0.5f;
    floorY = roomWorldBounds.min.y;
    // Anclas de pared solo hacen falta para colocar camaras (props) a omitir en modo rapido
    if (!LIGHTING_FAST_LOAD)
        wallAnchors = collectWallAnchors(*backroomsCollisionsModel, backroomsPos);
    colManager.addStaticBox(*backroomsCollisionsModel, backroomsPos);
    progressBase += W_COLLISION;
    if (!drawSplashFrame("Backrooms - Colisiones listas", progressBase)) { glfwTerminate(); return 0; }
    std::cout << "[Load] Colisiones OK (meshes " << backroomsCollisionsModel->meshes.size() << ")\n";

    // ETAPA 3: nivel visual (suele ser la mas pesada; necesario para ver iluminacion)
    if (!drawSplashFrame("Backrooms - Cargando nivel (Backrooms)...", progressBase + W_LEVEL * 0.15f)) { glfwTerminate(); return 0; }
    backroomsModel = std::make_unique<Model>("models/backrooms_level_0/backrooms.obj");
    progressBase += W_LEVEL;
    if (!drawSplashFrame("Backrooms - Nivel listo", progressBase)) { glfwTerminate(); return 0; }
    std::cout << "[Load] Nivel visual OK\n";

    // ETAPA 4: border (omitido en modo rapido: no aporta a iluminacion)
    if (LIGHTING_FAST_LOAD)
    {
        progressBase += W_BORDER;
        std::cout << "[Load] LIGHTING_FAST_LOAD: border omitido\n";
    }
    else
    {
        if (!drawSplashFrame("Backrooms - Cargando limites del mapa...", progressBase + W_BORDER * 0.2f)) { glfwTerminate(); return 0; }
        border = std::make_unique<Model>("models/border/border.obj");
        colManager.addStaticBox(*border, backroomsPos);
        progressBase += W_BORDER;
        if (!drawSplashFrame("Backrooms - Limites listos", progressBase)) { glfwTerminate(); return 0; }
        std::cout << "[Load] Border OK\n";
    }

    // ETAPA 5: luces de techo (rejilla CPU, no lee el mesh)
    if (!drawSplashFrame("Backrooms - Preparando luces...", progressBase + W_LIGHTS * 0.1f)) { glfwTerminate(); return 0; }
    std::vector<CeilingLight> ceilingLights;
    findCeilingLights(backroomsPos, ceilingLights, lightsBlackoutMode);
    g_ceilingLightsPtr = &ceilingLights;
    progressBase += W_LIGHTS;
    if (!drawSplashFrame("Backrooms - Luces listas", progressBase)) { glfwTerminate(); return 0; }
    std::cout << "[Load] Luces de techo: " << ceilingLights.size() << "\n";

    // ETAPA 6: audio (en modo rapido solo init SFX; sin musica de carga)
    if (!drawSplashFrame("Backrooms - Cargando audio...", progressBase + W_AUDIO * 0.3f)) { glfwTerminate(); return 0; }
    if (AudioBgm_Init())
    {
        if (!LIGHTING_FAST_LOAD)
        {
            if (!AudioBgm_PlayLoop("sounds/S1.mp3", BGM_DISTANT_VOLUME))
                std::cout << "[Audio] Continuando sin BGM.\n";
        }
        else
        {
            std::cout << "[Load] LIGHTING_FAST_LOAD: BGM omitido (SFX linterna/pasos siguen disponibles)\n";
        }
    }
    progressBase += W_AUDIO;
    if (!drawSplashFrame("Backrooms - Audio listo", progressBase)) { AudioBgm_Shutdown(); glfwTerminate(); return 0; }

    // Anclas de props (mismas del proyecto del grupo)
    const std::vector<glm::vec3> officeAnchors = {
        glm::vec3(roomCenter.x - 40.0f, 0.0f, roomCenter.z - 24.0f),
        glm::vec3(roomCenter.x + 12.0f, 0.0f, roomCenter.z - 20.0f),
        glm::vec3(roomCenter.x - 55.0f, 0.0f, roomCenter.z + 32.0f),
        glm::vec3(roomCenter.x + 24.0f, 0.0f, roomCenter.z + 22.0f)
    };
    const std::vector<float> officeYaws = { 20.0f, -35.0f, 110.0f, -145.0f };

    const std::vector<glm::vec3> boxesAnchors = {
        glm::vec3(roomCenter.x - 18.0f, 0.0f, roomCenter.z - 33.0f),
        glm::vec3(roomCenter.x + 14.0f, 0.0f, roomCenter.z - 36.0f),
        glm::vec3(roomCenter.x - 28.0f, 0.0f, roomCenter.z + 14.0f),
        glm::vec3(roomCenter.x + 26.0f, 0.0f, roomCenter.z + 32.0f)
    };
    const std::vector<float> boxesYaws = { 12.0f, 65.0f, -20.0f, 140.0f };

    const std::vector<glm::vec3> demonAnchors = {
        glm::vec3(roomCenter.x - 50.0f, 0.0f, roomCenter.z - 16.0f),
        glm::vec3(roomCenter.x + 32.0f, 0.0f, roomCenter.z - 20.0f),
        glm::vec3(roomWorldBounds.min.x + 28.0f, 0.0f, roomWorldBounds.min.z + 30.0f)
    };
    const std::vector<float> demonYaws = { 45.0f, -135.0f };

    const std::vector<glm::vec3> computerAnchors = {
        glm::vec3(roomWorldBounds.min.x + 25.0f, 0.0f, roomWorldBounds.min.z + 25.0f),
        glm::vec3(roomWorldBounds.max.x - 25.0f, 0.0f, roomWorldBounds.min.z + 25.0f),
        glm::vec3(roomWorldBounds.min.x + 25.0f, 0.0f, roomWorldBounds.max.z - 25.0f),
        glm::vec3(roomWorldBounds.max.x - 25.0f, 0.0f, roomWorldBounds.max.z - 25.0f)
    };
    const std::vector<float> computerYaws = { 35.0f, -35.0f, 145.0f, -145.0f };

    std::vector<CameraInstance> cameraInstances;
    std::vector<Instance> officeInstances;
    std::vector<Instance> boxesInstances;
    std::vector<Instance> demonInstances;
    std::vector<Instance> computerInstances;
    std::vector<Instance> boothInstances;
    std::vector<glm::vec3> deskLampPositions;

    // precargamos los props en el splash porque cargarlos al caminar congelaba el juego;
    // con LIGHTING_FAST_LOAD se omiten todos para arrancar rapido al probar iluminacion
    if (LIGHTING_FAST_LOAD)
    {
        progressBase += W_CAMS + W_OFFICE + W_BOXES + W_DEMON + W_COMP + W_BOOTHS;
        if (!drawSplashFrame("Backrooms - Modo rapido (solo mapa + luces)...", progressBase))
        {
            AudioBgm_Shutdown();
            glfwTerminate();
            return 0;
        }
        std::cout << "[Load] LIGHTING_FAST_LOAD=1: props omitidos (camaras/oficina/cajas/entidad/PCs/cabinas).\n";
        std::cout << "[Load] Solo nivel + colisiones + luces. Sin border/props/BGM/menu. LIGHTING_FAST_LOAD=false = demo completa.\n";
    }
    else
    {
    if (!drawSplashFrame("Backrooms - Cargando camaras...", progressBase + W_CAMS * 0.2f)) { AudioBgm_Shutdown(); glfwTerminate(); return 0; }
    surveillanceCameraModel = std::make_unique<Model>("models/surveillance_camera/camaras_vigilancia.obj");
    {
        const AABB cameraBounds = computeModelBounds(*surveillanceCameraModel);
        const float wallAttachBase = (-cameraBounds.min.z * cameraScale) + cameraWallGap;
        CameraPlacementConfig cameraCfg;
        cameraCfg.targetCount = 200;
        cameraInstances = generateCameraInstances(wallAnchors, roomCenter, wallAttachBase, cameraCfg);
        addFurnitureCollisions(colManager, cameraBounds, cameraInstances, glm::vec3(cameraScale));
    }
    progressBase += W_CAMS;
    if (!drawSplashFrame("Backrooms - Camaras listas", progressBase)) { AudioBgm_Shutdown(); glfwTerminate(); return 0; }
    std::cout << "[Load] Camaras: " << cameraInstances.size() << "\n";

    if (!drawSplashFrame("Backrooms - Cargando oficina...", progressBase + W_OFFICE * 0.15f)) { AudioBgm_Shutdown(); glfwTerminate(); return 0; }
    officeFurnitureModel = std::make_unique<Model>("models/office_furniture/office.obj");
    {
        const AABB officeBounds = computeModelBounds(*officeFurnitureModel);
        officeInstances = createFloorInstances(officeAnchors, roomWorldBounds, officeBounds, floorY, 3.0f, officeYaws, 0.45f);
        addInstancesCollision(colManager, *officeFurnitureModel, officeInstances);
        glm::vec3 localLampCenter = findLocalLampCenter(*officeFurnitureModel);
        deskLampPositions.clear();
        for (const Instance& instance : officeInstances)
        {
            glm::vec3 worldLampPos = glm::vec3(buildInstanceMatrix(instance) * glm::vec4(localLampCenter, 1.0f));
            deskLampPositions.push_back(worldLampPos);
        }
    }
    progressBase += W_OFFICE;
    if (!drawSplashFrame("Backrooms - Oficina lista", progressBase)) { AudioBgm_Shutdown(); glfwTerminate(); return 0; }
    std::cout << "[Load] Oficina: " << officeInstances.size() << "\n";

    if (!drawSplashFrame("Backrooms - Cargando cajas...", progressBase + W_BOXES * 0.3f)) { AudioBgm_Shutdown(); glfwTerminate(); return 0; }
    oldPaperBoxesModel = std::make_unique<Model>("models/old_paper__cardboard_boxes/carton_papel.obj");
    {
        const AABB boxesBounds = computeModelBounds(*oldPaperBoxesModel, true);
        boxesInstances = createFloorInstances(boxesAnchors, roomWorldBounds, boxesBounds, floorY, 2.2f, boxesYaws, 0.35f);
        addInstancesCollision(colManager, *oldPaperBoxesModel, boxesInstances, true);
    }
    progressBase += W_BOXES;
    if (!drawSplashFrame("Backrooms - Cajas listas", progressBase)) { AudioBgm_Shutdown(); glfwTerminate(); return 0; }

    if (!drawSplashFrame("Backrooms - Cargando entidad...", progressBase + W_DEMON * 0.3f)) { AudioBgm_Shutdown(); glfwTerminate(); return 0; }
    monsterAlienModel = std::make_unique<Model>("models/monster_alien/scene.obj");
    AABB demonBoundsLocal{};
    float demonGroundY = 0.0f;
    {
        demonBoundsLocal = computeModelBounds(*monsterAlienModel);
        g_demonBoundsLocal = demonBoundsLocal;
        demonInstances = createFloorInstances(demonAnchors, roomWorldBounds, demonBoundsLocal, floorY, 1.8f, demonYaws, 0.45f);
        // Guardar AABBs y registrarlos; en survival se borran para no dejar "paredes fantasma"
        g_demonStaticBoxes.clear();
        g_demonColStart = colManager.staticBoxCount();
        {
            const AABB localBounds = demonBoundsLocal;
            for (const Instance& instance : demonInstances)
            {
                AABB worldBounds = transformBounds(localBounds, buildInstanceMatrix(instance));
                g_demonStaticBoxes.push_back(worldBounds);
                colManager.addStaticBox(worldBounds);
            }
        }
        g_demonColCount = g_demonStaticBoxes.size();
        g_demonColsActive = true;
        if (!demonInstances.empty())
            demonGroundY = demonInstances[0].position.y;
        else
            demonGroundY = floorY - (demonBoundsLocal.min.y * 1.8f);
        SurvivalRoomBounds srb{};
        srb.min = roomWorldBounds.min;
        srb.max = roomWorldBounds.max;
        Survival_Init(srb, demonGroundY, 1.8f);
        Survival_SetCollisionProbe(survivalCorrectMove, &colManager);
        // Aviso de proximidad (loop; volumen por distancia, el mp3 es muy fuerte)
        if (!AudioBgm_ProximityLoad("sounds/entity_proximity.mp3"))
            std::cout << "[Audio] Aviso: no se cargo entity_proximity.mp3\n";
    }
    // Telefono + voces: siempre (aunque falle carga de entidad)
    AudioBgm_PhoneRingSetPath("sounds/phone_ring.mp3");
    AudioBgm_DistantVoicesSet(
        "sounds/voice_distant_1.mp3",
        "sounds/voice_distant_2.mp3",
        "sounds/voice_distant_3.mp3");
    AudioBgm_AdmiracionAlarmSetPath("sounds/admiracion_alarm.wav");
    AudioBgm_AmbientBuzzSetPath("sounds/buzz_harmonic.mp3");
    progressBase += W_DEMON;
    if (!drawSplashFrame("Backrooms - Entidad lista", progressBase)) { AudioBgm_Shutdown(); glfwTerminate(); return 0; }

    if (!drawSplashFrame("Backrooms - Cargando computadoras...", progressBase + W_COMP * 0.3f)) { AudioBgm_Shutdown(); glfwTerminate(); return 0; }
    sciFiComputerModel = std::make_unique<Model>("models/sci-fi_computer/computadora.obj");
    {
        const AABB computerBounds = computeModelBounds(*sciFiComputerModel);
        computerInstances = createFloorInstances(
            computerAnchors, roomWorldBounds, computerBounds, floorY, 1.0f, computerYaws, 0.35f);
        // Mesh tope ~ floorY+2.9 es 0.1 < ojo Y 0: sin minTopY se atraviesa (normal vertical).
        addInstancesCollision(colManager, *sciFiComputerModel, computerInstances, false, 0.30f, 1.25f);
        std::cout << "[Load] PCs: " << computerInstances.size() << " (colision solida)\n";
    }
    progressBase += W_COMP;
    if (!drawSplashFrame("Backrooms - Computadoras listas", progressBase)) { AudioBgm_Shutdown(); glfwTerminate(); return 0; }

    if (!drawSplashFrame("Backrooms - Cargando cabinas...", progressBase + W_BOOTHS * 0.2f)) { AudioBgm_Shutdown(); glfwTerminate(); return 0; }
    publicPhoneBoothModel = std::make_unique<Model>("models/payphone/payphone.obj");
    {
        const AABB boothBounds = computeModelBounds(*publicPhoneBoothModel);
        boothInstances = generatePhoneBooths(roomWorldBounds, boothBounds, floorY, 50);
        // Cabinas estrechas (~0.7m) y tope bajo el ojo: pad XZ + techo de colision alto
        addInstancesCollision(colManager, *publicPhoneBoothModel, boothInstances, false, 0.40f, 1.25f);
    }
    progressBase += W_BOOTHS;
    if (!drawSplashFrame("Backrooms - Cabinas listas", progressBase)) { AudioBgm_Shutdown(); glfwTerminate(); return 0; }
    std::cout << "[Load] Cabinas: " << boothInstances.size() << "\n";
    std::cout << "[Load] Todos los props precargados (sin stream al caminar).\n";
    } // end !LIGHTING_FAST_LOAD

    // spawn: mantenemos la altura Y 0 (la correcta a escala del mapa) pero elegimos XZ y
    // yaw para no aparecer mirando de frente a una pared
    if (!drawSplashFrame("Backrooms - Preparando spawn...", progressBase + W_SPAWN * 0.4f)) { AudioBgm_Shutdown(); glfwTerminate(); return 0; }
    if (LIGHTING_FAST_LOAD)
    {
        // Sin busqueda de pasillo: spawn fijo (ahorra mucho CPU al arrancar)
        camera.Position = glm::vec3(0.0f, 0.0f, 3.0f);
        camera.Yaw = -90.0f;
        camera.Pitch = -3.0f;
        camera.ProcessMouseMovement(0.0f, 0.0f);
        progressBase += W_SPAWN;
        std::cout << "[Spawn] LIGHTING_FAST_LOAD: spawn fijo (0,0,3) yaw=-90\n";
        if (!drawSplashFrame("Backrooms - Spawn listo", progressBase)) { AudioBgm_Shutdown(); glfwTerminate(); return 0; }
    }
    else
    {
        const float eyeY = 0.0f; // altura original del repo (NO floorY+1.7)
        auto freeWalk = [&](glm::vec3 p, glm::vec3 dir, float maxDist) -> float {
            float lenDir = glm::length(dir);
            if (lenDir < 1e-4f) return 0.0f;
            dir /= lenDir;
            float traveled = 0.0f;
            const float stepSize = 0.5f;
            while (traveled < maxDist)
            {
                glm::vec3 step = colManager.correctMovement(p, dir * stepSize, RADIUS);
                float sLen = glm::length(step);
                if (sLen < 0.08f) break;
                if (glm::dot(step / sLen, dir) < 0.55f) break;
                p += step;
                traveled += sLen;
            }
            return traveled;
        };

        // Candidatos: alrededor del spawn original y un poco hacia el centro del mapa
        std::vector<glm::vec3> seeds;
        seeds.emplace_back(0.0f, eyeY, 3.0f); // original GitHub
        for (int ix = -3; ix <= 3; ++ix)
            for (int iz = 0; iz <= 6; ++iz)
                seeds.emplace_back((float)ix * 4.0f, eyeY, 2.0f + (float)iz * 4.0f);
        // mezcla suave hacia el centro del laberinto (sin usar roomCenter a ciegas)
        for (float t : { 0.15f, 0.25f, 0.35f, 0.45f })
        {
            seeds.emplace_back(
                0.0f * (1.0f - t) + roomCenter.x * t,
                eyeY,
                3.0f * (1.0f - t) + roomCenter.z * t);
        }

        const float margin = 2.5f;
        const float minX = roomWorldBounds.min.x + margin;
        const float maxX = roomWorldBounds.max.x - margin;
        const float minZ = roomWorldBounds.min.z + margin;
        const float maxZ = roomWorldBounds.max.z - margin;

        glm::vec3 bestPos(0.0f, eyeY, 3.0f);
        float bestYaw = -90.0f;
        float bestScore = -1.0f;

        for (glm::vec3 p : seeds)
        {
            p.x = glm::clamp(p.x, minX, maxX);
            p.z = glm::clamp(p.z, minZ, maxZ);
            p.y = eyeY;

            for (int yi = 0; yi < 16; ++yi)
            {
                float yaw = -180.0f + yi * 22.5f;
                float rad = glm::radians(yaw);
                glm::vec3 dir(std::cos(rad), 0.0f, std::sin(rad));
                float forward = freeWalk(p, dir, 22.0f);
                if (forward < 4.0f) continue; // muy pegado a pared
                float left = freeWalk(p, glm::vec3(-dir.z, 0.0f, dir.x), 5.0f);
                float right = freeWalk(p, glm::vec3(dir.z, 0.0f, -dir.x), 5.0f);
                // pasillo abierto al frente + algo de ancho
                float score = forward * 4.0f + std::min(left, right) * 2.0f + (left + right) * 0.35f;
                if (score > bestScore)
                {
                    bestScore = score;
                    bestPos = p;
                    bestYaw = yaw;
                }
            }
        }

        // Si no hay buen score, al menos original + mejor yaw
        if (bestScore < 8.0f)
        {
            bestPos = glm::vec3(0.0f, eyeY, 3.0f);
            float localBest = -1.0f;
            for (int yi = 0; yi < 16; ++yi)
            {
                float yaw = -180.0f + yi * 22.5f;
                float rad = glm::radians(yaw);
                float s = freeWalk(bestPos, glm::vec3(std::cos(rad), 0.0f, std::sin(rad)), 20.0f);
                if (s > localBest) { localBest = s; bestYaw = yaw; }
            }
            bestScore = localBest;
        }

        // Avanzar un poco al centro del pasillo abierto (mejor panorama)
        {
            float rad = glm::radians(bestYaw);
            glm::vec3 dir(std::cos(rad), 0.0f, std::sin(rad));
            float fwd = freeWalk(bestPos, dir, 18.0f);
            if (fwd > 5.0f)
            {
                float push = std::min(3.5f, fwd * 0.25f);
                bestPos += colManager.correctMovement(bestPos, dir * push, RADIUS);
            }
        }

        camera.Position = bestPos;
        camera.Position.y = eyeY; // forzar altura original del repo
        camera.Yaw = bestYaw;
        camera.Pitch = -3.0f; // leve mirada al pasillo (casi horizontal como el original)
        camera.ProcessMouseMovement(0.0f, 0.0f);

        std::cout << "[Spawn] GitHub altura Y=0 conservada. Vista abierta a pasillos: score=" << bestScore
                  << " pos=(" << camera.Position.x << ", " << camera.Position.y << ", " << camera.Position.z
                  << ") yaw=" << camera.Yaw << " (repo original era 0,0,3 yaw=-90)\n";
        progressBase += W_SPAWN;
        if (!drawSplashFrame("Backrooms - Spawn listo", progressBase)) { AudioBgm_Shutdown(); glfwTerminate(); return 0; }
    } // end !LIGHTING_FAST_LOAD spawn search

    // Texturas de menu (pausa sigue disponible en modo rapido)
    if (!drawSplashFrame("Backrooms - Cargando interfaz...", progressBase + W_UI * 0.4f)) { AudioBgm_Shutdown(); glfwTerminate(); return 0; }
    menuStartTex = loadUiTexture("textures/menu_start.png");
    menuPauseTex = loadUiTexture("textures/menu_pause.png"); // APAGAR LUCES
    menuPauseLightsOffTex = loadUiTexture("textures/menu_pause_lights_off.png"); // ENCENDER LUCES
    // Titulos: A2 fijo (Chase); B1/B3 aleatorios (Rage)
    unsigned int titleLucesTex = loadUiTexture("textures/survival/title_luces_apagadas.png"); // A2
    unsigned int titleHuyeB1Tex = loadUiTexture("textures/survival/title_huye_b1.png");
    unsigned int titleHuyeB3Tex = loadUiTexture("textures/survival/title_huye_b3.png");
    AudioBgm_TimerTickSetPaths("sounds/timer_tick_chase.mp3", "sounds/timer_tick_rage.mp3");
    ensureBlackTex();
    progressBase += W_UI;
    if (!drawSplashFrame("Backrooms - Interfaz lista", progressBase)) { AudioBgm_Shutdown(); glfwTerminate(); return 0; }

    // Espera final: en modo rapido casi cero; en demo completa ~2.2 s
    {
        const double settleStart = glfwGetTime();
        const double settleSeconds = LIGHTING_FAST_LOAD ? 0.05 : 2.2;
        while (true)
        {
            double t = (glfwGetTime() - settleStart) / settleSeconds;
            if (t > 1.0) t = 1.0;
            float p = progressBase + W_SETTLE * static_cast<float>(t);
            if (!drawSplashFrame(LIGHTING_FAST_LOAD ? "Backrooms - Listo (modo rapido)..." : "Backrooms - Finalizando...", p))
            {
                AudioBgm_Shutdown();
                glfwTerminate();
                return 0;
            }
            if (t >= 1.0)
                break;
        }
        progressBase += W_SETTLE;
        if (!drawSplashFrame("Backrooms - 100% listo", 1.0f))
        {
            AudioBgm_Shutdown();
            glfwTerminate();
            return 0;
        }
        if (!LIGHTING_FAST_LOAD)
        {
            // un instante en 100% para que se lea (solo demo completa)
            for (int i = 0; i < 20; ++i)
            {
                if (!drawSplashFrame("Backrooms - Listo para jugar", 1.0f))
                {
                    AudioBgm_Shutdown();
                    glfwTerminate();
                    return 0;
                }
            }
        }
    }

    if (LIGHTING_FAST_LOAD)
    {
        // Entra directo al juego (sin menu de COMENZAR)
        appState = AppState::FadeIn;
        fadeBlack = 1.0f;
        glfwSetWindowTitle(window, "Backrooms - Iluminacion RAPIDA");
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        firstMouse = true;
        lastFrame = static_cast<float>(glfwGetTime());
        AudioBgm_EnterGameAmbient();
        AudioBgm_AmbientBuzzSetActive(lightPresentation == LightPresentation::Normal);
        std::cout << "[UI] LIGHTING_FAST_LOAD: entrada directa al juego (sin menu).\n";
        std::cout << "[UI] Pon LIGHTING_FAST_LOAD=false para carga/demo completa.\n";
    }
    else
    {
        // Menu principal: SOLO ahora (carga completa, % es 100)
        appState = AppState::Menu;
        fadeBlack = 1.0f;
        glfwSetWindowTitle(window, "Backrooms - Menu");
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        firstMouse = true;
        lastFrame = static_cast<float>(glfwGetTime());
        std::cout << "[UI] Carga 100%. Menu COMENZAR / SALIR disponible.\n";
    }

    // 9. Bucle principal
    while (!glfwWindowShouldClose(window)) {
        float currentFrame = static_cast<float>(glfwGetTime());
        deltaTime = currentFrame - lastFrame;
        lastFrame = currentFrame;
        if (deltaTime > 0.1f) deltaTime = 0.1f;

        // el audio de juego (rachas, telefono lejano) solo corre jugando; la alarma de
        // admiracion se actualiza siempre, incluso en pausa si el modo esta activo
        AudioBgm_Update(deltaTime, appState == AppState::Playing || appState == AppState::FadeIn);

        processInput(window);

        // Supervivencia: update (timers/IA/cinematicas)
        {
            const bool worldPlaying = (appState == AppState::Playing || appState == AppState::FadeIn);
            if (Survival_IsEnabled() && Survival_IsCycleActive())
                survivalRemoveDemonCollisions();
            Survival_Update(deltaTime, camera.Position, camera.Front, survivalCorrectMove, &colManager, worldPlaying);
            survivalSyncLights();

            // Transiciones de estado app desde survival
            const SurvivalPhase sp = Survival_GetPhase();
            if (sp == SurvivalPhase::Jumpscare && appState != AppState::Jumpscare
                && appState != AppState::Menu && appState != AppState::Ending)
            {
                appState = AppState::Jumpscare;
                glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
                firstMouse = true;
            }
            else if (Survival_IsCinematic()
                && appState != AppState::Ending && appState != AppState::Menu)
            {
                appState = AppState::Ending;
                glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
                firstMouse = true;
                AudioBgm_EnterMenuLoop();
            }
            if (Survival_ConsumeReturnToMenu())
            {
                appState = AppState::Menu;
                glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
                firstMouse = true;
                AudioBgm_EnterMenuLoop();
                AudioBgm_ProximitySetVolume(0.0f);
                lightPresentation = LightPresentation::Normal;
                applyLightPresentation();
                glfwSetWindowTitle(window, "Backrooms - Grupo 7");
            }
        }

        // Caches de texto 2D (bake solo cuando cambia el string)
        static TextBake s_promptBake{};
        static std::string s_promptKey;
        static TextBake s_cineBake{};
        static std::string s_cineKey;
        static TextBake s_phaseBake{};
        static std::string s_phaseKey;
        static TextBake s_timerNumBake{};
        static std::string s_timerKey;
        static TextBake s_hintBake{};
        static bool s_hintReady = false;
        auto ensureText = [&](TextBake& bake, std::string& key, const char* str,
                              int px, float r, float g, float b) {
            if (!str) str = "";
            if (key == str && bake.tex != 0)
                return;
            Text_Free(bake);
            key = str;
            if (str[0])
                bake = Text_Bake(str, px, r, g, b, 0.02f, 0.02f, 0.02f, 0.55f, 900);
        };

        // Ending / jumpscare overlay (pantalla especial)
        if (appState == AppState::Ending || appState == AppState::Jumpscare)
        {
            int w = 0, h = 0;
            glfwGetFramebufferSize(window, &w, &h);
            if (w > 0 && h > 0)
            {
                SCR_WIDTH = static_cast<unsigned int>(w);
                SCR_HEIGHT = static_cast<unsigned int>(h);
                glViewport(0, 0, w, h);
            }
            const float aspect = (float)std::max(1u, SCR_WIDTH) / (float)std::max(1u, SCR_HEIGHT);

            if (appState == AppState::Jumpscare)
            {
                // Jumpscare a la CARA: camara fija en los ojos del monstruo
                const float flash = Survival_JumpscareFlash();
                const float shock = Survival_JumpscareProgress(); // 1 al inicio a 0 al final
                const float t = (float)glfwGetTime();
                const float pulse = 0.5f + 0.5f * std::sin(t * 48.0f);
                // Flash blanco al inicio + sangre
                const float whiteFlash = std::clamp((shock - 0.85f) / 0.15f, 0.0f, 1.0f);
                glClearColor(
                    flash * (0.55f + 0.35f * pulse) + whiteFlash * 0.85f,
                    flash * 0.02f + whiteFlash * 0.75f,
                    flash * 0.02f + whiteFlash * 0.7f,
                    1.0f);
                glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

                if (monsterAlienModel && backroomsShader)
                {
                    // Mas cerca y grande: rostro/torso dominan el frame
                    const float sc = Survival_MonsterScale() * (1.32f + pulse * 0.06f);
                    float headLocalY = g_demonBoundsLocal.max.y * 0.80f;
                    if (headLocalY < 0.4f)
                        headLocalY = 1.45f;
                    const float headFromPivot = headLocalY * sc;

                    // Cara casi pegada a la camara
                    const float faceDist = 0.52f + (1.0f - shock) * 0.12f;

                    // Direccion frontal estable (horizontal) para no mirar al suelo
                    glm::vec3 front = camera.Front;
                    front.y = 0.0f;
                    if (glm::length(front) < 1e-4f)
                        front = glm::vec3(0.0f, 0.0f, -1.0f);
                    else
                        front = glm::normalize(front);

                    // Ancla de la CARA en el centro del FOV
                    glm::vec3 eye = camera.Position;
                    glm::vec3 faceAnchor = eye + front * faceDist;
                    // Pivote del monstruo: cabeza en faceAnchor
                    Instance scare{};
                    scare.scale = sc;
                    scare.position = faceAnchor - camera.WorldUp * headFromPivot;
                    // Mirar a la camara (yaw hacia el ojo)
                    {
                        float dx = eye.x - scare.position.x;
                        float dz = eye.z - scare.position.z;
                        scare.rotationDeg = glm::vec3(0.0f, glm::degrees(std::atan2(dx, dz)), 0.0f);
                    }

                    // Shake fuerte al inicio, luego micro
                    const float shakeAmp = (0.045f + 0.10f * shock) * (0.55f + 0.45f * pulse);
                    eye += camera.Right * (std::sin(t * 97.f) * shakeAmp);
                    eye += camera.WorldUp * (std::cos(t * 73.f) * shakeAmp * 0.7f);
                    // Look at SIEMPRE a la cara (no a camera.Front)
                    glm::vec3 lookTarget = faceAnchor + camera.Right * (std::sin(t * 40.f) * 0.012f * shock);

                    backroomsShader->use();
                    // FOV muy cerrado: claustrofobia, cara llena el cuadro
                    const float fov = 48.0f + (1.0f - shock) * 8.0f;
                    glm::mat4 projection = glm::perspective(glm::radians(fov), aspect, 0.05f, 40.0f);
                    glm::mat4 view = glm::lookAt(eye, lookTarget, camera.WorldUp);
                    backroomsShader->setMat4("projection", projection);
                    backroomsShader->setMat4("view", view);
                    backroomsShader->setVec3("viewPos", eye);
                    backroomsShader->setBool("lightsBlackout", false);
                    backroomsShader->setFloat("admiracionAmount", 1.0f);
                    backroomsShader->setFloat("admiracionFlicker", 0.4f + 0.6f * pulse);
                    glm::vec3 toFace = glm::normalize(faceAnchor - eye);
                    backroomsShader->setVec3("dirLight.direction", -toFace);
                    backroomsShader->setVec3("dirLight.ambient", glm::vec3(0.28f, 0.03f, 0.02f));
                    backroomsShader->setVec3("dirLight.diffuse", glm::vec3(2.2f, 0.18f, 0.1f));
                    backroomsShader->setVec3("dirLight.specular", glm::vec3(0.85f));
                    backroomsShader->setInt("nPointLights", 0);
                    std::vector<Instance> one{ scare };
                    glEnable(GL_DEPTH_TEST);
                    drawInstances(*backroomsShader, *monsterAlienModel, one, false, 50.0f);
                }
                // Vignette + sangre en bordes
                drawNdcTexturedQuad(-1.f, -1.f, 1.f, 1.f, barKnobEdgeTex, 0, 0, 1, 1, flash * 0.55f);
                // Barras de glitch
                float gy = std::sin(t * 13.f) * 0.4f;
                drawNdcTexturedQuad(-1.f, gy, 1.f, gy + 0.035f, barFillTex, 0, 0, 1, 1, 0.18f * flash);
                ensureText(s_cineBake, s_cineKey, "NO HAY SALIDA", 72, 1.0f, 0.12f, 0.08f);
                if (s_cineBake.tex)
                    Text_DrawCentered(s_cineBake.tex, s_cineBake.width, s_cineBake.height,
                        std::sin(t * 25.f) * 0.025f * flash,
                        -0.62f + std::sin(t * 18.f) * 0.015f,
                        0.055f, aspect, 0.35f + 0.55f * flash,
                        splashVAO, splashVBO, splashShader.get());
                glfwSetWindowTitle(window, "Backrooms | JUMPSCARE");
            }
            else // Ending: 1s blanco + maquina de escribir (sin glitch)
            {
                const int style = Survival_CinematicStyle();
                const bool whiteHold = Survival_CinematicIsWhiteHold();
                // Fondo blanco (kill/escape) o papel muy claro; lose: blanco roto
                if (style == 2)
                    glClearColor(0.97f, 0.94f, 0.92f, 1.0f);
                else
                    glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

                if (!whiteHold)
                {
                    // Texto negro maquina de escribir (Times), estable, sin vibracion
                    float tr = 0.08f, tg = 0.08f, tb = 0.08f;
                    if (style == 2) { tr = 0.22f; tg = 0.04f; tb = 0.04f; }

                    // Layout FIJO: se hornea el mensaje COMPLETO una vez para fijar
                    // tamano de lienzo y escala; el texto parcial (typewriter) se
                    // dibuja dentro de ese mismo lienzo a letras quietas, sin zoom.
                    const int   kTypePx = 62;    // ~72% mas grande que antes (36)
                    const int   kTypeWrap = 1180;
                    const float kTypeCx = 0.0f;
                    const float kTypeCy = 0.02f;

                    const char* full = Survival_CinematicFullMessage();
                    static std::string s_fullKey;
                    static int   s_canvasW = 0, s_canvasH = 0;
                    static float s_halfH = 0.38f;
                    if (full && full[0] && s_fullKey != full)
                    {
                        s_fullKey = full;
                        // Bake temporal del mensaje completo solo para medir el lienzo.
                        TextBake meas = Text_Bake(full, kTypePx, tr, tg, tb,
                            0, 0, 0, 0, kTypeWrap, L"Times New Roman");
                        s_canvasW = meas.width;
                        s_canvasH = meas.height;
                        if (s_canvasH > 0 && s_canvasW > 0)
                        {
                            const float texAspect = (float)s_canvasW / (float)s_canvasH;
                            s_halfH = std::clamp(0.62f / std::max(texAspect, 0.5f), 0.22f, 0.52f);
                        }
                        Text_Free(meas);
                    }

                    const char* body = Survival_CinematicBodyText();
                    static std::string s_typeKey;
                    static TextBake s_typeBake{};
                    if (body && body[0] && s_canvasW > 0 && s_canvasH > 0)
                    {
                        if (s_typeKey != body || s_typeBake.tex == 0)
                        {
                            Text_Free(s_typeBake);
                            s_typeKey = body;
                            // Prefijo dibujado dentro del lienzo FIJO del mensaje completo.
                            s_typeBake = Text_Bake(body, kTypePx, tr, tg, tb, 0, 0, 0, 0,
                                kTypeWrap, L"Times New Roman", s_canvasW, s_canvasH);
                        }
                        if (s_typeBake.tex)
                            Text_DrawCentered(s_typeBake.tex, s_typeBake.width, s_typeBake.height,
                                kTypeCx, kTypeCy, s_halfH, aspect, 1.0f,
                                splashVAO, splashVBO, splashShader.get());
                    }

                    if (Survival_CinematicDone())
                    {
                        static TextBake s_hintW{};
                        static bool s_hintWReady = false;
                        if (!s_hintWReady)
                        {
                            s_hintW = Text_Bake("Enter / Click para continuar", 32, 0.3f, 0.3f, 0.3f,
                                0, 0, 0, 0, 700, L"Times New Roman");
                            s_hintWReady = true;
                        }
                        if (s_hintW.tex)
                            Text_DrawCentered(s_hintW.tex, s_hintW.width, s_hintW.height,
                                0.0f, -0.78f, 0.030f, aspect, 0.9f,
                                splashVAO, splashVBO, splashShader.get());
                    }
                }

                const char* full = Survival_CinematicFullLine();
                if (full)
                {
                    char title[256];
                    std::snprintf(title, sizeof(title), "Backrooms | %s", full);
                    glfwSetWindowTitle(window, title);
                }
            }

            glfwSwapBuffers(window);
            glfwPollEvents();
            continue;
        }

        // menu / pausa: aca solo va la UI 2D, sin mundo 3D
        if (appState == AppState::Menu || appState == AppState::Paused)
        {
            // En pausa con Admiracion: monstruo "cerca" a nivel tenso bajo (no silencio total)
            if (appState == AppState::Paused
                && lightPresentation == LightPresentation::Admiracion)
                AudioBgm_ProximitySetVolume(0.024f);
            else
                AudioBgm_ProximitySetVolume(0.0f);
            int w = 0, h = 0;
            glfwGetFramebufferSize(window, &w, &h);
            if (w > 0 && h > 0)
            {
                SCR_WIDTH = static_cast<unsigned int>(w);
                SCR_HEIGHT = static_cast<unsigned int>(h);
                glViewport(0, 0, w, h);
            }
            unsigned int uiTex = menuStartTex;
            if (appState == AppState::Paused)
            {
                // Texto del boton: si luces ON a "APAGAR LUCES"; si OFF a "ENCENDER LUCES"
                uiTex = lightsBlackoutMode ? menuPauseLightsOffTex : menuPauseTex;
                if (uiTex == 0)
                    uiTex = menuPauseTex;
            }
            drawFullscreenTex(uiTex, 0.0f, 1.0f, true);

            // Relleno de sliders de volumen sobre la textura de pausa
            if (appState == AppState::Paused)
            {
                auto nyToNdc = [](float ny) { return 1.0f - 2.0f * ny; };
                const PauseSliderLayout sl = getPauseSliderLayout();
                const float trackX0 = sl.trackX0, trackX1 = sl.trackX1;
                const float* sliderNy = sl.sliderNy;
                const float insetX = 0.004f;
                const float trackHalfH_ny = 0.0085f;

                auto drawSliderFill = [&](float ny, float value01) {
                    value01 = std::max(0.0f, std::min(1.0f, value01));
                    const float yNdc = nyToNdc(ny);
                    const float x0 = (trackX0 + insetX) * 2.0f - 1.0f;
                    const float x1 = (trackX1 - insetX) * 2.0f - 1.0f;
                    const float halfH = trackHalfH_ny * 2.0f;
                    const float y0 = yNdc - halfH;
                    const float y1 = yNdc + halfH;
                    const float xf = x0 + (x1 - x0) * value01;

                    if (xf > x0 + 0.002f)
                    {
                        const float g = halfH * 0.85f;
                        drawNdcTexturedQuad(x0 - 0.004f, y0 - g, xf + 0.004f, y1 + g,
                            barGlowTex, 0, 0, 1, 1, 0.28f);
                        drawNdcTexturedQuad(x0, y0, xf, y1, barFillTex, 0, 0, 1, 1, 1.0f);
                        const float hiH = halfH * 0.35f;
                        drawNdcTexturedQuad(x0, y1 - hiH, xf, y1, barGlowTex, 0, 0, 1, 1, 0.55f);
                    }

                    // Knob del valor actual
                    const float kOuter = 0.014f;
                    const float kInner = 0.010f;
                    const float kAspect = 1.15f;
                    float kx = xf;
                    if (value01 < 0.001f)
                        kx = x0;
                    drawNdcTexturedQuad(kx - kOuter * 1.7f, yNdc - kOuter * 1.7f * kAspect,
                        kx + kOuter * 1.7f, yNdc + kOuter * 1.7f * kAspect,
                        barGlowTex, 0, 0, 1, 1, 0.30f);
                    drawNdcTexturedQuad(kx - kOuter, yNdc - kOuter * kAspect,
                        kx + kOuter, yNdc + kOuter * kAspect,
                        barKnobEdgeTex, 0, 0, 1, 1, 1.0f);
                    drawNdcTexturedQuad(kx - kInner, yNdc - kInner * kAspect,
                        kx + kInner, yNdc + kInner * kAspect,
                        barKnobTex, 0, 0, 1, 1, 1.0f);
                };

                if (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS)
                {
                    double mx = 0.0, my = 0.0;
                    glfwGetCursorPos(window, &mx, &my);
                    int winW = 0, winH = 0, fbW = 0, fbH = 0;
                    glfwGetWindowSize(window, &winW, &winH);
                    glfwGetFramebufferSize(window, &fbW, &fbH);
                    if (winW > 0 && winH > 0)
                    {
                        mx = mx * (double)fbW / (double)winW;
                        my = my * (double)fbH / (double)winH;
                    }
                    const float nx = (float)(mx / (double)std::max(1u, SCR_WIDTH));
                    const float ny = (float)(my / (double)std::max(1u, SCR_HEIGHT));

                    auto hitTrack = [&](float trackNy) -> bool {
                        // Un poco mas ancho en Y para facilitar el click
                        return nx >= (trackX0 - 0.02f) && nx <= (trackX1 + 0.02f) &&
                               std::abs(ny - trackNy) < 0.022f;
                    };
                    auto setFromNx = [&](void (*setter)(float)) {
                        float t = (nx - trackX0) / (trackX1 - trackX0);
                        if (t < 0.f) t = 0.f;
                        if (t > 1.f) t = 1.f;
                        setter(t);
                    };

                    if (g_volDrag == 0)
                    {
                        if (hitTrack(sliderNy[0])) g_volDrag = 1;
                        else if (hitTrack(sliderNy[1])) g_volDrag = 2;
                        else if (hitTrack(sliderNy[2])) g_volDrag = 3;
                    }
                    if (g_volDrag == 1) setFromNx(AudioBgm_SetMasterVolume);
                    else if (g_volDrag == 2) setFromNx(AudioBgm_SetMusicVolume);
                    else if (g_volDrag == 3) setFromNx(AudioBgm_SetSfxVolume);
                }
                else
                {
                    g_volDrag = 0;
                }

                {
                    const char* modeName = "Normal";
                    if (lightPresentation == LightPresentation::Blackout) modeName = "Blackout";
                    else if (lightPresentation == LightPresentation::Admiracion) modeName = "Admiracion";
                    char t[280];
                    std::snprintf(t, sizeof(t),
                        "Pausa [%s] | Surv %s | Master %d%%  Musica %d%%  SFX %d%% | G=supervivencia",
                        modeName,
                        Survival_IsEnabled() ? "ON" : "OFF",
                        (int)std::round(AudioBgm_GetMasterVolume() * 100.f),
                        (int)std::round(AudioBgm_GetMusicVolume() * 100.f),
                        (int)std::round(AudioBgm_GetSfxVolume() * 100.f));
                    glfwSetWindowTitle(window, t);
                }

                drawSliderFill(sliderNy[0], AudioBgm_GetMasterVolume());
                drawSliderFill(sliderNy[1], AudioBgm_GetMusicVolume());
                drawSliderFill(sliderNy[2], AudioBgm_GetSfxVolume());

                // Checkbox modo supervivencia: caja con borde + ✓ al activar (no bloque solido)
                {
                    float cx, cy, half, labelNy;
                    Survival_GetCheckboxLayout(cx, cy, half, labelNy);
                    auto nyToNdc = [](float ny) { return 1.0f - 2.0f * ny; };
                    auto nxToNdc = [](float nx) { return nx * 2.0f - 1.0f; };
                    const float aspect = (float)std::max(1u, SCR_WIDTH) / (float)std::max(1u, SCR_HEIGHT);
                    // Caja un poco mas grande y cuadrada en NDC
                    const float boxHalf = 0.022f;
                    const float x0 = nxToNdc(cx) - boxHalf;
                    const float x1 = nxToNdc(cx) + boxHalf;
                    const float y0 = nyToNdc(cy) - boxHalf * aspect; // compensar aspect a cuadrado visual
                    const float y1 = nyToNdc(cy) + boxHalf * aspect;
                    const float border = 0.0045f;
                    const bool on = Survival_IsEnabled();

                    // Fondo interior (vacio / oscuro suave)
                    drawNdcTexturedQuad(x0, y0, x1, y1, barGlowTex, 0, 0, 1, 1, on ? 0.22f : 0.14f);
                    // Marco: 4 lados (borde claro)
                    drawNdcTexturedQuad(x0, y0, x1, y0 + border, barKnobEdgeTex, 0, 0, 1, 1, 0.95f); // abajo
                    drawNdcTexturedQuad(x0, y1 - border, x1, y1, barKnobEdgeTex, 0, 0, 1, 1, 0.95f); // arriba
                    drawNdcTexturedQuad(x0, y0, x0 + border, y1, barKnobEdgeTex, 0, 0, 1, 1, 0.95f); // izq
                    drawNdcTexturedQuad(x1 - border, y0, x1, y1, barKnobEdgeTex, 0, 0, 1, 1, 0.95f); // der
                    // Sombra exterior sutil
                    drawNdcTexturedQuad(x0 - 0.003f, y0 - 0.003f, x1 + 0.003f, y1 + 0.003f,
                        barKnobEdgeTex, 0, 0, 1, 1, 0.25f);

                    // Simbolo check cuando esta ON
                    static TextBake s_checkMark{};
                    static bool s_checkMarkReady = false;
                    if (!s_checkMarkReady)
                    {
                        // UTF 8 check mark
                        s_checkMark = Text_Bake("\xE2\x9C\x93", 52, 0.55f, 0.95f, 0.45f, 0, 0, 0, 0, 80);
                        if (!s_checkMark.tex)
                            s_checkMark = Text_Bake("V", 48, 0.55f, 0.95f, 0.45f, 0, 0, 0, 0, 80);
                        s_checkMarkReady = true;
                    }
                    if (on && s_checkMark.tex)
                    {
                        Text_DrawCentered(s_checkMark.tex, s_checkMark.width, s_checkMark.height,
                            nxToNdc(cx), nyToNdc(cy), 0.020f, aspect, 1.0f,
                            splashVAO, splashVBO, splashShader.get());
                    }

                    // Etiqueta a la derecha
                    static TextBake s_survLabel{};
                    static TextBake s_survLabelOn{};
                    static bool s_survLabelsReady = false;
                    if (!s_survLabelsReady)
                    {
                        s_survLabel = Text_Bake("Modo supervivencia  (G)", 26, 0.82f, 0.78f, 0.68f, 0, 0, 0, 0, 420);
                        s_survLabelOn = Text_Bake("Modo supervivencia  ON  (G)", 26, 0.75f, 0.95f, 0.55f, 0, 0, 0, 0, 420);
                        s_survLabelsReady = true;
                    }
                    TextBake& lab = on ? s_survLabelOn : s_survLabel;
                    if (lab.tex)
                    {
                        Text_DrawCentered(lab.tex, lab.width, lab.height,
                            nxToNdc(cx + 0.14f), nyToNdc(labelNy), 0.020f, aspect, 0.96f,
                            splashVAO, splashVBO, splashShader.get());
                    }
                }
            }

            glfwSwapBuffers(window);
            glfwPollEvents();
            continue;
        }

        // fade in / playing: ya se dibuja el mundo 3D
        if (appState == AppState::FadeIn)
        {
            fadeBlack -= deltaTime / FADE_IN_SECONDS;
            if (fadeBlack <= 0.0f)
            {
                fadeBlack = 0.0f;
                appState = AppState::Playing;
                glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
                firstMouse = true;
                glfwSetWindowTitle(window, "Backrooms - Grupo 7");
            }
        }

        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

        backroomsShader->use();

        // Eye con head bob: camara + linterna comparten el mismo offset (coherente al caminar)
        const glm::vec3 eyePos = camera.Position + headBobOffset;
        const glm::vec3 eyeFront = camera.Front;
        const glm::vec3 eyeUp = camera.Up;

        // Volumen de la entidad segun distancia (en admiracion se oye mas cerca)
        {
            float minDemonDist = 1e9f;
            if (Survival_HasLiveMonster())
            {
                minDemonDist = glm::distance(camera.Position, Survival_MonsterPosition());
            }
            else
            {
                for (const Instance& demon : demonInstances)
                {
                    float d = glm::distance(camera.Position, demon.position);
                    if (d < minDemonDist)
                        minDemonDist = d;
                }
            }
            const float hearFar = 48.0f;
            const float hearNear = 5.0f;
            const float maxProxVol = 0.035f; // techo intencional (archivo desproporcionado)
            float t = 0.0f;
            if (minDemonDist < hearFar && (Survival_HasLiveMonster() || !demonInstances.empty()))
            {
                t = 1.0f - glm::clamp((minDemonDist - hearNear) / (hearFar - hearNear), 0.0f, 1.0f);
                t = t * t * (3.0f - 2.0f * t); // smoothstep
            }
            float proxVol = t * maxProxVol;
            // Persecución (Chase/Rage): feedback FUERTE y con rango amplio para que el
            // jugador oiga si el monstruo le corta terreno. Sube más cuanto más cerca.
            const SurvivalPhase proxPh = Survival_GetPhase();
            const bool chasing = Survival_IsCycleActive()
                && (proxPh == SurvivalPhase::Chase || proxPh == SurvivalPhase::Rage);
            if (chasing && !Survival_IsChaseGrace())
            {
                const float chaseHearFar = 40.0f;
                const float chaseHearNear = 2.0f;
                float ct = 1.0f - glm::clamp(
                    (minDemonDist - chaseHearNear) / (chaseHearFar - chaseHearNear), 0.0f, 1.0f);
                ct = ct * ct * (3.0f - 2.0f * ct);
                // Rage un poco más intenso que Chase (bestia enfurecida).
                const float chaseCeil = (proxPh == SurvivalPhase::Rage) ? 0.85f : 0.7f;
                const float chaseFloor = 0.12f; // siempre audible mientras persigue
                proxVol = glm::max(chaseFloor, ct * chaseCeil);
            }
            else if (lightPresentation == LightPresentation::Admiracion)
            {
                const float admProxFloor = 0.024f;
                const float admProxCap   = 0.032f;
                proxVol = glm::clamp(glm::max(proxVol, admProxFloor), 0.0f, admProxCap);
                if (AudioBgm_AdmiracionAlarmIsPlaying())
                    proxVol *= 0.82f;
            }
            AudioBgm_ProximitySetVolume(proxVol);
        }

        const bool admiracionOn = (lightPresentation == LightPresentation::Admiracion);
        float admFlicker = 1.0f;
        bool admTotalBlackout = false;
        if (admiracionOn)
        {
            // Timers de apagon total en admiracion
            if (appState == AppState::Playing)
            {
                if (admBlackoutTimer > 0.0f)
                {
                    admBlackoutTimer -= deltaTime;
                    if (admBlackoutTimer <= 0.0f)
                    {
                        admBlackoutTimer = 0.0f;
                        admBlackoutNext = 2.5f + static_cast<float>(std::rand() % 3001) / 1000.0f;
                        std::cout << "[Admiracion] Luces de emergencia vuelven. Proximo apagon ~"
                                  << admBlackoutNext << "s\n";
                    }
                }
                else
                {
                    admBlackoutNext -= deltaTime;
                    if (admBlackoutNext <= 0.0f)
                    {
                        admBlackoutTimer = 1.5f;
                        std::cout << "[Admiracion] APAGON TOTAL ~" << admBlackoutTimer
                                  << "s (usa la linterna F)\n";
                    }
                }
            }

            admTotalBlackout = (admBlackoutTimer > 0.0f);
            if (admTotalBlackout)
            {
                admFlicker = 0.0f;
            }
            else
            {
                const float gt = static_cast<float>(glfwGetTime());
                admFlicker = 0.72f + 0.22f * std::sin(gt * 9.0f) * std::sin(gt * 3.7f);
                const float frac = gt * 0.37f - std::floor(gt * 0.37f);
                if (frac < 0.04f)
                    admFlicker *= 0.35f;
                admFlicker = glm::clamp(admFlicker, 0.18f, 1.0f);
            }
            g_admFlicker = admFlicker;
        }
        else
        {
            g_admFlicker = 1.0f;
        }

        // Iluminacion (lit/dark + techos coherentes + linterna que sigue la mirada)
        backroomsShader->setVec3("viewPos", eyePos);
        backroomsShader->setFloat("shininess", 30.0f);
        backroomsShader->setBool("lightsBlackout",
            lightsBlackoutMode || (admiracionOn && admTotalBlackout));
        backroomsShader->setFloat("admiracionAmount", admiracionOn ? 1.0f : 0.0f);
        backroomsShader->setFloat("admiracionFlicker", admiracionOn ? admFlicker : 1.0f);
        backroomsShader->setVec3("dirLight.direction", glm::vec3(-0.2f, -1.0f, -0.3f));

        // Direccional segun modo de luces
        if (lightsBlackoutMode || (admiracionOn && admTotalBlackout))
        {
            backroomsShader->setVec3("dirLight.ambient", glm::vec3(0.0f));
            backroomsShader->setVec3("dirLight.diffuse", glm::vec3(0.0f));
            backroomsShader->setVec3("dirLight.specular", glm::vec3(0.0f));
        }
        else if (admiracionOn)
        {
            const float fillPulse = 0.55f + 0.45f * admFlicker;
            backroomsShader->setVec3("dirLight.ambient",
                glm::vec3(0.085f, 0.012f, 0.010f) * fillPulse);
            backroomsShader->setVec3("dirLight.diffuse",
                glm::vec3(0.18f, 0.025f, 0.02f) * admFlicker);
            backroomsShader->setVec3("dirLight.specular",
                glm::vec3(0.12f, 0.03f, 0.02f) * admFlicker);
        }
        else
        {
            backroomsShader->setVec3("dirLight.ambient", glm::vec3(0.010f, 0.009f, 0.008f));
            backroomsShader->setVec3("dirLight.diffuse", glm::vec3(0.028f, 0.026f, 0.022f));
            backroomsShader->setVec3("dirLight.specular", glm::vec3(0.04f, 0.04f, 0.035f));
        }
        int lightIndex = 0;

        // Point lights de techo cercanas (si no hay apagon total)
        std::vector<size_t> nearLightIndices;
        nearLightIndices.reserve(64);
        if (!(admiracionOn && admTotalBlackout))
        {
            for (size_t i = 0; i < ceilingLights.size(); ++i)
            {
                if (!ceilingLights[i].isOn)
                    continue;
                if (distXZ(camera.Position, ceilingLights[i].position) <= STREAM_LIGHT_RADIUS)
                    nearLightIndices.push_back(i);
            }
        }
        std::sort(nearLightIndices.begin(), nearLightIndices.end(), [&](size_t a, size_t b) {
            return distXZ(camera.Position, ceilingLights[a].position) < distXZ(camera.Position, ceilingLights[b].position);
            });

        for (size_t si = 0; si < nearLightIndices.size() && lightIndex < STREAM_MAX_POINT_LIGHTS; si++)
        {
            size_t i = nearLightIndices[si];
            std::string base = "pointLights[" + std::to_string(lightIndex) + "].";
            backroomsShader->setVec3(base + "position", ceilingLights[i].position);
            // Atenuacion de las point lights
            backroomsShader->setFloat(base + "constant", 1.0f);
            backroomsShader->setFloat(base + "linear", 0.048f);
            backroomsShader->setFloat(base + "quadratic", 0.0055f);

            float minDistance = 1e9f;
            if (Survival_HasLiveMonster())
            {
                minDistance = glm::distance(ceilingLights[i].position, Survival_MonsterPosition());
            }
            for (const Instance& demon : demonInstances)
            {
                if (Survival_HasLiveMonster()) break;
                float d = glm::distance(ceilingLights[i].position, demon.position);
                if (d < minDistance)
                    minDistance = d;
            }
            float monsterFactor = 1.0f;
            if (minDistance < 15.0f)
                monsterFactor = glm::clamp((minDistance - 4.0f) / 11.0f, 0.12f, 1.0f);

            // Color de la luz (normal o admiracion)
            glm::vec3 baseColor(0.96f, 0.93f, 0.86f);
            glm::vec3 diffuse = baseColor * 0.46f * monsterFactor;
            glm::vec3 specular = baseColor * 0.07f * monsterFactor;
            glm::vec3 ambient = baseColor * 0.028f * monsterFactor;
            if (admiracionOn)
            {
                const glm::vec3 redBase(1.0f, 0.12f, 0.08f);
                diffuse  = redBase * (0.55f * admFlicker * monsterFactor);
                ambient  = redBase * (0.10f * admFlicker * monsterFactor);
                specular = redBase * (0.08f * admFlicker * monsterFactor);
            }

            // Fade con la distancia a la camara
            float distToCam = glm::distance(camera.Position, ceilingLights[i].position);
            float distanceFade = 1.0f - glm::clamp((distToCam - 72.0f) / 28.0f, 0.0f, 1.0f);
            diffuse *= distanceFade;
            specular *= distanceFade;
            ambient *= distanceFade;

            backroomsShader->setVec3(base + "ambient", ambient);
            backroomsShader->setVec3(base + "diffuse", diffuse);
            backroomsShader->setVec3(base + "specular", specular);
            lightIndex++;
        }

        // Lamparas de escritorio
        if (!lightsBlackoutMode && !(admiracionOn && admTotalBlackout))
        {
            for (size_t i = 0; i < deskLampPositions.size() && lightIndex < STREAM_MAX_POINT_LIGHTS + 8; i++)
            {
                if (distXZ(camera.Position, deskLampPositions[i]) > STREAM_DRAW_RADIUS)
                    continue;

                std::string base = "pointLights[" + std::to_string(lightIndex) + "].";
                backroomsShader->setVec3(base + "position", deskLampPositions[i]);
                backroomsShader->setFloat(base + "constant", 1.0f);
                backroomsShader->setFloat(base + "linear", 0.12f);
                backroomsShader->setFloat(base + "quadratic", 0.18f);
                if (admiracionOn)
                {
                    backroomsShader->setVec3(base + "ambient",
                        glm::vec3(0.06f, 0.008f, 0.006f) * admFlicker);
                    backroomsShader->setVec3(base + "diffuse",
                        glm::vec3(0.42f, 0.06f, 0.04f) * admFlicker);
                    backroomsShader->setVec3(base + "specular",
                        glm::vec3(0.28f, 0.05f, 0.04f) * admFlicker);
                }
                else
                {
                    backroomsShader->setVec3(base + "ambient", glm::vec3(0.01f, 0.005f, 0.015f));
                    backroomsShader->setVec3(base + "diffuse", glm::vec3(0.28f, 0.14f, 0.35f));
                    backroomsShader->setVec3(base + "specular", glm::vec3(0.22f, 0.12f, 0.28f));
                }
                lightIndex++;
            }
        }
        // Luz tenue anclada al monstruo vivo durante la persecucion, para que
        // siga siendo visible aun en apagon total del modo admiracion.
        if (Survival_HasLiveMonster() && lightIndex < STREAM_MAX_POINT_LIGHTS + 8)
        {
            const SurvivalPhase lp = Survival_GetPhase();
            if (lp == SurvivalPhase::Chase || lp == SurvivalPhase::Rage)
            {
                glm::vec3 mp = Survival_MonsterPosition();
                mp.y += 1.4f;
                std::string base = "pointLights[" + std::to_string(lightIndex) + "].";
                backroomsShader->setVec3(base + "position", mp);
                backroomsShader->setFloat(base + "constant", 1.0f);
                backroomsShader->setFloat(base + "linear", 0.22f);
                backroomsShader->setFloat(base + "quadratic", 0.28f);
                backroomsShader->setVec3(base + "ambient", glm::vec3(0.05f, 0.012f, 0.012f));
                backroomsShader->setVec3(base + "diffuse", glm::vec3(0.55f, 0.10f, 0.09f));
                backroomsShader->setVec3(base + "specular", glm::vec3(0.30f, 0.07f, 0.06f));
                lightIndex++;
            }
        }
        backroomsShader->setInt("numActivePointLights", lightIndex);

        // Linterna (sigue la camara)
        const glm::vec3 flashPos = eyePos + eyeFront * 0.12f;
        backroomsShader->setVec3("spotLight.position", flashPos);
        backroomsShader->setVec3("spotLight.direction", eyeFront);
        // Cono mas definido: centro claro vs corona exterior
        backroomsShader->setFloat("spotLight.cutOff", glm::cos(glm::radians(11.5f)));
        backroomsShader->setFloat("spotLight.outerCutOff", glm::cos(glm::radians(19.0f)));
        backroomsShader->setFloat("spotLight.constant", 1.0f);
        backroomsShader->setFloat("spotLight.linear", 0.018f);
        backroomsShader->setFloat("spotLight.quadratic", 0.0045f);
        if (flashlightOn)
        {
            backroomsShader->setVec3("spotLight.ambient", glm::vec3(0.0f));
            if (lightsBlackoutMode || (admiracionOn && admTotalBlackout))
            {
                backroomsShader->setVec3("spotLight.diffuse", glm::vec3(0.72f, 0.70f, 0.62f));
                backroomsShader->setVec3("spotLight.specular", glm::vec3(0.25f, 0.24f, 0.20f));
            }
            else
            {
                backroomsShader->setVec3("spotLight.diffuse", glm::vec3(1.35f, 1.30f, 1.15f));
                backroomsShader->setVec3("spotLight.specular", glm::vec3(1.0f, 0.98f, 0.88f));
            }
        }
        else
        {
            backroomsShader->setVec3("spotLight.ambient", glm::vec3(0.0f));
            backroomsShader->setVec3("spotLight.diffuse", glm::vec3(0.0f));
            backroomsShader->setVec3("spotLight.specular", glm::vec3(0.0f));
        }

        float aspect = (SCR_HEIGHT > 0) ? (float)SCR_WIDTH / (float)SCR_HEIGHT : 16.0f / 9.0f;
        glm::mat4 projection = glm::perspective(glm::radians(camera.Zoom), aspect, 0.1f, 200.0f);
        // View con head bob
        glm::mat4 view = glm::lookAt(eyePos, eyePos + eyeFront, eyeUp);
        backroomsShader->setMat4("projection", projection);
        backroomsShader->setMat4("view", view);

        // Nivel: wallpaper intermedio. Props: albedo real (sin filtro de pared).
        glm::mat4 model = glm::mat4(1.0f);
        model = glm::translate(model, backroomsPos);
        backroomsShader->setMat4("model", model);
        backroomsShader->setBool("applyLevelWallpaper", true);
        backroomsModel->Draw(*backroomsShader);
        if (border)
            border->Draw(*backroomsShader);

        backroomsShader->setBool("applyLevelWallpaper", false);
        // Props (ya precargados) + culling por distancia al dibujar
        if (surveillanceCameraModel)
            drawCameraInstances(*backroomsShader, *surveillanceCameraModel, cameraInstances, glm::vec3(1.0f), STREAM_DRAW_RADIUS);
        if (officeFurnitureModel)
            drawInstances(*backroomsShader, *officeFurnitureModel, officeInstances, false, STREAM_DRAW_RADIUS);
        if (oldPaperBoxesModel)
            drawInstances(*backroomsShader, *oldPaperBoxesModel, boxesInstances, true, STREAM_DRAW_RADIUS);
        if (monsterAlienModel)
        {
            if (Survival_HasLiveMonster())
            {
                Instance live{};
                live.position = Survival_MonsterPosition();
                live.rotationDeg = glm::vec3(0.0f, Survival_MonsterYawDeg(), 0.0f);
                live.scale = Survival_MonsterScale();
                std::vector<Instance> one{ live };
                drawInstances(*backroomsShader, *monsterAlienModel, one, false, STREAM_DRAW_RADIUS);
            }
            else
            {
                drawInstances(*backroomsShader, *monsterAlienModel, demonInstances, false, STREAM_DRAW_RADIUS);
            }
        }
        if (sciFiComputerModel)
            drawInstances(*backroomsShader, *sciFiComputerModel, computerInstances, false, STREAM_DRAW_RADIUS);
        if (publicPhoneBoothModel)
            drawInstances(*backroomsShader, *publicPhoneBoothModel, boothInstances, false, STREAM_DRAW_RADIUS);

        // las sombras planas siempre se dibujan; con la linterna encendida solo se atenuan
        // dentro de su cono, fuera del circulo la sombra se queda igual
        {
            glm::vec3 lightDirection(-0.2f, -1.0f, -0.3f);

            cubeShader->use();
            cubeShader->setMat4("projection", projection);
            cubeShader->setMat4("view", view);
            cubeShader->setBool("flashlightOn", flashlightOn);
            cubeShader->setVec3("flashPos", flashPos);
            cubeShader->setVec3("flashDir", eyeFront);
            // Mismos angulos que spotLight del mundo (circulo de luz)
            cubeShader->setFloat("flashCutOff", glm::cos(glm::radians(11.5f)));
            cubeShader->setFloat("flashOuterCutOff", glm::cos(glm::radians(19.0f)));
            cubeShader->setFloat("flashRange", 32.0f);

            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glEnable(GL_STENCIL_TEST);

            glStencilFunc(GL_EQUAL, 0, 0xFF);
            glStencilOp(GL_KEEP, GL_KEEP, GL_INCR);
            glDepthMask(GL_FALSE);

            cubeShader->setVec4("cubeColor", glm::vec4(0.0f, 0.0f, 0.0f, 0.50f));

            if (officeFurnitureModel)
                drawPlanarShadows(*cubeShader, *officeFurnitureModel, officeInstances, floorY, lightDirection, false, STREAM_SHADOW_RADIUS);
            if (oldPaperBoxesModel)
                drawPlanarShadows(*cubeShader, *oldPaperBoxesModel, boxesInstances, floorY, lightDirection, true, STREAM_SHADOW_RADIUS);
            if (monsterAlienModel)
            {
                if (Survival_HasLiveMonster())
                {
                    Instance live{};
                    live.position = Survival_MonsterPosition();
                    live.rotationDeg = glm::vec3(0.0f, Survival_MonsterYawDeg(), 0.0f);
                    live.scale = Survival_MonsterScale();
                    std::vector<Instance> one{ live };
                    drawPlanarShadows(*cubeShader, *monsterAlienModel, one, floorY, lightDirection, false, STREAM_SHADOW_RADIUS);
                }
                else
                {
                    drawPlanarShadows(*cubeShader, *monsterAlienModel, demonInstances, floorY, lightDirection, false, STREAM_SHADOW_RADIUS);
                }
            }
            if (sciFiComputerModel)
                drawPlanarShadows(*cubeShader, *sciFiComputerModel, computerInstances, floorY, lightDirection, false, STREAM_SHADOW_RADIUS);
            if (publicPhoneBoothModel)
                drawPlanarShadows(*cubeShader, *publicPhoneBoothModel, boothInstances, floorY, lightDirection, false, STREAM_SHADOW_RADIUS);

            glDepthMask(GL_TRUE);
            glDisable(GL_STENCIL_TEST);
            glDisable(GL_BLEND);
        }

        // Fundido de negro al entrar al juego (no se ve frame congelado)
        if (appState == AppState::FadeIn || fadeBlack > 0.001f)
            drawBlackOverlay(fadeBlack);

        // HUD supervivencia: SOLO digitos serif (sin barra, sin panel, sin fase)
        if (appState == AppState::Playing && Survival_IsCycleActive()
            && (Survival_GetPhase() == SurvivalPhase::Hunt
                || Survival_GetPhase() == SurvivalPhase::Chase
                || Survival_GetPhase() == SurvivalPhase::Rage)
            && !Survival_TitleCardActive())
        {
            const float aspect = (float)std::max(1u, SCR_WIDTH) / (float)std::max(1u, SCR_HEIGHT);
            const SurvivalPhase ph = Survival_GetPhase();
            const float secs = Survival_TimerSeconds();
            char tbuf[16];
            Survival_FormatTimer(secs, tbuf, sizeof(tbuf));

            // Color: crema muerte; rojo suave en Rage; frio en Chase
            float tr = 0.96f, tg = 0.93f, tb = 0.86f;
            if (ph == SurvivalPhase::Chase) { tr = 0.88f; tg = 0.90f; tb = 0.98f; }
            if (ph == SurvivalPhase::Rage)  { tr = 1.0f;  tg = 0.42f; tb = 0.36f; }

            // Rebake solo si cambia el string/color fase
            static std::string s_timerSerifKey;
            static TextBake s_timerSerif{};
            static SurvivalPhase s_timerPh = SurvivalPhase::Inactive;
            char keyBuf[48];
            std::snprintf(keyBuf, sizeof(keyBuf), "%s|%d", tbuf, (int)ph);
            if (s_timerSerifKey != keyBuf || s_timerSerif.tex == 0)
            {
                Text_Free(s_timerSerif);
                s_timerSerifKey = keyBuf;
                s_timerPh = ph;
                const int px = (ph == SurvivalPhase::Rage) ? 92 : 84;
                // Times New Roman, sin fondo
                s_timerSerif = Text_Bake(tbuf, px, tr, tg, tb, 0.f, 0.f, 0.f, 0.f, 0, L"Times New Roman");
            }

            const float cy = 0.88f;
            float alpha = 0.92f;
            // Ultimos 10s: pulso de muerte
            if (secs <= 10.0f)
                alpha = 0.55f + 0.45f * std::abs(std::sin((float)glfwGetTime() * 4.5f));
            float halfH = (ph == SurvivalPhase::Rage) ? 0.055f : 0.048f;
            halfH *= std::clamp(Survival_TimerHudScale(), 1.0f, 1.25f);
            if (s_timerSerif.tex)
                Text_DrawCentered(s_timerSerif.tex, s_timerSerif.width, s_timerSerif.height,
                    0.0f, cy, halfH, aspect, alpha,
                    splashVAO, splashVBO, splashShader.get());

            // Aviso de despertar (Chase, gracia): cuenta + instrucción de correr.
            if (Survival_IsChaseGrace())
            {
                const int gsec = (int)std::ceil(Survival_ChaseGraceSeconds());
                char wbuf[64];
                std::snprintf(wbuf, sizeof(wbuf), "El monstruo se despertará en: %d", gsec);
                static std::string s_wakeKey;
                static TextBake s_wakeBake{};
                if (s_wakeKey != wbuf || s_wakeBake.tex == 0)
                {
                    Text_Free(s_wakeBake);
                    s_wakeKey = wbuf;
                    s_wakeBake = Text_Bake(wbuf, 54, 1.0f, 0.86f, 0.5f, 0.f, 0.f, 0.f, 0.f, 0, L"Times New Roman");
                }
                if (s_wakeBake.tex)
                    Text_DrawCentered(s_wakeBake.tex, s_wakeBake.width, s_wakeBake.height,
                        0.0f, 0.06f, 0.045f, aspect, 0.95f,
                        splashVAO, splashVBO, splashShader.get());

                static TextBake s_runBake{};
                static bool s_runReady = false;
                if (!s_runReady)
                {
                    s_runBake = Text_Bake("¡Corre si quieres sobrevivir!", 46, 1.0f, 0.3f, 0.26f,
                        0.f, 0.f, 0.f, 0.f, 0, L"Times New Roman");
                    s_runReady = true;
                }
                const float rp = 0.6f + 0.4f * std::abs(std::sin((float)glfwGetTime() * 4.0f));
                if (s_runBake.tex)
                    Text_DrawCentered(s_runBake.tex, s_runBake.width, s_runBake.height,
                        0.0f, -0.04f, 0.04f, aspect, rp,
                        splashVAO, splashVBO, splashShader.get());
            }

            // Prompt flotante 2D anclado al monstruo
            if (Survival_WantsKillPrompt() && Survival_HasLiveMonster())
            {
                const char* prompt = Survival_PromptText();
                ensureText(s_promptBake, s_promptKey, prompt ? prompt : "Presiona E", 40, 1.0f, 0.92f, 0.35f);
                glm::vec3 world = Survival_MonsterPosition();
                world.y = camera.Position.y + 0.35f + 0.08f * std::sin((float)glfwGetTime() * 3.2f);
                glm::mat4 projection = glm::perspective(glm::radians(camera.Zoom), aspect, 0.1f, 200.0f);
                glm::mat4 view = camera.GetViewMatrix();
                glm::vec4 clip = projection * view * glm::vec4(world, 1.0f);
                if (clip.w > 0.15f)
                {
                    float ndcX = clip.x / clip.w;
                    float ndcY = clip.y / clip.w;
                    if (ndcX > -1.2f && ndcX < 1.2f && ndcY > -1.2f && ndcY < 1.2f)
                    {
                        float bob = 0.02f * std::sin((float)glfwGetTime() * 4.0f);
                        float flicker = 0.75f + 0.25f * std::abs(std::sin((float)glfwGetTime() * 9.0f));
                        if (s_promptBake.tex)
                            Text_DrawCentered(s_promptBake.tex, s_promptBake.width, s_promptBake.height,
                                ndcX, ndcY + 0.08f + bob, 0.04f, aspect, flicker,
                                splashVAO, splashVBO, splashShader.get());
                    }
                }
            }
        }

        // Tarjeta de titulo Chase/Rage (~1s) , PNG HTML/serif de calidad
        if (appState == AppState::Playing && Survival_TitleCardActive())
        {
            const float aspect = (float)std::max(1u, SCR_WIDTH) / (float)std::max(1u, SCR_HEIGHT);
            const float a = Survival_TitleCardAlpha();
            const int kind = Survival_TitleCardKind();
            // Oscurecer mundo
            drawNdcTexturedQuad(-1.f, -1.f, 1.f, 1.f, barKnobEdgeTex, 0, 0, 1, 1, 0.55f * a);
            unsigned int card = 0;
            if (kind == 1) card = titleLucesTex;       // A2
            else if (kind == 2) card = titleHuyeB1Tex; // B1
            else if (kind == 3) card = titleHuyeB3Tex; // B3
            if (card)
            {
                // Banner ancho centrado.
                // loadUiTexture: stbi_flip true + UVs normales (no Text_DrawNdcQuad).
                const float halfH = 0.26f;
                const float halfW = halfH * (1600.f / 500.f) / aspect;
                const float y0 = -halfH * 0.2f;
                const float y1 = halfH * 0.9f;
                drawNdcTexturedQuad(-halfW, y0, halfW, y1, card, 0.f, 0.f, 1.f, 1.f, a);
            }
        }

        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    AudioBgm_Shutdown();
    glfwTerminate();
    return 0;
}

// callbacks (input, mouse, resize)

static void startGameFromMenu(GLFWwindow* window)
{
    appState = AppState::FadeIn;
    fadeBlack = 1.0f;
    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    firstMouse = true;
    glfwSetWindowTitle(window, "Backrooms - Grupo 7");
    // Musica por rachas en juego
    AudioBgm_EnterGameAmbient();
    AudioBgm_PhoneRingReset();
    AudioBgm_DistantVoicesReset();
    Survival_SetCollisionProbe(survivalCorrectMove, &colManager);
    if (Survival_IsEnabled())
        survivalRemoveDemonCollisions();
    Survival_OnEnterPlaying(camera.Position);
    survivalSyncLights();
    AudioBgm_AmbientBuzzSetActive(lightPresentation == LightPresentation::Normal);
    std::cout << "[UI] COMENZAR -> fundido al juego\n";
}

static void resumeFromPause(GLFWwindow* window)
{
    appState = AppState::Playing;
    fadeBlack = 0.0f;
    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    firstMouse = true;
    Survival_SetCollisionProbe(survivalCorrectMove, &colManager);
    // Si el check esta ON pero el ciclo no arranco (bug legacy / race), forzarlo
    if (Survival_IsEnabled() && !Survival_IsCycleActive())
    {
        survivalRemoveDemonCollisions();
        Survival_StartCycle(camera.Position);
        std::cout << "[Survival] CONTINUAR: ciclo Hunt arrancado (estaba Inactive)\n";
    }
    else if (Survival_IsEnabled())
    {
        survivalRemoveDemonCollisions();
    }
    survivalSyncLights();
    if (Survival_IsEnabled() && Survival_IsCycleActive())
        glfwSetWindowTitle(window, (std::string("Backrooms | ") + Survival_WindowStatus()).c_str());
    else
        glfwSetWindowTitle(window, "Backrooms - Grupo 7");
    std::cout << "[UI] CONTINUAR\n";
}

// procesa la entrada del teclado
void processInput(GLFWwindow* window)
{
    static bool escWasDown = false;
    static bool enterWasDown = false;
    const bool escDown = glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS;
    const bool enterDown = glfwGetKey(window, GLFW_KEY_ENTER) == GLFW_PRESS
        || glfwGetKey(window, GLFW_KEY_KP_ENTER) == GLFW_PRESS;

    // menu
    if (appState == AppState::Menu)
    {
        if (enterDown && !enterWasDown)
            startGameFromMenu(window);
        if (escDown && !escWasDown)
            glfwSetWindowShouldClose(window, true);
        escWasDown = escDown;
        enterWasDown = enterDown;
        return;
    }

    // Ending cinematica
    if (appState == AppState::Ending)
    {
        if ((enterDown && !enterWasDown) || (escDown && !escWasDown))
        {
            if (Survival_TryAdvanceCinematicConfirm() || Survival_CinematicDone())
            {
                Survival_StopCycle();
                appState = AppState::Menu;
                glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
                firstMouse = true;
                AudioBgm_EnterMenuLoop();
                lightPresentation = LightPresentation::Normal;
                applyLightPresentation();
                glfwSetWindowTitle(window, "Backrooms - Grupo 7");
            }
        }
        escWasDown = escDown;
        enterWasDown = enterDown;
        return;
    }

    // Jumpscare: sin control
    if (appState == AppState::Jumpscare)
    {
        escWasDown = escDown;
        enterWasDown = enterDown;
        return;
    }

    // pausa
    if (appState == AppState::Paused)
    {
        static bool rWasDown = false;
        static bool lWasDown = false;
        static bool mWasDown = false;
        static bool gWasDown = false;
        const bool rDown = glfwGetKey(window, GLFW_KEY_R) == GLFW_PRESS;
        const bool lDown = glfwGetKey(window, GLFW_KEY_L) == GLFW_PRESS;
        const bool mDown = glfwGetKey(window, GLFW_KEY_M) == GLFW_PRESS;
        const bool gDown = glfwGetKey(window, GLFW_KEY_G) == GLFW_PRESS;
        if (enterDown && !enterWasDown)
            resumeFromPause(window);
        if (escDown && !escWasDown)
            resumeFromPause(window); // Esc en pausa es continuar (tambien se puede salir con boton)
        if (rDown && !rWasDown)
            restartApplicationFast(); // R es recarga rapida del proceso
        if (lDown && !lWasDown)
            toggleLightsBlackoutMode(); // L es APAGAR / ENCENDER LUCES
        if (mDown && !mWasDown)
            toggleAdmiracionMode(); // M es MODO ADMIRACION
        if (gDown && !gWasDown)
            survivalToggleFromUi(camera.Position); // G es check supervivencia
        rWasDown = rDown;
        lWasDown = lDown;
        mWasDown = mDown;
        gWasDown = gDown;
        escWasDown = escDown;
        enterWasDown = enterDown;
        return;
    }

    // fade in + playing: se puede caminar en cuanto empieza el fundido
    // (el fade es visual; el jugador no se queda "congelado" esperando)
    if (appState == AppState::FadeIn || appState == AppState::Playing)
    {
        if (appState == AppState::Playing && escDown && !escWasDown)
        {
            appState = AppState::Paused;
            headBobAmount = 0.0f;
            headBobOffset = glm::vec3(0.0f);
            playerIsWalking = false;
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
            firstMouse = true;
            glfwSetWindowTitle(window, "Backrooms - Pausa");
            std::cout << "[UI] PAUSA\n";
            escWasDown = escDown;
            enterWasDown = enterDown;
            return;
        }
        // Durante fade no pausar con Esc (evita cortar la animacion)
        if (appState == AppState::FadeIn)
            escWasDown = escDown;
        else
            escWasDown = escDown;
        enterWasDown = enterDown;

        // Congelar movimiento durante titulo Chase/Rage (~1s)
        const bool freezeForTitle = Survival_TitleCardActive();
        const bool wantW = !freezeForTitle && glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS;
        const bool wantS = !freezeForTitle && glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS;
        const bool wantA = !freezeForTitle && glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS;
        const bool wantD = !freezeForTitle && glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS;
        // Sprint: Ctrl + W
        const bool wantCtrl =
            glfwGetKey(window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS ||
            glfwGetKey(window, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS;
        const bool playerIsSprinting = wantW && wantCtrl && !wantS;
        playerIsWalking = wantW || wantS || wantA || wantD;

        // 1.0 caminar | ~1.55 sprint (mas rapido + bob/pasos mas frecuentes)
        const float moveMul = playerIsSprinting ? 1.55f : 1.0f;
        if (wantW)
            camera.ProcessKeyboard(FORWARD, deltaTime, colManager, moveMul);
        if (wantS)
            camera.ProcessKeyboard(BACKWARD, deltaTime, colManager, 1.0f);
        if (wantA)
            camera.ProcessKeyboard(LEFT, deltaTime, colManager, playerIsSprinting ? 1.15f : 1.0f);
        if (wantD)
            camera.ProcessKeyboard(RIGHT, deltaTime, colManager, playerIsSprinting ? 1.15f : 1.0f);

        // Balanceo de camara y pasos
        {
            const bool admShake = (lightPresentation == LightPresentation::Admiracion);
            const float targetBob = playerIsWalking ? 1.0f : (admShake ? 0.55f : 0.0f);
            const float bobLerp = 1.0f - std::exp(-deltaTime * (playerIsWalking ? 10.0f : 8.0f));
            headBobAmount += (targetBob - headBobAmount) * bobLerp;

            const float bobHz = playerIsSprinting ? 9.2f
                : (playerIsWalking ? 9.5f : 7.0f);

            if (playerIsWalking || admShake)
                headBobTimer += deltaTime * bobHz;
            else
                headBobTimer += deltaTime * 2.0f;

            // Amplitudes (mas fuerte en admiracion)
            float bobAmpY = playerIsSprinting ? 1.20f : 1.0f;
            float bobAmpX = playerIsSprinting ? 1.35f : 1.0f;
            float baseSway = 0.055f;
            const float despairSprint = 4.0f; // shake fuerte al correr en admiracion
            const float hSwayScale = 0.65f;   // menos desplazamiento lateral al sprint
            if (admShake)
            {
                baseSway = 0.078f;
                if (playerIsSprinting)
                {
                    bobAmpY = 2.05f * despairSprint;
                    bobAmpX = 4.50f * despairSprint * hSwayScale;
                }
                else if (playerIsWalking)
                {
                    bobAmpY = 1.75f;
                    bobAmpX = 2.75f;
                }
                else
                {
                    bobAmpY = 0.95f;
                    bobAmpX = 1.45f;
                }
            }

            const float sway = std::sin(headBobTimer) * baseSway * headBobAmount * bobAmpX;
            const float bobY = std::sin(headBobTimer * 2.0f) * 0.038f * headBobAmount * bobAmpY;

            glm::vec3 offset = camera.Right * sway + camera.WorldUp * bobY;
            if (admShake)
            {
                const float t = static_cast<float>(glfwGetTime());
                // Micro shake
                const float shake =
                    std::sin(t * 31.0f) * 0.42f +
                    std::sin(t * 47.0f) * 0.33f +
                    std::sin(t * 19.0f) * 0.25f;
                float shakeAmp = playerIsSprinting ? 0.042f
                    : (playerIsWalking ? 0.032f : 0.024f);
                if (playerIsSprinting)
                    shakeAmp *= despairSprint;
                offset += camera.Right * (shake * shakeAmp);
                offset += camera.WorldUp * (shake * shakeAmp * 0.70f);
                // Balanceo lateral al correr
                if (playerIsSprinting)
                {
                    offset += camera.Right * (std::sin(t * 3.8f) * 0.078f * despairSprint * hSwayScale * headBobAmount);
                    offset += camera.Right * (std::sin(headBobTimer) * 0.055f * despairSprint * hSwayScale * headBobAmount);
                    // Pico vertical
                    offset += camera.WorldUp * (std::abs(std::sin(headBobTimer)) * 0.028f * despairSprint * headBobAmount);
                }
                else if (playerIsWalking)
                {
                    offset += camera.Right * (std::sin(t * 5.0f) * 0.032f * headBobAmount);
                }
            }
            headBobOffset = offset;

            // Pasos al ritmo del bob
            static float prevSwaySin = 0.0f;
            const float swaySin = std::sin(headBobTimer);
            if (playerIsWalking && headBobAmount > 0.35f && appState == AppState::Playing)
            {
                const bool crossed =
                    (prevSwaySin <= 0.0f && swaySin > 0.0f) ||
                    (prevSwaySin >= 0.0f && swaySin < 0.0f);
                if (crossed)
                {
                    const float stepVol = playerIsSprinting ? 0.16f : 0.1375f;
                    AudioBgm_PlaySfx("sounds/footstep.wav", stepVol);
                }
            }
            if (!playerIsWalking)
                prevSwaySin = 0.0f;
            else
                prevSwaySin = swaySin;
        }

        static bool fKeyWasPressed = false;
        static bool eKeyWasPressed = false;
        if (appState == AppState::Playing)
        {
            // HUD titulo supervivencia
            if (Survival_IsEnabled() && Survival_IsCycleActive())
            {
                char t[200];
                std::snprintf(t, sizeof(t), "Backrooms | %s", Survival_WindowStatus());
                glfwSetWindowTitle(window, t);
            }

            // Hunt cerca monstruo: E es matar
            if (glfwGetKey(window, GLFW_KEY_E) == GLFW_PRESS)
            {
                if (!eKeyWasPressed)
                {
                    Survival_ConsumeKillInteract();
                    eKeyWasPressed = true;
                }
            }
            else if (glfwGetKey(window, GLFW_KEY_E) == GLFW_RELEASE)
            {
                eKeyWasPressed = false;
            }

            // F es solo linterna
            if (glfwGetKey(window, GLFW_KEY_F) == GLFW_PRESS)
            {
                if (!fKeyWasPressed)
                {
                    flashlightOn = !flashlightOn;
                    AudioBgm_PlaySfx("sounds/flashlight.mp3", 0.9f);
                    fKeyWasPressed = true;
                }
            }
            else if (glfwGetKey(window, GLFW_KEY_F) == GLFW_RELEASE)
            {
                fKeyWasPressed = false;
            }

            // DEBUG PREVIEW (quitar en el futuro)
            // Fuerza jumpscare/finales sin jugar el ciclo. Ver game_survival.h.
#if SURVIVAL_DEBUG_PREVIEW
            {
                static const int kDbgN = 6;
                static bool dbgWas[kDbgN] = { false, false, false, false, false, false };
                const int dbgKeys[kDbgN] = {
                    GLFW_KEY_F4, GLFW_KEY_F5, GLFW_KEY_F6,
                    GLFW_KEY_F7, GLFW_KEY_F8, GLFW_KEY_F9
                };
                const SurvivalPhase dbgPhase[kDbgN] = {
                    SurvivalPhase::Chase,         // F4 es CHASE ("Luces Apagadas" + gracia 5s)
                    SurvivalPhase::Rage,          // F5 es RAGE ("¡Huye!" + persecución ya despierta)
                    SurvivalPhase::EndingKill,    // F6 es VICTORIA (mataste)
                    SurvivalPhase::EndingEscape,  // F7 es ESCAPASTE
                    SurvivalPhase::EndingLose,    // F8 es PERDISTE
                    SurvivalPhase::Jumpscare      // F9 es JUMPSCARE
                };
                for (int i = 0; i < kDbgN; ++i)
                {
                    const bool down = glfwGetKey(window, dbgKeys[i]) == GLFW_PRESS;
                    if (down && !dbgWas[i])
                        Survival_DebugForcePhase(dbgPhase[i], camera.Position);
                    dbgWas[i] = down;
                }
            }
#endif
            // FIN DEBUG PREVIEW
        }
        return;
    }

    escWasDown = escDown;
    enterWasDown = enterDown;
}

void mouse_button_callback(GLFWwindow* window, int button, int action, int mods)
{
    (void)mods;
    if (button != GLFW_MOUSE_BUTTON_LEFT || action != GLFW_PRESS)
        return;

    double mx = 0.0, my = 0.0;
    glfwGetCursorPos(window, &mx, &my);
    // Convertir a espacio del framebuffer si hay DPI scaling
    int winW = 0, winH = 0, fbW = 0, fbH = 0;
    glfwGetWindowSize(window, &winW, &winH);
    glfwGetFramebufferSize(window, &fbW, &fbH);
    if (winW > 0 && winH > 0)
    {
        mx = mx * (double)fbW / (double)winW;
        my = my * (double)fbH / (double)winH;
    }

    if (appState == AppState::Ending)
    {
        if (Survival_CinematicDone() || Survival_TryAdvanceCinematicConfirm())
        {
            Survival_StopCycle();
            appState = AppState::Menu;
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
            firstMouse = true;
            AudioBgm_EnterMenuLoop();
            lightPresentation = LightPresentation::Normal;
            applyLightPresentation();
            glfwSetWindowTitle(window, "Backrooms - Grupo 7");
        }
        return;
    }

    if (appState != AppState::Menu && appState != AppState::Paused)
        return;

    if (uiHitPrimaryButton(mx, my))
    {
        if (appState == AppState::Menu)
            startGameFromMenu(window);
        else
            resumeFromPause(window); // CONTINUAR
    }
    else if (uiHitSecondaryButton(mx, my))
    {
        if (appState == AppState::Paused)
            toggleLightsBlackoutMode(); // APAGAR / ENCENDER LUCES
        else
            glfwSetWindowShouldClose(window, true); // menu inicio: SALIR
    }
    else if (appState == AppState::Paused && uiHitAdmiracionButton(mx, my))
    {
        toggleAdmiracionMode();
    }
    else if (appState == AppState::Paused && uiHitTertiaryButton(mx, my))
    {
        restartApplicationFast(); // REINICIAR
    }
    else if (appState == AppState::Paused && uiHitQuaternaryButton(mx, my))
    {
        glfwSetWindowShouldClose(window, true); // SALIR
    }
    else if (appState == AppState::Paused
        && Survival_HitCheckbox(mx, my, SCR_WIDTH, SCR_HEIGHT))
    {
        survivalToggleFromUi(camera.Position);
    }
}

void framebuffer_size_callback(GLFWwindow* window, int width, int height)
{
    if (width <= 0 || height <= 0)
        return;
    glViewport(0, 0, width, height);
    SCR_WIDTH = static_cast<unsigned int>(width);
    SCR_HEIGHT = static_cast<unsigned int>(height);
}

void mouse_callback(GLFWwindow* window, double xpos, double ypos)
{
    // Mirar en juego y durante el fundido (ya se puede mover)
    if (appState != AppState::Playing && appState != AppState::FadeIn)
    {
        lastX = static_cast<float>(xpos);
        lastY = static_cast<float>(ypos);
        firstMouse = true;
        return;
    }

    if (firstMouse)
    {
        lastX = static_cast<float>(xpos);
        lastY = static_cast<float>(ypos);
        firstMouse = false;
    }

    float xoffset = static_cast<float>(xpos) - lastX;
    float yoffset = lastY - static_cast<float>(ypos);

    lastX = static_cast<float>(xpos);
    lastY = static_cast<float>(ypos);

    camera.ProcessMouseMovement(xoffset, yoffset);
}

void scroll_callback(GLFWwindow* window, double xoffset, double yoffset)
{
    if (appState != AppState::Playing)
        return;
    camera.ProcessMouseScroll(static_cast<float>(yoffset));
}
