#pragma once

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/gtc/matrix_transform.hpp>

struct OrthographicCamera2D
{
    float zoom = 1.0f;
    glm::vec2 dimensions;
    glm::vec2 position;
};

struct PerspectiveCamera3D
{
    // Lens parameters
    float fov = glm::radians(80.0f);        // vertical field of view in radians
    float aspectRatio = 16.0f/9.0f;
    float nearClip = 0.1f;
    float farClip = 1000.0f;

    // Transform (right-handed: x-right, y-up, z-forward by convention)
    // rotation = { pitch (x), yaw (y), roll (z) } in radians
    glm::vec3 position {0.0f, 0.0f, 3.0f};
    glm::vec3 rotation {0.0f, 0.0f, 0.0f};

    // Caching to avoid recomputing each frame
    bool m_ViewDirty = true;
    bool m_ProjDirty = true;
    glm::mat4 m_ViewCache = glm::mat4(1.0f);
    glm::mat4 m_ProjCache = glm::mat4(1.0f);

    inline void invalidate()
    {
        m_ViewDirty = true; m_ProjDirty = true;
    }

    glm::mat4 get_view_matrix()
    {
        if (!m_ViewDirty)
        {
            return m_ViewCache;
        }
        glm::mat4 T  = glm::translate(glm::mat4(1.0f), -position);
        glm::mat4 Rx = glm::rotate(glm::mat4(1.0f), -rotation.x, {1,0,0});
        glm::mat4 Ry = glm::rotate(glm::mat4(1.0f), -rotation.y, {0,1,0});
        glm::mat4 Rz = glm::rotate(glm::mat4(1.0f), -rotation.z, {0,0,1});
        m_ViewCache = (Rz * Ry * Rx) * T;

        m_ViewDirty = false;
        return m_ViewCache;
    }

    glm::mat4 get_projection_matrix()
    {
        if (!m_ProjDirty)
        {
            return m_ProjCache;
        }

        m_ProjCache = glm::perspectiveRH_ZO(fov, aspectRatio, nearClip, farClip);
        m_ProjDirty = false;
        return m_ProjCache;
    }
};