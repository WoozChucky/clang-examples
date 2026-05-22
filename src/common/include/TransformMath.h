#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include "ECS.h" // TransformComponent

// The single source of truth for an entity's world matrix: T * Rz * Ry * Rx * S
// (translate * rotZ * rotY * rotX * scale), Euler radians from TransformComponent::Rotation.
inline glm::mat4 ModelMatrix(const TransformComponent& t)
{
    glm::mat4 T  = glm::translate(glm::mat4(1.0f), t.Position);
    glm::mat4 Rx = glm::rotate(glm::mat4(1.0f), t.Rotation.x, glm::vec3(1.f, 0.f, 0.f));
    glm::mat4 Ry = glm::rotate(glm::mat4(1.0f), t.Rotation.y, glm::vec3(0.f, 1.f, 0.f));
    glm::mat4 Rz = glm::rotate(glm::mat4(1.0f), t.Rotation.z, glm::vec3(0.f, 0.f, 1.f));
    glm::mat4 S  = glm::scale(glm::mat4(1.0f), t.Scale);
    return T * Rz * Ry * Rx * S;
}
