#pragma once

#include "input.h"

// #############################################################################
//                           UI Constants
// #############################################################################
constexpr int MAX_UI_ELEMENTS = 100;
constexpr int MAX_TEXT_CHARS = 256;

// #############################################################################
//                           UI Structs
// #############################################################################
struct UIID
{
  int ID;
  int layer;
};

struct UIElement
{
  // SpriteID spriteID;
  glm::vec2 pos;
};

struct UIText
{
  int charCount;
  char text[MAX_TEXT_CHARS];
  glm::vec2 pos;
};

struct UIState
{
  UIID hotLastFrame;
  UIID hotThisFrame;
  UIID active;

  Array<UIText, 100> texts;
  Array<UIElement, MAX_UI_ELEMENTS> uiElements;
};

// #############################################################################
//                           UI Globals
// #############################################################################
static UIState* g_UIState;