#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <iostream>

// Incluimos Assimp SOLO para probar que el vinculador (Linker) no dé errores.
// Si esto compila, significa que configuraste bien tu .lib en las Propiedades.
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

#include <shader.h>
#include <camera.h>
#include <model.h>
#include <collisions.h>

#define STB_IMAGE_IMPLEMENTATION 
#include <stb_image.h>

#include <vector>
#include <random>
#include <cfloat>
#include <algorithm>

void framebuffer_size_callback(GLFWwindow* window, int width, int height);
void mouse_callback(GLFWwindow* window, double xpos, double ypos);
void scroll_callback(GLFWwindow* window, double xoffset, double yoffset);
void processInput(GLFWwindow* window);

// settings
const unsigned int SCR_WIDTH = 800;
const unsigned int SCR_HEIGHT = 600;

// camera
Camera camera(glm::vec3(0.0f, 0.0f, 3.0f));
float lastX = SCR_WIDTH / 2.0f;
float lastY = SCR_HEIGHT / 2.0f;
bool firstMouse = true;

// timing
float deltaTime = 0.0f;
float lastFrame = 0.0f;

//posicion
glm::vec3 backroomsPos(0.0f, -3.0f, 0.0f);
glm::vec3 surveillanceCameraScale(1.5f, 1.5f, 1.5f);

CollisionManager colManager;

// Estructura para una cámara de vigilancia
struct CameraInstance
{
    glm::vec3 position;
    float yawDeg;
    float pitchDeg;
};

// Estructura para un segmento de pared detectado
struct WallAnchor
{
    glm::vec3 min;
    glm::vec3 max;
    bool thinX;
};

// Estructura para mobiliario genérico
struct FurnitureInstance
{
    glm::vec3 position;
    glm::vec3 scale;
};

// Estructura para props montados en pared
struct WallMountedInstance
{
    glm::vec3 position;
    float yawDeg;
    glm::vec3 scale;
};

// Configuración para la colocación procedural de cámaras
struct CameraPlacementConfig
{
    int targetCount = 200;
    int maxAttempts = 5000;
    float minCamDistance = 6.0f;
    float nearTopMin = 0.80f;
    float nearTopMax = 1.2f;
    float wallClearance = 0.02f; // Margen para evitar z-fighting
    float pitchDownMin = -28.0f;
    float pitchDownMax = -12.0f;
    float yawJitterMin = -10.0f;
    float yawJitterMax = 10.0f;
};

// Función para calcular el AABB de todos los meshes de un modelo
AABB computeModelBounds(const Model& model)
{
    AABB bounds;
    bounds.min = glm::vec3(FLT_MAX, FLT_MAX, FLT_MAX);
    bounds.max = glm::vec3(-FLT_MAX, -FLT_MAX, -FLT_MAX);

    for (const Mesh& mesh : model.meshes)
    {
        for (const Vertex& vertex : mesh.vertices)
        {
            bounds.min = glm::min(bounds.min, vertex.Position);
            bounds.max = glm::max(bounds.max, vertex.Position);
        }
    }

    return bounds;
}

// Función para calcular el AABB de un solo mesh
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

// Calcula los bounds específicos para el modelo de cajas de cartón
AABB computeOldPaperBoxesBounds(const Model& model)
{
    if (model.meshes.empty())
        return computeModelBounds(model);
    return computeMeshBounds(model.meshes[0]); // Solo el mesh 0 contiene las cajas
}

// Convierte un AABB local a coordenadas de mundo
AABB buildScaledBounds(const AABB& localBounds, const glm::vec3& worldPosition, const glm::vec3& scale)
{
    AABB worldBounds;
    worldBounds.min = worldPosition + localBounds.min * scale;
    worldBounds.max = worldPosition + localBounds.max * scale;
    return worldBounds;
}

// Ajusta la posición XZ para mantener el modelo dentro del cuarto
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

// Crea instancias de props en el piso a partir de posiciones base
std::vector<FurnitureInstance> createFloorInstancesFromAnchors(
    const std::vector<glm::vec3>& anchorPositions,
    const AABB& roomBounds,
    const AABB& localBounds,
    const glm::vec3& floorOffset,
    float uniformScale,
    float wallClearance)
{
    const float floorY = floorOffset.y;
    std::vector<FurnitureInstance> instances;
    instances.reserve(anchorPositions.size());

    for (const glm::vec3& anchor : anchorPositions)
    {
        FurnitureInstance instance{};
        instance.scale = glm::vec3(uniformScale);

        const glm::vec3 desiredPosition = glm::vec3(
            anchor.x,
            floorY - (localBounds.min.y * uniformScale),
            anchor.z
        );

        instance.position = keepInstanceInsideRoomXZ(
            desiredPosition,
            roomBounds,
            localBounds,
            instance.scale,
            wallClearance);

        instances.push_back(instance);
    }

    return instances;
}

// Detecta las paredes en el modelo de colisiones
std::vector<WallAnchor> collectWallAnchors(const Model& collisionsModel, const glm::vec3& worldOffset)
{
    std::vector<WallAnchor> wallAnchors;
    wallAnchors.reserve(collisionsModel.meshes.size());

    for (const Mesh& mesh : collisionsModel.meshes)
    {
        glm::vec3 min(FLT_MAX, FLT_MAX, FLT_MAX);
        glm::vec3 max(-FLT_MAX, -FLT_MAX, -FLT_MAX);

        for (const Vertex& vertex : mesh.vertices)
        {
            min = glm::min(min, vertex.Position + worldOffset);
            max = glm::max(max, vertex.Position + worldOffset);
        }

        glm::vec3 size = max - min;
        bool tallEnough = size.y > 2.0f;
        bool thinX = size.x < 1.2f && size.z > 1.5f;
        bool thinZ = size.z < 1.2f && size.x > 1.5f;

        if (tallEnough && (thinX || thinZ))
            wallAnchors.push_back({ min, max, thinX });
    }

    return wallAnchors;
}

// Genera instancias de cámaras distribuidas en las paredes detectadas
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
            // Pared orientada a lo largo del eje Z
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
            // Pared orientada a lo largo del eje X
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

        // Verifica que no esté demasiado cerca de otra cámara
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

// Crea la matriz de modelo para mobiliario
glm::mat4 buildFurnitureModelMatrix(const FurnitureInstance& instance)
{
    glm::mat4 model = glm::mat4(1.0f);
    model = glm::translate(model, instance.position);
    model = glm::scale(model, instance.scale);
    return model;
}

// Crea instancias de mobiliario de oficina
std::vector<FurnitureInstance> createOfficeFurnitureInstances(
    const AABB& roomBounds,
    const AABB& localBounds,
    const glm::vec3& floorOffset)
{
    const float furnitureScale = 3.0f;
    const float wallClearance = 0.45f;

    std::vector<glm::vec3> anchorPositions = {
        glm::vec3(50.0f, 0.0f, -5.0f),
        glm::vec3(-14.0f, 0.0f, -22.0f),
        glm::vec3(16.0f, 0.0f, -34.0f)
    };

    return createFloorInstancesFromAnchors(
        anchorPositions,
        roomBounds,
        localBounds,
        floorOffset,
        furnitureScale,
        wallClearance);
}

// Crea instancias de cajas de cartón
std::vector<FurnitureInstance> createOldPaperBoxesInstances(
    const AABB& roomBounds,
    const AABB& localBounds,
    const glm::vec3& floorOffset)
{
    const float boxesScale = 2.2f;
    const float wallClearance = 0.35f;

    std::vector<glm::vec3> anchorPositions = {
        glm::vec3(1.0f, 0.0f, -12.0f),
        glm::vec3(12.0f, 0.0f, -40.0f),
        glm::vec3(-30.0f, 0.0f, -30.0f)
    };

    return createFloorInstancesFromAnchors(
        anchorPositions,
        roomBounds,
        localBounds,
        floorOffset,
        boxesScale,
        wallClearance);
}

// Crea instancias de computadoras sci-fi
std::vector<FurnitureInstance> createSciFiComputerInstances(
    const AABB& roomBounds,
    const AABB& localBounds,
    const glm::vec3& floorOffset)
{
    const float computerScale = 1.0f;
    const float cornerMargin = 22.0f;
    const float wallClearance = 0.35f;

    std::vector<glm::vec3> anchorPositions = {
        glm::vec3(roomBounds.min.x + cornerMargin, 0.0f, roomBounds.min.z + cornerMargin),
        glm::vec3(roomBounds.max.x - cornerMargin, 0.0f, roomBounds.min.z + cornerMargin),
        glm::vec3(roomBounds.min.x + cornerMargin, 0.0f, roomBounds.max.z - cornerMargin),
        glm::vec3(roomBounds.max.x - cornerMargin, 0.0f, roomBounds.max.z - cornerMargin)
    };

    return createFloorInstancesFromAnchors(
        anchorPositions,
        roomBounds,
        localBounds,
        floorOffset,
        computerScale,
        wallClearance);
}

// Crea instancias del demonio
std::vector<FurnitureInstance> createFacelessBoneyHorrorDemonInstances(
    const AABB& roomBounds,
    const AABB& localBounds,
    const glm::vec3& floorOffset)
{
    const float demonScale = 6.0f;
    const float wallClearance = 0.45f;

    std::vector<glm::vec3> anchorPositions = {
        glm::vec3(roomBounds.max.x - 10.0f, 0.0f, roomBounds.max.z - 12.0f),
        glm::vec3(roomBounds.min.x + 12.0f, 0.0f, roomBounds.min.z + 14.0f)
    };

    return createFloorInstancesFromAnchors(
        anchorPositions,
        roomBounds,
        localBounds,
        floorOffset,
        demonScale,
        wallClearance);
}

// Agrega colisiones para props de piso
void addFurnitureCollisions(
    CollisionManager& manager,
    const AABB& localBounds,
    const std::vector<FurnitureInstance>& instances)
{
    for (const FurnitureInstance& instance : instances)
        manager.addStaticBox(buildScaledBounds(localBounds, instance.position, instance.scale));
}

// Agrega colisiones para cámaras
void addFurnitureCollisions(
    CollisionManager& manager,
    const AABB& localBounds,
    const glm::vec3& uniformScale,
    const std::vector<CameraInstance>& instances)
{
    for (const CameraInstance& instance : instances)
        manager.addStaticBox(buildScaledBounds(localBounds, instance.position, uniformScale));
}

// Agrega colisiones para props montados en pared
void addFurnitureCollisions(
    CollisionManager& manager,
    const AABB& localBounds,
    const std::vector<WallMountedInstance>& instances)
{
    for (const WallMountedInstance& instance : instances)
        manager.addStaticBox(buildScaledBounds(localBounds, instance.position, instance.scale));
}

// Genera cabinas telefónicas distribuidas en el nivel
std::vector<WallMountedInstance> createPublicPhoneBoothInstances(
    const AABB& roomBounds,
    const AABB& boothBounds,
    const glm::vec3& worldOffset)
{
    std::vector<WallMountedInstance> instances;

    std::random_device rd;
    std::mt19937 rng(rd());
    std::uniform_real_distribution<float> xDist(roomBounds.min.x + 2.5f, roomBounds.max.x - 2.5f);
    std::uniform_real_distribution<float> zDist(roomBounds.min.z + 2.5f, roomBounds.max.z - 2.5f);
    std::uniform_real_distribution<float> scaleVariation(1.3f, 1.7f);
    std::uniform_real_distribution<float> yawVariation(0.0f, 360.0f);

    const int targetCount = 50;
    const int maxAttempts = 2500;
    const float minDistance = 4.5f;
    const float wallClearance = 0.25f;

    for (int attempt = 0; attempt < maxAttempts && (int)instances.size() < targetCount; attempt++)
    {
        WallMountedInstance instance{};

        float s = scaleVariation(rng);
        instance.scale = glm::vec3(s, s, s);
        const glm::vec3 desiredPosition = glm::vec3(
            xDist(rng),
            worldOffset.y - (boothBounds.min.y * s),
            zDist(rng)
        );
        instance.position = keepInstanceInsideRoomXZ(
            desiredPosition,
            roomBounds,
            boothBounds,
            instance.scale,
            wallClearance);
        instance.yawDeg = yawVariation(rng);

        // Verifica que no esté demasiado cerca de otra cabina
        bool tooClose = false;
        for (const WallMountedInstance& placed : instances)
        {
            if (glm::distance(placed.position, instance.position) < minDistance)
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

// Dibuja las instancias de cámaras de vigilancia
void drawCameraInstances(
    Shader& shader,
    Model& cameraModel,
    const std::vector<CameraInstance>& instances,
    const glm::vec3& cameraScale)
{
    for (const CameraInstance& instance : instances)
    {
        glm::mat4 surveillanceModel = glm::mat4(1.0f);
        surveillanceModel = glm::translate(surveillanceModel, instance.position);
        surveillanceModel = glm::rotate(surveillanceModel, glm::radians(instance.yawDeg), glm::vec3(0.0f, 1.0f, 0.0f));
        surveillanceModel = glm::rotate(surveillanceModel, glm::radians(instance.pitchDeg), glm::vec3(1.0f, 0.0f, 0.0f));
        surveillanceModel = glm::scale(surveillanceModel, cameraScale);
        shader.setMat4("model", surveillanceModel);
        cameraModel.Draw(shader);
    }
}

// Dibuja las cajas de cartón (solo el mesh 0)
void drawOldPaperBoxesInstances(
    Shader& shader,
    Model& boxesModel,
    const std::vector<FurnitureInstance>& instances)
{
    if (boxesModel.meshes.empty())
        return;

    for (const FurnitureInstance& instance : instances)
    {
        glm::mat4 boxesModelMatrix = buildFurnitureModelMatrix(instance);
        shader.setMat4("model", boxesModelMatrix);
        boxesModel.meshes[0].Draw(shader);
    }
}

// Dibuja instancias genéricas de mobiliario
void drawFurnitureInstances(
    Shader& shader,
    Model& furnitureModel,
    const std::vector<FurnitureInstance>& instances)
{
    for (const FurnitureInstance& instance : instances)
    {
        glm::mat4 furnitureModelMatrix = buildFurnitureModelMatrix(instance);
        shader.setMat4("model", furnitureModelMatrix);
        furnitureModel.Draw(shader);
    }
}

// Dibuja instancias de props montados en pared
void drawFurnitureInstances(
    Shader& shader,
    Model& model,
    const std::vector<WallMountedInstance>& instances)
{
    for (const WallMountedInstance& instance : instances)
    {
        glm::mat4 modelMatrix = glm::mat4(1.0f);
        modelMatrix = glm::translate(modelMatrix, instance.position);
        modelMatrix = glm::rotate(modelMatrix, glm::radians(instance.yawDeg), glm::vec3(0.0f, 1.0f, 0.0f));
        modelMatrix = glm::scale(modelMatrix, instance.scale);
        shader.setMat4("model", modelMatrix);
        model.Draw(shader);
    }
}

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

    // 3. Inicializar GLAD (Cargador de punteros)
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        std::cout << "Error al inicializar GLAD. Asegurate de que glad.c este en el proyecto." << std::endl;
        return -1;
    }

    glEnable(GL_DEPTH_TEST);

    // 4. PRUEBA DE ASSIMP
    // Si esta linea no genera error de compilacion, tu .lib esta perfecto.
    Assimp::Importer importer;
    std::cout << "Las librerias y Assimp enlazaron correctamente!" << std::endl;

    // 5. Generar shaders
    Shader backroomsShader("shaders/backrooms.vs", "shaders/backrooms.fs");
    Shader cubeShader("shaders/basico.vs", "shaders/basico.fs");

    // 6. Cargar modelos
    Model backroomsModel("models/backrooms_level_0/backrooms.obj");
    Model backroomsCollisionsModel("models/backrooms_level_0_collisions/backrooms.obj");
    Model surveillanceCameraModel("models/surveillance_camera/scene.gltf");
    Model officeFurnitureModel("models/office_furniture/scene.gltf");
    Model oldPaperBoxesModel("models/old_paper__cardboard_boxes/scene.gltf");
    Model facelessBoneyHorrorDemonModel("models/faceless_boney_horror_demon/scene.gltf");
    Model sciFiComputerModel("models/sci-fi_computer/scene.gltf");
    Model publicPhoneBoothModel("models/public_phone_booth/scene.gltf");
    std::cout << "Meshes: " << backroomsCollisionsModel.meshes.size() << std::endl;

    camera.MovementSpeed = 10;

    // Generar instancias procedurales de cámaras y props
    CameraPlacementConfig cameraCfg;
    const AABB backroomsBounds = computeModelBounds(backroomsCollisionsModel);
    const std::vector<WallAnchor> wallAnchors = collectWallAnchors(backroomsCollisionsModel, backroomsPos);
    const AABB surveillanceCameraBounds = computeModelBounds(surveillanceCameraModel);
    const glm::vec3 roomCenter = (backroomsBounds.min + backroomsBounds.max) * 0.5f + backroomsPos;
    const float cameraWallAttachOffset = (-surveillanceCameraBounds.min.z * surveillanceCameraScale.z) + cameraCfg.wallClearance;
    
    const std::vector<CameraInstance> surveillanceInstances = generateCameraInstances(
        wallAnchors,
        roomCenter,
        cameraWallAttachOffset,
        cameraCfg);

    const AABB officeFurnitureBounds = computeModelBounds(officeFurnitureModel);
    const std::vector<FurnitureInstance> officeFurnitureInstances = createOfficeFurnitureInstances(
        backroomsBounds,
        officeFurnitureBounds,
        backroomsPos);
        
    const AABB oldPaperBoxesBounds = computeOldPaperBoxesBounds(oldPaperBoxesModel);
    const std::vector<FurnitureInstance> oldPaperBoxesInstances = createOldPaperBoxesInstances(
        backroomsBounds,
        oldPaperBoxesBounds,
        backroomsPos);
        
    const AABB facelessBoneyHorrorDemonBounds = computeModelBounds(facelessBoneyHorrorDemonModel);
    const std::vector<FurnitureInstance> facelessBoneyHorrorDemonInstances = createFacelessBoneyHorrorDemonInstances(
        backroomsBounds,
        facelessBoneyHorrorDemonBounds,
        backroomsPos);
        
    const AABB sciFiComputerBounds = computeModelBounds(sciFiComputerModel);
    const std::vector<FurnitureInstance> sciFiComputerInstances = createSciFiComputerInstances(
        backroomsBounds,
        sciFiComputerBounds,
        backroomsPos);
        
    const AABB publicPhoneBoothBounds = computeModelBounds(publicPhoneBoothModel);
    const std::vector<WallMountedInstance> publicPhoneBoothInstances = createPublicPhoneBoothInstances(
        backroomsBounds, 
        publicPhoneBoothBounds, 
        backroomsPos);

    std::cout << "Camaras colocadas: " << surveillanceInstances.size() << std::endl;
    
    // 7. Colisiones
    colManager.addStaticBox(backroomsCollisionsModel, backroomsPos);

    addFurnitureCollisions(colManager, surveillanceCameraBounds, surveillanceCameraScale, surveillanceInstances);
    addFurnitureCollisions(colManager, officeFurnitureBounds, officeFurnitureInstances);
    addFurnitureCollisions(colManager, oldPaperBoxesBounds, oldPaperBoxesInstances);
    addFurnitureCollisions(colManager, facelessBoneyHorrorDemonBounds, facelessBoneyHorrorDemonInstances);
    addFurnitureCollisions(colManager, sciFiComputerBounds, sciFiComputerInstances);
    addFurnitureCollisions(colManager, publicPhoneBoothBounds, publicPhoneBoothInstances);

    // 8. Bucle de Renderizado
    while (!glfwWindowShouldClose(window)) {
        
        float currentFrame = glfwGetTime();
        deltaTime = currentFrame - lastFrame;
        lastFrame = currentFrame;

        processInput(window);

        // Renderizado
        glClearColor(0.2f, 0.2f, 0.2f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        
        backroomsShader.use();

        // transformaciones de vista/proyeccion
        glm::mat4 projection = glm::perspective(glm::radians(camera.Zoom), (float)SCR_WIDTH / (float)SCR_HEIGHT, 0.1f, 100.0f);
        glm::mat4 view = camera.GetViewMatrix();
        backroomsShader.setMat4("projection", projection);
        backroomsShader.setMat4("view", view);

        // dibujar el modelo de backrooms
        glm::mat4 model = glm::mat4(1.0f);
        model = glm::translate(model, backroomsPos);
        backroomsShader.setMat4("model", model);
        backroomsModel.Draw(backroomsShader);

        // mostrar cajas de colision
        //cubeShader.use();
        //cubeShader.setMat4("projection", projection);
        //cubeShader.setMat4("view", view);
        //cubeShader.setVec3("cubeColor", glm::vec3(1.0f, 0.0f, 0.0f));

        //colManager.drawCollisionBoxes(cubeShader);

        // Dibujar todas las instancias de props
        drawCameraInstances(backroomsShader, surveillanceCameraModel, surveillanceInstances, surveillanceCameraScale);
        drawFurnitureInstances(backroomsShader, officeFurnitureModel, officeFurnitureInstances);
        drawOldPaperBoxesInstances(backroomsShader, oldPaperBoxesModel, oldPaperBoxesInstances);
        drawFurnitureInstances(backroomsShader, facelessBoneyHorrorDemonModel, facelessBoneyHorrorDemonInstances);
        drawFurnitureInstances(backroomsShader, sciFiComputerModel, sciFiComputerInstances);
        drawFurnitureInstances(backroomsShader, publicPhoneBoothModel, publicPhoneBoothInstances);

        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    // 9. Limpiar memoria
    glfwTerminate();
    return 0;
}

// process all input: query GLFW whether relevant keys are pressed/released this frame and react accordingly
void processInput(GLFWwindow* window)
{
    if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS)
        glfwSetWindowShouldClose(window, true);

    glm::vec3 oldPosition = camera.Position;

    if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS)
        camera.ProcessKeyboard(FORWARD, deltaTime, colManager);
    if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS)
        camera.ProcessKeyboard(BACKWARD, deltaTime, colManager);
    if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS)
        camera.ProcessKeyboard(LEFT, deltaTime, colManager);
    if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS)
        camera.ProcessKeyboard(RIGHT, deltaTime, colManager);
}

// glfw: whenever the window size changed (by OS or user resize) this callback function executes
void framebuffer_size_callback(GLFWwindow* window, int width, int height)
{
    // make sure the viewport matches the new window dimensions; note that width and 
    // height will be significantly larger than specified on retina displays.
    glViewport(0, 0, width, height);
}

// glfw: whenever the mouse moves, this callback is called
void mouse_callback(GLFWwindow* window, double xpos, double ypos)
{
    if (firstMouse)
    {
        lastX = xpos;
        lastY = ypos;
        firstMouse = false;
    }

    float xoffset = xpos - lastX;
    float yoffset = lastY - ypos; // reversed since y-coordinates go from bottom to top

    lastX = xpos;
    lastY = ypos;

    camera.ProcessMouseMovement(xoffset, yoffset);
}

// glfw: whenever the mouse scroll wheel scrolls, this callback is called
void scroll_callback(GLFWwindow* window, double xoffset, double yoffset)
{
    camera.ProcessMouseScroll(yoffset);
}