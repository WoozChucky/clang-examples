#pragma once
#include <cstdint>

#include <glm/vec3.hpp>

struct TransformComponent {
    glm::vec3 Position;
    glm::vec3 Rotation;
    glm::vec3 Scale;
};

struct MeshComponent {

};

using EntityId = uint64_t;

struct ComponentStore {

};

struct EntityStore {

};