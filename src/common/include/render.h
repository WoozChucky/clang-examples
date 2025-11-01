#pragma once

#include "input.h"
#include "lib.h"


#include "glm/matrix.hpp"
#include "glm/gtx/quaternion.hpp"

// #############################################################################
//                           Cameras
// #############################################################################
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

// #############################################################################
//                     Font glyph metrics (renderer-owned atlas uses this)
// #############################################################################
struct Glyph
{
  glm::vec2 size;          // pixel size in the atlas
  glm::vec2 offset;        // bearing from baseline (top-left convention)
  glm::vec2 advance;       // advance in pixels
  glm::vec2 textureCoords; // top-left in atlas (pixels)
};

// #############################################################################
//                     UI Command Buffer (renderer-agnostic)
// #############################################################################
constexpr int UI_MAX_RECTS = 8192;
constexpr int UI_MAX_TEXTS = 1024;
constexpr int UI_TEXT_BUFFER_BYTES = 16384;

struct UIRectCmd
{
  glm::vec2 pos;    // top-left in UI pixels
  glm::vec2 size;   // width/height in pixels
  glm::vec4 color;  // rgba (straight alpha)
};

struct UITextCmd
{
  glm::vec2 pos;         // baseline top-left in UI pixels
  glm::vec4 color;       // rgba
  uint32_t textOffset;   // offset into RenderData::uiTextBuffer
  uint32_t textLength;   // number of bytes (without null terminator)
  uint32_t glyphCount;   // precomputed ASCII glyph count (<128) to avoid double iteration in renderer
  uint16_t fontIndex;    // which font to use (index into renderer font list)
  uint16_t _pad = 0;
};

// #############################################################################
//                           3D Primitives (game -> renderer)
// #############################################################################
enum class PrimitiveType : uint32_t { Plane = 0, Cube = 1, Sphere = 2, Cone = 3, Line = 4 };

struct PrimitiveInstance
{
  PrimitiveType type;
  glm::mat4     transform; // world transform
  glm::vec4     color;     // base color (not all primitives use it)
  glm::vec4     params;    // optional parameters per primitive
};

constexpr int PRIM_MAX = 1024;

typedef struct RenderData
{
  glm::vec4 clearColor;

  PerspectiveCamera3D gameCamera;
  // 3D model transform for the cube (or current object)
  glm::mat4 modelMatrix3D = glm::mat4(1.0f);
  glm::vec3 modelPosition {0.f, 0.f, 0.f};
  glm::vec3 modelRotation {0.f, 0.f, 0.f};
  glm::vec3 modelScale    {1.f, 1.f, 1.f};

  OrthographicCamera2D uiCamera;
  glm::mat4 orthoProjectionUI;

  // UI command buffers for the current frame
  Array<UIRectCmd, UI_MAX_RECTS> uiRects;
  Array<UITextCmd, UI_MAX_TEXTS> uiTexts;
  char uiTextBuffer[UI_TEXT_BUFFER_BYTES] = {};
  uint32_t uiTextBufferCount = 0; // bytes used in uiTextBuffer

  // 3D primitives (immediate mode style, cleared every frame)
  Array<PrimitiveInstance, PRIM_MAX> primitives;
} RenderData;

// #############################################################################
//                           Render Interface Globals
// #############################################################################
// static RenderData* g_RenderData;

// #############################################################################
//                           Render Interface Camera Utility
// #############################################################################
inline glm::ivec2 screen_to_camera(const Input* input, OrthographicCamera2D camera, glm::ivec2 screenPos)
{
  float xPos = (float)screenPos.x /
               input->screenSize.x *
               camera.dimensions.x; // [0; dimensions.x]

  // Offset using dimensions and position
  xPos += -camera.dimensions.x / 2.0f + camera.position.x;

  float yPos = (float)screenPos.y /
               input->screenSize.y *
               camera.dimensions.y; // [0; dimensions.y]

  // Offset using dimensions and position
  yPos += camera.dimensions.y / 2.0f + camera.position.y;

  return {(int)xPos, (int)yPos};
}

inline glm::ivec2 screen_to_ui(const Input* input, const RenderData* renderData, glm::ivec2 screenPos)
{
  return screen_to_camera(input, renderData->uiCamera, screenPos);
}

inline glm::ivec2 screen_to_world(const Input* input, const RenderData* renderData, glm::ivec2 screenPos)
{
  // For now, map screen to 2D UI space; 3D picking can be added later.
  return screen_to_camera(input, renderData->uiCamera, screenPos);
}

// #############################################################################
//                           UI Helper Functions (game layer calls these)
// #############################################################################
inline void ui_draw_rect(RenderData* renderData, glm::vec2 topLeft, glm::vec2 size, glm::vec4 color)
{
  if (!renderData) return;
  if (renderData->uiRects.count >= renderData->uiRects.maxElements) return;
  UIRectCmd cmd{};
  cmd.pos = topLeft;
  cmd.size = size;
  cmd.color = color;
  renderData->uiRects.add(cmd);
}

inline void ui_draw_hline(RenderData* renderData, glm::vec2 startTopLeft, float length, float thickness, glm::vec4 color)
{
  ui_draw_rect(renderData, startTopLeft, {length, thickness}, color);
}

inline void ui_draw_vline(RenderData* renderData, glm::vec2 startTopLeft, float length, float thickness, glm::vec4 color)
{
  ui_draw_rect(renderData, startTopLeft, {thickness, length}, color);
}

inline void ui_draw_text_ex(RenderData* renderData, const char* text, glm::vec2 pos, glm::vec4 color, uint16_t fontIndex)
{
  if (!renderData || !text) return;
  const size_t len = strlen(text);
  if (len == 0) return;
  if (len + renderData->uiTextBufferCount >= UI_TEXT_BUFFER_BYTES)
    return; // out of space, drop for now
  if (renderData->uiTexts.count >= renderData->uiTexts.maxElements)
    return; // out of commands

  // Precompute ASCII glyph count (<128) so renderer doesn't need a counting pass
  uint32_t glyphCount = 0;
  for (size_t i = 0; i < len; ++i)
  {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    if (c < 128u) ++glyphCount;
  }

  const uint32_t offset = renderData->uiTextBufferCount;
  memcpy(renderData->uiTextBuffer + offset, text, len);
  renderData->uiTextBufferCount += static_cast<uint32_t>(len);

  UITextCmd cmd{};
  cmd.pos = pos;
  cmd.color = color;
  cmd.textOffset = offset;
  cmd.textLength = static_cast<uint32_t>(len);
  cmd.glyphCount = glyphCount;
  cmd.fontIndex = fontIndex;
  renderData->uiTexts.add(cmd);
}

inline void ui_draw_text(RenderData* renderData, const char* text, glm::vec2 pos, glm::vec4 color)
{
  ui_draw_text_ex(renderData, text, pos, color, 0);
}

inline void ui_begin_frame(RenderData* renderData)
{
  if (!renderData) return;
  renderData->uiRects.clear();
  renderData->uiTexts.clear();
  renderData->uiTextBufferCount = 0;
}

// #############################################################################
//                           Primitives Helper Functions
// #############################################################################
inline void prim_begin_frame(RenderData* renderData)
{
  if (!renderData) return;
  renderData->primitives.clear();
}

inline void prim_draw_grid_plane(RenderData* renderData, const glm::mat4& transform)
{
  if (!renderData) return;
  if (renderData->primitives.count >= renderData->primitives.maxElements) return;
  PrimitiveInstance inst{};
  inst.type = PrimitiveType::Plane;
  inst.transform = transform; // Typically identity for Y=0 grid
  inst.color = {1,1,1,1};
  inst.params = {0,0,0,0};
  renderData->primitives.add(inst);
}