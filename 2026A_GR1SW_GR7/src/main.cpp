#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <iostream>
#include <vector>
#include <random>
#include <cfloat>

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

void framebuffer_size_callback(GLFWwindow * window, int width, int height);
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
glm::vec3 surveillanceCameraScale(1.1f, 1.1f, 1.1f);

struct CameraInstance
{
    glm::vec3 position;
    float yawDeg;
    float pitchDeg;
};

struct WallAnchor
{
    glm::vec3 min;
    glm::vec3 max;
    bool thinX;
};

struct FurnitureInstance
{
    glm::vec3 position;
    glm::vec3 scale;
};

struct CameraPlacementConfig
{
    int targetCount = 200;
    int maxAttempts = 5000;
    float minCamDistance = 6.0f;
    float nearTopMin = 0.80f;
    float nearTopMax = 1.2f;
    // Distancia desde la pared para evitar que el cuerpo se meta en el muro.
    float faceOffsetMin = 0.45f;
    float faceOffsetMax = 0.75f;
    float pitchDownMin = -28.0f;
    float pitchDownMax = -12.0f;
    float yawJitterMin = -10.0f;
    float yawJitterMax = 10.0f;
};

CollisionManager colManager;

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
    const CameraPlacementConfig& cfg)
{
    std::vector<CameraInstance> instances;
    instances.reserve(cfg.targetCount);

    if (wallAnchors.empty())
        return instances;

    std::random_device rd;
    std::mt19937 rng(rd());
    std::uniform_real_distribution<float> nearTop(cfg.nearTopMin, cfg.nearTopMax);
    std::uniform_real_distribution<float> faceOffset(cfg.faceOffsetMin, cfg.faceOffsetMax);
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
            std::uniform_real_distribution<float> along(anchor.min.z + 0.2f, anchor.max.z - 0.2f);
            bool plusXFace = (rng() % 2) == 0;
            instance.position = glm::vec3(
                plusXFace ? (anchor.max.x + faceOffset(rng)) : (anchor.min.x - faceOffset(rng)),
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
            std::uniform_real_distribution<float> along(anchor.min.x + 0.2f, anchor.max.x - 0.2f);
            bool plusZFace = (rng() % 2) == 0;
            instance.position = glm::vec3(
                along(rng),
                y,
                plusZFace ? (anchor.max.z + faceOffset(rng)) : (anchor.min.z - faceOffset(rng))
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

void addCameraCollisions(CollisionManager& manager, const Model& cameraModel, const std::vector<CameraInstance>& instances)
{
    for (const CameraInstance& instance : instances)
        manager.addStaticBox(cameraModel, instance.position);
}

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

AABB buildScaledBounds(const AABB& localBounds, const glm::vec3& worldPosition, const glm::vec3& scale)
{
    AABB worldBounds;
    worldBounds.min = worldPosition + localBounds.min * scale;
    worldBounds.max = worldPosition + localBounds.max * scale;
    return worldBounds;
}

glm::mat4 buildFurnitureModelMatrix(const FurnitureInstance& instance)
{
    glm::mat4 model = glm::mat4(1.0f);
    model = glm::translate(model, instance.position);
    model = glm::scale(model, instance.scale);
    return model;
}

std::vector<FurnitureInstance> createOfficeFurnitureInstances(const AABB& localBounds, const glm::vec3& floorOffset)
{
    const float furnitureScale = 3.0f;
    const float floorY = floorOffset.y;

    std::vector<glm::vec3> anchorPositions =
    {
        // Distribuidos en zonas separadas del mapa para que no aparezcan juntos.
        glm::vec3(6.0f, 0.0f, -8.0f),
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

    // 6. Cargar modelos

    Model backroomsModel("models/backrooms_level_0/backrooms.obj");
    Model backroomsCollisionsModel("models/backrooms_level_0_collisions/backrooms.obj");
    Model surveillanceCameraModel("models/surveillance_camera/scene.gltf");
    Model officeFurnitureModel("models/office_furniture/scene.gltf");
    std::cout << "Meshes: " << backroomsCollisionsModel.meshes.size() << std::endl;

    CameraPlacementConfig cameraCfg;
    const std::vector<WallAnchor> wallAnchors = collectWallAnchors(backroomsCollisionsModel, backroomsPos);
    const std::vector<CameraInstance> surveillanceInstances = generateCameraInstances(wallAnchors, cameraCfg);

    const AABB officeFurnitureBounds = computeModelBounds(officeFurnitureModel);
    const std::vector<FurnitureInstance> officeFurnitureInstances = createOfficeFurnitureInstances(officeFurnitureBounds, backroomsPos);

    std::cout << "Camaras colocadas: " << surveillanceInstances.size() << std::endl;

    camera.MovementSpeed = 10;

    // 7. Colisiones

    colManager.addStaticBox(backroomsCollisionsModel, backroomsPos);
    addCameraCollisions(colManager, surveillanceCameraModel, surveillanceInstances);
    for (const FurnitureInstance& instance : officeFurnitureInstances)
    {
        colManager.addStaticBox(buildScaledBounds(officeFurnitureBounds, instance.position, instance.scale));
    }

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

        // dibujar camaras clonadas en paredes altas
        drawCameraInstances(backroomsShader, surveillanceCameraModel, surveillanceInstances, surveillanceCameraScale);

        // dibujar 3 modelos de office furniture como obstaculos solidos
        for (const FurnitureInstance& instance : officeFurnitureInstances)
        {
            glm::mat4 furnitureModel = buildFurnitureModelMatrix(instance);
            backroomsShader.setMat4("model", furnitureModel);
            officeFurnitureModel.Draw(backroomsShader);
        }

        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    // 9. Limpiar memoria
    glfwTerminate();

    return 0;
}

// process all input: query GLFW whether relevant keys are pressed/released this frame and react accordingly
// ---------------------------------------------------------------------------------------------------------
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

// glfw: whenever the window size changed (by OS or user resize) this callback function executes
// ---------------------------------------------------------------------------------------------
void framebuffer_size_callback(GLFWwindow* window, int width, int height)
{
    // make sure the viewport matches the new window dimensions; note that width and 
    // height will be significantly larger than specified on retina displays.
    glViewport(0, 0, width, height);
}

// glfw: whenever the mouse moves, this callback is called
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

// glfw: whenever the mouse scroll wheel scrolls, this callback is called
// ----------------------------------------------------------------------
void scroll_callback(GLFWwindow* window, double xoffset, double yoffset)
{
    (void)xoffset;
    camera.ProcessMouseScroll(yoffset);
}