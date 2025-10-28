#pragma once

#include <variant>
#include <vector>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include "glm/common.hpp"

#include "lib.h"
#include "input.h"
#include "glm/vec2.hpp"
#include "glm/vec4.hpp"
#include "render.h"

/**
DO NOT REMOVE THIS COMMENT BLOCK

Ok I want you to design a simple immediate mode ui library that will integrate with calls to my render.h (they may need to be extended / updated as well).

The goal is to have some chield and parents nodes that will compose the ui. Each node on its own does nothing, but may contain UIData that will hold information about each specific element. For starters we go with a button, panel, label, and will add more as we go.
The id generation of each element should also be something that needs careful tought.
Another thing to care about  is how 1 node/element can reference its parent/children.
Should raw pointers be used, or using the elementid for lookup, and have a flat container for all elements?
Features like docking and alignment of elements is also a priority.
Should sizes be percentages or fixed px units ? The window can be resized by the user so that is a preocupation to take into consideration as well.

Also UIState and RenderData are split, that is fine, but for rendering itself, what is important is the RenderData. the UIState maybe be used to hold state of the elements themselves.

Make the changes you find necessary, total freedom but do not break renderer api.
*/

// #############################################################################
//                           UI Constants
// #############################################################################
constexpr int MAX_UI_ELEMENTS = 100;
// Default font metrics used by UI widgets (must match font loaded by renderer)
constexpr float UI_DEFAULT_FONT_SIZE_PX = 12.0f;
constexpr float UI_DEFAULT_FONT_ASCENT_PX = 9.0f; // approx ascender
constexpr float UI_DEFAULT_FONT_DESCENT_PX = UI_DEFAULT_FONT_SIZE_PX - UI_DEFAULT_FONT_ASCENT_PX;

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

struct UIRect { glm::vec2 pos; glm::vec2 size; };

enum class UIDock : uint8_t { None, Top, Bottom, Left, Right, Fill };

enum class UIUnit : uint8_t { Px, Percent };

struct UILength { float value = 0.0f; UIUnit unit = UIUnit::Px; };

inline float ui_resolve(const UILength& v, float parentPixels)
{
  return (v.unit == UIUnit::Percent) ? (parentPixels * v.value) : v.value;
}

struct UIPanelOptions
{
  // Position/size relative to parent available rect
  UILength x{0, UIUnit::Px};
  UILength y{0, UIUnit::Px};
  UILength w{100, UIUnit::Percent};
  UILength h{100, UIUnit::Percent};
  UIDock dock = UIDock::None; // if dock != None, x/y are ignored and size is taken from w/h (percent or px)
  float padding = 0.0f;       // inner content padding in pixels
  glm::vec4 bgColor = {0,0,0,0}; // optional background (alpha 0 = skip)
};

struct PanelContext
{
  UIRect panelRect;   // outer panel
  UIRect contentRect; // inner, after padding
  UIRect prevAvail;   // parent's remaining area before pushing this panel as a sibling
  float padding;
};

struct UIEditorState
{
  ElementId selectedElementId = 0;
  bool dragging = false;
  glm::vec2 dragStartMousePos{0,0};
  glm::vec2 dragStartElementPos{0,0};
  float gridScale = 1.0f;
  uint32_t gridSpacing = 32;
};

struct UIState
{
  // Retained elements container (placeholder for future retained-mode usage)
  Array<UIElement, MAX_UI_ELEMENTS> uiElements;

  // Immediate-mode state
  std::vector<uint64_t> idStack;     // hierarchical ID seed stack
  std::vector<UIRect>   availStack;  // remaining area for docking at each level
  std::vector<PanelContext> panelStack; // nested panels
  std::vector<glm::vec2> cursorStack;   // layout cursor per panel

  uint64_t hotId = 0;
  uint64_t activeId = 0;

  glm::vec2 mousePosUI{0,0};
  bool mouseDown = false;
  bool mouseReleased = false;
  glm::vec2 viewportSize{0,0};

  bool showDiagnostics = true;
  bool showHUD = true;
  bool showEditor = false;

  UIEditorState editorState{};
};

// #############################################################################
//                    Immediate-Mode UI: ID utilities (FNV-1a 64)
// #############################################################################
inline uint64_t ui_fnv1a64(uint64_t seed, const void* data, size_t len)
{
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(data);
  uint64_t hash = (seed != 0) ? seed : 14695981039346656037ull; // offset basis when seed==0
  const uint64_t prime = 1099511628211ull;
  for (size_t i = 0; i < len; ++i)
  {
    hash ^= bytes[i];
    hash *= prime;
  }
  return hash;
}

inline uint64_t ui_hash_str(uint64_t seed, const char* s)
{
  if (!s) return seed;
  return ui_fnv1a64(seed, s, strlen(s));
}

inline void ui_push_id(UIState* ui, uint64_t id)
{
  uint64_t seed = ui->idStack.empty() ? 0ull : ui->idStack.back();
  uint64_t h = ui_fnv1a64(seed, &id, sizeof(id));
  ui->idStack.push_back(h);
}

inline void ui_push_id(UIState* ui, const char* str)
{
  uint64_t seed = ui->idStack.empty() ? 0ull : ui->idStack.back();
  uint64_t h = ui_hash_str(seed, str);
  ui->idStack.push_back(h);
}

inline void ui_pop_id(UIState* ui)
{
  if (!ui->idStack.empty()) ui->idStack.pop_back();
}

inline uint64_t ui_make_id(UIState* ui, const char* label)
{
  uint64_t seed = ui->idStack.empty() ? 0ull : ui->idStack.back();
  return ui_hash_str(seed, label);
}

// #############################################################################
//                    Immediate-Mode UI: Frame begin/end
// #############################################################################
inline void ui_im_begin_frame(UIState* ui, RenderData* rd, const Input* input)
{
  if (!ui || !rd || !input) return;
  ui_begin_frame(rd); // clear render UI command buffers
  ui->viewportSize = rd->uiCamera.dimensions;
  ui->mousePosUI = screen_to_ui(input, rd, input->mousePos);
  ui->mouseDown = key_is_down(input, KEY_MOUSE_LEFT);
  ui->mouseReleased = key_released_this_frame(input, KEY_MOUSE_LEFT);

  ui->hotId = 0; // will be set by widgets

  // Root available rect = full viewport
  ui->availStack.clear();
  ui->availStack.push_back(UIRect{ {0,0}, ui->viewportSize });

  // Reset panel/cursor stacks each frame; begin_panel will repopulate as needed
  ui->panelStack.clear();
  ui->cursorStack.clear();
}

inline void ui_im_end_frame(UIState* /*ui*/)
{
  // Nothing for now (renderer consumes RenderData’s command buffers)
}

// #############################################################################
//                    Docking helpers and panel management
// #############################################################################
inline UIRect ui_dock_take(UIRect avail, UIDock dock, glm::vec2 desired)
{
  UIRect out = avail;
  switch (dock)
  {
    case UIDock::Top:
      out.size.y = desired.y;
      break;
    case UIDock::Bottom:
      out.pos.y = avail.pos.y + avail.size.y - desired.y;
      out.size.y = desired.y;
      break;
    case UIDock::Left:
      out.size.x = desired.x;
      break;
    case UIDock::Right:
      out.pos.x = avail.pos.x + avail.size.x - desired.x;
      out.size.x = desired.x;
      break;
    case UIDock::Fill:
      // take all
      break;
    case UIDock::None:
      // not used here
      break;
  }
  return out;
}

inline UIRect ui_dock_remaining(UIRect avail, UIDock dock, glm::vec2 taken)
{
  switch (dock)
  {
    case UIDock::Top:
      avail.pos.y += taken.y; avail.size.y -= taken.y; break;
    case UIDock::Bottom:
      avail.size.y -= taken.y; break;
    case UIDock::Left:
      avail.pos.x += taken.x; avail.size.x -= taken.x; break;
    case UIDock::Right:
      avail.size.x -= taken.x; break;
    case UIDock::Fill:
    case UIDock::None:
      break;
  }
  return avail;
}

inline PanelContext ui_begin_panel(UIState* ui, RenderData* rd, const UIPanelOptions& opt)
{
  PanelContext ctx{};
  if (!ui || !rd) return ctx;

  const UIRect parentAvail = ui->availStack.empty() ? UIRect{{0,0}, ui->viewportSize} : ui->availStack.back();

  UIRect panel = parentAvail; // default fill

  if (opt.dock == UIDock::None)
  {
    // Absolute within parent
    const float x = parentAvail.pos.x + ui_resolve(opt.x, parentAvail.size.x);
    const float y = parentAvail.pos.y + ui_resolve(opt.y, parentAvail.size.y);
    const float w = ui_resolve(opt.w, parentAvail.size.x);
    const float h = ui_resolve(opt.h, parentAvail.size.y);
    panel = UIRect{{x, y}, {w, h}};

    // Do not consume parent availability for siblings
    ctx.prevAvail = parentAvail;
  }
  else
  {
    // Docking: take a slice from parent's available area
    const float w = (opt.dock == UIDock::Left || opt.dock == UIDock::Right || opt.dock == UIDock::Fill)
                    ? parentAvail.size.x : ui_resolve(opt.w, parentAvail.size.x);
    const float h = (opt.dock == UIDock::Top || opt.dock == UIDock::Bottom || opt.dock == UIDock::Fill)
                    ? parentAvail.size.y : ui_resolve(opt.h, parentAvail.size.y);
    const glm::vec2 desired = { (opt.dock == UIDock::Left || opt.dock == UIDock::Right || opt.dock == UIDock::Fill) ? ui_resolve(opt.w, parentAvail.size.x) : parentAvail.size.x,
                                (opt.dock == UIDock::Top  || opt.dock == UIDock::Bottom || opt.dock == UIDock::Fill) ? ui_resolve(opt.h, parentAvail.size.y) : parentAvail.size.y };

    panel = ui_dock_take(parentAvail, opt.dock, {desired.x, desired.y});

    // Update parent's remaining area for subsequent siblings
    const UIRect nextAvail = (opt.dock == UIDock::Fill) ? parentAvail : ui_dock_remaining(parentAvail, opt.dock, {panel.size.x, panel.size.y});
    ctx.prevAvail = parentAvail; // store to restore on end
    if (!ui->availStack.empty()) ui->availStack.back() = nextAvail;
  }

  // Apply padding to compute content rect
  const float pad = opt.padding;
  UIRect content = panel;
  content.pos.x += pad; content.pos.y += pad;
  content.size.x = glm::max(0.0f, content.size.x - 2.0f * pad);
  content.size.y = glm::max(0.0f, content.size.y - 2.0f * pad);

  ctx.panelRect = panel;
  ctx.contentRect = content;
  ctx.padding = pad;

  // Draw panel background if any
  if (opt.bgColor.a > 0.0f && content.size.x > 0 && content.size.y > 0)
  {
    ui_draw_rect(rd, panel.pos, panel.size, opt.bgColor);
  }

  // Push context and initialize child layout cursor
  ui->panelStack.push_back(ctx);
  ui->availStack.push_back(content); // children operate within content area
  ui->cursorStack.push_back(content.pos);

  return ctx;
}

inline void ui_end_panel(UIState* ui)
{
  if (!ui) return;
  if (ui->panelStack.empty()) return;

  // Pop child context
  ui->panelStack.pop_back();
  if (!ui->cursorStack.empty()) ui->cursorStack.pop_back();

  // Restore parent's availability for siblings using stored prevAvail
  if (!ui->availStack.empty()) ui->availStack.pop_back();
}

// #############################################################################
//                    Widgets
// #############################################################################
inline void ui_label(UIState* ui, RenderData* rd, const char* text, glm::vec2 offsetPx, glm::vec4 color)
{
  if (!ui || !rd || !text) return;
  UIRect content = ui->availStack.empty() ? UIRect{{0,0},{0,0}} : ui->availStack.back();
  // Position is baseline origin relative to content top-left
  const glm::vec2 pos = { content.pos.x + offsetPx.x, content.pos.y + offsetPx.y };
  ui_draw_text(rd, text, pos, color);

  // Advance panel layout cursor vertically so subsequent auto-laid-out widgets (e.g., buttons)
  // appear below this label, preserving the manual offset positioning of the label itself.
  if (!ui->cursorStack.empty())
  {
    const float lineH  = UI_DEFAULT_FONT_SIZE_PX;
    const float ascent = UI_DEFAULT_FONT_ASCENT_PX;
    const float itemSpacing = 4.0f; // consistent with button spacing

    const float top    = pos.y - ascent;        // approximate top of the text line box
    const float bottom = top + lineH;           // bottom of the text line box

    glm::vec2 cursor = ui->cursorStack.back();
    const float newY = bottom + itemSpacing;
    if (newY > cursor.y)
    {
      cursor.y = newY; // only move forward
      ui->cursorStack.back() = cursor;
    }
  }
}

inline bool ui_button(UIState* ui, RenderData* rd, const char* label, glm::vec2 sizePx)
{
  if (!ui || !rd || !label) return false;
  // Layout: place at current cursor in top panel
  if (ui->cursorStack.empty())
  {
    // No panel; create an implicit full-viewport panel without bg
    UIPanelOptions opt; opt.dock = UIDock::Fill; opt.bgColor = {0,0,0,0}; opt.padding = 0;
    ui_begin_panel(ui, rd, opt);
  }

  glm::vec2 cursor = ui->cursorStack.back();
  UIRect content = ui->availStack.back();

  // Clamp size to content
  glm::vec2 size = { glm::min(sizePx.x, content.size.x), glm::min(sizePx.y, content.size.y) };
  UIRect rect{ cursor, size };

  // Advance cursor vertically (simple column layout)
  cursor.y += size.y + 4.0f; // 4px spacing
  ui->cursorStack.back() = cursor;

  const uint64_t id = ui_make_id(ui, label);

  // Hit test
  const bool hovered = (ui->mousePosUI.x >= rect.pos.x && ui->mousePosUI.x <= rect.pos.x + rect.size.x &&
                        ui->mousePosUI.y >= rect.pos.y && ui->mousePosUI.y <= rect.pos.y + rect.size.y);
  if (hovered) ui->hotId = id;

  bool pressed = false;
  if (hovered && ui->mouseDown && (ui->activeId == 0 || ui->activeId == id))
  {
    ui->activeId = id; // capture
  }
  if (ui->mouseReleased)
  {
    if (ui->activeId == id && hovered) pressed = true;
    if (ui->activeId == id) ui->activeId = 0;
  }

  // Visuals
  const glm::vec4 base = {0.20f, 0.20f, 0.20f, 1.0f};
  const glm::vec4 hov  = {0.30f, 0.30f, 0.30f, 1.0f};
  const glm::vec4 act  = {0.15f, 0.15f, 0.15f, 1.0f};
  const glm::vec4 col = (ui->activeId == id) ? act : (hovered ? hov : base);
  ui_draw_rect(rd, rect.pos, rect.size, col);

  // Text: left padding + vertical centering using baseline metrics
  const float leftPad = 8.0f;
  const float ascent = UI_DEFAULT_FONT_ASCENT_PX;
  const float lineH = UI_DEFAULT_FONT_SIZE_PX;
  const float top = rect.pos.y;
  const float h = rect.size.y;
  float baselineY = top + glm::max(0.0f, (h - lineH) * 0.5f) + ascent;
  // Clamp baseline to stay within button box (account for ascent/descent)
  const float minBaseline = top + ascent;
  const float maxBaseline = top + h - UI_DEFAULT_FONT_DESCENT_PX;
  baselineY = glm::clamp(baselineY, minBaseline, maxBaseline);
  const glm::vec2 textPos = { rect.pos.x + leftPad, baselineY };
  ui_draw_text(rd, label, textPos, {1,1,1,1});

  return pressed;
}