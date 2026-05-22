#pragma once
#include <algorithm>
#include <cmath>
#include <limits>
#include <glm/glm.hpp>

struct Ray { glm::vec3 Origin; glm::vec3 Dir; };

// World-space ray from a click at (mouseX,mouseY) screen coords inside the Viewport image rect
// [vpMinX,vpMinY, vpW x vpH]. ImGui Y is top-down -> NDC Y flipped. Unprojects near (z=0) and
// far (z=1) — depth [0,1] (ZO) — through inverse(proj*view).
inline Ray ScreenPointToRay(float mouseX, float mouseY,
                            float vpMinX, float vpMinY, float vpW, float vpH,
                            const glm::mat4& view, const glm::mat4& proj)
{
    const float ndcX = 2.0f * (mouseX - vpMinX) / vpW - 1.0f;
    const float ndcY = 1.0f - 2.0f * (mouseY - vpMinY) / vpH;
    const glm::mat4 invVP = glm::inverse(proj * view);
    glm::vec4 nearP = invVP * glm::vec4(ndcX, ndcY, 0.0f, 1.0f);
    glm::vec4 farP  = invVP * glm::vec4(ndcX, ndcY, 1.0f, 1.0f);
    // For a finite RH perspective and a click inside the viewport, nearP.w/farP.w are > 0.
    const glm::vec3 n = glm::vec3(nearP) / nearP.w;
    const glm::vec3 f = glm::vec3(farP)  / farP.w;
    Ray r;
    r.Origin = n;
    r.Dir = glm::normalize(f - n);
    return r;
}

// Slab test. Returns true and the entry distance tHit (clamped >= 0; 0 if origin is inside)
// when the ray hits the AABB. A box entirely behind the ray returns false.
inline bool RayIntersectsAABB(const Ray& r, glm::vec3 aabbMin, glm::vec3 aabbMax, float& tHit)
{
    float tmin = 0.0f;
    float tmax = std::numeric_limits<float>::max();
    for (int i = 0; i < 3; ++i)
    {
        const float o = r.Origin[i];
        const float d = r.Dir[i];
        if (std::abs(d) < 1e-8f)
        {
            if (o < aabbMin[i] || o > aabbMax[i])
                return false; // parallel to this slab and outside it
        }
        else
        {
            const float inv = 1.0f / d;
            float t1 = (aabbMin[i] - o) * inv;
            float t2 = (aabbMax[i] - o) * inv;
            if (t1 > t2) std::swap(t1, t2);
            if (t1 > tmin) tmin = t1;
            if (t2 < tmax) tmax = t2;
            if (tmin > tmax) return false;
        }
    }
    tHit = tmin;
    return true;
}
