#pragma once

#include "input.h"
#include "lib.h"

#include "glm/matrix.hpp"
#include "glm/gtx/quaternion.hpp"

inline int RENDERING_OPTION_FONT = BIT(1);
inline int RENDERING_OPTION_TRANSPARENT = BIT(2);

// #############################################################################
//                           Rendering Structs
// #############################################################################
struct Transform
{
  glm::vec2 pos; // This is currently the Top Left!!
  glm::vec2 size;
  glm::vec2 atlasOffset;
  glm::vec2 spriteSize;
  int renderOptions;
  float layer;
};

struct Material
{
  glm::vec4 color;
};

// #############################################################################
//                           Render Interface Constants
// #############################################################################
constexpr int MAX_TRANSFORMS = 10000;

// #############################################################################
//                           Render Interface Structs
// #############################################################################
struct DrawData
{
  // Used to generate an X - Offset based on the 
  // X - Size of the Sprite
  int animationIdx;
  int renderOptions;
  float layer = 1.0f;
};

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

struct Glyph
{
  glm::vec2 size;
  glm::vec2 offset;
  glm::vec2 advance;
  glm::vec2 textureCoords;
};

struct RenderData
{
  glm::vec4 clearColor;
  Glyph glyphs[127];
  PerspectiveCamera3D gameCamera;
  // 3D model transform for the cube (or current object)
  glm::mat4 modelMatrix3D;

  OrthographicCamera2D uiCamera;
  glm::mat4 orthoProjectionUI;

  Array<Transform, MAX_TRANSFORMS> transforms;
  Array<Transform, MAX_TRANSFORMS> transparentTransforms;
  Array<Transform, MAX_TRANSFORMS> uiTransforms;
  Array<Transform, MAX_TRANSFORMS> uiTransparentTransforms;
};

// #############################################################################
//                           Render Interface Globals
// #############################################################################
static RenderData* g_RenderData;

// #############################################################################
//                           Render Interface Camera Utility
// #############################################################################
inline glm::ivec2 screen_to_camera(OrthographicCamera2D camera, glm::ivec2 screenPos)
{
  float xPos = (float)screenPos.x / 
               g_Input->screenSize.x * 
               camera.dimensions.x; // [0; dimensions.x]

  // Offset using dimensions and position
  xPos += -camera.dimensions.x / 2.0f + camera.position.x;

  float yPos = (float)screenPos.y / 
               g_Input->screenSize.y * 
               camera.dimensions.y; // [0; dimensions.y]

  // Offset using dimensions and position
  yPos += camera.dimensions.y / 2.0f + camera.position.y;

  return {(int)xPos, (int)yPos};
}


inline glm::ivec2 screen_to_ui(glm::ivec2 screenPos)
{
  return screen_to_camera(g_RenderData->uiCamera, screenPos);
}

inline glm::ivec2 screen_to_world(glm::ivec2 screenPos)
{
  // For now, map screen to 2D UI space; 3D picking can be added later.
  return screen_to_camera(g_RenderData->uiCamera, screenPos);
}

// #############################################################################
//                     Render Interface Utility
// #############################################################################
inline Transform get_transform(glm::vec2 pos, glm::vec2 size, DrawData drawData = {})
{
  Transform transform = {};
  transform.pos = pos;
  transform.size = size;
  // References SPRITE_WHITE from the Atlas
  transform.spriteSize = {1.0f, 1.0f}; 
  transform.layer = drawData.layer;

  // Center the Quad
  transform.pos = {transform.pos.x - transform.size.x / 2.0f, 
                   transform.pos.y + transform.size.y / 2.0f};

  return transform;
}

inline Transform get_transform(glm::vec2 pos, Glyph glyph)
{
  Transform transform = {};
  transform.pos.x = pos.x + glyph.offset.x;
  transform.pos.y = pos.y - glyph.offset.y;
  transform.atlasOffset = glyph.textureCoords;
  transform.spriteSize = glyph.size;
  transform.size = glyph.size;
  transform.renderOptions = RENDERING_OPTION_FONT;
  transform.layer = 1.0f;

  return transform;
}

// #############################################################################
//                     Render Interface UI Quad Rendering
// #############################################################################
inline void draw_ui_quad(Transform transform)
{
  if(transform.renderOptions & RENDERING_OPTION_TRANSPARENT)
  {
    g_RenderData->uiTransparentTransforms.add(transform);
  }
  else
  {
    g_RenderData->uiTransforms.add(transform);
  }
}

inline void draw_ui_quad(glm::vec2 pos, glm::vec2 size, DrawData drawData = {})
{
  Transform transform = get_transform(pos, size, drawData);
  draw_ui_quad(transform);
}

inline void draw_ui_quad(glm::ivec2 pos, glm::ivec2 size, DrawData drawData = {})
{
  draw_ui_quad(glm::vec2(pos), glm::vec2(size));
}

// #############################################################################
//                     Render Interface Game Quad Rendering
// #############################################################################
inline void draw_quad(Transform transform)
{
  if(transform.renderOptions & RENDERING_OPTION_TRANSPARENT)
  {
    g_RenderData->transparentTransforms.add(transform);
  }
  else 
  {
    g_RenderData->transforms.add(transform);
  }
}

inline void draw_quad(glm::vec2 pos, glm::vec2 size, DrawData drawData = {})
{
  Transform transform = get_transform(pos, size, drawData);
  draw_quad(transform);
}

inline void draw_quad(glm::ivec2 pos, glm::ivec2 size, DrawData drawData = {})
{
  draw_quad(glm::vec2(pos), glm::vec2(size));
}

// #############################################################################
//                              Font Rendering
// #############################################################################
void load_font(char* filePath, int fontSize);

// #############################################################################
//                     Render Interface Game Font Rendering
// #############################################################################
inline void draw_text(char* text, glm::vec2 pos)
{
  SM_ASSERT(text, "No Text Supplied!");
  if(!text)
  {
    return;
  }

  char prev = 0;
  while(char c = *(text++))
  {
    Glyph glyph = g_RenderData->glyphs[c];
    Transform transform = get_transform(pos, glyph);
    draw_quad(transform);

    prev = c;
    pos.x += glyph.advance.x;
  }
}
template <typename... Args>
void draw_format_text(char* format, glm::vec2 pos, Args... args)
{
  char* text = format_text(format, args...);
  draw_text(text, pos);
}

inline void draw_text_drop_shadow(char* text, glm::vec2 pos)
{
  draw_text(text, pos);
  draw_text(text, pos - 1.0f);
}

// #############################################################################
//                     Render Interface UI Font Rendering
// #############################################################################
inline void draw_ui_text(char* text, glm::vec2 pos)
{
  SM_ASSERT(text, "No Text Supplied!");
  if(!text)
  {
    return;
  }

  char prev = 0;
  while(char c = *(text++))
  {
    Glyph glyph = g_RenderData->glyphs[c];
    Transform transform = get_transform(pos, glyph);
    draw_ui_quad(transform);

    prev = c;
    pos.x += glyph.advance.x;
  }
}
template <typename... Args>
void draw_format_ui_text(char* format, glm::vec2 pos, Args... args)
{
  char* text = format_text(format, args...);
  draw_ui_text(text, pos);
}

inline void draw_ui_text_drop_shadow(char* text, glm::vec2 pos)
{
  draw_ui_text(text, pos);
  draw_ui_text(text, pos - 1.0f);
}