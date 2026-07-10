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

// Incluimos Assimp SOLO para probar que el vinculador (Linker) no de errores.
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

#include <shader.h>
#include <camera.h>
#include <model.h>
#include <collisions.h>

#include "audio_bgm.h"

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

// Volumen de la musica de fondo: bajo = se oye "a la distancia" (no tapa el ambiente).
// ~1/4 del valor anterior (0.18) para que se oiga muy de fondo / a la distancia
static const float BGM_DISTANT_VOLUME = 0.045f;

// Callbacks
void framebuffer_size_callback(GLFWwindow* window, int width, int height);
void mouse_callback(GLFWwindow* window, double xpos, double ypos);
void scroll_callback(GLFWwindow* window, double xoffset, double yoffset);
void processInput(GLFWwindow* window);
void mouse_button_callback(GLFWwindow* window, int button, int action, int mods);

// Settings (se actualizan al crear la ventana / resize)
unsigned int SCR_WIDTH = 1280;
unsigned int SCR_HEIGHT = 720;

// Estados de la app: no mostrar 3D congelado antes de poder jugar
enum class AppState
{
    Menu,      // COMENZAR / SALIR
    FadeIn,    // fundido de negro al mundo
    Playing,   // jugando
    Paused     // CONTINUAR / SALIR
};
static AppState appState = AppState::Menu;
static float fadeBlack = 1.0f;          // 1 = negro total, 0 = sin overlay
static const float FADE_IN_SECONDS = 0.65f; // fundido rapido (< 1 s)
static bool uiClickLatch = false;       // evita multi-click el mismo frame

// Streaming / culling por distancia (mismos modelos; solo carga y dibuja lo cercano)
static const float STREAM_LOAD_RADIUS   = 55.0f; // carga el modelo al acercarte
static const float STREAM_DRAW_RADIUS   = 48.0f; // no dibuja instancias lejanas
static const float STREAM_SHADOW_RADIUS = 28.0f; // sombras solo cerca
static const float STREAM_LIGHT_RADIUS  = 52.0f; // point lights del techo relevantes
static const int   STREAM_MAX_POINT_LIGHTS = 36;

// Camara: valores del repo original (commit a7354ab / main de GitHub)
// Position (0, 0, 3), Yaw -90, Pitch 0. NO usar floorY+1.7 (escala del mapa es mayor).
Camera camera(glm::vec3(0.0f, 0.0f, 3.0f));
float lastX = 640.0f;
float lastY = 360.0f;
bool firstMouse = true;

// Timing
float deltaTime = 0.0f;
float lastFrame = 0.0f;

// Posición del nivel
glm::vec3 backroomsPos(0.0f, -3.0f, 0.0f);

// Manager de colisiones
CollisionManager colManager;

bool flashlightOn = false;

// UI: fracciones de pantalla (menu 1920x1080). Botones centrados.
// COMENZAR/CONTINUAR ~ y = 0.5 + 60/1080 ≈ 0.556  |  SALIR ≈ 0.648
static bool uiHitPrimaryButton(double mx, double my)
{
    if (SCR_WIDTH == 0 || SCR_HEIGHT == 0) return false;
    double nx = mx / (double)SCR_WIDTH;
    double ny = my / (double)SCR_HEIGHT;
    const double cx = 0.5, cy = 0.556, hw = 0.11, hh = 0.042;
    return nx > cx - hw && nx < cx + hw && ny > cy - hh && ny < cy + hh;
}
static bool uiHitSecondaryButton(double mx, double my)
{
    if (SCR_WIDTH == 0 || SCR_HEIGHT == 0) return false;
    double nx = mx / (double)SCR_WIDTH;
    double ny = my / (double)SCR_HEIGHT;
    const double cx = 0.5, cy = 0.648, hw = 0.11, hh = 0.042;
    return nx > cx - hw && nx < cx + hw && ny > cy - hh && ny < cy + hh;
}

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

// ============================================================================
// ESTRUCTURAS Y TIPOS COMUNES
// ============================================================================

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

// ============================================================================
// FUNCIONES DE UTILIDAD
// ============================================================================

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

// ============================================================================
// FUNCIONES DE CÁLCULO DE BOUNDS
// ============================================================================

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

// ============================================================================
// FUNCIONES DE TRANSFORMACIÓN
// ============================================================================

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

// ============================================================================
// FUNCIONES DE COLOCACIÓN Y GENERACIÓN
// ============================================================================

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

// ============================================================================
// GENERACIÓN DE CÁMARAS
// ============================================================================

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

// ============================================================================
// GENERACIÓN DE INSTANCIAS EN EL PISO
// ============================================================================

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

// ============================================================================
// GENERACIÓN DE CABINAS TELEFÓNICAS
// ============================================================================

std::vector<Instance> generatePhoneBooths(const AABB& roomBounds, const AABB& boothBounds, float floorY, int targetCount)
{
    std::vector<Instance> out;
    out.reserve(targetCount);

    std::random_device rd;
    std::mt19937 rng(rd());

    const int maxAttempts = 30000;
    const float minDistance = 4.5f;
    const float wallClearance = 0.25f;

    for (int attempt = 0; attempt < maxAttempts && (int)out.size() < targetCount; ++attempt)
    {
        Instance instance{};
        instance.scale = randomRange(rng, 1.3f, 1.7f);
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

// ============================================================================
// FUNCIONES DE DIBUJADO
// ============================================================================

void drawPlanarShadows(Shader& shadowShader, Model& model, const std::vector<Instance>& instances, float floorY, glm::vec3 lightDir, bool firstMeshOnly = false, float maxDist = 1e9f)
{
    // Crear la matriz de proyección de sombra plana para GLM (Column-Major)
    glm::mat4 shadowMat(1.0f);

    // Evitar la división por cero si la luz apunta completamente horizontal
    if (std::abs(lightDir.y) < 0.001f) lightDir.y = -0.001f;

    // Columna 1 de la matriz: Proyecta la altura (Y) hacia los lados dependiendo de la inclinación de la luz
    shadowMat[1][0] = -lightDir.x / lightDir.y;
    shadowMat[1][1] = 0.0f;
    shadowMat[1][2] = -lightDir.z / lightDir.y;

    // Columna 3 de la matriz: Traslada la sombra proyectada al nivel exacto del suelo
    shadowMat[3][0] = floorY * (lightDir.x / lightDir.y);
    shadowMat[3][1] = floorY + 0.002f; // Offset milimétrico para evitar Z-Fighting (parpadeo de texturas)
    shadowMat[3][2] = floorY * (lightDir.z / lightDir.y);

    for (const Instance& instance : instances)
    {
        if (distXZ(camera.Position, instance.position) > maxDist)
            continue;

        // Matriz de transformación original de la instancia de objeto
        glm::mat4 modelMat = buildInstanceMatrix(instance);

        // La matriz final combina la proyección de sombra con la posición del objeto
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

// ============================================================================
// FUNCIONES DE COLISIONES
// ============================================================================

void addInstancesCollision(CollisionManager& manager, const Model& model, const std::vector<Instance>& instances, bool firstMeshOnly = false)
{
    const AABB localBounds = computeModelBounds(model, firstMeshOnly);

    for (const Instance& instance : instances)
    {
        const AABB worldBounds = transformBounds(localBounds, buildInstanceMatrix(instance));
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

struct CeilingLight
{
    glm::vec3 position;
    bool isOn;
};

// ============================================================================
// GENERAR LUCES DE TECHO EN CUADRICULA (CON CALIBRACIÓN)
// ============================================================================
void findCeilingLights(const Model& model, const glm::vec3& worldOffset, std::vector<CeilingLight>& lights)
{
    std::random_device rd;
    std::mt19937 rng(rd());
    std::uniform_real_distribution<float> roll(0.0f, 1.0f);

    // === VARIABLES DE CALIBRACIÓN ===
    // Modifica estos números (pueden ser positivos o negativos) para mover toda la cuadrícula
    float offsetX = 2.9f;   // <- Incrementa o disminuye para mover las luces en el eje X
    float offsetZ = -2.3f;  // <- Incrementa o disminuye para mover las luces en el eje Z

    float spacingX = 6.0f; // Espaciado en X entre lámparas
    float spacingZ = 8.0f; // Espaciado en Z entre lámparas
    // ================================

    float xMin = -182.4f + worldOffset.x;
    float xMax = 194.8f + worldOffset.x;
    float zMin = -185.7f + worldOffset.z;
    float zMax = 191.5f + worldOffset.z;

    float ceilY = 8.565f + worldOffset.y - 1.2f;

    for (float x = xMin + spacingX * 0.5f + offsetX; x < xMax; x += spacingX)
    {
        for (float z = zMin + spacingZ * 0.5f + offsetZ; z < zMax; z += spacingZ)
        {
            CeilingLight cl;
            cl.position = glm::vec3(x, ceilY, z);
            cl.isOn = roll(rng) > 0.40f;
            lights.push_back(cl);
        }
    }
    std::cout << "Ceiling lights generated: " << lights.size() << std::endl;
}


// ============================================================================
// OBTENER CENTRO LOCAL DE LAMPARA
// ============================================================================
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

// ============================================================================
// MAIN
// ============================================================================

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

    // monitor = NULL => modo ventana (no exclusive fullscreen)
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

    // --- Splash 2D (pantalla de carga real, no solo titulo de ventana) ---
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
        return tex;
    };

    unsigned int menuStartTex = 0;
    unsigned int menuPauseTex = 0;
    unsigned int digitsTex = loadUiTexture("textures/digits.png");
    unsigned int barBgTex = 0;
    unsigned int barFillTex = 0;
    {
        // texturas 1x1 para barra de progreso
        auto solidTex = [](unsigned char r, unsigned char g, unsigned char b) {
            unsigned int t = 0;
            glGenTextures(1, &t);
            glBindTexture(GL_TEXTURE_2D, t);
            unsigned char px[] = { r, g, b, 255 };
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glBindTexture(GL_TEXTURE_2D, 0);
            return t;
        };
        barBgTex = solidTex(40, 44, 52);
        barFillTex = solidTex(200, 180, 90);
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

        // Barra de progreso realista (NDC, zona inferior-centro)
        const float barX0 = -0.22f, barX1 = 0.22f;
        const float barY0 = -0.28f, barY1 = -0.24f;
        drawNdcTexturedQuad(barX0, barY0, barX1, barY1, barBgTex, 0, 0, 1, 1, 1.0f);
        float fillX1 = barX0 + (barX1 - barX0) * progress01;
        if (progress01 > 0.001f)
            drawNdcTexturedQuad(barX0, barY0, fillX1, barY1, barFillTex, 0, 0, 1, 1, 1.0f);

        // Porcentaje grande debajo de la barra
        drawPercentNumber(pct, 0.0f, -0.36f, 0.045f, 0.07f);

        glfwSwapBuffers(window);
        return true;
    };

    // Pesos relativos por etapa (aproximan tiempo real de I/O + Assimp + CPU)
    // Suma = 1.0
    const float W_SHADERS = 0.04f;
    const float W_COLLISION = 0.18f;
    const float W_LEVEL = 0.30f;
    const float W_BORDER = 0.12f;
    const float W_LIGHTS = 0.18f;
    const float W_AUDIO = 0.06f;
    const float W_SPAWN = 0.04f;
    const float W_UI = 0.03f;
    const float W_SETTLE = 0.05f; // espera final antes del menu
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

    // --- ETAPA 1: shaders de juego ---
    if (!drawSplashFrame("Backrooms - Cargando shaders...", progressBase + W_SHADERS * 0.3f)) { glfwTerminate(); return 0; }
    backroomsShader = std::make_unique<Shader>("shaders/backrooms.vs", "shaders/backrooms.fs");
    cubeShader = std::make_unique<Shader>("shaders/basico.vs", "shaders/basico.fs");
    progressBase += W_SHADERS;
    if (!drawSplashFrame("Backrooms - Shaders listos", progressBase)) { glfwTerminate(); return 0; }
    std::cout << "[Load] Shaders OK\n";

    // --- ETAPA 2: colisiones del mapa ---
    if (!drawSplashFrame("Backrooms - Cargando colisiones del mapa...", progressBase + W_COLLISION * 0.2f)) { glfwTerminate(); return 0; }
    backroomsCollisionsModel = std::make_unique<Model>("models/backrooms_level_0_collisions/backrooms.obj");
    roomLocalBounds = computeModelBounds(*backroomsCollisionsModel);
    roomWorldBounds = { roomLocalBounds.min + backroomsPos, roomLocalBounds.max + backroomsPos };
    roomCenter = (roomWorldBounds.min + roomWorldBounds.max) * 0.5f;
    floorY = roomWorldBounds.min.y;
    wallAnchors = collectWallAnchors(*backroomsCollisionsModel, backroomsPos);
    colManager.addStaticBox(*backroomsCollisionsModel, backroomsPos);
    progressBase += W_COLLISION;
    if (!drawSplashFrame("Backrooms - Colisiones listas", progressBase)) { glfwTerminate(); return 0; }
    std::cout << "[Load] Colisiones OK (meshes " << backroomsCollisionsModel->meshes.size() << ")\n";

    // --- ETAPA 3: nivel visual (suele ser la mas pesada) ---
    if (!drawSplashFrame("Backrooms - Cargando nivel (Backrooms)...", progressBase + W_LEVEL * 0.15f)) { glfwTerminate(); return 0; }
    backroomsModel = std::make_unique<Model>("models/backrooms_level_0/backrooms.obj");
    progressBase += W_LEVEL;
    if (!drawSplashFrame("Backrooms - Nivel listo", progressBase)) { glfwTerminate(); return 0; }
    std::cout << "[Load] Nivel visual OK\n";

    // --- ETAPA 4: border ---
    if (!drawSplashFrame("Backrooms - Cargando limites del mapa...", progressBase + W_BORDER * 0.2f)) { glfwTerminate(); return 0; }
    border = std::make_unique<Model>("models/border/border.obj");
    colManager.addStaticBox(*border, backroomsPos);
    progressBase += W_BORDER;
    if (!drawSplashFrame("Backrooms - Limites listos", progressBase)) { glfwTerminate(); return 0; }
    std::cout << "[Load] Border OK\n";

    // --- ETAPA 5: luces de techo (CPU pesada: miles de luces) ---
    if (!drawSplashFrame("Backrooms - Preparando luces...", progressBase + W_LIGHTS * 0.1f)) { glfwTerminate(); return 0; }
    std::vector<CeilingLight> ceilingLights;
    findCeilingLights(*backroomsModel, backroomsPos, ceilingLights);
    progressBase += W_LIGHTS;
    if (!drawSplashFrame("Backrooms - Luces listas", progressBase)) { glfwTerminate(); return 0; }
    std::cout << "[Load] Luces de techo: " << ceilingLights.size() << "\n";

    // --- ETAPA 6: audio ---
    if (!drawSplashFrame("Backrooms - Cargando audio...", progressBase + W_AUDIO * 0.3f)) { glfwTerminate(); return 0; }
    if (AudioBgm_Init())
    {
        if (!AudioBgm_PlayLoop("sounds/S1.mp3", BGM_DISTANT_VOLUME))
            std::cout << "[Audio] Continuando sin BGM.\n";
    }
    progressBase += W_AUDIO;
    if (!drawSplashFrame("Backrooms - Audio listo", progressBase)) { AudioBgm_Shutdown(); glfwTerminate(); return 0; }

    // Anclas (posiciones planificadas) para streamear props durante el juego
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

    std::vector<glm::vec3> boothLoadAnchors;
    {
        const float minX = roomWorldBounds.min.x + 20.0f;
        const float maxX = roomWorldBounds.max.x - 20.0f;
        const float minZ = roomWorldBounds.min.z + 20.0f;
        const float maxZ = roomWorldBounds.max.z - 20.0f;
        for (int i = 0; i < 8; ++i)
        {
            float t = (i + 0.5f) / 8.0f;
            boothLoadAnchors.emplace_back(minX + t * (maxX - minX), 0.0f, minZ + t * (maxZ - minZ));
            boothLoadAnchors.emplace_back(minX + t * (maxX - minX), 0.0f, maxZ - t * (maxZ - minZ));
        }
    }

    // Camaras: NO incluir spawn/center al inicio (evita cargar 200 camaras en el primer frame de juego)
    std::vector<glm::vec3> cameraLoadAnchors;
    for (size_t i = 0; i < wallAnchors.size() && cameraLoadAnchors.size() < 32; i += std::max<size_t>(1, wallAnchors.size() / 32))
        cameraLoadAnchors.push_back((wallAnchors[i].min + wallAnchors[i].max) * 0.5f);

    std::vector<CameraInstance> cameraInstances;
    std::vector<Instance> officeInstances;
    std::vector<Instance> boxesInstances;
    std::vector<Instance> demonInstances;
    std::vector<Instance> computerInstances;
    std::vector<Instance> boothInstances;
    std::vector<glm::vec3> deskLampPositions;

    bool camsReady = false, officeReady = false, boxesReady = false;
    bool demonReady = false, computersReady = false, boothsReady = false;

    std::cout << "[Load] Jugable. Props se cargan al explorar (radio " << STREAM_LOAD_RADIUS << ")\n";

    auto tryStreamAssets = [&]() {
        // Una carga pesada por llamada (durante el juego, entre frames)
        if (!camsReady && nearAnyXZ(camera.Position, cameraLoadAnchors, STREAM_LOAD_RADIUS))
        {
            std::cout << "[Stream] Cargando camaras de vigilancia...\n";
            surveillanceCameraModel = std::make_unique<Model>("models/surveillance_camera/camaras_vigilancia.obj");
            const AABB cameraBounds = computeModelBounds(*surveillanceCameraModel);
            const float wallAttachBase = (-cameraBounds.min.z * cameraScale) + cameraWallGap;
            CameraPlacementConfig cameraCfg;
            cameraCfg.targetCount = 200;
            cameraInstances = generateCameraInstances(wallAnchors, roomCenter, wallAttachBase, cameraCfg);
            addFurnitureCollisions(colManager, cameraBounds, cameraInstances, glm::vec3(cameraScale));
            camsReady = true;
            std::cout << "[Stream] Camaras: " << cameraInstances.size() << "\n";
            return;
        }
        if (!officeReady && nearAnyXZ(camera.Position, officeAnchors, STREAM_LOAD_RADIUS))
        {
            std::cout << "[Stream] Cargando muebles de oficina...\n";
            officeFurnitureModel = std::make_unique<Model>("models/office_furniture/office.obj");
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
            officeReady = true;
            std::cout << "[Stream] Oficina lista (" << officeInstances.size() << ")\n";
            return;
        }
        if (!boxesReady && nearAnyXZ(camera.Position, boxesAnchors, STREAM_LOAD_RADIUS))
        {
            std::cout << "[Stream] Cargando cajas...\n";
            oldPaperBoxesModel = std::make_unique<Model>("models/old_paper__cardboard_boxes/carton_papel.obj");
            const AABB boxesBounds = computeModelBounds(*oldPaperBoxesModel, true);
            boxesInstances = createFloorInstances(boxesAnchors, roomWorldBounds, boxesBounds, floorY, 2.2f, boxesYaws, 0.35f);
            addInstancesCollision(colManager, *oldPaperBoxesModel, boxesInstances, true);
            boxesReady = true;
            std::cout << "[Stream] Cajas listas\n";
            return;
        }
        if (!demonReady && nearAnyXZ(camera.Position, demonAnchors, STREAM_LOAD_RADIUS))
        {
            std::cout << "[Stream] Cargando entidad...\n";
            monsterAlienModel = std::make_unique<Model>("models/monster_alien/scene.obj");
            const AABB demonBounds = computeModelBounds(*monsterAlienModel);
            demonInstances = createFloorInstances(demonAnchors, roomWorldBounds, demonBounds, floorY, 1.8f, demonYaws, 0.45f);
            addInstancesCollision(colManager, *monsterAlienModel, demonInstances);
            demonReady = true;
            std::cout << "[Stream] Entidad lista (" << demonInstances.size() << ")\n";
            return;
        }
        if (!computersReady && nearAnyXZ(camera.Position, computerAnchors, STREAM_LOAD_RADIUS))
        {
            std::cout << "[Stream] Cargando computadoras...\n";
            sciFiComputerModel = std::make_unique<Model>("models/sci-fi_computer/computadora.obj");
            const AABB computerBounds = computeModelBounds(*sciFiComputerModel);
            computerInstances = createFloorInstances(computerAnchors, roomWorldBounds, computerBounds, floorY, 1.0f, computerYaws, 0.35f);
            addInstancesCollision(colManager, *sciFiComputerModel, computerInstances);
            computersReady = true;
            std::cout << "[Stream] Computadoras listas\n";
            return;
        }
        if (!boothsReady && nearAnyXZ(camera.Position, boothLoadAnchors, STREAM_LOAD_RADIUS))
        {
            std::cout << "[Stream] Cargando cabinas telefonicas...\n";
            publicPhoneBoothModel = std::make_unique<Model>("models/public_phone_booth/public_phone.obj");
            const AABB boothBounds = computeModelBounds(*publicPhoneBoothModel);
            boothInstances = generatePhoneBooths(roomWorldBounds, boothBounds, floorY, 50);
            addInstancesCollision(colManager, *publicPhoneBoothModel, boothInstances);
            boothsReady = true;
            std::cout << "[Stream] Cabinas: " << boothInstances.size() << "\n";
            return;
        }
    };

    // --- Spawn ---
    // GitHub a7354ab: Camera(0, 0, 3), Yaw=-90, Pitch=0  -> altura Y=0 (CORRECTA a escala del mapa).
    // Eso a menudo mira de frente a una pared. Mantenemos Y=0 y buscamos XZ+yaw con vista abierta a pasillos.
    if (!drawSplashFrame("Backrooms - Preparando spawn...", progressBase + W_SPAWN * 0.4f)) { AudioBgm_Shutdown(); glfwTerminate(); return 0; }
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
    }
    progressBase += W_SPAWN;
    if (!drawSplashFrame("Backrooms - Spawn listo", progressBase)) { AudioBgm_Shutdown(); glfwTerminate(); return 0; }

    // Texturas de menu
    if (!drawSplashFrame("Backrooms - Cargando interfaz...", progressBase + W_UI * 0.4f)) { AudioBgm_Shutdown(); glfwTerminate(); return 0; }
    menuStartTex = loadUiTexture("textures/menu_start.png");
    menuPauseTex = loadUiTexture("textures/menu_pause.png");
    ensureBlackTex();
    progressBase += W_UI;
    if (!drawSplashFrame("Backrooms - Interfaz lista", progressBase)) { AudioBgm_Shutdown(); glfwTerminate(); return 0; }

    // Espera final realista (GPU/drivers terminan de asentar; evita "COMENZAR" prematuro + hitch)
    {
        const double settleStart = glfwGetTime();
        const double settleSeconds = 2.2; // tiempo extra visible al 95-100%
        while (true)
        {
            double t = (glfwGetTime() - settleStart) / settleSeconds;
            if (t > 1.0) t = 1.0;
            float p = progressBase + W_SETTLE * static_cast<float>(t);
            if (!drawSplashFrame("Backrooms - Finalizando...", p))
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
        // un instante en 100% para que se lea
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

    // Menu principal: SOLO ahora (carga completa, % = 100)
    appState = AppState::Menu;
    fadeBlack = 1.0f;
    glfwSetWindowTitle(window, "Backrooms - Menu");
    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
    firstMouse = true;
    lastFrame = static_cast<float>(glfwGetTime());
    std::cout << "[UI] Carga 100%. Menu COMENZAR / SALIR disponible.\n";

    // 9. Bucle principal
    while (!glfwWindowShouldClose(window)) {
        float currentFrame = static_cast<float>(glfwGetTime());
        deltaTime = currentFrame - lastFrame;
        lastFrame = currentFrame;
        if (deltaTime > 0.1f) deltaTime = 0.1f;

        processInput(window);

        // ---------- MENU / PAUSA: solo UI 2D (sin mundo 3D) ----------
        if (appState == AppState::Menu || appState == AppState::Paused)
        {
            int w = 0, h = 0;
            glfwGetFramebufferSize(window, &w, &h);
            if (w > 0 && h > 0)
            {
                SCR_WIDTH = static_cast<unsigned int>(w);
                SCR_HEIGHT = static_cast<unsigned int>(h);
                glViewport(0, 0, w, h);
            }
            unsigned int uiTex = (appState == AppState::Menu) ? menuStartTex : menuPauseTex;
            drawFullscreenTex(uiTex, 0.0f, 1.0f, true);
            glfwSwapBuffers(window);
            glfwPollEvents();
            continue;
        }

        // ---------- FADE-IN / PLAYING: mundo 3D ----------
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

        // Streaming solo cuando ya se juega / funde
        tryStreamAssets();

        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

        backroomsShader->use();

        // Calcular las luces
        backroomsShader->setVec3("viewPos", camera.Position);
        backroomsShader->setFloat("shininess", 30.0f);
        backroomsShader->setVec3("dirLight.direction", glm::vec3(-0.2f, -1.0f, -0.3f));

        backroomsShader->setVec3("dirLight.ambient", glm::vec3(0.0f, 0.0f, 0.0f));
        backroomsShader->setVec3("dirLight.diffuse", glm::vec3(0.18f, 0.18f, 0.16f));
        backroomsShader->setVec3("dirLight.specular", glm::vec3(0.2f, 0.2f, 0.2f));
        int lightIndex = 0;

        // Solo luces cercanas (sin ordenar TODO el arreglo cada frame)
        std::vector<size_t> nearLightIndices;
        nearLightIndices.reserve(64);
        for (size_t i = 0; i < ceilingLights.size(); ++i)
        {
            if (distXZ(camera.Position, ceilingLights[i].position) <= STREAM_LIGHT_RADIUS)
                nearLightIndices.push_back(i);
        }
        std::sort(nearLightIndices.begin(), nearLightIndices.end(), [&](size_t a, size_t b) {
            return distXZ(camera.Position, ceilingLights[a].position) < distXZ(camera.Position, ceilingLights[b].position);
        });

        for (size_t si = 0; si < nearLightIndices.size() && lightIndex < STREAM_MAX_POINT_LIGHTS; si++)
        {
            size_t i = nearLightIndices[si];
            std::string base = "pointLights[" + std::to_string(lightIndex) + "].";
            backroomsShader->setVec3(base + "position", ceilingLights[i].position);
            backroomsShader->setFloat(base + "constant", 1.0f);
            backroomsShader->setFloat(base + "linear", 0.05f);
            backroomsShader->setFloat(base + "quadratic", 0.003f);

            glm::vec3 diffuse(0.0f);
            glm::vec3 specular(0.0f);
            glm::vec3 ambient(0.0f);

            if (ceilingLights[i].isOn)
            {
                float minDistance = 1e9f;
                for (const Instance& demon : demonInstances)
                {
                    float d = glm::distance(ceilingLights[i].position, demon.position);
                    if (d < minDistance)
                        minDistance = d;
                }
                float monsterFactor = 1.0f;
                if (minDistance < 15.0f)
                    monsterFactor = glm::clamp((minDistance - 4.0f) / 11.0f, 0.15f, 1.0f);

                glm::vec3 baseColor(0.9f, 0.88f, 0.82f);
                diffuse = baseColor * 0.22f * monsterFactor;
                specular = baseColor * 0.05f * monsterFactor;
                ambient = glm::vec3(0.0f);

                float distToCam = glm::distance(camera.Position, ceilingLights[i].position);
                float distanceFade = 1.0f - glm::clamp((distToCam - 50.0f) / 15.0f, 0.0f, 1.0f);
                diffuse *= distanceFade;
                specular *= distanceFade;
            }

            backroomsShader->setVec3(base + "ambient", ambient);
            backroomsShader->setVec3(base + "diffuse", diffuse);
            backroomsShader->setVec3(base + "specular", specular);
            lightIndex++;
        }

        for (size_t i = 0; i < deskLampPositions.size() && lightIndex < STREAM_MAX_POINT_LIGHTS + 8; i++)
        {
            if (distXZ(camera.Position, deskLampPositions[i]) > STREAM_DRAW_RADIUS)
                continue;

            std::string base = "pointLights[" + std::to_string(lightIndex) + "].";
            backroomsShader->setVec3(base + "position", deskLampPositions[i]);
            backroomsShader->setFloat(base + "constant", 1.0f);
            backroomsShader->setFloat(base + "linear", 0.1f);
            backroomsShader->setFloat(base + "quadratic", 0.15f);
            backroomsShader->setVec3(base + "ambient", glm::vec3(0.08f, 0.02f, 0.1f));
            backroomsShader->setVec3(base + "diffuse", glm::vec3(0.2f, 0.1f, 0.3f));
            backroomsShader->setVec3(base + "specular", glm::vec3(0.2f, 0.1f, 0.3f));
            lightIndex++;
        }
        backroomsShader->setInt("numActivePointLights", lightIndex);
        backroomsShader->setVec3("spotLight.position", camera.Position);
        backroomsShader->setVec3("spotLight.direction", camera.Front);
        backroomsShader->setFloat("spotLight.cutOff", glm::cos(glm::radians(12.5f)));
        backroomsShader->setFloat("spotLight.outerCutOff", glm::cos(glm::radians(17.5f)));
        backroomsShader->setFloat("spotLight.constant", 1.0f);
        backroomsShader->setFloat("spotLight.linear", 0.025f);
        backroomsShader->setFloat("spotLight.quadratic", 0.005f);
        if (flashlightOn)
        {
            backroomsShader->setVec3("spotLight.ambient", glm::vec3(0.0f));
            backroomsShader->setVec3("spotLight.diffuse", glm::vec3(0.9f, 0.9f, 0.8f));
            backroomsShader->setVec3("spotLight.specular", glm::vec3(0.9f, 0.9f, 0.8f));
        }
        else
        {
            backroomsShader->setVec3("spotLight.ambient", glm::vec3(0.0f));
            backroomsShader->setVec3("spotLight.diffuse", glm::vec3(0.0f));
            backroomsShader->setVec3("spotLight.specular", glm::vec3(0.0f));
        }

        float aspect = (SCR_HEIGHT > 0) ? (float)SCR_WIDTH / (float)SCR_HEIGHT : 16.0f / 9.0f;
        glm::mat4 projection = glm::perspective(glm::radians(camera.Zoom), aspect, 0.1f, 200.0f);
        glm::mat4 view = camera.GetViewMatrix();
        backroomsShader->setMat4("projection", projection);
        backroomsShader->setMat4("view", view);

        // Dibujar backrooms (siempre)
        glm::mat4 model = glm::mat4(1.0f);
        model = glm::translate(model, backroomsPos);
        backroomsShader->setMat4("model", model);
        backroomsModel->Draw(*backroomsShader);
        border->Draw(*backroomsShader);

        // Props solo si ya se streamearon + culling por distancia
        if (camsReady && surveillanceCameraModel)
            drawCameraInstances(*backroomsShader, *surveillanceCameraModel, cameraInstances, glm::vec3(1.0f), STREAM_DRAW_RADIUS);
        if (officeReady && officeFurnitureModel)
            drawInstances(*backroomsShader, *officeFurnitureModel, officeInstances, false, STREAM_DRAW_RADIUS);
        if (boxesReady && oldPaperBoxesModel)
            drawInstances(*backroomsShader, *oldPaperBoxesModel, boxesInstances, true, STREAM_DRAW_RADIUS);
        if (demonReady && monsterAlienModel)
            drawInstances(*backroomsShader, *monsterAlienModel, demonInstances, false, STREAM_DRAW_RADIUS);
        if (computersReady && sciFiComputerModel)
            drawInstances(*backroomsShader, *sciFiComputerModel, computerInstances, false, STREAM_DRAW_RADIUS);
        if (boothsReady && publicPhoneBoothModel)
            drawInstances(*backroomsShader, *publicPhoneBoothModel, boothInstances, false, STREAM_DRAW_RADIUS);

        // ============================================================================
        // === SOMBRAS (solo instancias cercanas) ===
        // ============================================================================
        glm::vec3 lightDirection(-0.2f, -1.0f, -0.3f);

        cubeShader->use();
        cubeShader->setMat4("projection", projection);
        cubeShader->setMat4("view", view);

        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glEnable(GL_STENCIL_TEST);

        glStencilFunc(GL_EQUAL, 0, 0xFF);
        glStencilOp(GL_KEEP, GL_KEEP, GL_INCR);
        glDepthMask(GL_FALSE);

        cubeShader->setVec4("cubeColor", glm::vec4(0.0f, 0.0f, 0.0f, 0.50f));

        if (officeReady && officeFurnitureModel)
            drawPlanarShadows(*cubeShader, *officeFurnitureModel, officeInstances, floorY, lightDirection, false, STREAM_SHADOW_RADIUS);
        if (boxesReady && oldPaperBoxesModel)
            drawPlanarShadows(*cubeShader, *oldPaperBoxesModel, boxesInstances, floorY, lightDirection, true, STREAM_SHADOW_RADIUS);
        if (demonReady && monsterAlienModel)
            drawPlanarShadows(*cubeShader, *monsterAlienModel, demonInstances, floorY, lightDirection, false, STREAM_SHADOW_RADIUS);
        if (computersReady && sciFiComputerModel)
            drawPlanarShadows(*cubeShader, *sciFiComputerModel, computerInstances, floorY, lightDirection, false, STREAM_SHADOW_RADIUS);
        if (boothsReady && publicPhoneBoothModel)
            drawPlanarShadows(*cubeShader, *publicPhoneBoothModel, boothInstances, floorY, lightDirection, false, STREAM_SHADOW_RADIUS);

        glDepthMask(GL_TRUE);
        glDisable(GL_STENCIL_TEST);
        glDisable(GL_BLEND);

        // Fundido de negro al entrar al juego (no se ve frame congelado)
        if (appState == AppState::FadeIn || fadeBlack > 0.001f)
            drawBlackOverlay(fadeBlack);

        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    AudioBgm_Shutdown();
    glfwTerminate();
    return 0;
}

// ============================================================================
// CALLBACKS
// ============================================================================

static void startGameFromMenu(GLFWwindow* window)
{
    appState = AppState::FadeIn;
    fadeBlack = 1.0f;
    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    firstMouse = true;
    glfwSetWindowTitle(window, "Backrooms - Grupo 7");
    std::cout << "[UI] COMENZAR -> fundido al juego\n";
}

static void resumeFromPause(GLFWwindow* window)
{
    appState = AppState::Playing;
    fadeBlack = 0.0f;
    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    firstMouse = true;
    glfwSetWindowTitle(window, "Backrooms - Grupo 7");
    std::cout << "[UI] CONTINUAR\n";
}

// ============================================================================
// PROCESAR ENTRADA
// ============================================================================
void processInput(GLFWwindow* window)
{
    static bool escWasDown = false;
    static bool enterWasDown = false;
    const bool escDown = glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS;
    const bool enterDown = glfwGetKey(window, GLFW_KEY_ENTER) == GLFW_PRESS
        || glfwGetKey(window, GLFW_KEY_KP_ENTER) == GLFW_PRESS;

    // ----- MENU -----
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

    // ----- PAUSA -----
    if (appState == AppState::Paused)
    {
        if (enterDown && !enterWasDown)
            resumeFromPause(window);
        if (escDown && !escWasDown)
            resumeFromPause(window); // Esc en pausa = continuar (tambien se puede salir con boton)
        escWasDown = escDown;
        enterWasDown = enterDown;
        return;
    }

    // ----- FADE-IN + PLAYING: se puede caminar en cuanto empieza el fundido -----
    // (el fade es visual; el jugador no se queda "congelado" esperando)
    if (appState == AppState::FadeIn || appState == AppState::Playing)
    {
        if (appState == AppState::Playing && escDown && !escWasDown)
        {
            appState = AppState::Paused;
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

        if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS)
            camera.ProcessKeyboard(FORWARD, deltaTime, colManager);
        if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS)
            camera.ProcessKeyboard(BACKWARD, deltaTime, colManager);
        if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS)
            camera.ProcessKeyboard(LEFT, deltaTime, colManager);
        if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS)
            camera.ProcessKeyboard(RIGHT, deltaTime, colManager);

        static bool fKeyWasPressed = false;
        if (appState == AppState::Playing)
        {
            if (glfwGetKey(window, GLFW_KEY_F) == GLFW_PRESS)
            {
                if (!fKeyWasPressed)
                {
                    flashlightOn = !flashlightOn;
                    fKeyWasPressed = true;
                }
            }
            else if (glfwGetKey(window, GLFW_KEY_F) == GLFW_RELEASE)
            {
                fKeyWasPressed = false;
            }
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
    if (appState != AppState::Menu && appState != AppState::Paused)
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

    if (uiHitPrimaryButton(mx, my))
    {
        if (appState == AppState::Menu)
            startGameFromMenu(window);
        else
            resumeFromPause(window);
    }
    else if (uiHitSecondaryButton(mx, my))
    {
        glfwSetWindowShouldClose(window, true);
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