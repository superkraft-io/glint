#pragma once

/**
 * glint_input.hpp
 * Single-line text-input component (HTML <input> equivalent).
 *
 * Extends glint_text_editor_base with:
 *   - Input types: "text", "number", "password", "email"
 *   - Placeholder text
 *   - Horizontal text scrolling when content overflows
 *   - onSubmit callback (Enter key)
 *   - Number type: key filtering, min/max clamping on blur
 *   - Click-to-position cursor, drag-to-select
 *   - Select-all on focus (matches browser default behaviour)
 *   - Full Skia draw path: selection highlight, caret blink
 *   - inspector DrawContentToCanvas path
 *
 * Usage via builder:
 *   add.input([](glint_input& _c) {
 *     _c.style.width  = "100%";
 *     _c.style.height = 36.f;
 *     _c.placeholder  = "Enter value…";
 *     _c.type         = "number";
 *     _c.min          = 0.f;
 *     _c.max          = 100.f;
 *     _c.onChange  = [](const std::string& v) { DBGMSG("changed: %s\n", v.c_str()); };
 *     _c.onSubmit  = [](const std::string& v) { DBGMSG("submit:  %s\n", v.c_str()); };
 *   });
 */

#include "glint_text_editor_base.hpp"
#include "glint_form.hpp"
#include "../default_style.hpp"
#include "../render/glint_resource_request.hpp"
#include "glint_button.hpp"
#include "glint_slider.hpp"
#include "glint_checkbox.hpp"
#include "glint_radio.hpp"

#include <cmath>
#include <cctype>
#include <limits>
#include <string>
#include <functional>
#include <regex>

#include "include/core/SkCanvas.h"
#include "include/core/SkPaint.h"
#include "include/core/SkFont.h"

// ─── glint_text_input ─────────────────────────────────────────────────────────
// Internal text-editing delegate — use glint_input for the public-facing shell.

class glint_text_input : public glint_text_editor_base
{
public:
  // ── Configuration (set in builder callback) ────────────────────────────────

  // Set to true when a parent wants the delegate to render against the
  // parent's content box and computed style instead of its own.
  bool mUseParentStyle = false;

  /** Input type: text-like values plus checkbox, radio, range, and hidden. */
  std::string type = "text";

  /** Virtual keyboard hint only; does not change validation semantics. */
  std::string inputmode;

  /** Virtual keyboard return-key label hint only. */
  std::string enterkeyhint;

  /** Autofill hint token, mirroring the HTML autocomplete attribute. */
  std::string autocomplete;

  /** Autocapitalization hint, e.g. none, sentences, words, characters. */
  std::string autocapitalize;

  /** Spellchecking hint, typically true or false. */
  std::string spellcheck;

  /** Maximum number of Unicode codepoints allowed, or -1 when unlimited. */
  int maxlength = -1;

  /** Minimum number of Unicode codepoints required, or -1 when unlimited. */
  int minlength = -1;

  /** When true, an empty value is invalid. */
  bool required = false;

  /** When true, email inputs accept comma-separated addresses. */
  bool multiple = false;

  /** Regex pattern used for validity checks, matching HTML pattern semantics. */
  std::string pattern;

  /** Placeholder text shown when the field is empty and unfocused. */
  std::string placeholder;

  /** Minimum value (number type only). */
  float min = std::numeric_limits<float>::lowest();

  /** Maximum value (number type only). */
  float max = std::numeric_limits<float>::max();

  /** Step used for number validity and arrow-key increments (0 = ignore). */
  float step = 0.f;

  /**
   * Called when the user presses Enter.
   * The current text value is passed as the argument.
   * The field does NOT automatically blur — call blur() in the handler if desired.
   */
  std::function<void(const std::string&)> onSubmit;

  /**
   * Optional key interceptor — called first in OnKeyDown, before built-in handling.
   * Return true to consume the key (suppresses default behaviour).
   */
  std::function<bool(const glint_key_press&)> onKeyDown;

  // ── Tag (for glint_document::GetNodeWithTag) ──────────────────────────────────
  int tag = glint_no_tag;

  // ── Construction ────────────────────────────────────────────────────────────

  glint_text_input()
  {
    setCssStyleLayer(glint_default_user_agent_style_for(*this));
    computedStyle = mergedStyleForLayout();
  }

  const char* typeName() const override { return "text-input"; }

protected:
  int maxTextLength() const override { return maxlength; }
  int minTextLength() const override { return minlength; }
  bool isRequiredTextValue() const override { return required; }
  std::string validationPattern() const override { return pattern; }

public:

  bool wantsPeriodicRedraw() const override { return mFocused; }

  bool hasParsableNumberValue() const
  {
    if (type != "number" || mText.empty()) return true;
    double parsed = 0.0;
    return _tryParseCommittedNumberValue(mText, parsed);
  }

  bool satisfiesMinNumberValue() const
  {
    if (type != "number" || mText.empty()) return true;
    double parsed = 0.0;
    if (!_tryParseCommittedNumberValue(mText, parsed)) return true;
    if (min == std::numeric_limits<float>::lowest()) return true;
    return parsed >= static_cast<double>(min);
  }

  bool satisfiesMaxNumberValue() const
  {
    if (type != "number" || mText.empty()) return true;
    double parsed = 0.0;
    if (!_tryParseCommittedNumberValue(mText, parsed)) return true;
    if (max == std::numeric_limits<float>::max()) return true;
    return parsed <= static_cast<double>(max);
  }

  bool satisfiesStepNumberValue() const
  {
    if (type != "number" || mText.empty() || step <= 0.f) return true;
    double parsed = 0.0;
    if (!_tryParseCommittedNumberValue(mText, parsed)) return true;

    const double stepValue = static_cast<double>(step);
    const double base = (min != std::numeric_limits<float>::lowest()) ? static_cast<double>(min) : 0.0;
    const double remainder = std::fmod(parsed - base, stepValue);
    const double tolerance = std::max(1e-6, std::abs(stepValue) * 1e-6);
    return std::abs(remainder) <= tolerance || std::abs(remainder - stepValue) <= tolerance;
  }

  bool satisfiesNumberConstraints() const
  {
    return satisfiesRequiredTextValue()
        && hasParsableNumberValue()
        && satisfiesMinNumberValue()
        && satisfiesMaxNumberValue()
        && satisfiesStepNumberValue();
  }

  bool satisfiesEmailValue() const
  {
    if (type != "email" || mText.empty()) return true;

    static const std::regex kEmailPattern(
      R"(^[A-Za-z0-9.!#$%&'*+/=?^_`{|}~-]+@[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?(?:\.[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?)*$)",
      std::regex::ECMAScript);

    auto trimAsciiWhitespace = [](std::string_view value) {
      size_t start = 0;
      size_t end = value.size();
      while (start < end && std::isspace(static_cast<unsigned char>(value[start]))) ++start;
      while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1]))) --end;
      return std::string(value.substr(start, end - start));
    };

    if (!multiple) return std::regex_match(trimAsciiWhitespace(mText), kEmailPattern);

    size_t start = 0;
    while (start <= mText.size())
    {
      const size_t comma = mText.find(',', start);
      const size_t end = (comma == std::string::npos) ? mText.size() : comma;
      const std::string token = trimAsciiWhitespace(std::string_view(mText).substr(start, end - start));
      if (token.empty() || !std::regex_match(token, kEmailPattern)) return false;
      if (comma == std::string::npos) break;
      start = comma + 1;
    }

    return true;
  }

  bool satisfiesUrlValue() const
  {
    if (type != "url" || mText.empty()) return true;

    auto trimAsciiWhitespace = [](std::string_view value) {
      size_t start = 0;
      size_t end = value.size();
      while (start < end && std::isspace(static_cast<unsigned char>(value[start]))) ++start;
      while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1]))) --end;
      return std::string(value.substr(start, end - start));
    };

    const std::string trimmed = trimAsciiWhitespace(mText);
    if (trimmed.empty()) return true;

    for (char ch : trimmed)
      if (std::isspace(static_cast<unsigned char>(ch))) return false;

    static const std::regex kAbsoluteUrlPattern(
      R"(^[A-Za-z][A-Za-z0-9+.-]*:.+$)",
      std::regex::ECMAScript);
    if (!std::regex_match(trimmed, kAbsoluteUrlPattern)) return false;

    const size_t colonPos = trimmed.find(':');
    const std::string scheme = trimmed.substr(0, colonPos);
    const std::string suffix = trimmed.substr(colonPos + 1);

    if (suffix.rfind("/", 0) == 0 && suffix.rfind("//", 0) != 0 && scheme != "file")
    {
      if (suffix.size() == 1) return false;
      const char next = suffix[1];
      if (next == '?' || next == '#') return false;
      return true;
    }

    if (trimmed.find("://") == std::string::npos) return true;

    glint_resource_request req;
    req.url = trimmed;
    req.parseUrl();
    if (req.scheme == "file") return !req.pathname.empty();
    return !req.scheme.empty() && !req.host.empty();
  }

  std::chrono::steady_clock::time_point nextPeriodicRedrawTime() const override
  {
    return nextCaretToggleTime();
  }

  /**
   * Map a screen-space X coordinate to a byte index in getValue().
   * Uses the same text-base calculation as click targeting so it correctly
   * accounts for horizontal scroll and textAlign.
   * Returns 0 when clientX is to the left of the text, text.size() at the right.
   */
  int charIndexAtMouseX(float clientX) const
  {
    const std::string disp = _display();
    if (disp.empty()) return 0;
    const glint_rect content = _contentRect();
    const float fs      = _fontSize();
    const float baseX   = _textBaseX(disp, content, fs);
    return charIndexAtX(disp, clientX - baseX, fs);
  }

  // ── Focus ─────────────────────────────────────────────────────────────────

  void onFocusGained() override
  {
    glint_text_editor_base::onFocusGained();
    selectAll();   // match browser: focus selects all existing text
  }

  void onFocusLost() override
  {
    glint_text_editor_base::onFocusLost();
    mScrollOffsetX = 0.f;   // reset scroll when focus leaves
    // Reset click-count state so the next focus session starts fresh.
    mClickCount     = 0;
    mLastClickTime  = {};
    setDirty(false);
  }

  // ── Keyboard overrides ────────────────────────────────────────────────────

  bool OnKeyDown(const glint_key_press& key) override
  {
    if (disabled) return false;

    // External interceptor — checked before any built-in handling.
    if (onKeyDown && onKeyDown(key)) return true;

    // Enter → fire onSubmit, then blur.
    if (!key.ctrl && key.vk == glint_vk::RETURN)
    {
      if (onSubmit) onSubmit(mText);
      blur();
      return true;
    }

    // Up/Down → increment/decrement for number type.
    if (!key.ctrl && type == "number")
    {
      if (key.vk == glint_vk::UP)   { _adjustNumber( 1.f); return true; }
      if (key.vk == glint_vk::DOWN) { _adjustNumber(-1.f); return true; }
    }

    return glint_text_editor_base::OnKeyDown(key);
  }

  // ── Mouse — click to position cursor, drag to select ─────────────────────

  void OnMouseDown(float x, float y, const glint_mouse_mod& mod) override
  {
    // Consume the "just gained focus" flag regardless of click type.
    const bool justFocused = mJustGainedFocus;
    mJustGainedFocus = false;

    // Right-click: show context menu and bail out.
    if (mod.R)
    {
      _showContextMenu(x, y);
      return;
    }

    // -- Cursor position under click ------------------------------------------
    const float fontSize_pre = _fontSize();
    const std::string disp_pre = _display();
    const glint_rect content_pre  = _contentRect();
    const float relX_pre     = x - _textBaseX(disp_pre, content_pre, fontSize_pre);
    int pos_pre = charIndexAtX(disp_pre, relX_pre, fontSize_pre);
    if (type == "password") pos_pre = _displayToTextIdx(pos_pre);

    // -- Shift+Click: extend selection from anchor ----------------------------
    // Skip when the input just got focused this click (treat as regular click,
    // matching Chrome's behaviour: Shift+Click on an unfocused field does not
    // extend the previous selection).
    if (mod.S && !justFocused)
    {
      if (mSelStart == -1) mSelStart = mCursorPos;   // anchor at current cursor
      mSelEnd    = pos_pre;
      mCursorPos = pos_pre;
      if (mSelStart == mSelEnd) mSelStart = mSelEnd = -1;
      mDragStartPos = pos_pre;
      mDragMode = DragMode::CHAR;
      resetBlink();
      setDirty(false);
      return;
    }

    // ── Click-count increment (time + distance check, matching browser rules) ─
    const auto now = std::chrono::steady_clock::now();
    const long elapsedMs = static_cast<long>(
      std::chrono::duration_cast<std::chrono::milliseconds>(now - mLastClickTime).count());
#ifdef _WIN32
    const long dblMs = static_cast<long>(::GetDoubleClickTime());
#else
    const long dblMs = 500L;
#endif
    const float dist = std::abs(x - mLastClickX);
    if (elapsedMs <= dblMs && dist <= 4.f)
    {
      ++mClickCount;
      if (mClickCount >= 4) mClickCount = 1;   // Chrome resets on 4th click
    }
    else
    {
      mClickCount = 1;
    }
    mLastClickTime = now;
    mLastClickX    = x;

    // ── Cursor position under click ────────────────────────────────────────
    const float fontSize     = _fontSize();
    const std::string disp   = _display();
    const glint_rect content      = _contentRect();
    const float relX         = x - _textBaseX(disp, content, fontSize);
    int pos = charIndexAtX(disp, relX, fontSize);
    if (type == "password") pos = _displayToTextIdx(pos);

    // ── Apply action based on click count ─────────────────────────────────
    if (mClickCount == 1)
    {
      // Single click: place cursor, clear selection.
      mCursorPos    = pos;
      mSelStart = mSelEnd = -1;
      mDragStartPos = pos;
      mDragMode     = DragMode::CHAR;
    }
    else if (mClickCount == 2)
    {
      // Double click: select word / whitespace / punct under cursor.
      // Password type has no meaningful word boundaries — select all.
      if (type == "password")
      {
        selectAll();
        mDragMode = DragMode::ALL;
      }
      else
      {
        auto [ws, we]    = wordBoundary(mText, pos);
        mSelStart        = ws;
        mSelEnd          = we;
        mCursorPos       = we;
        mWordAnchorStart = ws;
        mWordAnchorEnd   = we;
        mDragMode        = DragMode::WORD;
      }
    }
    else
    {
      // Triple click (or reset count=1 after 3): select all.
      selectAll();
      mDragMode = DragMode::ALL;
    }

    resetBlink();
    setDirty(false);
  }

  void OnMouseDrag(float x, float /*y*/, float /*dX*/, float /*dY*/,
                   const glint_mouse_mod& /*mod*/) override
  {
    if (mDragMode == DragMode::ALL) return;   // triple-click: nothing to extend

    const float fontSize   = _fontSize();
    const std::string disp = _display();
    const glint_rect content    = _contentRect();
    const float relX       = x - _textBaseX(disp, content, fontSize);
    int pos = charIndexAtX(disp, relX, fontSize);
    if (type == "password") pos = _displayToTextIdx(pos);

    if (mDragMode == DragMode::CHAR)
    {
      // Character-by-character selection from the initial click position.
      if (pos != mDragStartPos)
      {
        mSelStart  = mDragStartPos;
        mSelEnd    = pos;
        mCursorPos = pos;
      }
      else
      {
        mCursorPos = pos;
        mSelStart  = mSelEnd = -1;
      }
    }
    else   // WORD
    {
      // Word-by-word selection: the initially double-clicked word is the anchor.
      // Dragging left extends selStart to the word-start of the drag position;
      // dragging right extends selEnd to the word-end of the drag position.
      if (pos <= mWordAnchorStart)
      {
        auto [ws, we] = wordBoundary(mText, pos);
        mSelStart     = mWordAnchorEnd;   // anchor right edge stays fixed
        mSelEnd       = ws;
        mCursorPos    = ws;
      }
      else if (pos >= mWordAnchorEnd)
      {
        auto [ws, we] = wordBoundary(mText, pos);
        mSelStart     = mWordAnchorStart; // anchor left edge stays fixed
        mSelEnd       = we;
        mCursorPos    = we;
      }
      else
      {
        // Back inside the anchor word: restore anchor selection.
        mSelStart  = mWordAnchorStart;
        mSelEnd    = mWordAnchorEnd;
        mCursorPos = mWordAnchorEnd;
      }
    }

    resetBlink();
    onCursorMoved();
    setDirty(false);
  }

  // ── Drawing ────────────────────────────────────────────────────────────────

  void drawContent(glint_canvas& g) override
  {
    SkCanvas* canvas = static_cast<SkCanvas*>(g.GetDrawContext());
    if (!canvas) return;

    _drawToSkia(canvas);
  }

  void DrawContentToCanvas(SkCanvas* canvas) override
  {
    if (!canvas) return;
    _drawToSkia(canvas);
  }

protected:
  // ── Scroll ────────────────────────────────────────────────────────────────

  float mScrollOffsetX = 0.f;   // horizontal scroll offset in pixels
  int   mDragStartPos  = 0;     // byte pos where a char-drag started

  // ── Click count & drag mode ────────────────────────────────────────────────

  enum class DragMode { CHAR, WORD, ALL };

  int        mClickCount     = 0;
  float      mLastClickX     = -9999.f;   // screen X of previous mousedown
  std::chrono::steady_clock::time_point mLastClickTime {};  // epoch = far past

  DragMode   mDragMode         = DragMode::CHAR;
  int        mWordAnchorStart  = 0;   // left boundary of the double-clicked word
  int        mWordAnchorEnd    = 0;   // right boundary

  // ── Subclass hooks ────────────────────────────────────────────────────────

  void onCursorMoved() override
  {
    ensureCursorVisible();
  }

  /**
   * Number type character filter.
   * Accepts only numeric editing states for direct typing.
   */
  bool filterChar(const std::string& s) override
  {
    if (type != "number") return true;
    return allowsTextInsertion(s);
  }

  bool allowsTextInsertion(const std::string& s) const override
  {
    if (type != "number") return true;

    const int replaceLo = hasSelection() ? std::min(mSelStart, mSelEnd) : mCursorPos;
    const int replaceHi = hasSelection() ? std::max(mSelStart, mSelEnd) : mCursorPos;

    std::string candidate = mText;
    candidate.replace(static_cast<size_t>(replaceLo),
                      static_cast<size_t>(replaceHi - replaceLo),
                      s);

    return _isValidNumberEditState(candidate);
  }

private:
  // ── Helpers ────────────────────────────────────────────────────────────────

  static bool _isValidNumberEditState(const std::string& value)
  {
    if (value.empty()) return true;

    size_t index = 0;
    if (value[0] == '-')
    {
      index = 1;
      if (index == value.size()) return true;
    }

    bool seenDot = false;
    for (; index < value.size(); ++index)
    {
      const unsigned char c = static_cast<unsigned char>(value[index]);
      if (c >= '0' && c <= '9') continue;
      if (c == '.' && !seenDot)
      {
        seenDot = true;
        continue;
      }
      return false;
    }

    return true;
  }

  // When acting as a delegate, text should render inside the *parent's* content
  // area (respecting the parent's padding) rather than the delegate's own rect.
  glint_rect _contentRect() const
  {
    if (mUseParentStyle && mParent) return mParent->getContent();
    return getContent();
  }

  const glint_style& _activeStyle() const
  {
    if (mUseParentStyle && mParent) return mParent->computedStyle;
    return computedStyle;
  }

  float _fontSize() const
  {
    const float fs = _activeStyle().fontSize.toFloat();
    return (fs > 0.f) ? fs : 13.f;
  }

  // -- Context menu (right-click) ------------------------------------------
  // Uses Win32 TrackPopupMenu directly (synchronous, TPM_RETURNCMD) so the
  // result is handled inline.
  // x/y are scroll-adjusted content-space; we reverse the ancestor scroll
  // chain to get window-client coords for ClientToScreen.

  void _showContextMenu(float x, float y)
  {
#ifdef _WIN32
    const bool hasSelection = (mSelStart != -1 && mSelStart != mSelEnd);

    // Reverse scroll-adjusted content-space coords back to window-client space.
    float wx = x, wy = y;
    for (glint_element* p = mParent; p; p = p->mParent)
    {
      wx -= p->mScrollLeft;
      wy -= p->mScrollTop;
    }

    HWND hwnd = mpG ? static_cast<HWND>(mpG->GetWindow())
                     : (mRoot ? mRoot->hwnd : nullptr);
    if (!hwnd) return;

    // Load localized strings from user32.dll (edit-control context menu strings).
    // These IDs are stable across all Windows versions and honour the OS language.
    auto sysStr = [](UINT id, const wchar_t* fallback) -> std::wstring {
      wchar_t buf[256] = {};
      if (int n = ::LoadStringW(::GetModuleHandleW(L"user32.dll"), id, buf, 256); n > 0)
        return std::wstring(buf, static_cast<size_t>(n));
      return fallback;
    };
    const std::wstring sCut       = sysStr(31961, L"Cu&t");
    const std::wstring sCopy      = sysStr(31962, L"&Copy");
    const std::wstring sPaste     = sysStr(31963, L"&Paste");
    const std::wstring sSelectAll = sysStr(31965, L"Select &All");

    HMENU hMenu = ::CreatePopupMenu();
    ::AppendMenuW(hMenu, MF_STRING | ((!readonly && hasSelection) ? 0u : MF_GRAYED), 1, sCut.c_str());
    ::AppendMenuW(hMenu, MF_STRING | (hasSelection ? 0u : MF_GRAYED),               2, sCopy.c_str());
    ::AppendMenuW(hMenu, MF_STRING | (!readonly ? 0u : MF_GRAYED),                  3, sPaste.c_str());
    ::AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(hMenu, MF_STRING | MF_ENABLED, 5, sSelectAll.c_str());

    POINT pt = { static_cast<LONG>(wx), static_cast<LONG>(wy) };
    ::ClientToScreen(hwnd, &pt);

    const int result = static_cast<int>(::TrackPopupMenu(
      hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_LEFTALIGN,
      pt.x, pt.y, 0, hwnd, nullptr));
    ::DestroyMenu(hMenu);

    switch (result)
    {
      case 1: cut();                      break;
      case 2: copy();                     break;
      case 3: if (!readonly) { paste(); } break;
      case 5: selectAll();                break;
    }
    setDirty(false);
#elif defined(__APPLE__)
    {
      const bool hasSelection = (mSelStart != -1 && mSelStart != mSelEnd);
      using P = std::pair<int, std::string>;
      const std::vector<P> items = {
        {1, "Cut"},
        {2, "Copy"},
        {3, "Paste"},
        {0, "-"},
        {5, "Select All"},
      };
      std::vector<int> disabled;
      if (readonly || !hasSelection) disabled.push_back(1);  // Cut
      if (!hasSelection)             disabled.push_back(2);  // Copy
      if (readonly)                  disabled.push_back(3);  // Paste
      const int result = glint_platform::showContextMenu(0, 0, items, disabled);
      switch (result) {
        case 1: cut();                      break;
        case 2: copy();                     break;
        case 3: if (!readonly) { paste(); } break;
        case 5:
          // The NSMenu tracking run loop can cause focus to be cleared while
          // the menu is open.  Re-focus unconditionally before selecting so
          // the selection state survives until the next render frame.
          // SetFocus is a no-op if this element is already focused.
          if (mRoot) mRoot->SetFocus(this);
          selectAll();
          break;
      }
      setDirty(false);
    }
#endif
  }

  // -- Number field increment/decrement (Up/Down arrow) ----------------------

  void _adjustNumber(float delta)
  {
    float v = 0.f;
    try { v = mText.empty() ? 0.f : std::stof(mText); } catch (...) {}
    const float increment = (step > 0.f ? step : 1.f) * delta;
    v = std::max(min, std::min(max, v + increment));
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%g", v);
    pushUndo();
    mText      = buf;
    mCursorPos = static_cast<int>(mText.size());
    mSelStart = mSelEnd = -1;
    onTextChanged();
    if (onChange) onChange(mText);
    resetBlink();
    onCursorMoved();
    setDirty(false);
  }

  /** Returns the display string (password-masked or raw). */
  std::string _display() const
  {
    if (type != "password") return mText;
    // Count codepoints for masking.
    int n = 0, pos = 0;
    while (pos < static_cast<int>(mText.size()))
    { pos = nextCodepoint(mText, pos); ++n; }
    return std::string(static_cast<size_t>(n), '*');
  }

  /** Map a byte index in the display (masked) string back to mText byte index.
   *  For non-password types this is identity.
   *  For password, display char i  → mText codepoint i → byte boundary. */
  int _displayToTextIdx(int displayByteIdx) const
  {
    if (type != "password") return displayByteIdx;
    // display string is all ASCII '*', so displayByteIdx == codepoint count.
    int cp = displayByteIdx;
    int pos = 0;
    while (cp > 0 && pos < static_cast<int>(mText.size()))
    { pos = nextCodepoint(mText, pos); --cp; }
    return pos;
  }

  /** Adjust mScrollOffsetX so the cursor byte position is visible. */
  void ensureCursorVisible()
  {
    const float fs     = _fontSize();
    const std::string disp = _display();
    const glint_rect content = _contentRect();
    const float viewW  = content.W();
    if (viewW <= 0.f) return;

    // Map cursor byte pos to display byte pos for password type.
    int dispCursorPos = mCursorPos;
    if (type == "password")
    {
      // Count codepoints up to mCursorPos.
      int cp = 0, p = 0;
      while (p < mCursorPos && p < static_cast<int>(mText.size()))
      { p = nextCodepoint(mText, p); ++cp; }
      dispCursorPos = cp;   // each CP → one '*' byte
    }

    const float cx = charXOffset(disp, dispCursorPos, fs);
    const float margin = 4.f;

    if (cx - mScrollOffsetX > viewW - margin)
      mScrollOffsetX = cx - viewW + margin;
    if (cx - mScrollOffsetX < margin)
      mScrollOffsetX = cx - margin;

    mScrollOffsetX = std::max(0.f, mScrollOffsetX);
  }

  /** Clamp numeric value to [min, max] and update mText. */
  void _clampNumber()
  {
    if (mText.empty() || mText == "-" || mText == ".") return;
    try
    {
      const float v = std::stof(mText);
      const float clamped = std::max(min, std::min(max, v));
      if (clamped != v)
      {
        // Format: strip trailing zeros.
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%g", clamped);
        mText = buf;
        mCursorPos = static_cast<int>(mText.size());
        if (onChange) onChange(mText);
      }
    }
    catch (...) {}   // non-numeric string: leave as-is
  }
  static bool _tryParseCommittedNumberValue(const std::string& value, double& parsed)
  {
    try
    {
      size_t consumed = 0;
      parsed = std::stod(value, &consumed);
      return consumed == value.size() && std::isfinite(parsed);
    }
    catch (...)
    {
      return false;
    }
  }

  void _drawToSkia(SkCanvas* canvas)
  {
    const float fs      = _fontSize();
    const glint_rect content = _contentRect();
    const std::string disp = _display();
    const glint_style& active = _activeStyle();

    // Map cursor/selection positions to display positions for password type.
    int dispCursor = mCursorPos;
    int dispSelSt  = mSelStart;
    int dispSelEnd = mSelEnd;
    if (type == "password")
    {
      dispCursor = _cpCount(mText, mCursorPos);
      dispSelSt  = (mSelStart >= 0) ? _cpCount(mText, mSelStart) : -1;
      dispSelEnd = (mSelEnd   >= 0) ? _cpCount(mText, mSelEnd)   : -1;
    }

    // ── Clip to content area ────────────────────────────────────────────────
    canvas->save();
    canvas->clipRect(SkRect::MakeLTRB(content.L, content.T, content.R, content.B));

    const float textY = content.T + content.H() * 0.5f + fs * 0.35f;

    SkFont font = skFont(fs);

    // ── Compute text base X according to style.textAlign ────────────────────
    // Shared with OnMouseDown/OnMouseDrag via _textBaseX().
    const bool isCentered = (active.textAlign == EAlign::Center);
    const bool isRight    = (active.textAlign == EAlign::Far);
    const float baseX = _textBaseX(disp, content, fs);

    // ── Selection highlight ─────────────────────────────────────────────────
    if (dispSelSt != -1 && dispSelSt != dispSelEnd)
    {
      const int lo = std::min(dispSelSt, dispSelEnd);
      const int hi = std::max(dispSelSt, dispSelEnd);
      const float x0 = baseX + charXOffset(disp, lo, fs);
      const float x1 = baseX + charXOffset(disp, hi, fs);
      SkPaint selP;
      selP.setColor(SkColorSetARGB(180, 93, 177, 255));
      canvas->drawRect(
        SkRect::MakeLTRB(x0, textY - fs * 0.85f, x1, textY + fs * 0.3f), selP);
    }

    // ── Placeholder ─────────────────────────────────────────────────────────
    if (disp.empty() && !placeholder.empty() && !mFocused)
    {
      SkPaint ph;
      ph.setColor(SkColorSetARGB(160, 140, 140, 140));
      ph.setAntiAlias(true);
      const float phW = font.measureText(placeholder.c_str(), placeholder.size(), SkTextEncoding::kUTF8);
      const float phX = isCentered ? content.L + (content.W() - phW) * 0.5f
                      : isRight    ? content.R - phW
                      : content.L - mScrollOffsetX;
      canvas->drawString(placeholder.c_str(), phX, textY, font, ph);
    }
    else if (!disp.empty())
    {
      // ── Text ───────────────────────────────────────────────────────────────
      const glint_color& col = active.color.value;
      SkPaint tp;
      tp.setColor(SkColorSetARGB(col.A, col.R, col.G, col.B));
      tp.setAntiAlias(true);
      canvas->drawString(disp.c_str(), baseX, textY, font, tp);
    }

    // ── Caret ───────────────────────────────────────────────────────────────
    if (mFocused && !readonly && !disabled && caretVisible() && (dispSelSt == -1 || dispSelSt == dispSelEnd))
    {
      const float cx       = baseX + charXOffset(disp, dispCursor, fs);
      const float caretTop = textY - fs * 0.9f;   // near cap-height
      const float caretBot = textY + fs * 0.3f;    // includes descenders
      SkPaint cp;
      cp.setColor(SkColorSetARGB(255, 220, 220, 220));
      cp.setStrokeWidth(1.5f);
      cp.setAntiAlias(true);
      canvas->drawLine(cx, caretTop, cx, caretBot, cp);
    }

    canvas->restore();
  }

  /** Returns the X pixel origin of the text (left edge of glyph 0).
   *  Matches the baseX used by _drawToSkia() so click/drag hit-testing
   *  lands in the same coordinate space as the rendered text for all
   *  textAlign values (Near/Center/Far).
   *
   *  Chrome parity: text-align only takes effect when text FITS inside the
   *  content box.  Once text overflows (textW > viewW) the input switches to
   *  left-aligned scroll behaviour regardless of text-align, exactly as
   *  Chrome does.  mScrollOffsetX (kept up-to-date by ensureCursorVisible)
   *  drives the scroll in both centered and right-aligned overflow cases. */
  float _textBaseX(const std::string& disp, const glint_rect& content, float fs) const
  {
    const glint_style& active = _activeStyle();
    const bool isCentered = (active.textAlign == EAlign::Center);
    const bool isRight    = (active.textAlign == EAlign::Far);
    if (isCentered || isRight)
    {
      SkFont font = skFont(fs);
      const float textW = disp.empty() ? 0.f
        : font.measureText(disp.c_str(), disp.size(), SkTextEncoding::kUTF8);
      const float viewW = content.W();
      // When text fits, apply alignment.  When text overflows, fall through
      // to the left-aligned scroll path so ensureCursorVisible works.
      if (textW <= viewW)
        return isCentered ? content.L + (viewW - textW) * 0.5f
                          : content.R - textW;
    }
    return content.L - mScrollOffsetX;   // left-aligned or overflow scroll
  }

  /** Count Unicode codepoints in `s` up to byte position `byteEnd`. */
  static int _cpCount(const std::string& s, int byteEnd)
  {
    int cp = 0, pos = 0;
    while (pos < byteEnd && pos < static_cast<int>(s.size()))
    { pos = nextCodepoint(s, pos); ++cp; }
    return cp;
  }
};

// ─── glint_input ──────────────────────────────────────────────────────────────
// Thin shell that owns a glint_text_input (for text-like types), glint_button
// (for button-like types), or a specialized delegate child for range/checkbox/radio.
// The shell still owns layout chrome like border and padding; delegates can
// supply their own background when needed.
// All interaction logic lives inside the delegate child.

class glint_input : public glint_element
{
public:
  // ── Configuration ─────────────────────────────────────────────────────────

  /** Input type: text-like values plus range, checkbox, radio, hidden, button, submit, and reset. */
  std::string type        = "text";

  /** Virtual keyboard hint only; does not change validation semantics. */
  std::string inputmode;

  /** Virtual keyboard return-key label hint only. */
  std::string enterkeyhint;

  /** Autofill hint token, mirroring the HTML autocomplete attribute. */
  std::string autocomplete;

  /** Autocapitalization hint, e.g. none, sentences, words, characters. */
  std::string autocapitalize;

  /** Spellchecking hint, typically true or false. */
  std::string spellcheck;

  /** Placeholder text shown when the field is empty and unfocused. */
  std::string placeholder;

  /** Maximum number of Unicode codepoints allowed, or -1 when unlimited. */
  int maxlength = -1;

  /** Minimum number of Unicode codepoints required, or -1 when unlimited. */
  int minlength = -1;

  /** When true, an empty value is invalid. */
  bool required = false;

  /** When true, email inputs accept comma-separated addresses. */
  bool multiple = false;

  /** Regex pattern used for validity checks on text-like inputs. */
  std::string pattern;

  /** Minimum value.  For "range": lower bound of the slider. */
  float min  = std::numeric_limits<float>::lowest();

  /** Maximum value.  For "range": upper bound of the slider. */
  float max  = std::numeric_limits<float>::max();

  /** Range step (0 = continuous).  Only used when type is "range". */
  float step = 0.f;

  /** When true, keyboard input is ignored but selection/copy still work. */
  bool readonly = false;

  /** When true, the field is entirely non-interactive. */
  bool disabled = false;

  /** Form field name used during submit serialization. */
  std::string name;

  /** Tag for glint_document::GetNodeWithTag. */
  int tag = glint_no_tag;

  // ── Callbacks ─────────────────────────────────────────────────────────────

  /** Content change: text types → current text; "range" → numeric string. */
  std::function<void(const std::string&)> onChange;

  /** Called when the user presses Enter (text types only). */
  std::function<void(const std::string&)> onSubmit;

  /** Called when the control is clicked (button, submit, reset). */
  std::function<void(const std::string&)> onClick;

  /** Optional key interceptor (text types only). Return true to consume the key. */
  std::function<bool(const glint_key_press&)> onKeyDown;

  /** Called when the field gains focus (text types only). */
  std::function<void()> onFocus;

  /** Called when the field loses focus (text types only). */
  std::function<void()> onBlur;

  // ── Checkbox / radio fields ────────────────────────────────────────────────

  /** Initial checked state (type="checkbox" / type="radio"). */
  bool checked = false;

  /** Label text displayed next to the box/circle (type="checkbox" / type="radio"). */
  std::string text;

  /** Box or circle size in pixels (type="checkbox" / type="radio"). */
  float checkSize = 16.f;

  /** Logical value this input represents — used by radio groups. */
  std::string value;

  /** Shared group that deselects other radios when this one is selected (type="radio"). */
  std::shared_ptr<glint_radio_group> group;

  /** Fired when checked state toggles (type="checkbox" / type="radio"). */
  std::function<void(bool)> onCheck;

  // ── Construction ──────────────────────────────────────────────────────────

  glint_input()
  {
    setCssStyleLayer(glint_default_user_agent_style_for(*this));
    computedStyle = mergedStyleForLayout();
  }

  // ── Value API ─────────────────────────────────────────────────────────────

  /** Returns the current value as a string. */
  std::string getValue() const
  {
    if (mCheckbox) return mCheckbox->checked ? "true" : "false";
    if (mRadio)    return mRadio->value;
    if (mTextInput) return mTextInput->getValue();
    if (mButton) return _resolvedButtonLabel();
    if (mSlider)
    {
      char buf[64];
      std::snprintf(buf, sizeof(buf), "%g", mSlider->value);
      return std::string(buf);
    }
    return mPendingValue;   // pre-Layout: return whatever was last setValue'd
  }

  /** Sets the current value from a string. */
  void setValue(const std::string& v)
  {
    mPendingValue = v;   // buffer for pre-Layout calls
    if (mTextInput) { mTextInput->setValue(v); return; }
    if (mButton)    { mButton->SetLabel(_resolvedButtonLabel()); return; }
    if (mSlider)    { try { mSlider->SetValue(std::stof(v)); } catch (...) {} }
  }

  bool satisfiesMinLength() const
  {
    if (mTextInput) return mTextInput->satisfiesMinTextLength();
    return true;
  }

  bool satisfiesRequired() const
  {
    if (disabled || !required) return true;
    if (mTextInput) return mTextInput->satisfiesRequiredTextValue();
    if (mCheckbox) return mCheckbox->checked;
    if (mRadio) return checked;
    return true;
  }

  bool satisfiesPattern() const
  {
    if (mTextInput) return mTextInput->satisfiesPatternConstraint();
    return true;
  }

  bool hasValidNumberValue() const
  {
    if (mTextInput) return mTextInput->hasParsableNumberValue();
    return true;
  }

  bool satisfiesEmailValue() const
  {
    if (mTextInput) return mTextInput->satisfiesEmailValue();
    return true;
  }

  bool satisfiesUrlValue() const
  {
    if (mTextInput) return mTextInput->satisfiesUrlValue();
    return true;
  }

  bool satisfiesMinValue() const
  {
    if (mTextInput) return mTextInput->satisfiesMinNumberValue();
    return true;
  }

  bool satisfiesMaxValue() const
  {
    if (mTextInput) return mTextInput->satisfiesMaxNumberValue();
    return true;
  }

  bool satisfiesStepValue() const
  {
    if (mTextInput) return mTextInput->satisfiesStepNumberValue();
    return true;
  }

  bool satisfiesConstraints() const
  {
    if (mTextInput)
    {
      if (type == "number") return mTextInput->satisfiesNumberConstraints();
      if (type == "email") return mTextInput->satisfiesTextConstraints() && mTextInput->satisfiesEmailValue();
      if (type == "url") return mTextInput->satisfiesTextConstraints() && mTextInput->satisfiesUrlValue();
      return mTextInput->satisfiesTextConstraints();
    }
    return satisfiesRequired();
  }

  /** Returns the current value as a float (convenience for type "range"). */
  float getFloatValue() const
  {
    if (mSlider) return mSlider->value;
    try { return std::stof(getValue()); } catch (...) { return 0.f; }
  }

  /** Sets the current value from a float (convenience for type "range"). */
  void setFloatValue(float v)
  {
    mInitialFloatValue = v;
    if (mSlider) { mSlider->SetValue(v); return; }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%g", v);
    setValue(std::string(buf));
  }

  /** Maps a screen-space X coordinate to a byte index in the current text
   *  (text types only — delegates to the inner glint_text_input). */
  int charIndexAtMouseX(float clientX) const
  {
    if (mTextInput) return mTextInput->charIndexAtMouseX(clientX);
    return 0;
  }

  // ── Metadata ──────────────────────────────────────────────────────────────

  const char* typeName() const override { return "input"; }

  std::string getAttribute(const std::string& name, bool& found) const override
  {
    if (name == "type") { found = true; return type.empty() ? "text" : type; }
    if (name == "name") { found = true; return this->name; }
    if (name == "value" && _isButtonLikeType(type)) { found = true; return getValue(); }
    if (name == "inputmode") { found = true; return inputmode; }
    if (name == "enterkeyhint") { found = true; return enterkeyhint; }
    if (name == "autocomplete") { found = true; return autocomplete; }
    if (name == "autocapitalize") { found = true; return autocapitalize; }
    if (name == "spellcheck") { found = true; return spellcheck; }
    if (name == "maxlength") { found = true; return maxlength >= 0 ? std::to_string(maxlength) : std::string(); }
    if (name == "minlength") { found = true; return minlength >= 0 ? std::to_string(minlength) : std::string(); }
    if (name == "required") { found = true; return required ? "true" : std::string(); }
    if (name == "multiple") { found = true; return multiple ? "true" : std::string(); }
    if (name == "pattern") { found = true; return pattern; }
    if (name == "min") { found = true; return min != std::numeric_limits<float>::lowest() ? std::to_string(min) : std::string(); }
    if (name == "max") { found = true; return max != std::numeric_limits<float>::max() ? std::to_string(max) : std::string(); }
    if (name == "step") { found = true; return step > 0.f ? std::to_string(step) : std::string(); }
    return glint_element::getAttribute(name, found);
  }

  bool isFormAssociatedControl() const override { return true; }
  std::string formControlName() const override { return name; }
  bool isFormControlDisabled() const override { return disabled; }

  bool formControlIsValid() const override
  {
    if (disabled || type == "hidden" || _isButtonLikeType(type)) return true;
    return satisfiesConstraints();
  }

  void captureFormDefaultsIfNeeded() override
  {
    if (mFormDefaultsCaptured) return;
    mDefaultChecked = checked;
    mDefaultValue = getValue();
    mDefaultFloatValue = getFloatValue();
    mFormDefaultsCaptured = true;
  }

  void resetFormControl() override
  {
    captureFormDefaultsIfNeeded();

    if (type == "checkbox" || type == "radio")
    {
      checked = mDefaultChecked;
      _syncDelegateProps();
      setDirty(false);
      return;
    }

    if (type == "range")
    {
      setFloatValue(mDefaultFloatValue);
      _syncDelegateProps();
      setDirty(false);
      return;
    }

    setValue(mDefaultValue);
    _syncDelegateProps();
    setDirty(false);
  }

  void appendFormValues(std::vector<glint_form_value>& values,
                        const glint_element* submitter) const override
  {
    if (disabled || name.empty() || type == "button" || type == "reset")
      return;

    if (type == "submit")
    {
      if (submitter != this) return;
      values.push_back({name, getValue(), const_cast<glint_input*>(this)});
      return;
    }

    if (type == "checkbox" || type == "radio")
    {
      if (!checked) return;
      values.push_back({
        name,
        value.empty() ? std::string("on") : value,
        const_cast<glint_input*>(this)
      });
      return;
    }

    values.push_back({name, getValue(), const_cast<glint_input*>(this)});
  }

  // ── Hit testing ───────────────────────────────────────────────────────────
  // Forward hits in the padding/border area to the delegate so clicking
  // anywhere within glint_input always interacts with it.

  glint_element* HitTest(float x, float y) override
  {
    if (!GetPaintRECT().Contains(x, y)) return nullptr;
    auto* hit = glint_element::HitTest(x, y);
    if (hit && hit != this) return hit;
    if (mButton)    return mButton;
    if (mTextInput) return mTextInput;
    if (mSlider)    return mSlider;
    if (mCheckbox)  return mCheckbox;
    if (mRadio)     return mRadio;
    return this;
  }

  // ── Layout ────────────────────────────────────────────────────────────────

  // ── Focus redirect ────────────────────────────────────────────────────────
  // When SetFocus() is called on the shell (e.g. programmatic auto-focus after
  // a panel rebuild), redirect it to the inner delegate so keyboard events
  // reach the actual text editor.  If the delegate hasn't been built yet
  // (Layout hasn't run), remember to redirect once _buildDelegate() creates it.

  void onFocusGained() override
  {
    if (disabled)
    {
      mFocusPending = false;
      return;
    }

    if (mTextInput && mRoot)
    {
      mRoot->SetFocus(mTextInput);   // delegate already exists — forward immediately
      return;
    }
    mFocusPending = true;   // delegate not yet built; forward on first _buildDelegate()
  }

  void tickTransitionsAll() override
  {
    // Keep hidden/disabled shell state in sync before parent layout decides
    // whether to skip this subtree based on computedStyle.display.
    _syncDelegateProps();
    glint_element::tickTransitionsAll();
  }
  
  void syncBeforeLayout() override
  {
    if (mActiveDelegateKind != _delegateKindForType(type))
      _buildDelegate();
    _syncDelegateProps();
  }
  
  float preferredW() const override
  {
    if (type == "checkbox" || type == "radio")
    {
      const float controlSize = checkSize > 0.f ? checkSize : 16.f;
      if (text.empty()) return controlSize;

      const float gap = std::roundf(controlSize * 0.5f);
      SkFont font = skFont(controlSize,
                           computedStyle.fontFamily.c_str(),
                           computedStyle.fontWeight,
                           computedStyle.fontStyle.c_str());
      SkRect bounds;
      const float textWidth = font.measureText(text.c_str(), text.size(), SkTextEncoding::kUTF8, &bounds);
      return controlSize + gap + textWidth;
    }

    if (!_isButtonLikeType(type)) return glint_element::preferredW();
  
    const std::string label = _resolvedButtonLabel();
    if (label.empty()) return 0.f;
  
    const float sz = computedStyle.fontSize.toFloat() > 0.f ? computedStyle.fontSize.toFloat() : 13.f;
    SkFont font = skFont(sz,
                         computedStyle.fontFamily.c_str(),
                         computedStyle.fontWeight,
                         computedStyle.fontStyle.c_str());
    SkRect bounds;
    return font.measureText(label.c_str(), label.size(), SkTextEncoding::kUTF8, &bounds) + 4.f;
  }
  
  float preferredH(float availW = 0.f) const override
  {
    if (type == "checkbox" || type == "radio")
      return checkSize > 0.f ? checkSize : 16.f;

    if (!_isButtonLikeType(type)) return glint_element::preferredH(availW);
  
    const std::string label = _resolvedButtonLabel();
    if (label.empty()) return 0.f;
  
    const float sz = computedStyle.fontSize.toFloat() > 0.f ? computedStyle.fontSize.toFloat() : 13.f;
    SkFont font = skFont(sz,
                         computedStyle.fontFamily.c_str(),
                         computedStyle.fontWeight,
                         computedStyle.fontStyle.c_str());
    SkFontMetrics metrics;
    font.getMetrics(&metrics);
    return -metrics.fAscent + metrics.fDescent + std::max(0.f, metrics.fLeading);
  }

  void Layout(glint_canvas* g) override
  {
    if (mActiveDelegateKind != _delegateKindForType(type))
      _buildDelegate();
    _syncDelegateProps();
    glint_element::Layout(g);
  }

private:
  static std::string _delegateKindForType(const std::string& inputType)
  {
    if (_isButtonLikeType(inputType)) return "button";
    if (inputType == "checkbox") return "checkbox";
    if (inputType == "radio")    return "radio";
    if (inputType == "range")    return "range";
    return "text";
  }

  static bool _isButtonLikeType(const std::string& inputType)
  {
    return inputType == "button" || inputType == "submit" || inputType == "reset";
  }

  std::string _defaultButtonLabel() const
  {
    if (type == "submit") return "Submit";
    if (type == "reset") return "Reset";
    return "Button";
  }

  std::string _resolvedButtonLabel() const
  {
    if (!mPendingValue.empty()) return mPendingValue;
    if (!value.empty()) return value;
    if (!text.empty()) return text;
    return _defaultButtonLabel();
  }

  std::string        mActiveDelegateKind;
  glint_button*      mButton     = nullptr;
  glint_text_input*  mTextInput  = nullptr;
  glint_slider*      mSlider     = nullptr;
  glint_checkbox*    mCheckbox   = nullptr;
  glint_radio*       mRadio      = nullptr;
  float              mInitialFloatValue = 0.f;
  std::string        mPendingValue;
  bool               mFocusPending = false;   // true when shell was focused before delegate existed
  float              mEnabledOpacity = 1.f;
  bool               mDisabledOpacityApplied = false;
  std::string        mDisplayBeforeHidden;
  bool               mHiddenDisplayApplied = false;
  bool               mFormDefaultsCaptured = false;
  bool               mDefaultChecked = false;
  float              mDefaultFloatValue = 0.f;
  std::string        mDefaultValue;

  void _buildDelegate()
  {
    if (mButton)    { removeChild(mButton);    mButton    = nullptr; }
    if (mTextInput) { removeChild(mTextInput); mTextInput = nullptr; }
    if (mSlider)    { removeChild(mSlider);    mSlider    = nullptr; }
    if (mCheckbox)  { removeChild(mCheckbox);  mCheckbox  = nullptr; }
    if (mRadio)     { removeChild(mRadio);     mRadio     = nullptr; }

    auto applyAttachedUserAgentStyle = [](glint_element* el)
    {
      el->setCssStyleLayer(glint_default_user_agent_style_for(*el));
      el->computedStyle = el->mergedStyleForLayout();
    };

    if (_isButtonLikeType(type))
    {
      auto* bt           = new glint_button();
      bt->SetLabel(_resolvedButtonLabel());
      bt->SetOnClick([this]() {
        if (disabled) return;
        const std::string currentValue = getValue();
        if (onClick) onClick(currentValue);
        if (type == "submit")
        {
          bool submitted = true;
          if (auto* form = glint_form::nearestFor(this))
            submitted = form->submit(this);
          if (submitted && onSubmit)
            onSubmit(currentValue);
        }
        else if (type == "reset")
        {
          if (auto* form = glint_form::nearestFor(this))
            form->reset();
        }
      });
      addChild(bt);
      applyAttachedUserAgentStyle(bt);
      mButton = bt;
      mFocusPending = false;
    }
    else if (type == "checkbox")
    {
      auto* cb           = new glint_checkbox();
      cb->onChange = [this](bool v)
      {
        checked = v;
        if (onCheck)  onCheck(v);
        if (onChange) onChange(v ? "true" : "false");
      };
      addChild(cb);
      applyAttachedUserAgentStyle(cb);
      mCheckbox = cb;
    }
    else if (type == "radio")
    {
      auto* r           = new glint_radio();
      r->onChange = [this](bool v)
      {
        checked = v;
        if (onCheck)  onCheck(v);
        if (onChange) onChange(this->value);
      };
      addChild(r);
      applyAttachedUserAgentStyle(r);
      mRadio = r;
    }
    else if (type == "range")
    {
      auto* sl           = new glint_slider();
      sl->onChange = [this](float v)
      {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%g", v);
        if (onChange) onChange(std::string(buf));
      };
      addChild(sl);
      applyAttachedUserAgentStyle(sl);
      mSlider = sl;
      mSlider->SetValue(mInitialFloatValue);
    }
    else
    {
      auto* ti            = new glint_text_input();
      ti->onChange  = [this](const std::string& v) { if (onChange)  onChange(v);  };
      ti->onSubmit  = [this](const std::string& v) {
        bool submitted = true;
        if (auto* form = glint_form::nearestFor(this))
          submitted = form->submit(this);
        if (submitted && onSubmit)
          onSubmit(v);
      };
      ti->onKeyDown = [this](const glint_key_press& k) -> bool
                      { return onKeyDown ? onKeyDown(k) : false; };
      ti->onFocus   = [this]() { if (onFocus) onFocus(); };
      ti->onBlur    = [this]() { if (onBlur)  onBlur();  };
      addChild(ti);
      applyAttachedUserAgentStyle(ti);
      mTextInput = ti;
      if (!mPendingValue.empty()) mTextInput->setValue(mPendingValue);
      // If SetFocus(shell) was called before the delegate existed, forward now.
      if (mFocusPending && mRoot)
      {
        mFocusPending = false;
        mRoot->SetFocus(mTextInput);
      }
    }
    mActiveDelegateKind = _delegateKindForType(type);
  }

  void _syncDelegateProps()
  {
    const bool isHiddenType = (type == "hidden");

    if (isHiddenType)
    {
      if (!mHiddenDisplayApplied)
      {
        mDisplayBeforeHidden = style.display;
        mHiddenDisplayApplied = true;
      }
      style.display = "none";

      if (mRoot && (mRoot->getFocusedNode() == mTextInput || mRoot->getFocusedNode() == this))
        mRoot->SetFocus(nullptr);
    }
    else if (mHiddenDisplayApplied)
    {
      style.display = mDisplayBeforeHidden;
      mHiddenDisplayApplied = false;
    }

    if (disabled)
    {
      if (!mDisabledOpacityApplied)
      {
        mEnabledOpacity = style.opacity;
        mDisabledOpacityApplied = true;
      }
      style.opacity = mEnabledOpacity * 0.5f;

      if (mRoot && (mRoot->getFocusedNode() == mTextInput || mRoot->getFocusedNode() == this))
        mRoot->SetFocus(nullptr);
    }
    else if (mDisabledOpacityApplied)
    {
      style.opacity = mEnabledOpacity;
      mDisabledOpacityApplied = false;
    }

    if (mTextInput)
    {
      mTextInput->mAcceptsFocus = !disabled && !isHiddenType;
      mTextInput->mTabStop      = !disabled && !isHiddenType;
      mTextInput->type        = type;
      mTextInput->inputmode   = inputmode;
      mTextInput->enterkeyhint = enterkeyhint;
      mTextInput->autocomplete = autocomplete;
      mTextInput->autocapitalize = autocapitalize;
      mTextInput->spellcheck  = spellcheck;
      mTextInput->maxlength   = maxlength;
      mTextInput->minlength   = minlength;
      mTextInput->required    = required;
      mTextInput->multiple    = multiple;
      mTextInput->pattern     = pattern;
      mTextInput->min         = min;
      mTextInput->max         = max;
      mTextInput->step        = step;
      mTextInput->placeholder = placeholder;
      mTextInput->readonly    = readonly;
      mTextInput->disabled    = disabled;
    }
    if (mButton)
    {
      mButton->innerText = _resolvedButtonLabel();
      mButton->style.userSelect = "none";
      mButton->style.textAlign  = EAlign::Center;
    }
    if (mSlider)
    {
      mSlider->min  = min;
      mSlider->max  = max;
      mSlider->step = step;
    }
    if (mCheckbox)
    {
      mCheckbox->checked = checked;
      mCheckbox->text    = text;
      mCheckbox->size    = checkSize;
    }
    if (mRadio)
    {
      mRadio->checked = checked;
      mRadio->text    = text;
      mRadio->size    = checkSize;
      mRadio->value   = value;
      mRadio->group   = group;
    }
  }
};

namespace { struct _glint_input_reg { _glint_input_reg() { glint_element::registerElement("input", []{ return new glint_input(); }); } } _glint_input_reg_; }
