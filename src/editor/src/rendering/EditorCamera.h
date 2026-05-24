#pragma once

#include <glm/glm.hpp>
#include "CameraView.h"

// Per-frame fly inputs, filled by the editor overlay from ImGui IO. Pure data.
struct EditorCameraInput {
    float MouseDX = 0.0f, MouseDY = 0.0f; // pixels this frame
    float Wheel   = 0.0f;                 // notches this frame
    bool  Look = false;                   // RMB held: mouse-look + WASD fly
    bool  Pan  = false;                   // MMB held: screen-space pan
    bool  Orbit = false;                  // Alt+LMB held: orbit the pivot
    bool  W=false, A=false, S=false, D=false, Q=false, E=false; // movement keys held
    bool  Frame = false;                  // F pressed this frame: frame the pivot
    bool  HasPivot = false;               // a selected entity exists (for orbit/frame)
    glm::vec3 PivotCenter{0.0f};          // selection AABB center (world)
    float     PivotRadius = 1.0f;         // selection bounding radius (world)
    float DeltaSeconds = 0.0f;
};

// Editor-owned free-look camera. Session-only state; no ImGui/NVRHI dependency (unit-testable).
class EditorCamera {
public:
    void Update(const EditorCameraInput& in);
    CameraView ToCameraView(float aspect) const; // perspectiveRH_ZO(Fov, aspect, Near, Far)

    glm::vec3 Forward() const;
    glm::vec3 Right() const;
    glm::vec3 Up() const;

    void FrameSelection(const glm::vec3& center, float radius);
    void OrbitAround(const glm::vec3& pivot, float dYaw, float dPitch);

    // Accessors for tests.
    glm::vec3 GetPosition() const { return m_Position; }
    float     GetYaw()   const { return m_Yaw; }
    float     GetPitch() const { return m_Pitch; }
    float     GetFlySpeed() const { return m_FlySpeed; }

private:
    glm::vec3 m_Position{0.0f, 5.0f, 10.0f}; // matches the game free-look default
    float m_Yaw   = 0.0f;   // radians around +Y; 0 => forward = -Z
    float m_Pitch = 0.0f;   // radians; +up; clamped +/- 89 deg
    float m_Fov   = glm::radians(60.0f);
    float m_FlySpeed = 7.5f; // units/sec; wheel-adjustable while looking

    static constexpr float kNear = 0.1f;
    static constexpr float kFar  = 1000.0f;
};
