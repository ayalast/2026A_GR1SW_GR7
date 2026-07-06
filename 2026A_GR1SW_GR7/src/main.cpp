#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <iostream>

// Incluimos Assimp SOLO para probar que el vinculador (Linker) no de errores.
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
const unsigned int SCR_WIDTH = 800, SCR_HEIGHT = 600;

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

// Instancia de una camara de vigilancia colocada sobre una pared.
struct CameraInstance
{
    glm::vec3 position;
    float yawDeg;
    float pitchDeg;
};

// Segmento de pared detectado en el modelo de colisiones, usado como "ancla" para pegar props (camaras) a el.
struct WallAnchor
{
    glm::vec3 min;
    glm::vec3 max;
    bool thinX;
};

// Instancia generica de mobiliario (posicion + escala uniforme por eje).
struct FurnitureInstance
{
    glm::vec3 position;
    glm::vec3 scale;
};

// Instancia de un prop montado en pared (con rotacion yaw y escala).
struct WallMountedInstance
{
    glm::vec3 position;
    float yawDeg;
    glm::vec3 scale;
};

// Parametros de distribucion procedural para camaras de vigilancia.
struct CameraPlacementConfig
{
    int targetCount = 200;
    int maxAttempts = 5000;
    float minCamDistance = 6.0f;
    float nearTopMin = 0.80f;
    float nearTopMax = 1.2f;
    // Pequeno margen para evitar z-fighting con la pared.
    float wallClearance = 0.02f;
    float pitchDownMin = -28.0f;
    float pitchDownMax = -12.0f;
    float yawJitterMin = -10.0f;
    float yawJitterMax = 10.0f;
};

//  CALCULO DE BOUNDS (AABB)
// Calcula el AABB (min/max) de TODOS los meshes de un modelo, en espacio local.
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

// Calcula el AABB de un unico mesh, en espacio local.
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

// Bounds especificos para el modelo de cajas de carton/papel viejo.
// En este asset, el mesh 0 es cajas; el mesh 1 es decal de papeles del piso.
// Por eso solo se toma el mesh 0 para no incluir el decal en el AABB.
AABB computeOldPaperBoxesBounds(const Model& model)
{
    if (model.meshes.empty())
        return computeModelBounds(model);

    return computeMeshBounds(model.meshes[0]);
}

// Transforma un AABB local (de un modelo) a espacio de mundo, aplicando
// una posicion y una escala uniforme/no uniforme.
AABB buildScaledBounds(const AABB& localBounds, const glm::vec3& worldPosition, const glm::vec3& scale)
{
    AABB worldBounds;
    worldBounds.min = worldPosition + localBounds.min * scale;
    worldBounds.max = worldPosition + localBounds.max * scale;
    return worldBounds;
}



//  DETECCION DE PAREDES Y COLOCACION PROCEDURAL DE CAMARAS DE VIGILANCIA
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
            // Pared orientada a lo largo del eje Z: elegimos la cara que
            // apunta al interior del nivel (hacia roomCenter).
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

            // Frente local del modelo ~= +Z, respaldo (WallAttach/Cord) ~= -Z.
            // Se rota para dejar el respaldo contra la pared y el lente hacia el pasillo.
            float baseYaw = plusXFace ? 90.0f : -90.0f;
            instance.yawDeg = baseYaw + yawJitter(rng);
        }
        else
        {
            // Pared orientada a lo largo del eje X: elegimos la cara que
            // apunta al interior del nivel (hacia roomCenter).
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

        // Descarta la instancia si queda demasiado cerca de otra ya colocada.
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

// Registra una caja de colision estatica por cada camara de vigilancia
// colocada, usando el modelo de la camara como base para el AABB.
void addCameraCollisions(
    CollisionManager& manager,
    const Model& cameraModel,
    const glm::vec3& cameraScale,
    const std::vector<CameraInstance>& instances)
{
    const AABB localBounds = computeModelBounds(cameraModel);

    for (const CameraInstance& instance : instances)
        manager.addStaticBox(buildScaledBounds(localBounds, instance.position, cameraScale));
}



//  CREACION DE INSTANCIAS DE MOBILIARIO (props sueltos en el piso)


// Construye la matriz de modelo (traslacion + escala) de una instancia de
// mobiliario. No aplica rotacion porque estos props no la necesitan.
glm::mat4 buildFurnitureModelMatrix(const FurnitureInstance& instance)
{
    glm::mat4 model = glm::mat4(1.0f);
    model = glm::translate(model, instance.position);
    model = glm::scale(model, instance.scale);
    return model;
}

// Crea 3 instancias fijas de "office furniture" en distintas zonas del mapa,
// apoyadas correctamente sobre el piso segun sus bounds locales.
std::vector<FurnitureInstance> createOfficeFurnitureInstances(const AABB& localBounds, const glm::vec3& floorOffset)
{
    const float furnitureScale = 3.0f;
    const float floorY = floorOffset.y;

    // Distribuidos en zonas separadas del mapa para que no aparezcan juntos.
    std::vector<glm::vec3> anchorPositions =
    {
        glm::vec3(50.0f, 0.0f, -5.0f),
        glm::vec3(-14.0f, 0.0f, -22.0f),
        glm::vec3(16.0f, 0.0f, -34.0f)
    };

    std::vector<FurnitureInstance> instances;
    instances.reserve(anchorPositions.size());

    for (const glm::vec3& anchor : anchorPositions)
    {
        FurnitureInstance instance{};
        instance.scale = glm::vec3(furnitureScale);
        instance.position = glm::vec3(
            anchor.x,
            floorY - (localBounds.min.y * furnitureScale),
            anchor.z
        );

        instances.push_back(instance);
    }

    return instances;
}

// Crea 3 instancias fijas de cajas de carton/papel viejo, apoyadas sobre
// el piso segun los bounds locales (ya filtrados para excluir el decal).
std::vector<FurnitureInstance> createOldPaperBoxesInstances(const AABB& localBounds, const glm::vec3& floorOffset)
{
    const float boxesScale = 2.2f;
    const float floorY = floorOffset.y;

    std::vector<glm::vec3> anchorPositions =
    {
        glm::vec3(1.0f, 0.0f, -12.0f),
        glm::vec3(12.0f, 0.0f, -40.0f),
        glm::vec3(-30.0f, 0.0f, -30.0f)
    };

    std::vector<FurnitureInstance> instances;
    instances.reserve(anchorPositions.size());

    for (const glm::vec3& anchor : anchorPositions)
    {
        FurnitureInstance instance{};
        instance.scale = glm::vec3(boxesScale);
        instance.position = glm::vec3(
            anchor.x,
            floorY - (localBounds.min.y * boxesScale),
            anchor.z
        );

        instances.push_back(instance);
    }

    return instances;
}

// Crea 3 computadoras sci-fi en zonas alejadas del mapa.
// Usa extremos del nivel para separarlas del resto de props.
std::vector<FurnitureInstance> createSciFiComputerInstances(
    const AABB& roomBounds,
    const AABB& localBounds,
    const glm::vec3& floorOffset)
{
    const float computerScale = 1.4f;
    const float floorY = floorOffset.y;

    std::vector<glm::vec3> anchorPositions =
    {
        glm::vec3(roomBounds.min.x + 6.0f, 0.0f, roomBounds.min.z + 6.0f),
        glm::vec3(roomBounds.max.x - 6.0f, 0.0f, roomBounds.min.z + 8.0f),
        glm::vec3(roomBounds.min.x + 8.0f, 0.0f, roomBounds.max.z - 6.0f)
    };

    std::vector<FurnitureInstance> instances;
    instances.reserve(anchorPositions.size());

    for (const glm::vec3& anchor : anchorPositions)
    {
        FurnitureInstance instance{};
        instance.scale = glm::vec3(computerScale);
        instance.position = glm::vec3(
            anchor.x,
            floorY - (localBounds.min.y * computerScale),
            anchor.z
        );

        instances.push_back(instance);
    }

    return instances;
}

// Agrega colisiones para props de piso con la misma regla (AABB escalado).
void addFurnitureCollisions(
    CollisionManager& manager,
    const AABB& localBounds,
    const std::vector<FurnitureInstance>& instances)
{
    for (const FurnitureInstance& instance : instances)
        manager.addStaticBox(buildScaledBounds(localBounds, instance.position, instance.scale));
}



//  CREACION Y COLISIONES DE PROPS MONTADOS EN PARED (cabinas telefonicas)


// Genera hasta 50 cabinas telefonicas publicas distribuidas aleatoriamente
// dentro de los bounds del nivel (roomBounds), respetando una distancia
// minima entre ellas y con escala/rotacion (yaw) aleatorias.
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

    for (int attempt = 0; attempt < maxAttempts && (int)instances.size() < targetCount; attempt++)
    {
        WallMountedInstance instance{};

        float s = scaleVariation(rng);
        instance.scale = glm::vec3(s, s, s);
        instance.position = glm::vec3(
            xDist(rng),
            worldOffset.y - (boothBounds.min.y * s),
            zDist(rng)
        );
        instance.yawDeg = yawVariation(rng);

        // Descarta la instancia si queda demasiado cerca de otra ya colocada.
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

// Registra una caja de colision estatica por cada instancia montada en
// pared (por ejemplo, cada cabina telefonica), usando el AABB local del
// modelo escalado y trasladado a la posicion de cada instancia.
void addWallMountedCollisions(CollisionManager& manager, const Model& model, const std::vector<WallMountedInstance>& instances)
{
    AABB localBounds = computeModelBounds(model);

    for (const WallMountedInstance& instance : instances)
    {
        manager.addStaticBox(buildScaledBounds(localBounds, instance.position, instance.scale));
    }
}



//  FUNCIONES DE DIBUJADO (por lote de instancias)


// Dibuja todas las camaras de vigilancia aplicando su transformacion
// individual (traslacion, yaw, pitch y escala global de camara).
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

// Dibuja las cajas de carton/papel viejo. Solo dibuja la malla de cajas
// (mesh 0) para evitar el piso negro del decal (mesh 1).
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

// Dibuja instancias genericas de mobiliario (traslacion + escala).
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

// Dibuja instancias montadas en pared (traslacion + rotacion yaw + escala).
void drawWallMountedInstances(
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



//  PUNTO DE ENTRADA
int main() {

    // 1) Inicializar GLFW y crear contexto OpenGL 3.3 Core
    glfwInit();
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    // 2) Crear ventana principal
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

    // 3) Inicializar GLAD (carga de funciones OpenGL)
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        std::cout << "Error al inicializar GLAD. Asegurate de que glad.c este en el proyecto." << std::endl;
        return -1;
    }

    glEnable(GL_DEPTH_TEST);

    // 4) PRUEBA DE ASSIMP
    // Si esta linea no genera error de compilacion, tu .lib esta perfecto.
    Assimp::Importer importer;
    std::cout << "Las librerias y Assimp enlazaron correctamente!" << std::endl;

    // 5) Cargar shaders
    Shader backroomsShader("shaders/backrooms.vs", "shaders/backrooms.fs");
    // Shader auxiliar para depurar AABB (uso opcional con codigo comentado).
    Shader cubeShader("shaders/basico.vs", "shaders/basico.fs");
    
    // 6) Cargar modelos
    Model backroomsModel("models/backrooms_level_0/backrooms.obj");
    Model backroomsCollisionsModel("models/backrooms_level_0_collisions/backrooms.obj");
    Model surveillanceCameraModel("models/surveillance_camera/scene.gltf");
    Model officeFurnitureModel("models/office_furniture/scene.gltf");
    Model oldPaperBoxesModel("models/old_paper__cardboard_boxes/scene.gltf");
    Model sciFiComputerModel("models/sci-fi_computer/scene.gltf");
    Model publicPhoneBoothModel("models/public_phone_booth/scene.gltf");
    std::cout << "Meshes: " << backroomsCollisionsModel.meshes.size() << std::endl;

    camera.MovementSpeed = 10;

    // 7) Generar instancias proceduralmente (camaras y props)
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
    const std::vector<FurnitureInstance> officeFurnitureInstances = createOfficeFurnitureInstances(officeFurnitureBounds, backroomsPos);
    const AABB oldPaperBoxesBounds = computeOldPaperBoxesBounds(oldPaperBoxesModel);
    const std::vector<FurnitureInstance> oldPaperBoxesInstances = createOldPaperBoxesInstances(oldPaperBoxesBounds, backroomsPos);
    const AABB sciFiComputerBounds = computeModelBounds(sciFiComputerModel);
    const std::vector<FurnitureInstance> sciFiComputerInstances = createSciFiComputerInstances(
        backroomsBounds,
        sciFiComputerBounds,
        backroomsPos);
    const AABB publicPhoneBoothBounds = computeModelBounds(publicPhoneBoothModel);
    const std::vector<WallMountedInstance> publicPhoneBoothInstances = createPublicPhoneBoothInstances(backroomsBounds, publicPhoneBoothBounds, backroomsPos);

    std::cout << "Camaras colocadas: " << surveillanceInstances.size() << std::endl;
    
    // 8) Registrar colisiones estaticas
    colManager.addStaticBox(backroomsCollisionsModel, backroomsPos);

    addCameraCollisions(colManager, surveillanceCameraModel, surveillanceCameraScale, surveillanceInstances);
    addFurnitureCollisions(colManager, officeFurnitureBounds, officeFurnitureInstances);
    addFurnitureCollisions(colManager, oldPaperBoxesBounds, oldPaperBoxesInstances);
    addFurnitureCollisions(colManager, sciFiComputerBounds, sciFiComputerInstances);
    addWallMountedCollisions(colManager, publicPhoneBoothModel, publicPhoneBoothInstances);

    
    // 9) Bucle principal de render
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

        // -- Capa de props: camaras y mobiliario --
        drawCameraInstances(backroomsShader, surveillanceCameraModel, surveillanceInstances, surveillanceCameraScale);

        drawFurnitureInstances(backroomsShader, officeFurnitureModel, officeFurnitureInstances);

        drawOldPaperBoxesInstances(backroomsShader, oldPaperBoxesModel, oldPaperBoxesInstances);

        drawFurnitureInstances(backroomsShader, sciFiComputerModel, sciFiComputerInstances);

        // -- Props montados en pared --
        drawWallMountedInstances(backroomsShader, publicPhoneBoothModel, publicPhoneBoothInstances);

        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    
    // 10) Liberar recursos de ventana/contexto
    glfwTerminate();

    return 0;
}

// Procesa el input continuo (teclas mantenidas presionadas) en cada frame:
// movimiento de camara (WASD) y salida con ESC.
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

// glfw: se ejecuta cuando el tamano de la ventana cambia (por el SO o el
// usuario), para ajustar el viewport de OpenGL al nuevo tamano.
// -------------------------------------------------------
void framebuffer_size_callback(GLFWwindow* window, int width, int height)
{
    // make sure the viewport matches the new window dimensions; note that width and 
    // height will be significantly larger than specified on retina displays.
    glViewport(0, 0, width, height);
}

// glfw: se llama cada vez que el mouse se mueve; actualiza la orientacion de la camara en primera persona.
// -------------------------------------------------------
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

// glfw: se llama cada vez que la rueda del mouse se desplaza; controla el zoom (FOV) de la camara.
void scroll_callback(GLFWwindow* window, double xoffset, double yoffset)
{
    camera.ProcessMouseScroll(yoffset);
}