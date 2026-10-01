#pragma once

/**
 * glint_picker_registry.hpp
 * Tracks the one visible picker popup of a given type (date, month, week,
 * time, datetime-local) and the capture-phase wheel listener that hides it
 * when the owning document scrolls.
 *
 * Thread rules — each glint window runs on its own thread:
 *   attachCanvas()  owning document's thread (called from the picker's reopen()).
 *                   The only call that touches the document.
 *   setActive() / clearActive()
 *                   any thread (the popup's message handlers).  They only swap
 *                   a pointer under a mutex, so popup threads never touch the
 *                   owning document's elements.
 *
 * The listener stays installed while the popup is hidden (it is a no-op with
 * no active picker) and is moved when a picker is opened from another canvas.
 */

#include "../../glint_element.hpp"

#include <memory>
#include <mutex>

template <class Window>
class glint_picker_registry
{
public:
  /** Make sure `docCanvas` carries the hide-on-wheel listener.  Owning thread only. */
  static void attachCanvas(glint_element* docCanvas)
  {
    if (!docCanvas) return;

    State& s = state();
    std::lock_guard<std::mutex> lk(s.mutex);
    // Same canvas, still alive (the token guards against address reuse).
    if (s.canvas == docCanvas && !s.canvasLife.expired()) return;

    _detachLocked(s);
    s.canvas       = docCanvas;
    s.canvasLife   = docCanvas->lifeToken();
    s.canvasPoster = docCanvas->ownerThreadPoster();
    s.listenerId   = docCanvas->addEventListener(
      "wheel",
      [](glint_event&) {
        Window* active = nullptr;
        {
          State& st = state();
          std::lock_guard<std::mutex> lk2(st.mutex);
          active = st.active;
        }
        if (active) active->hide();  // posts to the popup's thread
      },
      /*useCapture=*/true);
  }

  /** Mark `w` as the visible picker.  Any thread. */
  static void setActive(Window* w)
  {
    State& s = state();
    std::lock_guard<std::mutex> lk(s.mutex);
    s.active = w;
  }

  /** Clear the visible picker if it is `w` (nullptr clears unconditionally).  Any thread. */
  static void clearActive(Window* w)
  {
    State& s = state();
    std::lock_guard<std::mutex> lk(s.mutex);
    if (!w || s.active == w) s.active = nullptr;
  }

private:
  struct State
  {
    std::mutex          mutex;
    Window*             active     = nullptr;
    glint_element*      canvas     = nullptr;
    std::weak_ptr<void> canvasLife;
    glint_owner_poster  canvasPoster;
    int                 listenerId = -1;
  };

  static State& state()
  {
    static State s;
    return s;
  }

  // Remove the listener from the previous canvas on that canvas's own thread;
  // the poster skips it if the canvas is gone.
  static void _detachLocked(State& s)
  {
    if (s.canvas && s.listenerId >= 0 && s.canvasPoster)
    {
      glint_element* canvas = s.canvas;
      const int      id     = s.listenerId;
      s.canvasPoster([canvas, id] { canvas->removeEventListener(id); });
    }
    s.canvas       = nullptr;
    s.canvasLife.reset();
    s.canvasPoster = nullptr;
    s.listenerId   = -1;
  }
};
