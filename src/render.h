#pragma once

#include "input.h"
#include "lib.h"

inline int RENDERING_OPTION_FONT = BIT(1);
inline int RENDERING_OPTION_TRANSPARENT = BIT(2);

// #############################################################################
//                           Rendering Structs
// #############################################################################
struct Transform
{
  Vec2 pos; // This is currently the Top Left!!
  Vec2 size;
  Vec2 atlasOffset;
  Vec2 spriteSize;
  int renderOptions;
  float layer;
};

struct Material
{
  Vec4 color;
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
  Vec2 dimensions;
  Vec2 position;
};

struct Glyph
{
  Vec2 size;
  Vec2 offset;
  Vec2 advance;
  Vec2 textureCoords;
};

struct RenderData
{
  Vec4 clearColor;
  Glyph glyphs[127];
  OrthographicCamera2D gameCamera;
  OrthographicCamera2D uiCamera;
  Mat4 orthoProjectionGame;
  Mat4 orthoProjectionUI;
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
inline IVec2 screen_to_camera(OrthographicCamera2D camera, IVec2 screenPos)
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

inline IVec2 screen_to_ui(IVec2 screenPos)
{
  return screen_to_camera(g_RenderData->uiCamera, screenPos);
}

inline IVec2 screen_to_world(IVec2 screenPos)
{
  return screen_to_camera(g_RenderData->gameCamera, screenPos);
}

// #############################################################################
//                     Render Interface Utility
// #############################################################################
inline Transform get_transform(Vec2 pos, Vec2 size, DrawData drawData = {})
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

inline Transform get_transform(Vec2 pos, Glyph glyph)
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

inline void draw_ui_quad(Vec2 pos, Vec2 size, DrawData drawData = {})
{
  Transform transform = get_transform(pos, size, drawData);
  draw_ui_quad(transform);
}

inline void draw_ui_quad(IVec2 pos, IVec2 size, DrawData drawData = {})
{
  draw_ui_quad(vec_2(pos), vec_2(size));
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

inline void draw_quad(Vec2 pos, Vec2 size, DrawData drawData = {})
{
  Transform transform = get_transform(pos, size, drawData);
  draw_quad(transform);
}

inline void draw_quad(IVec2 pos, IVec2 size, DrawData drawData = {})
{
  draw_quad(vec_2(pos), vec_2(size));
}

// #############################################################################
//                              Font Rendering
// #############################################################################
void load_font(char* filePath, int fontSize);

// #############################################################################
//                     Render Interface Game Font Rendering
// #############################################################################
inline void draw_text(char* text, Vec2 pos)
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
void draw_format_text(char* format, Vec2 pos, Args... args)
{
  char* text = format_text(format, args...);
  draw_text(text, pos);
}

inline void draw_text_drop_shadow(char* text, Vec2 pos)
{
  draw_text(text, pos);
  draw_text(text, pos - 1.0f);
}

// #############################################################################
//                     Render Interface UI Font Rendering
// #############################################################################
inline void draw_ui_text(char* text, Vec2 pos)
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
void draw_format_ui_text(char* format, Vec2 pos, Args... args)
{
  char* text = format_text(format, args...);
  draw_ui_text(text, pos);
}

inline void draw_ui_text_drop_shadow(char* text, Vec2 pos)
{
  draw_ui_text(text, pos);
  draw_ui_text(text, pos - 1.0f);
}