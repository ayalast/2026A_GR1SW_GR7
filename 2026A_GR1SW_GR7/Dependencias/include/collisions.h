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

    //Comprobar colision esferica con caja de colision
    bool sphereIntersects(AABB box, glm::vec3 position, float radius) const
    {
        glm::vec3 closest = glm::clamp(position, box.min, box.max);
        glm::vec3 distance = position - closest;

        return glm::dot(distance, distance) < radius * radius;
    }

    glm::vec3 getCollisionNormal(const AABB& box, glm::vec3 position)
    {
        glm::vec3 closest = glm::clamp(position, box.min, box.max);
        glm::vec3 delta = position - closest;
        float dist2 = glm::dot(delta, delta);

        // Sphere center is outside the box (edge/corner collision)
        if (dist2 > 0.000001f)
            return glm::normalize(delta);

        // Sphere center is inside the box
        float left = position.x - box.min.x;
        float right = box.max.x - position.x;
        float bottom = position.y - box.min.y;
        float top = box.max.y - position.y;
        float back = position.z - box.min.z;
        float front = box.max.z - position.z;

        float minDist = left;
        glm::vec3 normal(-1, 0, 0);

        if (right < minDist)
        {
            minDist = right;
            normal = glm::vec3(1, 0, 0);
        }

        if (bottom < minDist)
        {
            minDist = bottom;
            normal = glm::vec3(0, -1, 0);
        }

        if (top < minDist)
        {
            minDist = top;
            normal = glm::vec3(0, 1, 0);
        }

        if (back < minDist)
        {
            minDist = back;
            normal = glm::vec3(0, 0, -1);
        }

        if (front < minDist)
        {
            normal = glm::vec3(0, 0, 1);
        }

        return normal;
    }

    glm::vec3 correctMovement(glm::vec3 position,glm::vec3 movement, float radius)
    {
        glm::vec3 corrected = movement;

        for (const AABB& box : boxes)
        {
            glm::vec3 newPos = position + corrected;

            if (!sphereIntersects(box, newPos, radius))
                continue;

            glm::vec3 normal = getCollisionNormal(box, newPos);

            float intoWall = glm::dot(corrected, normal);

            // Only remove movement into the wall
            if (intoWall < 0.0f)
                corrected -= intoWall * normal;
        }

        return corrected;
    }

    size_t staticBoxCount() const { return boxes.size(); }

    // Borra [start, start+count). Usado para quitar AABBs de monstruos estaticos en survival.
    void eraseStaticBoxes(size_t start, size_t count)
    {
        if (start >= boxes.size() || count == 0)
            return;
        size_t end = start + count;
        if (end > boxes.size())
            end = boxes.size();
        boxes.erase(boxes.begin() + static_cast<std::ptrdiff_t>(start),
                    boxes.begin() + static_cast<std::ptrdiff_t>(end));
    }

    bool sphereBlocked(const glm::vec3& position, float radius) const
    {
        for (const AABB& box : boxes)
        {
            if (sphereIntersects(box, position, radius))
                return true;
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