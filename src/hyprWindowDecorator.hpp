#pragma once

#define WLR_USE_UNSTABLE

#include <hyprland/src/render/decorations/IHyprWindowDecoration.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/gl/GLTexture.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/devices/IPointer.hpp>
#include <hyprland/src/devices/ITouch.hpp>
#include <hyprland/src/desktop/rule/windowRule/WindowRule.hpp>
#include <hyprland/src/helpers/AnimatedVariable.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <cairo/cairo.h>
#include "plugin.hpp"

class CHyprWindowDecorator : public IHyprWindowDecoration
{
public:
  CHyprWindowDecorator(PHLWINDOW);
  virtual ~CHyprWindowDecorator();

  virtual SDecorationPositioningInfo getPositioningInfo();

  // Reserved extents: the single source of truth shared by layout, drawing and input.
  SBoxExtents currentExtents() const;

  virtual void onPositioningReply(const SDecorationPositioningReply &reply);

  virtual void draw(PHLMONITOR, float const &a);

  virtual eDecorationType getDecorationType();

  virtual void updateWindow(PHLWINDOW);

  virtual void damageEntire();

  virtual eDecorationLayer getDecorationLayer();

  virtual uint64_t getDecorationFlags();

  bool m_bButtonsDirty = true;

  virtual std::string getDisplayName();

  PHLWINDOW getOwner();

  void updateRules();

  void invalidateTextures();


private:
  SBoxExtents m_seExtents;

  PHLWINDOWREF m_pWindow;

  CBox m_bAssignedBox;

  // Last window size we logged from getPositioningInfo(), to avoid log spam.
  Vector2D m_vLastLoggedSize = {-1, -1};

  SP<Render::ITexture> m_pTextTex;
  SP<Render::ITexture> m_pButtonsTex;
  SP<Render::ITexture> m_pBarFinalTex;

  SP<Render::ITexture> m_pAppIconTex;
  std::string m_szLastAppId;

  bool m_bWindowSizeChanged = false;
  bool m_bNeedsReposition = false;
  bool m_hidden = false;
  bool m_bTitleColorChanged = false;
  bool m_bLastEnabledState = false;
  bool m_bWindowHasFocus = false;
  bool m_bNinePatchChanged = false;
  std::optional<CHyprColor> m_bForcedBarColor;
  std::optional<CHyprColor> m_bForcedTitleColor;

  Time::steady_tp m_lastMouseDown = Time::steadyNow();

  PHLANIMVAR<CHyprColor> m_cRealBarColor;

  Vector2D cursorRelativeToBar();
  bool isMouseOnBar();

  void renderPass(PHLMONITOR, float const &a);
  void renderBarTitle(const CBox &band, const float scale);

  // Where the title (and its background) goes, in the same space as topBarBox.
  // The title fills the gap between left- and right-anchored elements unless a
  // max_width caps it. Both the text and its backdrop are derived from this, so they
  // cannot drift apart.
  CBox titleBox(const CBox &band, const float scale) const;
  CBox titleBoxPlaced(const CBox &band, const float scale) const;

  // Geometry of one element inside the top band, in DEVICE pixels. Shared by drawing
  // and hit-testing so a hovered element can never be tested where it is not drawn.
  struct SElementGeom
  {
    double x = 0, y = 0, w = 0, h = 0;
    double device = 1; // source px -> device px for this element
  };
  // The title band, in decoration-local pixels. One source for drawing and hit-testing.
  CBox bandBox();

  // Recomputes m_iHoveredElement / m_iPressedElement from the cursor, damaging only
  // when the state actually changes.
  void updateElementHover();
  int  elementIndexAt(const Vector2D &local);

  SElementGeom elementBox(const SElementTex &element, const CBox &band, const double bandW, const float monitorScale) const;

  // Which texture represents an element right now. `focus` is the window's focus.
  const SElementTexture &elementTexture(size_t index, const bool focus) const;

  int m_iHoveredElement = -1;
  int m_iPressedElement = -1;
  void renderText(SP<Render::ITexture> out, const std::string &text, const CHyprColor &color, const Vector2D &bufferSize, const float scale, const int fontSize);
  bool renderBarButtons(const Vector2D &bufferSize, const float scale);
  void renderBarButtonsText(CBox *barBox, const float scale, const float a);
  void damageOnButtonHover();

  bool inputIsValid();
  bool isPointOnBar(Vector2D COORDS);
  void onMouseButton(Event::SCallbackInfo &info, IPointer::SButtonEvent e);
  void onTouchDown(Event::SCallbackInfo &info, ITouch::SDownEvent e);
  void onTouchUp(Event::SCallbackInfo &info, ITouch::SUpEvent e);
  void onMouseMove(Vector2D coords);
  void onTouchMove(Event::SCallbackInfo &info, ITouch::SMotionEvent e);

  void handleDownEvent(Event::SCallbackInfo &info, std::optional<ITouch::SDownEvent> touchEvent);
  void handleUpEvent(Event::SCallbackInfo &info);
  void handleMovement();
  int indexToButton(Vector2D COORDS);

  CBox assignedBoxGlobal();

  CHyprSignalListener m_pMouseButtonCallback;
  CHyprSignalListener m_pTouchDownCallback;
  CHyprSignalListener m_pTouchUpCallback;

  CHyprSignalListener m_pTouchMoveCallback;
  CHyprSignalListener m_pMouseMoveCallback;

  std::string m_szLastTitle;
  Vector2D m_vLastTitleSize;

  bool m_bDraggingThis = false;
  bool m_bTouchEv = false;
  bool m_bDragPending = false;
  bool m_bCancelledDown = false;
  int m_touchId = 0;

  int m_iButtonPressedIdx = -1;

  // store hover state for buttons as a bitfield
  unsigned int m_iButtonHoverState = 0;

  // for dynamic updates
  int m_iLastHeight = 0;

  size_t getVisibleButtonCount(const Vector2D &bufferSize, const float scale);

  friend class CRenderPassElement;
};
