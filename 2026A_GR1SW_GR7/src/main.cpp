#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <iostream>
#include <vector>
#include <random>
#include <cfloat>
#include <cmath>
#include <algorithm>

// Incluimos Assimp SOLO para probar que el vinculador (Linker) no de errores.
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

#include <shader.h>
#include <camera.h>
#include <model.h>
#include <collisions.h>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

// Callbacks
void framebuffer_size_callback(GLFWwindow* window, int width, int height);
void mouse_callback(GLFWwindow* window, double xpos, double ypos);
void scroll_callback(GLFWwindow* window, double xoffset, double yoffset);
void processInput(GLFWwindow* window);

// Settings
const unsigned int SCR_WIDTH = 800;
const unsigned int SCR_HEIGHT = 600;

// Camera
Camera camera(glm::vec3(0.0f, 0.0f, 3.0f));
float lastX = SCR_WIDTH / 2.0f;
float lastY = SCR_HEIGHT / 2.0f;
bool firstMouse = true;

// Timing
float deltaTime = 0.0f;
float lastFrame = 0.0f;

// Posición del nivel
glm::vec3 backroomsPos(0.0f, -3.0f, 0.0f);

// Manager de colisiones
CollisionManager colManager;

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

void drawInstances(Shader& shader, Model& model, const std::vector<Instance>& instances, bool firstMeshOnly = false)
{
    for (const Instance& instance : instances)
    {
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
    const glm::vec3& cameraScale)
{
    for (const CameraInstance& instance : instances)
    {
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

// ============================================================================
// MAIN
// ============================================================================

int main() {
    // 1. Inicializar GLFW
    glfwInit();
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    // 2. Crear ventana
    GLFWwindow* window = glfwCreateWindow(800, 600, "Prueba de Entorno - Grupo", NULL, NULL);
    if (window == NULL) {
        std::cout << "Error al crear la ventana GLFW. Revisa la DLL." << std::endl;
        glfwTerminate();
        return -1;
    }
    glfwMakeContextCurrent(window);
    glfwSetFramebufferSizeCallback(window, framebuffer_size_callback);
    glfwSetCursorPosCallback(window, mouse_callback);
    glfwSetScrollCallback(window, scroll_callback);

    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);

    // 3. Inicializar GLAD
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        std::cout << "Error al inicializar GLAD." << std::endl;
        return -1;
    }

    glEnable(GL_DEPTH_TEST);

    // 4. Prueba de Assimp
    Assimp::Importer importer;
    std::cout << "Las librerias y Assimp enlazaron correctamente!" << std::endl;

    // 5. Generar shaders
    Shader backroomsShader("shaders/backrooms.vs", "shaders/backrooms.fs");
    Shader cubeShader("shaders/basico.vs", "shaders/basico.fs");

    // 6. Cargar modelos
    Model backroomsModel("models/backrooms_level_0/backrooms.obj");
    Model backroomsCollisionsModel("models/backrooms_level_0_collisions/backrooms.obj");
    Model border("models/border/border.obj");
    Model surveillanceCameraModel("models/surveillance_camera/camaras_vigilancia.obj");
    Model officeFurnitureModel("models/office_furniture/office.obj");
    Model oldPaperBoxesModel("models/old_paper__cardboard_boxes/carton_papel.obj");
    Model monsterAlienModel("models/monster_alien/scene.obj");
    Model sciFiComputerModel("models/sci-fi_computer/computadora.obj");
    Model publicPhoneBoothModel("models/public_phone_booth/public_phone.obj");
    std::cout << "Meshes: " << backroomsCollisionsModel.meshes.size() << std::endl;

    camera.MovementSpeed = 10;

    // 7. Calcular bounds y generar instancias
    const AABB roomLocalBounds = computeModelBounds(backroomsCollisionsModel);
    const AABB roomWorldBounds = { roomLocalBounds.min + backroomsPos, roomLocalBounds.max + backroomsPos };
    const glm::vec3 roomCenter = (roomWorldBounds.min + roomWorldBounds.max) * 0.5f;
    const float floorY = roomWorldBounds.min.y;

    // Cámaras
    const std::vector<WallAnchor> wallAnchors = collectWallAnchors(backroomsCollisionsModel, backroomsPos);
    const AABB cameraBounds = computeModelBounds(surveillanceCameraModel);
    const float cameraScale = 1.0f;
    const float cameraWallGap = 0.02f;
    const float wallAttachBase = (-cameraBounds.min.z * cameraScale) + cameraWallGap;

    CameraPlacementConfig cameraCfg;
    cameraCfg.targetCount = 200;
    const std::vector<CameraInstance> cameraInstances = generateCameraInstances(
        wallAnchors,
        roomCenter,
        wallAttachBase,
        cameraCfg);

    // Oficina
    const AABB officeBounds = computeModelBounds(officeFurnitureModel);
    const std::vector<glm::vec3> officeAnchors = {
        glm::vec3(roomCenter.x - 40.0f, 0.0f, roomCenter.z - 24.0f),
        glm::vec3(roomCenter.x + 32.0f, 0.0f, roomCenter.z - 20.0f),
        glm::vec3(roomCenter.x - 55.0f, 0.0f, roomCenter.z + 32.0f),
        glm::vec3(roomCenter.x + 24.0f, 0.0f, roomCenter.z + 22.0f)
    };
    const std::vector<float> officeYaws = { 20.0f, -35.0f, 110.0f, -145.0f };
    const std::vector<Instance> officeInstances = createFloorInstances(officeAnchors, roomWorldBounds, officeBounds, floorY, 3.0f, officeYaws, 0.45f);

    // Cajas
    const AABB boxesBounds = computeModelBounds(oldPaperBoxesModel, true);
    const std::vector<glm::vec3> boxesAnchors = {
        glm::vec3(roomCenter.x - 18.0f, 0.0f, roomCenter.z - 33.0f),
        glm::vec3(roomCenter.x + 14.0f, 0.0f, roomCenter.z - 36.0f),   
        glm::vec3(roomCenter.x - 28.0f, 0.0f, roomCenter.z + 14.0f),   
        glm::vec3(roomCenter.x + 26.0f, 0.0f, roomCenter.z + 32.0f)   
};
    const std::vector<float> boxesYaws = { 12.0f, 65.0f, -20.0f, 140.0f };
    const std::vector<Instance> boxesInstances = createFloorInstances(boxesAnchors, roomWorldBounds, boxesBounds, floorY, 2.2f, boxesYaws, 0.35f);

    // Demonio
    const AABB demonBounds = computeModelBounds(monsterAlienModel);
    const std::vector<glm::vec3> demonAnchors = {
        glm::vec3(roomCenter.x - 94.0f, 0.0f, roomCenter.z - 26.0f),
        glm::vec3(roomCenter.x + 32.0f, 0.0f, roomCenter.z - 20.0f),
        glm::vec3(roomCenter.x + 30.0f, 0.0f, roomCenter.z + 24.0f)
    };
    const std::vector<float> demonYaws = { 45.0f, -135.0f };
    const std::vector<Instance> demonInstances = createFloorInstances(demonAnchors, roomWorldBounds, demonBounds, floorY, 6.0f, demonYaws, 0.45f);

    // Computadoras
    const AABB computerBounds = computeModelBounds(sciFiComputerModel);
    const std::vector<glm::vec3> computerAnchors = {
        glm::vec3(roomWorldBounds.min.x + 25.0f, 0.0f, roomWorldBounds.min.z + 25.0f),
        glm::vec3(roomWorldBounds.max.x - 25.0f, 0.0f, roomWorldBounds.min.z + 25.0f),
        glm::vec3(roomWorldBounds.min.x + 25.0f, 0.0f, roomWorldBounds.max.z - 25.0f),
        glm::vec3(roomWorldBounds.max.x - 25.0f, 0.0f, roomWorldBounds.max.z - 25.0f)
    };
    const std::vector<float> computerYaws = { 35.0f, -35.0f, 145.0f, -145.0f };
    const std::vector<Instance> computerInstances = createFloorInstances(computerAnchors, roomWorldBounds, computerBounds, floorY, 1.0f, computerYaws, 0.35f);

    // Cabinas telefónicas
    const AABB boothBounds = computeModelBounds(publicPhoneBoothModel);
    const std::vector<Instance> boothInstances = generatePhoneBooths(roomWorldBounds, boothBounds, floorY, 50);

    std::cout << "Camaras colocadas: " << cameraInstances.size() << std::endl;
    std::cout << "Cabinas colocadas: " << boothInstances.size() << std::endl;

    // 8. Colisiones
    colManager.addStaticBox(backroomsCollisionsModel, backroomsPos);
    colManager.addStaticBox(border, backroomsPos);
    addFurnitureCollisions(colManager, cameraBounds, cameraInstances, glm::vec3(cameraScale));
    addInstancesCollision(colManager, officeFurnitureModel, officeInstances);
    addInstancesCollision(colManager, oldPaperBoxesModel, boxesInstances, true);
    addInstancesCollision(colManager, monsterAlienModel, demonInstances);
    addInstancesCollision(colManager, sciFiComputerModel, computerInstances);
    addInstancesCollision(colManager, publicPhoneBoothModel, boothInstances);

    // 9. Bucle de Renderizado
    while (!glfwWindowShouldClose(window)) {
        float currentFrame = glfwGetTime();
        deltaTime = currentFrame - lastFrame;
        lastFrame = currentFrame;

        processInput(window);

        glClearColor(0.2f, 0.2f, 0.2f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        backroomsShader.use();

        glm::mat4 projection = glm::perspective(glm::radians(camera.Zoom), (float)SCR_WIDTH / (float)SCR_HEIGHT, 0.1f, 100.0f);
        glm::mat4 view = camera.GetViewMatrix();
        backroomsShader.setMat4("projection", projection);
        backroomsShader.setMat4("view", view);

        // Dibujar backrooms
        glm::mat4 model = glm::mat4(1.0f);
        model = glm::translate(model, backroomsPos);
        backroomsShader.setMat4("model", model);
        backroomsModel.Draw(backroomsShader);
        border.Draw(backroomsShader);

        // Dibujar instancias (usando la versión para cámaras)
        drawCameraInstances(backroomsShader, surveillanceCameraModel, cameraInstances, glm::vec3(1.0f));
        drawInstances(backroomsShader, officeFurnitureModel, officeInstances);
        drawInstances(backroomsShader, oldPaperBoxesModel, boxesInstances, true);
        drawInstances(backroomsShader, monsterAlienModel, demonInstances);
        drawInstances(backroomsShader, sciFiComputerModel, computerInstances);
        drawInstances(backroomsShader, publicPhoneBoothModel, boothInstances);

        // Opcional: mostrar cajas de colisión
        // cubeShader.use();
        // cubeShader.setMat4("projection", projection);
        // cubeShader.setMat4("view", view);
        // cubeShader.setVec3("cubeColor", glm::vec3(1.0f, 0.0f, 0.0f));
        // colManager.drawCollisionBoxes(cubeShader);

        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    glfwTerminate();
    return 0;
}

// ============================================================================
// CALLBACKS
// ============================================================================

void processInput(GLFWwindow* window)
{
    if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS)
        glfwSetWindowShouldClose(window, true);

    if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS)
        camera.ProcessKeyboard(FORWARD, deltaTime, colManager);
    if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS)
        camera.ProcessKeyboard(BACKWARD, deltaTime, colManager);
    if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS)
        camera.ProcessKeyboard(LEFT, deltaTime, colManager);
    if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS)
        camera.ProcessKeyboard(RIGHT, deltaTime, colManager);
}

void framebuffer_size_callback(GLFWwindow* window, int width, int height)
{
    glViewport(0, 0, width, height);
}

void mouse_callback(GLFWwindow* window, double xpos, double ypos)
{
    if (firstMouse)
    {
        lastX = xpos;
        lastY = ypos;
        firstMouse = false;
    }

    float xoffset = xpos - lastX;
    float yoffset = lastY - ypos;

    lastX = xpos;
    lastY = ypos;

    camera.ProcessMouseMovement(xoffset, yoffset);
}

void scroll_callback(GLFWwindow* window, double xoffset, double yoffset)
{
    camera.ProcessMouseScroll(yoffset);
}