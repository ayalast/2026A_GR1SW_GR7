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
glm::vec3 surveillanceCameraPos(3.5f, -1.6f, 2.0f);
float surveillanceCameraYawDeg = 180.0f;
glm::vec3 surveillanceCameraScale(0.35f, 0.35f, 0.35f);

CollisionManager colManager;

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
    std::cout << "Meshes: " << backroomsCollisionsModel.meshes.size() << std::endl;

    camera.MovementSpeed = 10;

    // 7. Colisiones

    colManager.addStaticBox(backroomsCollisionsModel, backroomsPos);
    colManager.addStaticBox(surveillanceCameraModel, surveillanceCameraPos);

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

        // dibujar modelo secundario: camara de vigilancia
        glm::mat4 surveillanceModel = glm::mat4(1.0f);
        surveillanceModel = glm::translate(surveillanceModel, surveillanceCameraPos);
        surveillanceModel = glm::rotate(surveillanceModel, glm::radians(surveillanceCameraYawDeg), glm::vec3(0.0f, 1.0f, 0.0f));
        surveillanceModel = glm::scale(surveillanceModel, surveillanceCameraScale);
        backroomsShader.setMat4("model", surveillanceModel);
        surveillanceCameraModel.Draw(backroomsShader);

        // mostrar cajas de colision
        //cubeShader.use();
        //cubeShader.setMat4("projection", projection);
        //cubeShader.setMat4("view", view);
        //cubeShader.setVec3("cubeColor", glm::vec3(1.0f, 0.0f, 0.0f));

        //colManager.drawCollisionBoxes(cubeShader);

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
    camera.ProcessMouseScroll(yoffset);
}