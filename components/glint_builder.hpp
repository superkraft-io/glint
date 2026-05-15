#pragma once

/**
 * glint_builder.hpp
 * Declarative / callback-driven builder for glint.
 *
 * Usage:
 *
 *   glint_ctx _c(parentElement, parentElement.GetUI(), parentElement.root());
 *
 *     _c.add.div([](auto& _c) {
 *       _c.bounds                = {648.f, 14.f, 710.f, 38.f};
 *       _c.innerText                  = "32ms";
 *       _c.tag                   = kCtrlTagLatencyBadge;
 *       _c.style.backgroundColor = C_GREEN_BADGE;
 *       _c.style.borderRadius    = 12.f;
 *       _c.fontSize              = 14.f;
 *       _c.fontFace              = "Kanit-Regular";
 *     });
 *
 *     _c.add.button([](sk_ui_button_style& _c) {
 *       _c.bounds                = {10.f, 10.f, 110.f, 40.f};
 *       _c.innerText             = "Click";
 *       _c.style.backgroundColor = "#2a2a2a";
 *       _c.style.borderRadius    = 8.f;
 *       _c.onClick               = []{ DBGMSG("clicked\n"); };
 *     });
 *
 *     // Custom scene-graph component:
 *     _c.add.attach(new VCHeaderControl({0.f, 0.f, 738.f, 52.f}, logoBmp), kCtrlTagHeader);
 *
 *     // External user component with builder-style setup:
 *     _c.add.fromClass<MyUserControl>([](MyUserControl& _c) {
 *       _c.style.width  = 220.f;
 *       _c.style.height = 40.f;
 *       _c.title = "Hello";
 *     });
 *
 *     // Inline custom-draw component (no subclass needed):
 *     _c.add.component([](glint_component_style& _c) {
 *       _c.bounds = {0.f, 208.f, 738.f, 230.f};
 *       _c.tag    = kCtrlTagFooter;
 *       _c.style  = glint_style::Filled(C_HEADER_BG);
 *       _c.draw   = [](glint_canvas& g, const glint_rect& r) {
 *         g.DrawLine(C_BORDER, r.L, r.T, r.R, r.T, nullptr, 1.f);
 *       };
 *     });
 *   });
 */

#include "../glint_element.hpp"
#include "../glint_document.hpp"
// glint_element/button/checkbox/svg/img are included AFTER ComponentAdd::component
// is defined � see below.

#include <functional>

// Returns true when a raw length value means "shrink-wrap to content".
// Matches both the canonical CSS keyword (fit-content), the CSS shorthand
// for flex items (auto ? intrinsic size), and the empty/unset state.
static inline bool sk_is_fit_content(const std::string& raw)
{
  return raw.empty() || raw == "fit-content" || raw == "auto";
}

// Forward declaration so glint_component_style can reference glint_ctx
struct glint_ctx;
class glint_colorpicker;
class glint_dial;
class glint_gradient_editor;
class glint_list;

// --- glint_div ---------------------------------------------------------------
// A plain glint_element whose drawContent is supplied via a lambda.
// Useful for one-off controls that don't warrant a full subclass.

class glint_div : public glint_element
{
public:
  using DrawFn = std::function<void(glint_canvas&, const glint_rect&)>;

  glint_div(const glint_rect& bounds,
              glint_style  panelStyle,
              DrawFn       draw,
              int          /*paramIdx*/ = glint_no_val_idx)
    : mDraw(std::move(draw))
  {
    mRect = mPaintRECT = bounds;
    style = panelStyle;
  }

protected:
  void drawContent(glint_canvas& g) override
  {
    glint_element::drawContent(g);  // renders innerText (base class)
    if (mDraw) mDraw(g, getContent());
  }

  void DrawContentToCanvas(SkCanvas* canvas) override
  {
    glint_element::DrawContentToCanvas(canvas);  // renders innerText (base class)
    if (mDraw && canvas)
    {
      glint_canvas tempG(canvas);
      mDraw(tempG, getContent());
    }
  }

private:
  DrawFn mDraw;
};

// --- glint_component_style --------------------------------------------------
// Config struct for _c.add.component() � all settings live inside the callback.
//
// Children can be added directly inside the component setup callback via _c.add:
//
//   _c.add.component([](glint_component_style& _c) {
//     _c.left = 10; _c.top = 10; _c.width = 200; _c.height = 50;
//     _c.style.backgroundColor = "#222222";
//     _c.add.div([](auto& _c) {   // relative to component origin
//       _c.left = 4; _c.top = 4; _c.width = 100; _c.height = 20;
//       _c.innerText = "Hello";
//     });
//   });
//
// Method bodies for glint_component_adder are defined after glint_ctx is complete.

class glint_select;    // forward-declared early so glint_component_adder can reference it
class glint_slider;    // forward-declared early so glint_component_adder can reference it
class glint_tree;      // forward-declared early so glint_component_adder can reference it

struct glint_component_style
{
  glint_style         style;
  glint_div::DrawFn draw;
  int                 tag = glint_no_tag;

  // CSS-style layout shorthand � identical semantics to the `align` member on
  // glint_element. Tokens are applied to `style` before the panel is created.
  // Example:  _c.align = "left middle";  // flex-start + center cross-axis
  std::string align = "";

  // Proxy for element.id � mirrors the live node's element.id so you can write
  //   _c.element.id = "header";
  // inside an add.component() callback. The id is applied to the real
  // glint_element::element.id after the panel is created.
  struct { std::string id; } element;

  // Text content — copied to glint_element::innerText after the element is created.
  std::string innerText;

  // CSS class list — applied to glint_element::className after the element is created.
  // Mirrors el->classList: add/remove/toggle/contains work identically to JS.
  std::string className;

  // Build-time classList proxy.  Manipulates className above — no redraw needed
  // since the element doesn't exist yet at builder time.
  struct glint_build_class_list
  {
    glint_component_style* _cs = nullptr;

    bool contains(const std::string& cls) const
    {
      if (!_cs || cls.empty()) return false;
      std::istringstream ss(_cs->className);
      std::string tok;
      while (ss >> tok) if (tok == cls) return true;
      return false;
    }
    void add(const std::string& cls)
    {
      if (!_cs || cls.empty() || contains(cls)) return;
      if (!_cs->className.empty()) _cs->className += ' ';
      _cs->className += cls;
    }
    void remove(const std::string& cls)
    {
      if (!_cs || cls.empty()) return;
      std::string result;
      std::istringstream ss(_cs->className);
      std::string tok;
      while (ss >> tok) { if (tok == cls) continue; if (!result.empty()) result += ' '; result += tok; }
      _cs->className = std::move(result);
    }
    bool toggle(const std::string& cls)
    {
      if (contains(cls)) { remove(cls); return false; }
      add(cls); return true;
    }
  } classList;

  // Deferred event listeners — queued during the setup callback and flushed
  // onto the real element after it is created. Mirrors the DOM pattern where
  // addEventListener() is called inline during element construction.
  struct _ListenerEntry {
    std::string                           type;
    std::function<void(glint_event&)>     handler;
  };
  std::vector<_ListenerEntry> _eventListeners;

  // Queue a listener to be attached to the real element when it is created.
  // Identical call-site syntax to glint_element::addEventListener().
  void addEventListener(const std::string& type, std::function<void(glint_event&)> handler)
  {
    _eventListeners.push_back({ type, std::move(handler) });
  }

  // Width and height default to "" (empty = intrinsic / fit-content).
  // We do NOT bake "fit-content" into the inline style layer here — leaving it
  // empty means CSS stylesheets can override width/height (inline wins only when
  // the user explicitly assigns a value in their setup callback).
  // The layout engine treats "" identically to "fit-content" and "auto".
  glint_component_style()
  {
    classList._cs = this;
  }

  // Copy constructor and assignment: keep classList._cs pointing at *this*.
  glint_component_style(const glint_component_style& o)
    : style(o.style), draw(o.draw), tag(o.tag), align(o.align),
      element(o.element), innerText(o.innerText), className(o.className),
      _eventListeners(o._eventListeners), add(o.add)
  { classList._cs = this; }

  glint_component_style& operator=(const glint_component_style& o)
  {
    if (this == &o) return *this;
    style = o.style; draw = o.draw; tag = o.tag; align = o.align;
    element = o.element; innerText = o.innerText; className = o.className;
    _eventListeners = o._eventListeners; add = o.add;
    classList._cs = this;
    return *this;
  }

  // -- Inline child adder ----------------------------------------------------
  // Records children added directly in the setup callback (_c.add.div etc.).
  // Replayed by glint_adder::component() with component's resolved origin as offset.
  // Method bodies are defined inline below, after glint_ctx is complete.
  struct glint_component_adder
  {
    // Each entry pairs the create lambda with a measure lambda.
    // The measure lambda runs the user's setup on a temporary struct and
    // returns its glint_style so the flex algorithm can read width/height/margin
    // without constructing any host-owned control.
    struct OpEntry {
      std::function<void(glint_ctx&)>        create;   // create the real control
      std::function<glint_style(glint_canvas*, glint_element*)> measure;  // returns child style for flex; g may be null
    };
    std::vector<OpEntry> _ops;

    void button      (std::function<void(glint_button&)>            s, glint_button**      out = nullptr);
    void img       (std::function<void(glint_image&)>             s, glint_image**       out = nullptr);
    void input       (std::function<void(glint_input&)>             s, glint_input**       out = nullptr);
    template<typename T, typename Setup>
    void custom(Setup&& setup, T** out = nullptr);
    template<typename T, typename Setup>
    void fromClass(Setup&& setup, T** out = nullptr)
    {
      custom<T>(std::forward<Setup>(setup), out);
    }
    void colorpicker (std::function<void(glint_colorpicker&)>       s, glint_colorpicker** out = nullptr);
    void dial        (std::function<void(glint_dial&)>              s, glint_dial**        out = nullptr);
    void gradientEditor(std::function<void(glint_gradient_editor&)> s, glint_gradient_editor** out = nullptr);
    void list        (std::function<void(glint_list&)>              s, glint_list**        out = nullptr);
    void select      (std::function<void(glint_select&)>            s, glint_select**      out = nullptr);
    void slider      (std::function<void(glint_slider&)>            s, glint_slider**      out = nullptr);
    void tree        (std::function<void(glint_tree&)>              s, glint_tree**        out = nullptr);
    void div             (std::function<void(glint_component_style&)>      s);
    void div             (std::function<void(glint_component_style&)>      s, glint_element** out);
    void component      (std::function<void(glint_component_style&)>      s);  // backward-compat ? div()
    void spacer   ();    // flex-grow: 1 � fills remaining main-axis space
    template<typename T> void attach(T* ctrl, int tag = glint_no_tag);

    // Generic deferred construction for app-specific scene-graph subclasses.
    // Factory: (const glint_rect&) -> glint_element*
    template<typename Factory>
    void make(float w, float h, Factory factory, int tag = glint_no_tag);
  } add;
};

// --- Forward declarations ----------------------------------------------------
// Needed so glint_adder can declare its methods before the component headers
// are included. Inline bodies are defined after the includes, below.
class glint_button;
class glint_switch;
class glint_image;
class glint_input;
class glint_colorpicker;
class glint_dial;
class glint_gradient_editor;
class glint_list;
// glint_image_style is an alias of glint_image
// (defined at the bottom of its header).

// --- glint_adder -------------------------------------------------------------
// Exposed as _c.add inside glint_body and created by ComponentAdd methods.
// In the scene graph, nodes are added to mParent->mChildren via addChild()
// instead of being attached directly to a host bridge.

struct glint_adder
{
  // Scene-graph parent � all new nodes become children of this component.
  glint_element* mParent = nullptr;

  // Convenience aliases derived from mParent (or set explicitly for flex children).
  glint_canvas* mpG    = nullptr;
  float mOffsetX    = 0.f;   // accumulated parent origin X (window-space)
  float mOffsetY    = 0.f;   // accumulated parent origin Y
  float mParentW    = 0.f;   // parent content width  � for % resolution
  float mParentH    = 0.f;   // parent content height

  // Construct from a parent node (top-level usage in glint_body / ComponentAdd).
  explicit glint_adder(glint_element* parent)
    : mParent(parent)
    , mpG(parent ? parent->mpG : nullptr)
    , mOffsetX(parent ? parent->GetPaintRECT().L : 0.f)
    , mOffsetY(parent ? parent->GetPaintRECT().T : 0.f)
    , mParentW(parent ? parent->GetPaintRECT().W() : 0.f)
    , mParentH(parent ? parent->GetPaintRECT().H() : 0.f)
  {}

  // Construct from a parent node with explicit offsets � used by the flex
  // layout engine to position children at computed window-space coordinates.
  glint_adder(glint_element* parent, glint_canvas* g,
              float ox, float oy, float pw, float ph)
    : mParent(parent), mpG(g)
    , mOffsetX(ox), mOffsetY(oy), mParentW(pw), mParentH(ph)
  {}

  // Resolve style.left/top/width/height to an absolute window glint_rect.
  // Percentages are relative to the parent container's dimensions.
  // position:"absolute" uses raw coords without adding the parent offset.
  glint_rect resolve(const glint_style& s) const
  {
    const bool  abs = (s.position == "absolute");
    const float l   = s.left.resolve(mParentW);
    const float t   = s.top.resolve(mParentH);
    const float w   = s.width.resolve(mParentW);
    const float h   = s.height.resolve(mParentH);
    const float ax  = abs ? l : l + mOffsetX;
    const float ay  = abs ? t : t + mOffsetY;
    return glint_rect(ax, ay, ax + w, ay + h);
  }

  // -- dispatchChild ---------------------------------------------------------
  // Core scene-graph attach: adds node to mParent, registers its tag.
  // If pw/ph are > 0 they are pre-stamped as the child's parent dimensions
  // (used by the flex engine to pass content-area size rather than panel size).
  void dispatchChild(glint_element* ctrl, int tag,
                     float pw = 0.f, float ph = 0.f) const
  {
    ctrl->mTag = tag;
    if (pw > 0.f) { ctrl->mParentW = pw; ctrl->mParentH = ph; }
    mParent->addChild(ctrl);  // stamps mpG/mRoot/mRequestRedraw and finalizes if live
    if (ctrl->mRoot && tag != glint_no_tag)
      ctrl->mRoot->RegisterTag(tag, ctrl);
  }

  // -- attach -- any glint_element subclass (scene-graph node) -------------
  // Standalone/native code attaches only scene-graph nodes here.
  template<typename T>
  T* attach(T* ctrl, int tag = glint_no_tag) const
  {
    static_assert(std::is_base_of_v<glint_element, T>,
                  "glint_adder::attach only accepts glint_element-derived types in standalone mode");
    dispatchChild(ctrl, tag);
    return ctrl;
  }

  // -- button / img ----------------------------------------
  // Declared here; bodies defined after the component includes below.
  glint_button*        button      (std::function<void(glint_button&     )> setup) const;
  glint_image*         img       (std::function<void(glint_image&)> setup) const;
  glint_input*         input       (std::function<void(glint_input&      )> setup) const;
  template<typename T, typename Setup>
  T*                  custom      (Setup&& setup) const;
  template<typename T, typename Setup>
  T*                  fromClass   (Setup&& setup) const
  {
    return custom<T>(std::forward<Setup>(setup));
  }
  glint_colorpicker*   colorpicker (std::function<void(glint_colorpicker&)> setup) const;
  glint_dial*          dial        (std::function<void(glint_dial&)> setup) const;
  glint_gradient_editor* gradientEditor(std::function<void(glint_gradient_editor&)> setup) const;
  glint_list*          list        (std::function<void(glint_list&)> setup) const;
  glint_select*        select      (std::function<void(glint_select&)> setup) const;
  glint_slider*        slider      (std::function<void(glint_slider&)> setup) const;
  glint_tree*          tree        (std::function<void(glint_tree&)> setup) const;
  glint_element*       spacer() const;   // flex-grow:1 invisible node � shows in inspector

  // -- div -- generic container (HTML <div>); children relative to div's origin ----------------------------------
  glint_div* div      (std::function<void(glint_component_style&)> setup) const;
  glint_div* component(std::function<void(glint_component_style&)> setup) const;  // backward-compat ? div()
};

// --- glint_ctx ----------------------------------------------------------------
// Context object handed to the glint_body callback and to panel children.
// Access all factory methods via the public `add` member.

struct glint_ctx
{
  glint_adder add;

  // Construct from a parent node (normal usage).
  explicit glint_ctx(glint_element* parent)
    : add{parent} {}

  // Construct with explicit offsets for flex child contexts.
  glint_ctx(glint_element* parent, glint_canvas* g,
            float ox, float oy, float pw, float ph)
    : add{parent, g, ox, oy, pw, ph} {}
};

// --- glint_adder::component -- defined here so glint_ctx is complete --------
inline glint_style glint_builder_measure_component_style(glint_component_style s, glint_canvas* g,
                                                       glint_element* parent)
{
  if (!s.align.empty()) glint_style::ApplyAlign(s.align, s.style);

  glint_element probe;
  probe.mpG            = g;
  probe.mParent        = parent;
  probe.mRoot          = parent ? parent->mRoot : nullptr;
  probe.mRequestRedraw = parent ? parent->mRequestRedraw : std::function<void()>{};
  probe.mApplyCss      = parent ? parent->mApplyCss : std::function<void(glint_element*)>{};
  probe.style          = s.style;
  probe.computedStyle  = s.style;
  probe.className      = s.className;
  probe.innerText      = s.innerText;
  probe.element.id     = s.element.id;
  if (parent)
  {
    probe.mParentW = parent->getContent().W();
    probe.mParentH = parent->getContent().H();
  }
  probe.finalizeTreeState();
  return probe.computedStyle;
}

inline glint_div* glint_adder::div(std::function<void(glint_component_style&)> setup) const
{
  glint_component_style s;
  if (setup) setup(s);

  // Apply the align shorthand to style before any sizing or resolve.
  if (!s.align.empty()) glint_style::ApplyAlign(s.align, s.style);

  // -- Single measure pass ---------------------------------------------------
  // Run op.measure() ONCE per child and cache the results. These are reused
  // for both intrinsic sizing and flex layout so text measurement (MeasureText)
  // is never called twice for the same child, avoiding any size discrepancy.
  std::vector<glint_style> measured;
  if (!s.add._ops.empty())
  {
    measured.reserve(s.add._ops.size());
    for (auto& op : s.add._ops)
      measured.push_back(op.measure(mpG, mParent));
  }

  // -- Intrinsic ("fit-content") sizing ----------------------------------------
  const bool autoW = sk_is_fit_content(s.style.width.raw);
  const bool autoH = sk_is_fit_content(s.style.height.raw);
  if ((autoW || autoH) && !measured.empty())
  {
    const float pl = s.style.paddingLeft,  pr = s.style.paddingRight;
    const float pt = s.style.paddingTop,   pb = s.style.paddingBottom;

    if (s.style.display == "flex")
    {
      const bool  isRow = (s.style.flexDirection != "column");
      const float gap   = s.style.gap.toFloat();
      float totalMain = 0.f, maxCross = 0.f;
      bool first = true;
      for (const auto& cs : measured)
      {
        const float main  = isRow
          ? (cs.marginLeft + cs.width.toFloat()  + cs.marginRight)
          : (cs.marginTop  + cs.height.toFloat() + cs.marginBottom);
        const float cross = isRow ? cs.height.toFloat() : cs.width.toFloat();
        if (!first) totalMain += gap;
        totalMain += main;
        if (cross > maxCross) maxCross = cross;
        first = false;
      }
      if (autoW) s.style.width  = isRow ? (pl + totalMain + pr) : (pl + maxCross + pr);
      if (autoH) s.style.height = isRow ? (pt + maxCross + pb)  : (pt + totalMain + pb);
    }
    else
    {
      float maxRight = 0.f, maxBottom = 0.f;
      const float knownW = autoW ? 0.f : s.style.width.resolve(mParentW);
      const float knownH = autoH ? 0.f : s.style.height.resolve(mParentH);
      for (const auto& cs : measured)
      {
        const float r2 = cs.left.resolve(knownW) + cs.width.resolve(knownW);
        const float b2 = cs.top.resolve(knownH)  + cs.height.resolve(knownH);
        if (r2 > maxRight)  maxRight  = r2;
        if (b2 > maxBottom) maxBottom = b2;
      }
      if (autoW) s.style.width  = maxRight  + pr;
      if (autoH) s.style.height = maxBottom + pb;
    }
  }
  // fit-content from innerText alone (no child ops):
  // Bake the text size into s.style so resolve() gives the right initial mRect
  // and parent containers measure the correct child size at build time.
  // After ctrl is created, reset style.width/height to "fit-content" so every
  // runtime Layout pass calls preferredW()/preferredH() on the live innerText.
  else if ((autoW || autoH) && !s.innerText.empty())
  {
        const float pl = s.style.paddingLeft, pr = s.style.paddingRight;
        const float pt = s.style.paddingTop,  pb = s.style.paddingBottom;
        const float sz = s.style.fontSize.toFloat() > 0.f ? s.style.fontSize.toFloat() : 12.f;
        if (autoW)
        {
          SkFont font = glint_element::skFont(sz,
                                             s.style.fontFamily.c_str(),
                                             s.style.fontWeight,
                                             s.style.fontStyle.c_str());
          float maxW = 0.f;
          std::size_t pos = 0;
          while (pos <= s.innerText.size())
          {
            const std::size_t end     = s.innerText.find('\n', pos);
            const std::size_t lineEnd = (end == std::string::npos) ? s.innerText.size() : end;
            if (lineEnd > pos) { SkRect b; const float adv = font.measureText(s.innerText.c_str() + pos, lineEnd - pos, SkTextEncoding::kUTF8, &b); maxW = std::max(maxW, adv); }
            if (end == std::string::npos) break;
            pos = end + 1;
          }
          s.style.width = pl + maxW + 4.f + pr;
        }
        if (autoH)
        {
          int lines = 1; for (char c : s.innerText) if (c == '\n') ++lines;
          SkFont _fnt = glint_element::skFont(sz, s.style.fontFamily.c_str(), s.style.fontWeight, s.style.fontStyle.c_str());
          SkFontMetrics _fm; _fnt.getMetrics(&_fm);
          const float lh = s.style.lineHeight > 0.f ? sz * s.style.lineHeight : (-_fm.fAscent + _fm.fDescent);
          const std::size_t _fnl = s.innerText.find('\n');
          const std::size_t _fll = (_fnl == std::string::npos) ? s.innerText.size() : _fnl;
          SkRect _inkB; _fnt.measureText(s.innerText.c_str(), _fll, SkTextEncoding::kUTF8, &_inkB);
          s.style.height = pt + _inkB.height() + static_cast<float>(lines - 1) * lh + pb;
        }
      }

      const glint_rect r    = resolve(s.style);
      auto        draw = std::move(s.draw);
      auto*       ctrl = new glint_div(r, s.style, std::move(draw));
      // Reset auto-sized axes to "" (empty = intrinsic) so runtime Layout() re-measures
      // live innerText. We use "" instead of "fit-content" so CSS can still override
      // width/height when a stylesheet assigns an explicit value to this element.
      if (autoW) ctrl->style.width.raw  = "";
      if (autoH) ctrl->style.height.raw = "";
      if (!s.element.id.empty()) ctrl->element.id = s.element.id;
      if (!s.innerText.empty())  ctrl->innerText   = s.innerText;
      if (!s.className.empty())  ctrl->className   = s.className;
      for (auto& le : s._eventListeners)
        ctrl->addEventListener(le.type, le.handler);
      dispatchChild(ctrl, s.tag);
  if (s.add._ops.empty()) return ctrl;

  // -- Flex layout ------------------------------------------------------------
  if (s.style.display == "flex")
  {
    const bool  isRow     = (s.style.flexDirection != "column");
    const float pl = s.style.paddingLeft,  pr = s.style.paddingRight;
    const float pt = s.style.paddingTop,   pb = s.style.paddingBottom;
    const float contentW  = r.W() - pl - pr;
    const float contentH  = r.H() - pt - pb;
    const float mainAvail = isRow ? contentW : contentH;
    const float crsAvail  = isRow ? contentH : contentW;
    const float gap       = s.style.gap.toFloat();

    // -- Build ChildInfo from the already-cached measure results -----------
    struct ChildInfo { float main, cross, m1, m2, c1, c2, grow, growExtra; };
    std::vector<ChildInfo> infos;
    infos.reserve(measured.size());
    for (const auto& cs : measured)
    {
      const float w = cs.width.resolve(contentW);
      const float h = cs.height.resolve(contentH);
      ChildInfo ci;
      if (isRow) {
        ci = { w, h,
               cs.marginLeft, cs.marginRight,
               cs.marginTop,  cs.marginBottom,
               cs.flexGrow,   0.f };
      } else {
        ci = { h, w,
               cs.marginTop,  cs.marginBottom,
               cs.marginLeft, cs.marginRight,
               cs.flexGrow,   0.f };
      }
      infos.push_back(ci);
    }

    // -- Total main-axis size of all items (incl. their margins + gap) -----
    float totalMain = 0.f;
    for (auto& ci : infos)
      totalMain += ci.m1 + ci.main + ci.m2;
    if (!infos.empty())
      totalMain += gap * static_cast<float>(infos.size() - 1);

    // -- flex-grow: distribute remaining space to growing items -------------
    float totalGrow = 0.f;
    for (const auto& ci : infos) totalGrow += ci.grow;
    if (totalGrow > 0.f)
    {
      const float freeSpace = std::max(0.f, mainAvail - totalMain);
      for (auto& ci : infos)
        ci.growExtra = ci.grow / totalGrow * freeSpace;
    }

    // -- justify-content ? cursor start & per-item spacing -----------------
    // Skipped when flex-grow items are present — they absorb all free space.
    const float originMain = isRow ? (r.L + pl) : (r.T + pt);
    const float originCrs  = isRow ? (r.T + pt) : (r.L + pl);
    float cursor      = originMain;
    float itemSpacing = gap;

    if (totalGrow == 0.f)
    {
      const auto& jc = s.style.justifyContent;
      const auto  n  = static_cast<float>(infos.size());
      if (jc == "center")
        cursor = originMain + (mainAvail - totalMain) * 0.5f;
      else if (jc == "flex-end")
        cursor = originMain + mainAvail - totalMain;
      else if (jc == "space-between" && infos.size() > 1)
        itemSpacing = (mainAvail - totalMain + gap * (n - 1)) / (n - 1);
      else if (jc == "space-around") {
        const float extra = (mainAvail - totalMain) / n;
        cursor      = originMain + extra * 0.5f;
        itemSpacing = gap + extra;
      } else if (jc == "space-evenly") {
        const float extra = (mainAvail - totalMain) / (n + 1.f);
        cursor      = originMain + extra;
        itemSpacing = gap + extra;
      }
    }

    // -- Create pass: build controls at flex-computed positions -------------
    const auto& ai = s.style.alignItems;
    for (size_t i = 0; i < s.add._ops.size(); ++i)
    {
      const auto& ci = infos[i];
      cursor += ci.m1;

      // cross-axis position
      float crossPos = originCrs + ci.c1;  // flex-start
      if (ai == "center")
        crossPos = originCrs + (crsAvail - ci.cross) * 0.5f;
      else if (ai == "flex-end")
        crossPos = originCrs + crsAvail - ci.cross - ci.c2;
      // "stretch": crossPos stays at flex-start; caller should set full cross size

      const float absX = isRow ? cursor   : crossPos;
      const float absY = isRow ? crossPos : cursor;
      // child style.left/top default to 0 and are relative to this new origin
      glint_ctx childCtx(ctrl, mpG, absX, absY, contentW, contentH);
      s.add._ops[i].create(childCtx);

      cursor += ci.main + ci.growExtra + ci.m2 + itemSpacing;
    }
  }
  else
  {
    // -- Normal relative layout: children use their own left/top -------------
    glint_ctx childCtx(ctrl, mpG, r.L, r.T, r.W(), r.H());
    for (auto& op : s.add._ops) op.create(childCtx);
  }
  return ctrl;
}
inline glint_div* glint_adder::component(std::function<void(glint_component_style&)> setup) const
{ return div(std::move(setup)); } // backward-compat ? div()

// --- ComponentAdd::div body ----------------------------------------------------
// Defined here before the deferred component includes.
template<typename S>
inline glint_element* glint_element::ComponentAdd::div(S&& setup, glint_element** out)
{
  // Skip block-flow cursor injection when the parent is a flex/grid container.
  const bool skipCursor = (_owner->cssStyle_.display == "flex" ||
                           _owner->cssStyle_.display == "grid"  ||
                           _owner->style.display    == "flex"   ||
                           _owner->style.display    == "grid");
  glint_component_style peek;
  const bool isAbs = sk_inject_cursor(peek, std::forward<S>(setup), mCursorY, skipCursor);
  glint_adder a{_owner};
  auto* ctrl = a.div([p = std::move(peek)](glint_component_style& s){ s = p; });
  if (!skipCursor && !isAbs && ctrl) mCursorY = sk_builder_flow_bottom_for_child(_owner, ctrl, mCursorY);
  if (out) *out = ctrl;
  return ctrl;
}
template<typename S>
inline glint_element* glint_element::ComponentAdd::component(S&& setup, glint_element** out)
{ return div(std::forward<S>(setup), out); } // backward-compat ? div()

// --- Component includes (deferred so ComponentAdd::div is already defined) --
#include "glint_button.hpp"
#include "glint_image.hpp"
#include "glint_text_editor_base.hpp"
#include "glint_input.hpp"
#include "glint_colorpicker.hpp"
#include "glint_dial.hpp"
#include "glint_gradient_editor.hpp"
#include "glint_list/glint_list.hpp"
#include "glint_tooltip.hpp"
#include "glint_textarea.hpp"
#include "glint_progress.hpp"
#include "glint_datepicker.hpp"
#include "glint_datepicker_window.hpp"
#include "glint_date_input.hpp"
#include "glint_checkbox.hpp"
#include "glint_select.hpp"
#include "glint_tree.hpp"

inline glint_style glint_builder_initial_style(glint_element* ctrl)
{
  return ctrl ? ctrl->mergedStyleForLayout() : glint_style{};
}

// --- glint_adder method bodies (defined after component includes) -------------
inline glint_button* glint_adder::button(std::function<void(glint_button&)> setup) const
{
  auto* ctrl = new glint_button();
  if (setup) setup(*ctrl);
  ctrl->computedStyle = glint_builder_initial_style(ctrl);
  ctrl->mRect = ctrl->mPaintRECT = resolve(ctrl->computedStyle);
  dispatchChild(ctrl, ctrl->tag);
  return ctrl;
}

inline glint_input* glint_adder::input(std::function<void(glint_input&)> setup) const
{
  auto* ctrl = new glint_input();
  if (setup) setup(*ctrl);
  ctrl->computedStyle = glint_builder_initial_style(ctrl);
  ctrl->mRect = ctrl->mPaintRECT = resolve(ctrl->computedStyle);
  dispatchChild(ctrl, ctrl->tag);
  return ctrl;
}

template<typename T, typename Setup>
inline T* glint_adder::custom(Setup&& setup) const
{
  static_assert(std::is_base_of_v<glint_element, T>,
                "glint_adder::custom only accepts glint_element-derived types in standalone mode");
  static_assert(std::is_default_constructible_v<T>,
                "glint_adder::custom requires a default-constructible control; use add.make(...) for bounds-constructed controls");
  auto* ctrl = new T();
  std::forward<Setup>(setup)(*ctrl);
  // Measure pass: mirror Chromium's pre-layout sync so fit-content components
  // report their natural size at build time — no explicit row height needed.
  ctrl->syncBeforeLayout();
  ctrl->computedStyle = glint_builder_initial_style(ctrl);
  if (sk_is_fit_content(ctrl->computedStyle.height.raw)) {
    const float h = ctrl->preferredH(mParentW);
    if (h > 0.f) ctrl->computedStyle.height = h;
  }
  if (sk_is_fit_content(ctrl->computedStyle.width.raw)) {
    const float w = ctrl->preferredW();
    if (w > 0.f) ctrl->computedStyle.width = w;
  }
  ctrl->mRect = ctrl->mPaintRECT = resolve(ctrl->computedStyle);
  dispatchChild(ctrl, ctrl->tag);
  return ctrl;
}

inline glint_colorpicker* glint_adder::colorpicker(std::function<void(glint_colorpicker&)> setup) const
{
  auto* ctrl = new glint_colorpicker();
  if (setup) setup(*ctrl);
  ctrl->computedStyle = glint_builder_initial_style(ctrl);
  ctrl->mRect = ctrl->mPaintRECT = resolve(ctrl->computedStyle);
  dispatchChild(ctrl, ctrl->tag);
  return ctrl;
}

inline glint_dial* glint_adder::dial(std::function<void(glint_dial&)> setup) const
{
  auto* ctrl = new glint_dial();
  if (setup) setup(*ctrl);
  ctrl->computedStyle = glint_builder_initial_style(ctrl);
  ctrl->mRect = ctrl->mPaintRECT = resolve(ctrl->computedStyle);
  dispatchChild(ctrl, ctrl->tag);
  return ctrl;
}

inline glint_gradient_editor* glint_adder::gradientEditor(std::function<void(glint_gradient_editor&)> setup) const
{
  auto* ctrl = new glint_gradient_editor();
  if (setup) setup(*ctrl);
  ctrl->computedStyle = glint_builder_initial_style(ctrl);
  ctrl->mRect = ctrl->mPaintRECT = resolve(ctrl->computedStyle);
  dispatchChild(ctrl, ctrl->tag);
  return ctrl;
}

inline glint_list* glint_adder::list(std::function<void(glint_list&)> setup) const
{
  auto* ctrl = new glint_list();
  if (setup) setup(*ctrl);
  ctrl->computedStyle = glint_builder_initial_style(ctrl);
  ctrl->mRect = ctrl->mPaintRECT = resolve(ctrl->computedStyle);
  dispatchChild(ctrl, ctrl->tag);
  return ctrl;
}

inline glint_select* glint_adder::select(std::function<void(glint_select&)> setup) const
{
  auto* ctrl = new glint_select();
  if (setup) setup(*ctrl);
  ctrl->computedStyle = glint_builder_initial_style(ctrl);
  ctrl->mRect = ctrl->mPaintRECT = resolve(ctrl->computedStyle);
  dispatchChild(ctrl, glint_no_tag);
  return ctrl;
}

inline glint_slider* glint_adder::slider(std::function<void(glint_slider&)> setup) const
{
  auto* ctrl = new glint_slider();
  if (setup) setup(*ctrl);
  ctrl->computedStyle = glint_builder_initial_style(ctrl);
  ctrl->mRect = ctrl->mPaintRECT = resolve(ctrl->computedStyle);
  dispatchChild(ctrl, glint_no_tag);
  return ctrl;
}

inline glint_tree* glint_adder::tree(std::function<void(glint_tree&)> setup) const
{
  auto* ctrl = new glint_tree();
  if (setup) setup(*ctrl);
  ctrl->computedStyle = glint_builder_initial_style(ctrl);
  ctrl->mRect = ctrl->mPaintRECT = resolve(ctrl->computedStyle);
  dispatchChild(ctrl, glint_no_tag);
  return ctrl;
}

inline glint_element* glint_adder::spacer() const
{
  auto* ctrl = new glint_element();
  ctrl->style.flexGrow = 1.f;
  ctrl->style.width    = 0.f;
  ctrl->style.height   = 0.f;
  // Spacers are invisible and should not intercept mouse events.
  // Override HitTest via a flag rather than subclassing.
  dispatchChild(ctrl, glint_no_tag);
  return ctrl;
}

inline glint_image* glint_adder::img(std::function<void(glint_image&)> setup) const
{
  auto* ctrl = new glint_image();
  if (setup) setup(*ctrl);
  // If bitmap was set directly (not via src), mark it as already loaded.
  if (ctrl->bitmap.GetAPIBitmap()) ctrl->_loaded = true;
  ctrl->computedStyle = glint_builder_initial_style(ctrl);
  ctrl->mRect = ctrl->mPaintRECT = resolve(ctrl->computedStyle);
  dispatchChild(ctrl, ctrl->tag);
  // src loading is intentionally deferred to first drawContent
  // so that document.onRequest is guaranteed to be set by then.
  return ctrl;
}

// --- glint_component_adder method bodies -------------------------------------
// Defined here so glint_ctx is fully known. Each method pushes an OpEntry
// with a create lambda *and* a measure lambda (which runs the setup on a
// temporary struct to let the flex algorithm read width/height/margin without
// constructing any host-owned control).

inline void glint_component_style::glint_component_adder::button(std::function<void(glint_button&)> s, glint_button** out) {
  _ops.push_back({
    [s, out](glint_ctx& c){ auto* t = c.add.button(s); if (out) *out = t; },
    [s](glint_canvas*, glint_element*) -> glint_style { glint_button t; if (s) s(t); return t.style; }
  });
}

inline void glint_component_style::glint_component_adder::img(std::function<void(glint_image&)> s, glint_image** out) {
  _ops.push_back({
    [s, out](glint_ctx& c){ auto* t = c.add.img(s); if (out) *out = t; },
    [s](glint_canvas*, glint_element*) -> glint_style { glint_image t; if (s) s(t); return t.style; }
  });
}

inline void glint_component_style::glint_component_adder::input(std::function<void(glint_input&)> s, glint_input** out) {
  _ops.push_back({
    [s, out](glint_ctx& c){ auto* t = c.add.input(s); if (out) *out = t; },
    [s](glint_canvas*, glint_element*) -> glint_style { glint_input t; if (s) s(t); return t.style; }
  });
}

template<typename T, typename Setup>
inline void glint_component_style::glint_component_adder::custom(Setup&& setup, T** out) {
  static_assert(std::is_base_of_v<glint_element, T>,
                "glint_component_adder::custom only accepts glint_element-derived types in standalone mode");
  static_assert(std::is_default_constructible_v<T>,
                "glint_component_adder::custom requires a default-constructible control; use add.make(...) for bounds-constructed controls");

  auto createSetup = std::forward<Setup>(setup);
  auto measureSetup = createSetup;

  _ops.push_back({
    [createSetup, out](glint_ctx& c) mutable {
      auto* t = c.add.custom<T>(createSetup);
      if (out) *out = t;
    },
    [measureSetup](glint_canvas*, glint_element*) mutable -> glint_style {
      T t;
      measureSetup(t);
      // Measure pass: sync internal children then resolve intrinsic size,
      // matching Chromium's pre-layout measure step.
      t.syncBeforeLayout();
      glint_style s = t.style;
      if (sk_is_fit_content(s.height.raw)) {
        const float h = t.preferredH(0.f);
        if (h > 0.f) s.height = h;
      }
      if (sk_is_fit_content(s.width.raw)) {
        const float w = t.preferredW();
        if (w > 0.f) s.width = w;
      }
      return s;
    }
  });
}

inline void glint_component_style::glint_component_adder::colorpicker(std::function<void(glint_colorpicker&)> s, glint_colorpicker** out) {
  _ops.push_back({
    [s, out](glint_ctx& c){ auto* t = c.add.colorpicker(s); if (out) *out = t; },
    [s](glint_canvas*, glint_element*) -> glint_style { glint_colorpicker t; if (s) s(t); return t.style; }
  });
}

inline void glint_component_style::glint_component_adder::dial(std::function<void(glint_dial&)> s, glint_dial** out) {
  _ops.push_back({
    [s, out](glint_ctx& c){ auto* t = c.add.dial(s); if (out) *out = t; },
    [s](glint_canvas*, glint_element*) -> glint_style { glint_dial t; if (s) s(t); return t.style; }
  });
}

inline void glint_component_style::glint_component_adder::gradientEditor(std::function<void(glint_gradient_editor&)> s, glint_gradient_editor** out) {
  _ops.push_back({
    [s, out](glint_ctx& c){ auto* t = c.add.gradientEditor(s); if (out) *out = t; },
    [s](glint_canvas*, glint_element*) -> glint_style { glint_gradient_editor t; if (s) s(t); return t.style; }
  });
}

inline void glint_component_style::glint_component_adder::list(std::function<void(glint_list&)> s, glint_list** out) {
  _ops.push_back({
    [s, out](glint_ctx& c){ auto* t = c.add.list(s); if (out) *out = t; },
    [s](glint_canvas*, glint_element*) -> glint_style { glint_list t; if (s) s(t); return t.style; }
  });
}

inline void glint_component_style::glint_component_adder::select(std::function<void(glint_select&)> s, glint_select** out) {
  _ops.push_back({
    [s, out](glint_ctx& c){ auto* t = c.add.select(s); if (out) *out = t; },
    [s](glint_canvas*, glint_element*) -> glint_style { glint_select t; if (s) s(t); return t.style; }
  });
}

inline void glint_component_style::glint_component_adder::slider(std::function<void(glint_slider&)> s, glint_slider** out) {
  _ops.push_back({
    [s, out](glint_ctx& c){ auto* t = c.add.slider(s); if (out) *out = t; },
    [s](glint_canvas*, glint_element*) -> glint_style { glint_slider t; if (s) s(t); return t.style; }
  });
}

inline void glint_component_style::glint_component_adder::tree(std::function<void(glint_tree&)> s, glint_tree** out) {
  _ops.push_back({
    [s, out](glint_ctx& c){ auto* t = c.add.tree(s); if (out) *out = t; },
    [s](glint_canvas*, glint_element*) -> glint_style { glint_tree t; if (s) s(t); return t.style; }
  });
}

inline void glint_component_style::glint_component_adder::div(std::function<void(glint_component_style&)> s) {
  _ops.push_back({
    [s](glint_ctx& c){ c.add.div(s); },
    [s](glint_canvas* g, glint_element* parent) -> glint_style {
      glint_component_style t;
      if (s) s(t);
      t.style = glint_builder_measure_component_style(t, g, parent);
      // Mirror the intrinsic-sizing logic from glint_adder::component so that
      // auto-sized nested components report correct widths to their parent's
      // flex measure pass.
      const bool autoW = sk_is_fit_content(t.style.width.raw);
      const bool autoH = sk_is_fit_content(t.style.height.raw);
      glint_element probeParent;
      probeParent.style         = t.style;
      probeParent.computedStyle = t.style;
      probeParent.className     = t.className;
      probeParent.innerText     = t.innerText;
      probeParent.element.id    = t.element.id;
      probeParent.mpG           = g;
      probeParent.mParent       = parent;
      probeParent.mRoot         = parent ? parent->mRoot : nullptr;
      probeParent.mRequestRedraw = parent ? parent->mRequestRedraw : std::function<void()>{};
      probeParent.mApplyCss     = parent ? parent->mApplyCss : std::function<void(glint_element*)>{};
      if (parent)
      {
        probeParent.mParentW = parent->getContent().W();
        probeParent.mParentH = parent->getContent().H();
      }
      probeParent.finalizeTreeState();
      if ((autoW || autoH) && !t.add._ops.empty())
      {
        const float pl = t.style.paddingLeft,  pr = t.style.paddingRight;
        const float pt = t.style.paddingTop,   pb = t.style.paddingBottom;
        if (t.style.display == "flex")
        {
          const bool  isRow = (t.style.flexDirection != "column");
          const float gap   = t.style.gap.toFloat();
          float totalMain = 0.f, maxCross = 0.f;
          bool first = true;
          for (auto& op : t.add._ops)
          {
            const glint_style cs = op.measure(g, &probeParent);
            const float main  = isRow
              ? (cs.marginLeft + cs.width.toFloat()  + cs.marginRight)
              : (cs.marginTop  + cs.height.toFloat() + cs.marginBottom);
            const float cross = isRow ? cs.height.toFloat() : cs.width.toFloat();
            if (!first) totalMain += gap;
            totalMain += main;
            if (cross > maxCross) maxCross = cross;
            first = false;
          }
          if (autoW) t.style.width  = isRow ? (pl + totalMain + pr) : (pl + maxCross + pr);
          if (autoH) t.style.height = isRow ? (pt + maxCross + pb)  : (pt + totalMain + pb);
        }
        else
        {
          float maxRight = 0.f, maxBottom = 0.f;
          for (auto& op : t.add._ops)
          {
            const glint_style cs = op.measure(g, &probeParent);
            const float r2 = cs.left.toFloat() + cs.width.toFloat();
            const float b2 = cs.top.toFloat()  + cs.height.toFloat();
            if (r2 > maxRight)  maxRight  = r2;
            if (b2 > maxBottom) maxBottom = b2;
          }
          if (autoW) t.style.width  = maxRight  + pr;
          if (autoH) t.style.height = maxBottom + pb;
        }
      }
      // fit-content from innerText alone (no child ops):
      // Bake the text size into t.style so the parent's measure pass gets the
      // correct pixel size. The live ctrl will have its style reset to "fit-content"
      // by glint_adder::div() after construction so runtime Layout re-measures.
      else if ((autoW || autoH) && !t.innerText.empty())
      {
        const float pl = t.style.paddingLeft, pr = t.style.paddingRight;
        const float pt = t.style.paddingTop,  pb = t.style.paddingBottom;
        const float sz = t.style.fontSize.toFloat() > 0.f ? t.style.fontSize.toFloat() : 12.f;
        if (autoW)
        {
          SkFont font = glint_element::skFont(sz,
                                             t.style.fontFamily.c_str(),
                                             t.style.fontWeight,
                                             t.style.fontStyle.c_str());
          float maxW = 0.f;
          std::size_t pos = 0;
          while (pos <= t.innerText.size())
          {
            const std::size_t end     = t.innerText.find('\n', pos);
            const std::size_t lineEnd = (end == std::string::npos) ? t.innerText.size() : end;
            if (lineEnd > pos) { SkRect b; const float adv = font.measureText(t.innerText.c_str() + pos, lineEnd - pos, SkTextEncoding::kUTF8, &b); maxW = std::max(maxW, adv); }
            if (end == std::string::npos) break;
            pos = end + 1;
          }
          t.style.width = pl + maxW + 4.f + pr;
        }
        if (autoH)
        {
          int lines = 1; for (char c : t.innerText) if (c == '\n') ++lines;
          SkFont _fnt = glint_element::skFont(sz, t.style.fontFamily.c_str(), t.style.fontWeight, t.style.fontStyle.c_str()); SkFontMetrics _fm; _fnt.getMetrics(&_fm);
          const float lh = t.style.lineHeight > 0.f ? sz * t.style.lineHeight : (-_fm.fAscent + _fm.fDescent);
          const std::size_t _fnl = t.innerText.find('\n');
          const std::size_t _fll = (_fnl == std::string::npos) ? t.innerText.size() : _fnl;
          SkRect _inkB; _fnt.measureText(t.innerText.c_str(), _fll, SkTextEncoding::kUTF8, &_inkB);
          t.style.height = pt + _inkB.height() + static_cast<float>(lines - 1) * lh + pb;
        }
      }
      return t.style;
    }
  });
}
inline void glint_component_style::glint_component_adder::div(std::function<void(glint_component_style&)> s, glint_element** out) {
  div(s); // push the full op (including measure logic) via the 1-arg overload
  auto measure = _ops.back().measure; // preserve the measure lambda
  _ops.back() = {
    [s, out](glint_ctx& c){ auto* t = c.add.div(s); if (out) *out = t; },
    std::move(measure)
  };
}
inline void glint_component_style::glint_component_adder::component(std::function<void(glint_component_style&)> s)
{ div(std::move(s)); } // backward-compat ? div()

template<typename Factory>
inline void glint_component_style::glint_component_adder::make(float w, float h, Factory factory, int tag) {
  _ops.push_back({
    // create: called with the flex-computed child context ? build glint_rect, add to scene graph
    [factory, tag, w, h](glint_ctx& c) {
      const glint_rect r(c.add.mOffsetX, c.add.mOffsetY,
                    c.add.mOffsetX + w, c.add.mOffsetY + h);
      c.add.dispatchChild(factory(r), tag);
    },
    // measure: tell the flex engine the desired size
    [w, h](glint_canvas*, glint_element*) -> glint_style {
      glint_style s;
      s.width  = w;
      s.height = h;
      return s;
    }
  });
}

template<typename T>
inline void glint_component_style::glint_component_adder::attach(T* ctrl, int tag) {
  // No style available for raw attach � measure returns empty style (0�0).
  _ops.push_back({
    [ctrl, tag](glint_ctx& c){ c.add.attach(ctrl, tag); },
    [](glint_canvas*, glint_element*) -> glint_style { return {}; }
  });
}

inline void glint_component_style::glint_component_adder::spacer() {
  // A spacer is a real glint_spacer node (flexGrow=1, invisible, HitTest=false).
  // Being a real node means it appears in the inspector tree and participates
  // correctly in the reactive Layout() pass every frame.
  _ops.push_back({
    [](glint_ctx& c){ c.add.spacer(); },  // create: add real spacer to scene graph
    [](glint_canvas*, glint_element*) -> glint_style {       // measure: 0�0, flexGrow=1 for build-time flex
      glint_style s;
      s.flexGrow = 1.f;
      return s;
    }
  });
}

// --- Builder entry -----------------------------------------------------------
// Typical usage inside a component/layout function:
//
//   glint_ctx _c(parentElement, parentElement.GetUI(), parentElement.root());
//     _c.add.div([](auto& _c) { _c.left = ...; _c.innerText = ...; });
//     _c.add.component([](glint_component_style& _c) {
//       _c.left = 10; _c.top = 10; _c.width = 200; _c.height = 100;
//       _c.add.div([](auto& _c) {
//         _c.left = 4; _c.top = 4;  // relative to component origin ? abs (14, 14)
//         _c.innerText = "Hello";
//       });
//       // opt-out with position = "absolute":
//       _c.add.div([](auto& _c) {
//         _c.style.position = "absolute";
//         _c.left = 50; _c.top = 50;  // always window-relative
//         _c.innerText = "Pinned";
//       });
//     });
//
// The standalone app constructs glint_ctx directly and builds the tree in place.

// --- glint_element::ComponentAdd bodies ------------------------------------
// Defined here so glint_adder and all style types are fully known.
//
// Block-flow layout:
//   Each factory method peeks at the setup style before forwarding.
//   If style.top is unset (raw == "") and position != "absolute", we inject
//   the current mCursorY so children stack top-to-bottom by default � mirroring
//   browser block-flow.  If style.top is explicitly set, we honour it as-is.
//   After each non-absolute child is placed we advance mCursorY to the
//   control's actual bottom edge (relative to the component origin).

// Helper: peek and inject cursor into a style struct, return whether absolute.
// skipCursor — pass true when the parent is a flex/grid container; in that case
// the parent's layout engine handles child positioning and injecting a cursor
// value into style.top would create a spurious inline override that beats CSS.
template<typename StyleT, typename S>
static bool sk_inject_cursor(StyleT& peek, S&& setup, float cursorY, bool skipCursor = false)
{
  setup(peek);  // S is always a lambda � no operator bool, always callable
  const bool isAbs = (peek.style.position == "absolute");
  if (!skipCursor && !isAbs && peek.style.top.raw.empty()) {
    peek.style.top = cursorY;
    peek.style.top.builderInjected = true;
  }
  return isAbs;
}

static float sk_builder_flow_bottom_for_child(glint_element* owner, glint_element* child, float fallback)
{
  if (!owner || !child) return fallback;
  if (child->computedStyle.display == "none" || child->computedStyle.position == "absolute")
    return fallback;

  const float parentContentW = std::max(0.f, owner->getContent().W());
  const float marginBottom = child->computedStyle.marginBottom.resolve(parentContentW);
  return child->GetPaintRECT().B - owner->GetPaintRECT().T + marginBottom;
}

template<typename S>
inline auto glint_element::ComponentAdd::button(S&& setup)
{
  const float cursorY = mCursorY;
  const bool skipCursor = (_owner->cssStyle_.display == "flex" ||
                           _owner->cssStyle_.display == "grid"  ||
                           _owner->style.display    == "flex"   ||
                           _owner->style.display    == "grid");
  bool wasAbs = false;
  glint_adder a{_owner};
  auto* ctrl = a.button([s = std::forward<S>(setup), cursorY, skipCursor, &wasAbs](glint_button& c) mutable {
    s(c);
    wasAbs = (c.style.position == "absolute");
    if (!skipCursor && !wasAbs && c.style.top.raw.empty()) { c.style.top = cursorY; c.style.top.builderInjected = true; }

  });
  if (!skipCursor && !wasAbs && ctrl) mCursorY = sk_builder_flow_bottom_for_child(_owner, ctrl, mCursorY);
  return ctrl;
}
template<typename S>
inline auto glint_element::ComponentAdd::img(S&& setup)
{
  const float cursorY = mCursorY;
  const bool skipCursor = (_owner->cssStyle_.display == "flex" ||
                           _owner->cssStyle_.display == "grid"  ||
                           _owner->style.display    == "flex"   ||
                           _owner->style.display    == "grid");
  bool wasAbs = false;
  glint_adder a{_owner};
  auto* ctrl = a.img([s = std::forward<S>(setup), cursorY, skipCursor, &wasAbs](glint_image& c) mutable {
    s(c);
    wasAbs = (c.style.position == "absolute");
    if (!skipCursor && !wasAbs && c.style.top.raw.empty()) { c.style.top = cursorY; c.style.top.builderInjected = true; }
  });
  if (!skipCursor && !wasAbs && ctrl) mCursorY = sk_builder_flow_bottom_for_child(_owner, ctrl, mCursorY);
  return ctrl;
}
template<typename S>
inline auto glint_element::ComponentAdd::input(S&& setup)
{
  const float cursorY = mCursorY;
  const bool skipCursor = (_owner->cssStyle_.display == "flex" ||
                           _owner->cssStyle_.display == "grid"  ||
                           _owner->style.display    == "flex"   ||
                           _owner->style.display    == "grid");
  bool wasAbs = false;
  glint_adder a{_owner};
  auto* ctrl = a.input([s = std::forward<S>(setup), cursorY, skipCursor, &wasAbs](glint_input& c) mutable {
    s(c);
    wasAbs = (c.style.position == "absolute");
    if (!skipCursor && !wasAbs && c.style.top.raw.empty()) { c.style.top = cursorY; c.style.top.builderInjected = true; }
  });
  if (!skipCursor && !wasAbs && ctrl) mCursorY = sk_builder_flow_bottom_for_child(_owner, ctrl, mCursorY);
  return ctrl;
}
template<typename T, typename S>
inline T* glint_element::ComponentAdd::custom(S&& setup, T** out)
{
  static_assert(std::is_base_of_v<glint_element, T>,
                "ComponentAdd::custom only accepts glint_element-derived types in standalone mode");
  static_assert(std::is_default_constructible_v<T>,
                "ComponentAdd::custom requires a default-constructible control; use add.make(...) for bounds-constructed controls");
  const float cursorY = mCursorY;
  const bool skipCursor = (_owner->cssStyle_.display == "flex" ||
                           _owner->cssStyle_.display == "grid"  ||
                           _owner->style.display    == "flex"   ||
                           _owner->style.display    == "grid");
  bool wasAbs = false;
  glint_adder a{_owner};
  auto* ctrl = a.custom<T>([s = std::forward<S>(setup), cursorY, skipCursor, &wasAbs](T& c) mutable {
    s(c);
    wasAbs = (c.style.position == "absolute");
    if (!skipCursor && !wasAbs && c.style.top.raw.empty()) { c.style.top = cursorY; c.style.top.builderInjected = true; }
  });
  if (!skipCursor && !wasAbs && ctrl) mCursorY = sk_builder_flow_bottom_for_child(_owner, ctrl, mCursorY);
  if (out) *out = ctrl;
  return ctrl;
}
template<typename S>
inline auto glint_element::ComponentAdd::colorpicker(S&& setup, glint_colorpicker** out)
{
  const float cursorY = mCursorY;
  const bool skipCursor = (_owner->cssStyle_.display == "flex" ||
                           _owner->cssStyle_.display == "grid"  ||
                           _owner->style.display    == "flex"   ||
                           _owner->style.display    == "grid");
  bool wasAbs = false;
  glint_adder a{_owner};
  auto* ctrl = a.colorpicker([s = std::forward<S>(setup), cursorY, skipCursor, &wasAbs](glint_colorpicker& c) mutable {
    s(c);
    wasAbs = (c.style.position == "absolute");
    if (!skipCursor && !wasAbs && c.style.top.raw.empty()) { c.style.top = cursorY; c.style.top.builderInjected = true; }
  });
  if (!skipCursor && !wasAbs && ctrl) mCursorY = sk_builder_flow_bottom_for_child(_owner, ctrl, mCursorY);
  if (out) *out = ctrl;
  return ctrl;
}
template<typename S>
inline auto glint_element::ComponentAdd::dial(S&& setup, glint_dial** out)
{
  const float cursorY = mCursorY;
  const bool skipCursor = (_owner->cssStyle_.display == "flex" ||
                           _owner->cssStyle_.display == "grid"  ||
                           _owner->style.display    == "flex"   ||
                           _owner->style.display    == "grid");
  bool wasAbs = false;
  glint_adder a{_owner};
  auto* ctrl = a.dial([s = std::forward<S>(setup), cursorY, skipCursor, &wasAbs](glint_dial& c) mutable {
    s(c);
    wasAbs = (c.style.position == "absolute");
    if (!skipCursor && !wasAbs && c.style.top.raw.empty()) { c.style.top = cursorY; c.style.top.builderInjected = true; }
  });
  if (!skipCursor && !wasAbs && ctrl) mCursorY = sk_builder_flow_bottom_for_child(_owner, ctrl, mCursorY);
  if (out) *out = ctrl;
  return ctrl;
}
template<typename S>
inline auto glint_element::ComponentAdd::gradientEditor(S&& setup, glint_gradient_editor** out)
{
  const float cursorY = mCursorY;
  const bool skipCursor = (_owner->cssStyle_.display == "flex" ||
                           _owner->cssStyle_.display == "grid"  ||
                           _owner->style.display    == "flex"   ||
                           _owner->style.display    == "grid");
  bool wasAbs = false;
  glint_adder a{_owner};
  auto* ctrl = a.gradientEditor([s = std::forward<S>(setup), cursorY, skipCursor, &wasAbs](glint_gradient_editor& c) mutable {
    s(c);
    wasAbs = (c.style.position == "absolute");
    if (!skipCursor && !wasAbs && c.style.top.raw.empty()) { c.style.top = cursorY; c.style.top.builderInjected = true; }
  });
  if (!skipCursor && !wasAbs && ctrl) mCursorY = sk_builder_flow_bottom_for_child(_owner, ctrl, mCursorY);
  if (out) *out = ctrl;
  return ctrl;
}
inline glint_element* glint_element::ComponentAdd::spacer()
{
  glint_adder a{_owner};
  auto* ctrl = a.spacer();
  return ctrl;
}
template<typename S>
inline auto glint_element::ComponentAdd::list(S&& setup)
{
  const float cursorY = mCursorY;
  const bool skipCursor = (_owner->cssStyle_.display == "flex" ||
                           _owner->cssStyle_.display == "grid"  ||
                           _owner->style.display    == "flex"   ||
                           _owner->style.display    == "grid");
  bool wasAbs = false;
  glint_adder a{_owner};
  auto* ctrl = a.list([s = std::forward<S>(setup), cursorY, skipCursor, &wasAbs](glint_list& c) mutable {
    s(c);
    wasAbs = (c.style.position == "absolute");
    if (!skipCursor && !wasAbs && c.style.top.raw.empty()) { c.style.top = cursorY; c.style.top.builderInjected = true; }
  });
  if (!skipCursor && !wasAbs && ctrl) mCursorY = sk_builder_flow_bottom_for_child(_owner, ctrl, mCursorY);
  return ctrl;
}

// ComponentAdd::component is defined earlier � before the component includes.
// See the section titled "ComponentAdd::component body" above.
template<typename T>
inline auto glint_element::ComponentAdd::attach(T* ctrl, int tag)
{
  static_assert(std::is_base_of_v<glint_element, T>,
                "ComponentAdd::attach only accepts glint_element-derived types in standalone mode");
  ctrl->mTag = tag;
  _owner->addChild(ctrl);
  if (_owner->mRoot && tag != glint_no_tag)
    _owner->mRoot->RegisterTag(tag, ctrl);
  if (ctrl) mCursorY = sk_builder_flow_bottom_for_child(_owner, ctrl, mCursorY);
  return ctrl;
}
template<typename F>
inline auto glint_element::ComponentAdd::make(float w, float h, F factory, int tag)
{
  // Position at cursor using owner's absolute origin.
  const glint_rect r(_owner->GetPaintRECT().L,
                _owner->GetPaintRECT().T + mCursorY,
                _owner->GetPaintRECT().L + w,
                _owner->GetPaintRECT().T + mCursorY + h);
  auto* ctrl = factory(r);
  static_assert(std::is_base_of_v<glint_element, std::remove_pointer_t<decltype(ctrl)>>,
                "ComponentAdd::make factories must return glint_element-derived pointers in standalone mode");
  ctrl->mTag = tag;
  _owner->addChild(ctrl);
  if (_owner->mRoot && tag != glint_no_tag)
    _owner->mRoot->RegisterTag(tag, ctrl);
  mCursorY += h;
  return ctrl;
}

// -- Backward-compat aliases ---------------------------------------------------
using glint_panel   = glint_div;   // old name; prefer glint_div in new code
