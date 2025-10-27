#pragma once

#include <variant>

#include "input.h"
#include "glm/vec4.hpp"

// #############################################################################
//                           UI Constants
// #############################################################################
constexpr int MAX_UI_ELEMENTS = 100;

// #############################################################################
//                           UI Structs
// #############################################################################

typedef size_t ElementId;

static ElementId g_ElementIdCounter = 1;

enum class UIElementType : uint8_t
{
  NODE,
  BUTTON,
  LABEL,
  PANEL,
  COUNT
};

struct UIButton {
  glm::vec2 position;
  glm::vec2 size;
  void (*onClick)(ElementId id);
};

struct UIPanel {
  glm::ivec2 position;
  glm::ivec2 size;
  glm::vec4 color;
};

struct UILabel {
  glm::vec2 position;
  glm::vec4 color;
  char text[128];
};

using UIData = std::variant<std::monostate, UIButton, UIPanel, UILabel>;

struct UIElement {
  ElementId id = g_ElementIdCounter++;
  UIElementType type = UIElementType::NODE;
  UIElement* parent = nullptr;
  std::vector<UIElement*> children;
  UIData data;

  template<typename Fn>
  auto VisitData(Fn&& fn) {
    return std::visit(std::forward<Fn>(fn), data);
  }
};

struct UIState
{
  Array<UIElement, MAX_UI_ELEMENTS> uiElements;
};

// #############################################################################
//                           UI Globals
// #############################################################################
static UIState* g_UIState;