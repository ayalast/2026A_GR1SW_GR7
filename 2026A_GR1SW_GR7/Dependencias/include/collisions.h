#ifndef COLLISION_H
#define COLLISION_H

#include <glad/glad.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <vector>
#include <shader.h>
#include <mesh.h>
#include <model.h>

struct AABB
{
    glm::vec3 min;
    glm::vec3 max;
};

// An abstract camera class that processes input and calculates the corresponding Euler Angles, Vectors and Matrices for use in OpenGL
class CollisionManager
{
public:
    //Agregar una caja de colision
    void addStaticBox(const AABB& box)
    {
        this->boxes.push_back(box);
    }
    //Agregar vector de caja de colisiones
    void addStaticBox(const std::vector<AABB>& boxs)
    {
        for (const AABB& b : boxs)
        {
            this->boxes.push_back(b);
        }
    }
    //Calcular caja de colisiones para una malla
    void addStaticBox(const Mesh& mesh, glm::vec3 offset)
    {
        const auto& vertices = mesh.vertices;

        glm::vec3 min(FLT_MAX, FLT_MAX, FLT_MAX);
        glm::vec3 max(-FLT_MAX, -FLT_MAX, -FLT_MAX);

        for (const Vertex& vertex : vertices)
        {
            min = glm::min(min, vertex.Position);
            max = glm::max(max, vertex.Position);
        }

        AABB box = {min + offset, max + offset};
        this->addStaticBox(box);
    }
    //Calcular caja de colisiones para un modelo
    void addStaticBox(const Model& model, glm::vec3 offset)
    {
        const auto& meshes = model.meshes;

        for (const Mesh& mesh : meshes)
        {
            this->addStaticBox(mesh, offset);
        }
    }

    bool checkCameraCollision(glm::vec3 position, float radius)
    {
        for (const AABB& box : this->boxes)
        {
            glm::vec3 closest = glm::clamp(position, box.min, box.max);
            glm::vec3 distance = position - closest;

            if (glm::dot(distance, distance) < radius * radius) return true;
        }

        return false;
    }

    void drawCollisionBoxes(Shader& shader)
    {
        initializeBoxRenderer();

        shader.use();

        glBindVertexArray(boxVAO);

        for (const AABB& box : boxes)
        {
            glm::vec3 center = (box.min + box.max) * 0.5f;
            glm::vec3 size = box.max - box.min;

            glm::mat4 model(1.0f);

            model = glm::translate(model, center);
            model = glm::scale(model, size);

            shader.setMat4("model", model);

            glDrawElements(GL_LINES,
                24,
                GL_UNSIGNED_INT,
                0);
        }

        glBindVertexArray(0);
    }

private:
	std::vector<AABB> boxes;

    unsigned int boxVAO = 0;
    unsigned int boxVBO = 0;

    void initializeBoxRenderer()
    {
        if (boxVAO != 0)
            return;

        float vertices[] =
        {
            // back face
            -0.5f,-0.5f,-0.5f,
             0.5f,-0.5f,-0.5f,
             0.5f, 0.5f,-0.5f,
            -0.5f, 0.5f,-0.5f,

            // front face
            -0.5f,-0.5f, 0.5f,
             0.5f,-0.5f, 0.5f,
             0.5f, 0.5f, 0.5f,
            -0.5f, 0.5f, 0.5f
        };

        unsigned int indices[] =
        {
            0,1, 1,2, 2,3, 3,0,
            4,5, 5,6, 6,7, 7,4,
            0,4, 1,5, 2,6, 3,7
        };

        unsigned int EBO;

        glGenVertexArrays(1, &boxVAO);
        glGenBuffers(1, &boxVBO);
        glGenBuffers(1, &EBO);

        glBindVertexArray(boxVAO);

        glBindBuffer(GL_ARRAY_BUFFER, boxVBO);
        glBufferData(GL_ARRAY_BUFFER,
            sizeof(vertices),
            vertices,
            GL_STATIC_DRAW);

        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, EBO);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,
            sizeof(indices),
            indices,
            GL_STATIC_DRAW);

        glVertexAttribPointer(0,
            3,
            GL_FLOAT,
            GL_FALSE,
            3 * sizeof(float),
            (void*)0);
        glEnableVertexAttribArray(0);

        glBindVertexArray(0);
    }
};
#endif