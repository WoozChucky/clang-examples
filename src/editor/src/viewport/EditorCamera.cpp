#include "EditorCamera.h"

#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>

namespace {
constexpr float kLookSens = 0.0035f; // radians per pixel
constexpr float kPanScale = 0.01f;   // world units per pixel
constexpr float kDollyStep = 1.0f;   // world units per wheel notch
constexpr float kPitchLimit = glm::radians(89.0f);
}

glm::vec3 EditorCamera::Forward() const
{
    return glm::normalize(glm::vec3(
        -std::sin(m_Yaw) * std::cos(m_Pitch),
         std::sin(m_Pitch),
        -std::cos(m_Yaw) * std::cos(m_Pitch)));
}

glm::vec3 EditorCamera::Right() const
{
    return glm::normalize(glm::cross(Forward(), glm::vec3(0.0f, 1.0f, 0.0f)));
}

glm::vec3 EditorCamera::Up() const
{
    return glm::normalize(glm::cross(Right(), Forward()));
}

void EditorCamera::OrbitAround(const glm::vec3& pivot, float dYaw, float dPitch)
{
    glm::vec3 offset = m_Position - pivot;
    const float dist = glm::length(offset);
    if (dist < 1e-4f) return;

    float yaw   = std::atan2(offset.x, offset.z);
    float pitch = std::asin(std::clamp(offset.y / dist, -1.0f, 1.0f));
    yaw   += dYaw;
    pitch  = std::clamp(pitch + dPitch, -kPitchLimit, kPitchLimit);

    offset = dist * glm::vec3(std::cos(pitch) * std::sin(yaw),
                              std::sin(pitch),
                              std::cos(pitch) * std::cos(yaw));
    m_Position = pivot + offset;

    // Re-aim at the pivot.
    const glm::vec3 dir = glm::normalize(pivot - m_Position);
    m_Yaw   = std::atan2(-dir.x, -dir.z);
    m_Pitch = std::asin(std::clamp(dir.y, -1.0f, 1.0f));
}

void EditorCamera::FrameSelection(const glm::vec3& center, float radius)
{
    if (radius < 1e-4f) radius = 1.0f;
    const float dist = radius / std::sin(m_Fov * 0.5f); // fit the bounding sphere in the vertical FOV
    m_Position = center - Forward() * dist;             // back off along current forward -> looks at center
}

void EditorCamera::Update(const EditorCameraInput& in)
{
    if (in.Look) {
        m_Yaw   -= in.MouseDX * kLookSens;
        m_Pitch  = std::clamp(m_Pitch - in.MouseDY * kLookSens, -kPitchLimit, kPitchLimit);

        const glm::vec3 fwd = Forward();
        const glm::vec3 right = Right();
        const glm::vec3 worldUp(0.0f, 1.0f, 0.0f);
        const float v = m_FlySpeed * in.DeltaSeconds;
        if (in.W) m_Position += fwd * v;
        if (in.S) m_Position -= fwd * v;
        if (in.D) m_Position += right * v;
        if (in.A) m_Position -= right * v;
        if (in.E) m_Position += worldUp * v;
        if (in.Q) m_Position -= worldUp * v;

        if (in.Wheel != 0.0f)
            m_FlySpeed = std::clamp(m_FlySpeed * (1.0f + 0.1f * in.Wheel), 0.5f, 200.0f);
    } else if (in.Wheel != 0.0f) {
        m_Position += Forward() * (in.Wheel * kDollyStep); // dolly when not looking
    }

    if (in.Pan) {
        m_Position -= Right() * (in.MouseDX * kPanScale);
        m_Position += Up()    * (in.MouseDY * kPanScale);
    }

    if (in.Orbit) {
        const glm::vec3 pivot = in.HasPivot ? in.PivotCenter : glm::vec3(0.0f);
        OrbitAround(pivot, -in.MouseDX * kLookSens, -in.MouseDY * kLookSens);
    }

    if (in.Frame && in.HasPivot)
        FrameSelection(in.PivotCenter, in.PivotRadius);
}

CameraView EditorCamera::ToCameraView(float aspect) const
{
    if (aspect <= 0.0f) aspect = 16.0f / 9.0f;
    CameraView cv;
    cv.View       = glm::lookAtRH(m_Position, m_Position + Forward(), glm::vec3(0.0f, 1.0f, 0.0f));
    cv.Projection = glm::perspectiveRH_ZO(m_Fov, aspect, kNear, kFar);
    cv.Position   = m_Position;
    return cv;
}

EditorCameraState EditorCamera::GetState() const
{
    return EditorCameraState{ m_Position, m_Yaw, m_Pitch, m_FlySpeed };
}

void EditorCamera::SetState(const EditorCameraState& s)
{
    if (std::isfinite(s.Position.x) && std::isfinite(s.Position.y) && std::isfinite(s.Position.z))
        m_Position = s.Position;
    if (std::isfinite(s.Yaw))   m_Yaw   = s.Yaw;
    if (std::isfinite(s.Pitch)) m_Pitch = std::clamp(s.Pitch, -kPitchLimit, kPitchLimit);
    if (std::isfinite(s.FlySpeed))
        m_FlySpeed = std::clamp(s.FlySpeed, 0.5f, 200.0f);
}
