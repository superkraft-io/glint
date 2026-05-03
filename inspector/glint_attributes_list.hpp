#pragma once

/**
 * inspector/glint_attributes_list.hpp
 * A standalone Win32 popup window with a live-filtering search bar and a
 * scrollable list of all available style attributes.  Used by the inspector's
 * "add attribute" button.
 *
 * Lifecycle (same pattern as glint_colorpicker_window):
 *   auto* w = glint_attributes_list_window::open(anchor, allKeys, setKeys, onPick, onClosed);
 *   // later, to close programmatically:
 *   w->close();   // async — null your pointer immediately after calling this
 *
 * Keyboard navigation (search input must be focused, which happens automatically):
 *   ArrowDown / ArrowUp  — move list selection
 *   Enter                — pick the highlighted item
 *   Escape               — close without picking
 *
 * Mouse:
 *   Click any item → fires onPick(key) + auto-closes
 *
 * Already-set keys are shown dimmed (darker text) but remain clickable.
 */

#include "../platform/glint_window.hpp"   // platform-dispatching umbrella (glint_window)
#include "../components/glint_list/glint_list.hpp"  // glint_list, glint_list_item (used by both Win32 and macOS sections)

#include <functional>
#include <set>
#include <string>
#include <vector>

// =============================================================================
// glint_attributes_list_window
// =============================================================================
// Full Win32 implementation; a no-op stub is provided for non-Win32 platforms
// (macOS, Linux) where mOwnerHWND is always null so the picker is never opened.

#if defined(_WIN32)

class glint_attributes_list_window : public glint_window_win32
{
public:
  /**
   * Factory.
   * @param anchorScreenRect  The "add attribute" button bounding rect in
   *                          screen-space pixels.  Window opens ABOVE the button
   *                          (flips below when it would overflow the work area).
   * @param allKeys           Full ordered list of attribute names to display.
   *                          Typically glint_all_style_keys() value.
   * @param setKeys           Keys already present on the target component —
   *                          rendered dimmed but still clickable.
   * @param onPick            Called on the window thread when the user confirms
   *                          a selection (click or Enter).  Receives the key string.
   * @param onClosed          Called on the window thread in afterRun() just before
   *                          `delete this`.  Null your pointer here.
   */
  static glint_attributes_list_window* open(
      RECT                               anchorScreenRect,
      const std::vector<const char*>&    allKeys,
      std::set<std::string>              setKeys,
      std::function<void(std::string)>   onPick,
      std::function<void()>              onClosed = nullptr)
  {
    auto* w         = new glint_attributes_list_window();
    w->mAnchorRect  = anchorScreenRect;
    w->mAllKeys     = allKeys;
    w->mSetKeys     = std::move(setKeys);
    w->mOnPick      = std::move(onPick);
    w->mOnClosed    = std::move(onClosed);
    w->startThread();
    return w;
  }

  /** Async close.  Null your caller-side pointer immediately after calling this. */
  void close() { stopThread(); }

protected:
  // Auto-dismiss when the user clicks elsewhere (any other window gains focus).
  LRESULT handleMessage(UINT msg, WPARAM wp, LPARAM /*lp*/) override
  {
    if (msg == WM_ACTIVATE && LOWORD(wp) == WA_INACTIVE)
    {
      stopThread();   // posts WM_CLOSE → message loop exits → afterRun() → delete this
      return 0;
    }
    return -1;  // not handled — fall through to DefWindowProc
  }

  const wchar_t* windowClassName() const override { return L"glint_attr_list"; }
  const wchar_t* windowTitle()     const override { return L""; }
  DWORD          windowStyle()     const override { return WS_POPUP; }
  DWORD          windowExStyle()   const override { return WS_EX_TOOLWINDOW; }
  int            defaultWidth()    const override { return 260; }
  int            defaultHeight()   const override { return 320; }
  COLORREF       bgColor()         const override { return RGB(0, 0, 0); }
  // Alpha = 0: corners outside the rounded wrapper rect are fully transparent
  // once UpdateLayeredWindow composites the bitmap via useTransparency().
  SkColor        clearColor()      const override { return SkColorSetARGB(0, 22, 22, 28); }
  bool           useTransparency() const override { return true; }
  bool           useGpu()          const override { return false; }

  void buildUI() override
  {
    mOwnRoot->skipInspectMode = true;

    // ── Rounded wrapper ──────────────────────────────────────────────────────
    // initRoot() already sets canvas display:flex/column.
    // Paints the background and clips children. Corner pixels of the bitmap
    // remain at clearColor() alpha=0, making the OS-level window transparent
    // there (via UpdateLayeredWindow / useTransparency()).
    auto* wrap = mOwnRoot->add.div([](glint_component_style& _c) {
      _c.style.width           = "100%";
      _c.style.height          = "100%";
      _c.style.display         = "flex";
      _c.style.flexDirection   = "column";
      _c.style.backgroundColor = glint_color(255, 22, 22, 28);
      _c.style.borderColor     = glint_color(255, 64, 64, 64);
      _c.style.borderRadius    = 8.f;
      _c.style.overflow        = "hidden";
    });

    // ── Search input ────────────────────────────────────────────────────────
    mSearchInput = wrap->add.input([this](glint_input& _c) {
      _c.placeholder             = "Search attributes...";
      _c.style.width             = "100%";
      _c.style.height            = 36.f;
      _c.style.fontSize          = 13.f;
      _c.style.padding           = "0 10";
      _c.style.borderWidth       = 0.f;
      _c.style.borderBottomWidth = 1.f;
      _c.style.borderColor       = glint_color(255, 52, 52, 68);
      _c.style.backgroundColor   = glint_color(255, 22, 22, 28);
      _c.style.borderRadius      = 0.f;
      _c.style.color             = glint_color(255, 200, 200, 215);
      // Real-time filter: rebuild list on every keystroke.
      _c.onChange = [this](const std::string& filter) { _rebuildList(filter); };
      // Keyboard navigation forwarded from the focused search input.
      // glint_vk:: constants come from glint_text_editor_base.hpp (included transitively).
      _c.element.addEventListener("keydown", [this](glint_event& e) {
        auto& ke = static_cast<glint_keyboard_event&>(e);
        const int vk = ke.key.vk;
        if (vk == glint_vk::DOWN)
        {
          _moveHighlight(+1);
          e.preventDefault();   // don't let the input scroll or move its cursor
        }
        else if (vk == glint_vk::UP)
        {
          _moveHighlight(-1);
          e.preventDefault();
        }
        else if (vk == glint_vk::RETURN)
        {
          _pickHighlighted();   // no-op when nothing is highlighted
        }
        else if (vk == glint_vk::ESCAPE)
        {
          stopThread();
        }
      });
    });

    // ── Attribute list ───────────────────────────────────────────────────────
    // Do NOT wire onItemSelected — it fires from selectItemByIdx() too, which
    // is called for keyboard navigation and must not trigger a pick.
    mList = wrap->add.list([this](glint_list& _c) {
      _c.style.width        = "100%";
      _c.style.flexGrow     = 1.f;
      _c.style.borderRadius = 0.f;
      _c.highlightOnSelect  = true;
      _c.onItemClicked = [this](glint_list_item* item) { _pick(item->id); };
    });
  }

  void onCreated() override
  {
    // ── Position: prefer opening ABOVE the anchor button ────────────────────
    const int W    = defaultWidth();
    const int H    = defaultHeight();
    const int kGap = 4;

    RECT wa{};
    ::SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);

    // Prefer above the button; flip below if it would overflow the top edge.
    int y = mAnchorRect.top - kGap - H;
    if (y < wa.top)
      y = mAnchorRect.bottom + kGap;
    if (y + H > wa.bottom) y = wa.bottom - H;
    if (y < wa.top)        y = wa.top;

    // Align left; clamp if it overflows right edge.
    int x = mAnchorRect.left;
    if (x + W > wa.right)  x = wa.right - W;
    if (x < wa.left)       x = wa.left;

    ::SetWindowPos(mHWND, HWND_TOPMOST, x, y, 0, 0, SWP_NOSIZE);

    // Populate with all keys (no initial filter).
    _rebuildList("");

    // Auto-focus the search input so the user can type immediately.
    if (mSearchInput && mOwnRoot) mOwnRoot->SetFocus(mSearchInput);
  }

  void afterRun() override
  {
    if (mOnClosed) mOnClosed();
    delete this;
  }

private:
  // ── Construction data ────────────────────────────────────────────────────
  RECT                             mAnchorRect   = {};
  std::vector<const char*>         mAllKeys;
  std::set<std::string>            mSetKeys;
  std::function<void(std::string)> mOnPick;
  std::function<void()>            mOnClosed;

  // ── Live UI refs (valid after buildUI()) ─────────────────────────────────
  glint_input*             mSearchInput    = nullptr;
  glint_list*              mList           = nullptr;

  // ── Keyboard navigation state ────────────────────────────────────────────
  std::vector<std::string> mFilteredKeys;    // parallel to visible list rows
  int                      mHighlightedIdx = -1;

  // ── Helpers ──────────────────────────────────────────────────────────────

  void _rebuildList(const std::string& filter)
  {
    if (!mList) return;

    mHighlightedIdx = -1;
    mFilteredKeys.clear();
    mList->clear();

    const std::string lf = _toLower(filter);

    for (const char* k : mAllKeys)
    {
      const std::string key = k;
      if (!lf.empty() && _toLower(key).find(lf) == std::string::npos)
        continue;

      const bool alreadySet = mSetKeys.count(key) > 0;
      mFilteredKeys.push_back(key);

      mList->items.add([key, alreadySet](glint_list_item& item) {
        item.innerText = key;
        item.id        = key;
        if (alreadySet)
        {
          // Already present on the component — show dimmed but still clickable.
          item.style.color           = glint_color(255, 105, 105, 122);
          item.hover.color           = glint_color(255, 150, 150, 168);
          item.selectedStyle.color   = glint_color(255, 140, 140, 160);
        }
        else
        {
          // Normal attribute.
          item.style.color           = glint_color(255, 205, 205, 218);
          item.hover.color           = glint_color(255, 235, 235, 248);
          item.selectedStyle.color   = glint_color(255, 255, 255, 255);
        }
      });
    }

    if (mOwnRoot) mOwnRoot->setDirty(false);
  }

  // Move the keyboard highlight by `delta` (+1 = down, -1 = up), wrapping.
  void _moveHighlight(int delta)
  {
    if (mFilteredKeys.empty()) return;

    const int n = (int)mFilteredKeys.size();
    if (mHighlightedIdx < 0)
      mHighlightedIdx = (delta > 0) ? 0 : n - 1;
    else
      mHighlightedIdx = (mHighlightedIdx + delta + n) % n;

    if (mList) mList->selectItemByIdx(mHighlightedIdx);
  }

  // Confirm selection of the currently highlighted item (Enter key).
  void _pickHighlighted()
  {
    if (mHighlightedIdx >= 0 && mHighlightedIdx < (int)mFilteredKeys.size())
      _pick(mFilteredKeys[mHighlightedIdx]);
  }

  // Fire onPick and close the window.
  void _pick(const std::string& key)
  {
    if (mOnPick) mOnPick(key);
    stopThread();
  }

  static std::string _toLower(std::string s)
  {
    for (auto& c : s) c = (char)::tolower((unsigned char)c);
    return s;
  }
};

#elif defined(__APPLE__)

// =============================================================================
// glint_attributes_list_window — macOS implementation
// =============================================================================
// Derives from glint_window_mac (aliased as glint_window on Apple).
// The same buildUI() Glint layout is shared with the Win32 version above;
// only the lifecycle, positioning, and threading differ.
//
// Threading: startThread() creates the NSPanel on the main thread synchronously.
// The onPick callback is always invoked on the main thread before the window
// closes, so the InspStylePanel::commitAddProperty() call is safe.

class glint_attributes_list_window : public glint_window
{
public:
  static glint_attributes_list_window* open(
      RECT                               anchorScreenRect,
      const std::vector<const char*>&    allKeys,
      std::set<std::string>              setKeys,
      std::function<void(std::string)>   onPick,
      std::function<void()>              onClosed = nullptr)
  {
    auto* w        = new glint_attributes_list_window();
    w->mAnchorRect = anchorScreenRect;
    w->mAllKeys    = allKeys;
    w->mSetKeys    = std::move(setKeys);
    w->mOnPick     = std::move(onPick);
    w->mOnClosed   = std::move(onClosed);
    w->startThread();   // creates NSPanel on main thread, calls buildUI() + onCreated()
    return w;
  }

  void close() { stopThread(); }

  int  defaultWidth()  const override { return 260; }
  int  defaultHeight() const override { return 320; }
  const char* macTitleUTF8() const override { return ""; }
  bool usePopupStyle()  const override { return true; }
  // These wchar_t methods satisfy glint_window_base's pure-virtual interface;
  // glint_window_mac does not call them on macOS (macTitleUTF8 is used instead).
  const wchar_t* windowClassName() const override { return L"glint_attr_list"; }
  const wchar_t* windowTitle()     const override { return L""; }

  void buildUI() override
  {
    if (!mOwnRoot) return;
    mOwnRoot->skipInspectMode = true;

    auto* wrap = mOwnRoot->add.div([](glint_component_style& _c) {
      _c.style.width           = "100%";
      _c.style.height          = "100%";
      _c.style.display         = "flex";
      _c.style.flexDirection   = "column";
      _c.style.backgroundColor = glint_color(255, 22, 22, 28);
      _c.style.borderColor     = glint_color(255, 64, 64, 64);
      _c.style.borderWidth     = 1.f;
      _c.style.borderRadius    = 8.f;
      _c.style.overflow        = "hidden";
    });

    mSearchInput = wrap->add.input([this](glint_input& _c) {
      _c.placeholder             = "Search attributes...";
      _c.style.width             = "100%";
      _c.style.height            = 36.f;
      _c.style.fontSize          = 13.f;
      _c.style.padding           = "0 10";
      _c.style.borderWidth       = 0.f;
      _c.style.borderBottomWidth = 1.f;
      _c.style.borderColor       = glint_color(255, 52, 52, 68);
      _c.style.backgroundColor   = glint_color(255, 22, 22, 28);
      _c.style.borderRadius      = 0.f;
      _c.style.color             = glint_color(255, 200, 200, 215);
      _c.onChange = [this](const std::string& filter) { _rebuildList(filter); };
      _c.element.addEventListener("keydown", [this](glint_event& e) {
        auto& ke = static_cast<glint_keyboard_event&>(e);
        const int vk = ke.key.vk;
        if (vk == glint_vk::DOWN)
        {
          _moveHighlight(+1);
          e.preventDefault();
        }
        else if (vk == glint_vk::UP)
        {
          _moveHighlight(-1);
          e.preventDefault();
        }
        else if (vk == glint_vk::RETURN)
        {
          _pickHighlighted();
        }
        else if (vk == glint_vk::ESCAPE)
        {
          glint_window_mac::_dispatchMain([this]{ stopThread(); });
        }
      });
    });

    mList = wrap->add.list([this](glint_list& _c) {
      _c.style.width        = "100%";
      _c.style.flexGrow     = 1.f;
      _c.style.borderRadius = 0.f;
      _c.highlightOnSelect  = true;
      _c.onItemClicked = [this](glint_list_item* item) { _pick(item->id); };
    });
  }

  void onCreated() override
  {
    // Position: prefer opening ABOVE the anchor button; flip below if needed.
    const int W    = defaultWidth();
    const int H    = defaultHeight();
    const int kGap = 4;

    const RECT wa = screenWorkArea();

    int y = mAnchorRect.top - kGap - H;   // above button
    if (y < wa.top)    y = mAnchorRect.bottom + kGap;  // flip: show below
    if (y + H > wa.bottom) y = wa.bottom - H;
    if (y < wa.top)        y = wa.top;

    int x = mAnchorRect.left;
    if (x + W > wa.right)  x = wa.right - W;
    if (x < wa.left)       x = wa.left;

    setPanelFrameOrigin(x, y);

    _rebuildList("");
    if (mSearchInput && mOwnRoot) mOwnRoot->SetFocus(mSearchInput);
  }

  void afterRun() override
  {
    if (mOnClosed) mOnClosed();
    delete this;
  }

private:
  RECT                             mAnchorRect   = {};
  std::vector<const char*>         mAllKeys;
  std::set<std::string>            mSetKeys;
  std::function<void(std::string)> mOnPick;
  std::function<void()>            mOnClosed;

  glint_input*             mSearchInput    = nullptr;
  glint_list*              mList           = nullptr;
  std::vector<std::string> mFilteredKeys;
  int                      mHighlightedIdx = -1;

  void _rebuildList(const std::string& filter)
  {
    if (!mList) return;

    mHighlightedIdx = -1;
    mFilteredKeys.clear();
    mList->clear();

    const std::string lf = _toLower(filter);

    for (const char* k : mAllKeys)
    {
      const std::string key = k;
      if (!lf.empty() && _toLower(key).find(lf) == std::string::npos)
        continue;

      const bool alreadySet = mSetKeys.count(key) > 0;
      mFilteredKeys.push_back(key);

      mList->items.add([key, alreadySet](glint_list_item& item) {
        item.innerText = key;
        item.id        = key;
        if (alreadySet)
        {
          item.style.color           = glint_color(255, 105, 105, 122);
          item.hover.color           = glint_color(255, 150, 150, 168);
          item.selectedStyle.color   = glint_color(255, 140, 140, 160);
        }
        else
        {
          item.style.color           = glint_color(255, 205, 205, 218);
          item.hover.color           = glint_color(255, 235, 235, 248);
          item.selectedStyle.color   = glint_color(255, 255, 255, 255);
        }
      });
    }

    if (mOwnRoot) mOwnRoot->setDirty(false);
  }

  void _moveHighlight(int delta)
  {
    if (mFilteredKeys.empty()) return;
    const int n = (int)mFilteredKeys.size();
    if (mHighlightedIdx < 0)
      mHighlightedIdx = (delta > 0) ? 0 : n - 1;
    else
      mHighlightedIdx = (mHighlightedIdx + delta + n) % n;
    if (mList) mList->selectItemByIdx(mHighlightedIdx);
  }

  void _pickHighlighted()
  {
    if (mHighlightedIdx >= 0 && mHighlightedIdx < (int)mFilteredKeys.size())
      _pick(mFilteredKeys[mHighlightedIdx]);
  }

  void _pick(const std::string& key)
  {
    if (mOnPick) mOnPick(key);
    // Defer stopThread() to the next run-loop turn.  Calling it synchronously
    // would destroy mOwnRoot while we are still inside its OnKeyDown call —
    // a use-after-free crash on the way back up the call stack.
    glint_window_mac::_dispatchMain([this]{ stopThread(); });
  }

  static std::string _toLower(std::string s)
  {
    for (auto& c : s) c = (char)::tolower((unsigned char)c);
    return s;
  }
};

#else  // !defined(_WIN32) && !defined(__APPLE__) — no-op stub

class glint_attributes_list_window
{
public:
  static glint_attributes_list_window* open(
      RECT,
      const std::vector<const char*>&,
      std::set<std::string>,
      std::function<void(std::string)>,
      std::function<void()> = nullptr)
  {
    return nullptr;  // attribute-picker not supported on this platform
  }
  void close() { delete this; }
};

#endif  // _WIN32 / __APPLE__

