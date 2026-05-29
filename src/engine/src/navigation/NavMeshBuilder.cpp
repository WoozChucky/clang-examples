#include "navigation/NavMeshBuilder.h"

#include <algorithm>
#include <cmath>
#include <span>

#include <glm/gtc/matrix_transform.hpp>

#include "ECS.h"
#include "navigation/NavMeshSystem.h"   // GetMeshCpuData
#include "lib.h"          // SM_WARN
#include "Engine.h"
#include "TransformMath.h"   // ModelMatrix — single source of truth for the entity world transform

namespace {

inline void EmitTri(NavMeshTriangleSoup& out, int i0, int i1, int i2, uint8_t areaId) {
    out.Tris.push_back(i0);
    out.Tris.push_back(i1);
    out.Tris.push_back(i2);
    out.Areas.push_back(areaId);
}

inline int EmitVert(NavMeshTriangleSoup& out, const glm::vec3& v) {
    const int idx = static_cast<int>(out.Verts.size() / 3);
    out.Verts.push_back(v.x);
    out.Verts.push_back(v.y);
    out.Verts.push_back(v.z);
    return idx;
}

inline void GrowAabb(NavMeshTriangleSoup& out, const glm::vec3& v) {
    if (out.Empty) {
        out.AabbMin = out.AabbMax = v;
        out.Empty = false;
    } else {
        out.AabbMin = glm::min(out.AabbMin, v);
        out.AabbMax = glm::max(out.AabbMax, v);
    }
}

inline glm::vec3 Xform(const glm::mat4& m, const glm::vec3& p) {
    const glm::vec4 r = m * glm::vec4(p, 1.0f);
    return glm::vec3(r.x, r.y, r.z);
}

// Use the canonical render transform (TransformMath::ModelMatrix) so the navmesh
// soup matches what's drawn. The old hand-rolled version treated Rotation as
// DEGREES (glm::radians) — but Rotation is radians (see ModelMatrix) — and used a
// different Euler order, so any rotated mesh baked misaligned from its visual.
inline glm::mat4 BuildWorld(const TransformComponent& t) {
    return ModelMatrix(t);
}

} // namespace

namespace NavMeshBuilder {

void TriangulateBox(const glm::vec3& he, const glm::mat4& xf, uint8_t area, NavMeshTriangleSoup& out)
{
    const glm::vec3 corners[8] = {
        {-he.x, -he.y, -he.z}, { he.x, -he.y, -he.z}, { he.x,  he.y, -he.z}, {-he.x,  he.y, -he.z},
        {-he.x, -he.y,  he.z}, { he.x, -he.y,  he.z}, { he.x,  he.y,  he.z}, {-he.x,  he.y,  he.z},
    };
    int idx[8];
    for (int i = 0; i < 8; ++i) {
        const glm::vec3 w = Xform(xf, corners[i]);
        idx[i] = EmitVert(out, w);
        GrowAabb(out, w);
    }
    auto Q = [&](int a, int b, int c, int d) {
        EmitTri(out, idx[a], idx[b], idx[c], area);
        EmitTri(out, idx[a], idx[c], idx[d], area);
    };
    // Wind each quad so its triangle normal points OUT of the box. Recast keys
    // walkability off normal.y vs cos(slope): the +Y face MUST have normal=+Y or
    // rcClearUnwalkableTriangles flags the box top NULL, then a flat-only scene
    // ends up with the (inverted) -Y face as the only walkable span, which the
    // overhead-clearance filter promptly kills (top-of-box NULL span sits a few
    // voxels above, gap < walkableHeight). Pin order: top (3,7,6,2), bottom
    // (0,1,5,4); sides match. Verified via cross-product (see test T08).
    Q(3,2,1,0); // -Z (normal -Z)
    Q(4,5,6,7); // +Z (normal +Z)
    Q(0,4,7,3); // -X (normal -X)
    Q(2,6,5,1); // +X (normal +X)
    Q(0,1,5,4); // -Y (normal -Y, bottom face)
    Q(3,7,6,2); // +Y (normal +Y, top face — must be walkable)
}

void TriangulateSphere(float r, const glm::mat4& xf, uint8_t area, int segs, NavMeshTriangleSoup& out)
{
    if (segs < 4) segs = 4;
    const int rings = segs / 2;
    std::vector<int> idx((rings + 1) * (segs + 1), 0);
    for (int ring = 0; ring <= rings; ++ring) {
        const float v = static_cast<float>(ring) / static_cast<float>(rings);
        const float phi = v * 3.14159265358979f;
        const float y = std::cos(phi);
        const float rr = std::sin(phi);
        for (int s = 0; s <= segs; ++s) {
            const float u = static_cast<float>(s) / static_cast<float>(segs);
            const float theta = u * 6.28318530717958f;
            const glm::vec3 local{ rr * std::cos(theta) * r, y * r, rr * std::sin(theta) * r };
            const glm::vec3 w = Xform(xf, local);
            const int i = EmitVert(out, w);
            GrowAabb(out, w);
            idx[ring * (segs + 1) + s] = i;
        }
    }
    for (int ring = 0; ring < rings; ++ring) {
        for (int s = 0; s < segs; ++s) {
            const int a = idx[ring * (segs + 1) + s];
            const int b = idx[ring * (segs + 1) + s + 1];
            const int c = idx[(ring + 1) * (segs + 1) + s + 1];
            const int d = idx[(ring + 1) * (segs + 1) + s];
            EmitTri(out, a, b, c, area);
            EmitTri(out, a, c, d, area);
        }
    }
}

void TriangulateCapsule(float r, float halfH, const glm::mat4& xf, uint8_t area, int segs, NavMeshTriangleSoup& out)
{
    if (segs < 4) segs = 4;
    std::vector<int> bot(segs + 1), top(segs + 1);
    for (int s = 0; s <= segs; ++s) {
        const float u = static_cast<float>(s) / static_cast<float>(segs);
        const float theta = u * 6.28318530717958f;
        const glm::vec3 lb{ r * std::cos(theta), -halfH, r * std::sin(theta) };
        const glm::vec3 lt{ r * std::cos(theta),  halfH, r * std::sin(theta) };
        const glm::vec3 wb = Xform(xf, lb); GrowAabb(out, wb);
        const glm::vec3 wt = Xform(xf, lt); GrowAabb(out, wt);
        bot[s] = EmitVert(out, wb);
        top[s] = EmitVert(out, wt);
    }
    for (int s = 0; s < segs; ++s) {
        EmitTri(out, bot[s], bot[s+1], top[s+1], area);
        EmitTri(out, bot[s], top[s+1], top[s],   area);
    }
    const glm::mat4 topCap = xf * glm::translate(glm::mat4(1.0f), glm::vec3(0,  halfH, 0));
    const glm::mat4 botCap = xf * glm::translate(glm::mat4(1.0f), glm::vec3(0, -halfH, 0));
    TriangulateSphere(r, topCap, area, segs, out);
    TriangulateSphere(r, botCap, area, segs, out);
}

NavMeshTriangleSoup CollectTriangles(const ECS& world)
{
    NavMeshTriangleSoup soup;

    world.Each<NavMeshSourceComponent>([&](EntityId e, const NavMeshSourceComponent& src) {
        const auto* tr = world.GetComponent<TransformComponent>(e);
        if (!tr) {
            SM_WARN("NavMeshSource entity %llu missing TransformComponent; skipped", e);
            return;
        }
        if (src.Geometry == NavMeshGeometrySource::Unset) {
            SM_WARN("NavMeshSource entity %llu Geometry=Unset; pick Collider or Mesh", e);
            return;
        }
        const glm::mat4 worldXform = BuildWorld(*tr);

        if (src.Geometry == NavMeshGeometrySource::Collider) {
            const auto* col = world.GetComponent<ColliderComponent>(e);
            if (!col) {
                SM_WARN("NavMeshSource entity %llu Geometry=Collider but no ColliderComponent; skipped", e);
                return;
            }
            const glm::mat4 xfWithOffset = glm::translate(worldXform, col->Offset);
            switch (col->Shape) {
                case ColliderShape::Box:
                    TriangulateBox(col->Size, xfWithOffset, src.AreaId, soup);
                    break;
                case ColliderShape::Sphere:
                    TriangulateSphere(col->Size.x, xfWithOffset, src.AreaId, 8, soup);
                    break;
                case ColliderShape::Capsule:
                    TriangulateCapsule(col->Size.x, col->Size.y, xfWithOffset, src.AreaId, 8, soup);
                    break;
            }
        } else { // NavMeshGeometrySource::Mesh
            const auto* mc = world.GetComponent<MeshComponent>(e);
            if (!mc) {
                SM_WARN("NavMeshSource entity %llu Geometry=Mesh but no MeshComponent; skipped", e);
                return;
            }
            std::span<const MeshVertex> meshVerts;
            std::span<const uint32_t>   meshIndices;
            if (!NavMeshSystem::Instance().GetMeshCpuData(mc->MeshId, meshVerts, meshIndices)) {
                SM_WARN("NavMeshSource entity %llu MeshId %u has no CPU data; skipped", e, mc->MeshId);
                return;
            }
            const int baseVert = static_cast<int>(soup.Verts.size() / 3);
            // MeshVertex layout: float px,py,pz; float nx,ny,nz; float u,v;  (ApplicationContext.h:57-62)
            for (const auto& v : meshVerts) {
                const glm::vec3 w = Xform(worldXform, glm::vec3(v.px, v.py, v.pz));
                soup.Verts.push_back(w.x);
                soup.Verts.push_back(w.y);
                soup.Verts.push_back(w.z);
                GrowAabb(soup, w);
            }
            for (size_t i = 0; i + 2 < meshIndices.size(); i += 3) {
                EmitTri(soup,
                        baseVert + static_cast<int>(meshIndices[i + 0]),
                        baseVert + static_cast<int>(meshIndices[i + 1]),
                        baseVert + static_cast<int>(meshIndices[i + 2]),
                        src.AreaId);
            }
        }
    });

    return soup;
}

} // namespace NavMeshBuilder
