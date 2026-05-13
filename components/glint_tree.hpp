#pragma once

/**
 * glint_tree.hpp
 * General-purpose collapsible tree-view component for glint.
 *
 * Each visible node in the data tree maps to one glint_tree_row child.
 * Nodes whose ancestor is collapsed are excluded from the row list entirely
 * (DFS traversal skips collapsed subtrees), so the scrollable content
 * remains a single flat list of variable-depth rows � no nested scroll.
 *
 * Indentation:
 *   The row's left edge starts at  content.L + depth * kIndent  so that
 *   child rows visually begin offset from their parent row's left edge,
 *   not from the tree's left edge.  The chevron and label sit at the
 *   row's own origin with no extra left padding for depth.
 *
 * Collapse / expand:
 *   mExpandedIds holds the set of node IDs whose children are currently
 *   visible.  Clicking a chevron toggles the ID and rebuilds the row list.
 *   setTree() preserves mExpandedIds across data refreshes.
 *   expandToDepth(n) forces all nodes at depth < n to be expanded.
 *   selectById(id) expands all ancestors of the target node.
 *
 * Scroll behaviour:
 *   style.overflowY = "auto" is set in the constructor.  All scroll
 *   infrastructure (scrollbar, clipping, wheel, HitTest) is handled by
 *   the base glint_element scroll system.
 */

#include "../glint_element.hpp"
#include "../default_style.hpp"
#include "../render/glint_tree_node.hpp"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

// -- glint_tree_row ------------------------------------------------------------
// One visible row.  The row's left edge is offset by depth (set by Layout),
// so the component does not apply any depth-based left padding internally.
// The chevron is drawn in the leading kIconW pixels of the row's own rect.

class glint_tree_row : public glint_element
{
public:
  static constexpr float kRowH   = 20.f;
  static constexpr float kIndent = 14.f;   // pixels per depth level (applied by Layout)
  static constexpr float kIconW  = 14.f;   // chevron column width
  static constexpr float kEyeW   = 18.f;   // eye button column width (right edge)

  // Stamped by glint_tree before addChild().
  const glint_tree_node* node        = nullptr;
  int                    depth       = 0;
  bool                   selected    = false;
  bool                   hasChildren = false;
  bool                   expanded    = false;
  bool                   eyeActive   = false;   // true = eye-pinned highlight is on

  // Callbacks wired by glint_tree.
  std::function<void(glint_tree_row*)> onRowClick;
  std::function<void(glint_tree_row*)> onChevronClick;
  std::function<void(glint_tree_row*)> onEyeClick;

  glint_tree_row()
  {
    element.addEventListener("mouseenter", [this](glint_event&)
    {
      mHovered = true;
      setDirty(false);
    });
    element.addEventListener("mouseleave", [this](glint_event&)
    {
      mHovered = false;
      setDirty(false);
    });

    element.addEventListener("mousedown", [this](glint_event& e)
    {
      e.stopPropagation();
      auto& me = static_cast<glint_mouse_event&>(e);
      const float localX = me.clientX - mRect.L;
      // Eye button: right kEyeW strip — visible when hovered or active.
      if ((mHovered || eyeActive) && localX >= (mRect.W() - kEyeW))
      {
        if (onEyeClick) onEyeClick(this);
        return;
      }
      if (hasChildren && localX < kIconW)
      {
        if (onChevronClick) onChevronClick(this);
      }
      else
      {
        if (onRowClick) onRowClick(this);
      }
    });
  }

  const char* typeName() const override { return "tree-row"; }

  void setSelected(bool v)
  {
    if (selected == v) return;
    selected = v;
    setDirty(false);
  }

  // Called by glint_tree when collapse/expand state changes on this row
  // (without a full rebuild � used to update the chevron glyph).
  void setExpanded(bool v)
  {
    if (expanded == v) return;
    expanded = v;
    setDirty(false);
  }

  void _syncLabel()
  {
    char labelBuf[128];
    buildLabel(labelBuf, sizeof(labelBuf));
    mLabelText = labelBuf;
  }

protected:
  // -- glint_canvas draw path --------------------------------------------------
  void drawContent(glint_canvas& g) override
  {
    _syncLabel();
    if (selected)
      g.FillRect(glint_color(150, 48, 100, 220), mRect);
    else if (mHovered || mInspectHovered)
      g.FillRect(glint_color(50, 200, 200, 200), mRect);

    // Simple filled triangle chevron for non-Skia builds.
    if (hasChildren)
    {
      const float cx = mRect.L + kIconW * 0.5f;
      const float cy = mRect.T + mRect.H() * 0.5f;
      const float r  = 3.5f;
      glint_color tri(200, 180, 180, 180);
      if (expanded)
      {
        const float xs[] = { cx - r, cx + r, cx };
        const float ys[] = { cy - r * 0.6f, cy - r * 0.6f, cy + r * 0.8f };
        g.FillConvexPolygon(tri, xs, ys, 3);
      }
      else
      {
        const float xs[] = { cx - r * 0.6f, cx - r * 0.6f, cx + r * 0.8f };
        const float ys[] = { cy - r, cy + r, cy };
        g.FillConvexPolygon(tri, xs, ys, 3);
      }
    }

    drawLabel(g);
    drawEye(g);
  }

  // -- Skia draw path -------------------------------------------------------
  void DrawContentToCanvas(SkCanvas* canvas) override
  {
    _syncLabel();

    // Selection / hover background.
    if (selected)
    {
      SkPaint p;  p.setColor(SkColorSetARGB(150, 48, 100, 220));
      canvas->drawRect(skRect(mRect), p);
    }
    else if (mHovered || mInspectHovered)
    {
      SkPaint p;  p.setColor(SkColorSetARGB(50, 200, 200, 200));
      canvas->drawRect(skRect(mRect), p);
    }

    // Chevron triangle � drawn as a filled SkPath to avoid glyph/font issues.
    if (hasChildren)
    {
      const float cx = mRect.L + kIconW * 0.5f;
      const float cy = mRect.T + mRect.H() * 0.5f;
      const float r  = 3.5f;

      SkPath path;
      if (expanded)
      {
        // ?  pointing down
        path.moveTo(cx - r, cy - r * 0.55f);
        path.lineTo(cx + r, cy - r * 0.55f);
        path.lineTo(cx,     cy + r * 0.8f);
      }
      else
      {
        // ?  pointing right
        path.moveTo(cx - r * 0.55f, cy - r);
        path.lineTo(cx + r * 0.8f,  cy);
        path.lineTo(cx - r * 0.55f, cy + r);
      }
      path.close();

      SkPaint tp;
      tp.setAntiAlias(true);
      tp.setColor(SkColorSetARGB(200, 160, 160, 160));
      canvas->drawPath(path, tp);
    }

    drawLabelToCanvas(canvas);
    drawEyeToCanvas(canvas);
  }

private:
  std::string mLabelText;
  bool mHovered        = false;
  bool mInspectHovered = false;

public:
  void setInspectHovered(bool v) { mInspectHovered = v; setDirty(false); }
  void setEyeActive(bool v)      { if (eyeActive == v) return; eyeActive = v; setDirty(false); }

private:
  // Eye button is shown when the row is hovered OR when it is already active.
  bool showEye() const { return mHovered || eyeActive; }

  // Centre of the eye icon.
  glint_rect eyeRect() const
  {
    const float cx = mRect.R - kEyeW * 0.5f;
    const float cy = mRect.T + mRect.H() * 0.5f;
    return glint_rect(cx - 7.f, cy - 5.f, cx + 7.f, cy + 5.f);
  }

  void drawEye(glint_canvas& g)
  {
    if (!showEye()) return;
    const uint8_t a = eyeActive ? 220 : 90;
    const glint_color col(a, 0, 210, 170);   // teal
    const glint_rect er = eyeRect();
    const float cx = (er.L + er.R) * 0.5f;
    const float cy = (er.T + er.B) * 0.5f;
    const float rw = (er.R - er.L) * 0.5f;   // half-width  (~7)
    const float rh = (er.B - er.T) * 0.5f;   // half-height (~5)
    // Approximate eye-lid arcs with two triangles (non-Skia build).
    // Top and bottom halves of the oval using 5-segment approximation.
    constexpr int N = 6;
    float xs[N], ys[N];
    for (int i = 0; i < N; ++i)
    {
      const float t = static_cast<float>(i) / (N - 1);   // 0..1 left to right
      xs[i] = cx + (-rw + 2.f * rw * t);
      const float sine = std::sqrt(std::max(0.f, 1.f - (t * 2.f - 1.f) * (t * 2.f - 1.f)));
      ys[i] = cy - rh * sine;
    }
    g.FillConvexPolygon(col, xs, ys, N);
    for (int i = 0; i < N; ++i)
      ys[i] = cy + rh * std::sqrt(std::max(0.f, 1.f - ((xs[i] - cx) / rw) * ((xs[i] - cx) / rw)));
    g.FillConvexPolygon(col, xs, ys, N);
    // Pupil: small square.
    const float pr = 2.f;
    const glint_rect pupil(cx - pr, cy - pr, cx + pr, cy + pr);
    g.FillRect(glint_color(a, 0, 0, 0), pupil);
  }

  void drawEyeToCanvas(SkCanvas* canvas)
  {
    if (!canvas || !showEye()) return;
    const uint8_t a = eyeActive ? 220 : 90;
    const glint_rect er = eyeRect();
    const float cx = (er.L + er.R) * 0.5f;
    const float cy = (er.T + er.B) * 0.5f;
    const float rw = (er.R - er.L) * 0.5f;
    const float rh = (er.B - er.T) * 0.5f * 0.7f;   // eye-lid curve height
    SkPaint ep;
    ep.setAntiAlias(true);
    ep.setStyle(SkPaint::kStroke_Style);
    ep.setStrokeWidth(1.4f);
    ep.setColor(SkColorSetARGB(a, 0, 210, 170));
    // Upper arc.
    SkPath upper;
    upper.moveTo(cx - rw, cy);
    upper.cubicTo(cx - rw * 0.5f, cy - rh * 2.5f,
                  cx + rw * 0.5f, cy - rh * 2.5f,
                  cx + rw,        cy);
    canvas->drawPath(upper, ep);
    // Lower arc.
    SkPath lower;
    lower.moveTo(cx - rw, cy);
    lower.cubicTo(cx - rw * 0.5f, cy + rh * 2.5f,
                  cx + rw * 0.5f, cy + rh * 2.5f,
                  cx + rw,        cy);
    canvas->drawPath(lower, ep);
    // Pupil.
    SkPaint pp;
    pp.setAntiAlias(true);
    pp.setStyle(eyeActive ? SkPaint::kFill_Style : SkPaint::kStroke_Style);
    pp.setStrokeWidth(1.2f);
    pp.setColor(SkColorSetARGB(a, 0, 210, 170));
    canvas->drawCircle(cx, cy, 2.2f, pp);
  }

  void drawLabel(glint_canvas& g)
  {
    if (mLabelText.empty()) return;

    const glint_color color = selected ? glint_color(255, 255, 255, 255)
                                  : glint_color(255, 195, 195, 195);
    // Reserve right edge for the eye button when visible.
    const float rEdge = showEye() ? mRect.R - kEyeW : mRect.R;
    const glint_rect textRect(mRect.L + kIconW, mRect.T, rEdge, mRect.B);
    const std::string fontId = glint_font_registry::resolveFontFaceId(
      style.fontFamily.c_str(), static_cast<int>(style.fontWeight), style.fontStyle.c_str());
    const char* fontName = fontId.empty()
      ? (style.fontFamily.empty() ? nullptr : style.fontFamily.c_str())
      : fontId.c_str();
    glint_text text(11.f, color, fontName, EAlign::Near, EVAlign::Middle);
    g.DrawText(text, mLabelText.c_str(), textRect);
  }

  void drawLabelToCanvas(SkCanvas* canvas)
  {
    if (!canvas || mLabelText.empty()) return;

    constexpr float fontSize = 11.f;
    SkFont font = skFont(fontSize, style.fontFamily.c_str(), style.fontWeight, style.fontStyle.c_str());
    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setColor(selected ? SkColorSetARGB(255, 255, 255, 255)
                            : SkColorSetARGB(255, 195, 195, 195));

    const float textLeft  = mRect.L + kIconW;
    // Clip to avoid overlap with eye button area.
    const float rEdge = showEye() ? mRect.R - kEyeW : mRect.R;
    SkAutoCanvasRestore acr(canvas, true);
    canvas->clipRect(SkRect::MakeLTRB(textLeft, mRect.T, rEdge, mRect.B));

    SkFontMetrics metrics;
    font.getMetrics(&metrics);
    const float textHeight = metrics.fDescent - metrics.fAscent;
    const float baseline = mRect.T + (mRect.H() - textHeight) * 0.5f - metrics.fAscent;
    canvas->drawString(mLabelText.c_str(), textLeft, baseline, font, paint);
  }

  void buildLabel(char* buf, size_t sz) const
  {
    if (!node) { buf[0] = '\0'; return; }

    std::string tagLabel = "<" + node->typeName;
    if (!node->elementId.empty())
      tagLabel += " id=\"" + node->elementId + "\"";
    if (!node->className.empty())
      tagLabel += " class=\"" + node->className + "\"";
    tagLabel += ">";

    // Build a short preview of innerText (first line, max 24 chars)
    char textPreview[32] = {};
    if (!node->innerText.empty())
    {
      // Take up to the first newline
      const std::string& t = node->innerText;
      const size_t nl  = t.find('\n');
      const size_t len = (nl == std::string::npos) ? t.size() : nl;
      const bool truncated = len > 24;
      std::snprintf(textPreview, sizeof(textPreview),
                    truncated ? " \"%.*s\u2026\"" : " \"%.*s\"",
                    static_cast<int>(truncated ? 24 : len), t.c_str());
    }

    std::snprintf(buf, sz, "%s%s", tagLabel.c_str(), textPreview);
  }
};


// -- glint_tree ----------------------------------------------------------------

class glint_tree : public glint_element
{
public:
  // -- Public API ------------------------------------------------------------

  /** Fired when the user clicks a row (not the chevron). */
  std::function<void(const glint_tree_node&)> onSelect;

  /** Fired on row hover (mouseenter — called with node; mouseleave — nullptr). */
  std::function<void(const glint_tree_node*)> onHover;

  /** Fired when the eye button is toggled.
   *  id=0 means the eye was turned off (no node pinned). */
  std::function<void(uint64_t id, bool active)> onEyeToggle;

  glint_tree()
  {
    setCssStyleLayer(glint_default_user_agent_style_for(*this));
    computedStyle  = mergedStyleForLayout();
    mAcceptsFocus  = true;
  }

  const char* typeName() const override { return "tree"; }

  // ── Keyboard navigation ───────────────────────────────────────────────────
  // Up/Down   — move selection one row
  // Right     — expand selected node; if already expanded, move into first child
  // Left      — collapse selected node; if already collapsed, move to parent
  bool OnKeyDown(const glint_key_press& key) override
  {
    // ── Arrow Up ──────────────────────────────────────────────────────────
    if (key.vk == 0x26)   // VK_UP
    {
      const int idx = selectedRowIndex();
      if (idx > 0)
        queuePendingAction(pending_action_type::select_row, mRowPtrs[idx - 1]->node->id);
      else if (idx < 0 && !mRowPtrs.empty())
        queuePendingAction(pending_action_type::select_row, mRowPtrs[0]->node->id);
      setDirty(false);
      return true;
    }
    // ── Arrow Down ────────────────────────────────────────────────────────
    if (key.vk == 0x28)   // VK_DOWN
    {
      const int idx  = selectedRowIndex();
      const int last = static_cast<int>(mRowPtrs.size()) - 1;
      if (idx < 0 && last >= 0)
        queuePendingAction(pending_action_type::select_row, mRowPtrs[0]->node->id);
      else if (idx >= 0 && idx < last)
        queuePendingAction(pending_action_type::select_row, mRowPtrs[idx + 1]->node->id);
      setDirty(false);
      return true;
    }
    // ── Arrow Right: expand or enter first child ──────────────────────────
    if (key.vk == 0x27)   // VK_RIGHT
    {
      const int idx = selectedRowIndex();
      if (idx >= 0)
      {
        glint_tree_row* row = mRowPtrs[idx];
        if (row->hasChildren && !row->expanded)
          queuePendingAction(pending_action_type::toggle_expand, row->node->id);
        else if (row->expanded && idx + 1 < static_cast<int>(mRowPtrs.size()))
          queuePendingAction(pending_action_type::select_row, mRowPtrs[idx + 1]->node->id);
      }
      setDirty(false);
      return true;
    }
    // ── Arrow Left: collapse or move to parent ────────────────────────────
    if (key.vk == 0x25)   // VK_LEFT
    {
      const int idx = selectedRowIndex();
      if (idx >= 0)
      {
        glint_tree_row* row = mRowPtrs[idx];
        if (row->hasChildren && row->expanded)
        {
          queuePendingAction(pending_action_type::toggle_expand, row->node->id);
        }
        else
        {
          // Move to nearest ancestor with smaller depth.
          for (int i = idx - 1; i >= 0; --i)
          {
            if (mRowPtrs[i]->depth < row->depth)
            {
              queuePendingAction(pending_action_type::select_row, mRowPtrs[i]->node->id);
              break;
            }
          }
        }
      }
      setDirty(false);
      return true;
    }
    return false;
  }

  /**
   * Replace the displayed tree.
   * Preserves mExpandedIds (existing open nodes stay open if their ID is
   * still present in the new data).  Resets selection and scroll-clamps.
   */
  void setTree(const glint_tree_node& root)
  {
    mRootNode          = root;
    mSelectedId        = 0;
    mInspectHoveredRow = nullptr;
    rebuildRows();
    setDirty(false);
  }

  /**
   * Expand all nodes whose depth is less than maxDepth.
   * Depth 0 = root node itself.  expandToDepth(1) reveals root's direct children.
   */
  void expandToDepth(int maxDepth)
  {
    collectExpandUpTo(mRootNode, 0, maxDepth);
    rebuildRows();
    setDirty(false);
  }

  /**
   * Programmatically select a node by id.
   * Expands all ancestors so the row is visible, then scrolls to it.
   * Does NOT fire onSelect.
   */
  void selectById(uint64_t id)
  {
    if (id == mSelectedId) return;
    expandAncestorsOf(id, mRootNode);   // ensure the row is in the visible list
    mSelectedId = id;
    rebuildRows();
    updateSelectionState();
    scrollToId(id);
    setDirty(false);
  }

  /**
   * Programmatically set which row shows the active eye highlight.
   * Clears any previously active eye row.  Pass 0 to clear.
   */
  void setEyedId(uint64_t id)
  {
    if (mEyedId == id) return;
    // Deactivate old row if it is currently visible.
    for (auto* r : mRowPtrs)
      if (r->node && r->node->id == mEyedId) r->setEyeActive(false);
    mEyedId = id;
    for (auto* r : mRowPtrs)
      if (r->node && r->node->id == mEyedId) r->setEyeActive(true);
    setDirty(false);
  }

  uint64_t eyedId() const { return mEyedId; }

  uint64_t selectedId() const { return mSelectedId; }

  /**
   * Highlight the row matching `id` with the faint hover style, without
   * changing selection.  Pass id = 0 to clear.  Also scrolls to the row.
   */
  void hoverById(uint64_t id)
  {
    if (mInspectHoveredRow)
    {
      mInspectHoveredRow->setInspectHovered(false);
      mInspectHoveredRow = nullptr;
    }
    if (id != 0)
    {
      for (auto* r : mRowPtrs)
      {
        if (r->node && r->node->id == id)
        {
          mInspectHoveredRow = r;
          r->setInspectHovered(true);
          scrollToId(id);
          break;
        }
      }
    }
    setDirty(false);
  }

  /**
   * Scroll so the row with `id` is visible � does not change selection.
   */
  void scrollToId(uint64_t id)
  {
    const float sbW   = (mScrollbarV && mScrollbarV->style.display != "none")
                          ? style.scrollbarWidth : 0.f;
    const float viewH = GetPaintRECT().H() - sbW;

    for (int i = 0; i < static_cast<int>(mRowPtrs.size()); ++i)
    {
      if (mRowPtrs[i]->node->id != id) continue;
      const float rowTop = static_cast<float>(i) * glint_tree_row::kRowH;
      const float rowBot = rowTop + glint_tree_row::kRowH;
      if (rowTop < mScrollTop)
        mScrollTop = rowTop;
      else if (rowBot > mScrollTop + viewH)
        mScrollTop = rowBot - viewH;
      const float maxScroll = std::max(0.f, mScrollHeight - viewH);
      mScrollTop = std::max(0.f, std::min(mScrollTop, maxScroll));
      _refreshRootHoverFromPointer();
      setDirty(false);
      return;
    }
  }

protected:
  // -- Layout ----------------------------------------------------------------

  /**
   * Position each visible row as a fixed-height band.
   * Each row's LEFT edge is offset by  depth * kIndent  so that child rows
   * begin indented relative to their parent's left edge, not the tree's.
   * Scroll is applied at draw/HitTest time by the base class.
   */
  void Layout(glint_canvas* g) override
  {
    applyPendingAction();

    const glint_rect content = getContent();
    const float sbW     = style.scrollbarWidth;
    const float totalH  = static_cast<float>(mRowPtrs.size())
                            * glint_tree_row::kRowH;

    const bool  needsY = (totalH > GetPaintRECT().H());
    const float rowW   = needsY ? std::max(0.f, content.W() - sbW) : content.W();

    for (int i = 0; i < static_cast<int>(mRowPtrs.size()); ++i)
    {
      glint_tree_row* row  = mRowPtrs[i];
      const float     rowT = content.T + static_cast<float>(i) * glint_tree_row::kRowH;
      const float     rowL = content.L + static_cast<float>(row->depth) * glint_tree_row::kIndent;
      row->mRect = row->mPaintRECT =
        glint_rect(rowL, rowT, content.L + rowW, rowT + glint_tree_row::kRowH);
      row->Layout(g);
    }

    // Publish content dimensions so the scrollbar and clamp know the range.
    mScrollHeight = totalH;
    mScrollWidth  = rowW;

    // Create / show / hide the vertical scrollbar child.
    _ensureScrollbars(/*needsX=*/false, needsY);
    _clampScroll(/*showX=*/false, /*showY=*/needsY);
    _positionScrollbars(g, /*showX=*/false, needsY);

    // Sync the DOM-compatible read-only properties.
    element.scrollHeight = mScrollHeight;
    element.scrollWidth  = mScrollWidth;
  }

  // HitTest: the base class handles scroll-aware routing automatically once
  // mScrollbarV is populated (which happens after the first Layout() call).

private:
  // -- State -----------------------------------------------------------------
  enum class pending_action_type
  {
    none,
    select_row,
    toggle_expand,
  };

  glint_tree_node              mRootNode;
  std::vector<glint_tree_row*> mRowPtrs;       // non-owning; mChildren holds ownership
  uint64_t                     mSelectedId  = 0;
  uint64_t                     mEyedId      = 0;   // node whose eye button is active
  std::unordered_set<uint64_t> mExpandedIds;   // IDs whose children are currently visible
  glint_tree_row*              mInspectHoveredRow = nullptr;
  pending_action_type         mPendingAction = pending_action_type::none;
  uint64_t                    mPendingActionId = 0;

  // -- Row management --------------------------------------------------------

  void rebuildRows()
  {
    mInspectHoveredRow = nullptr;  // clear before clearChildren() destroys the rows
    mRowPtrs.clear();
    clearChildren();
    flattenNode(mRootNode, 0);
  }

  /**
   * DFS traversal.  A node's children are only visited if the node's ID is
   * in mExpandedIds � collapsed subtrees are completely skipped.
   */
  void flattenNode(const glint_tree_node& node, int depth)
  {
    auto* row        = new glint_tree_row();
    row->node        = &node;
    row->depth       = depth;
    row->selected    = (node.id == mSelectedId);
    row->hasChildren = !node.children.empty();
    row->expanded    = mExpandedIds.count(node.id) > 0;

    // Placeholder bounds � correct rect is set by Layout() every frame.
    row->mRect = row->mPaintRECT =
      glint_rect(0.f, 0.f, mRect.W(), glint_tree_row::kRowH);

    row->onRowClick     = [this](glint_tree_row* r) { handleRowClick(r); };
    row->onChevronClick = [this](glint_tree_row* r) { handleChevronClick(r); };
    row->onEyeClick     = [this](glint_tree_row* r) { handleEyeClick(r); };
    row->eyeActive = (node.id == mEyedId);

    addChild(row);

    row->element.addEventListener("mouseenter", [this, row](glint_event&)
    {
      handleRowHover(row);
    });
    row->element.addEventListener("mouseleave", [this](glint_event&)
    {
      handleRowHover(nullptr);
    });

    mRowPtrs.push_back(row);

    // Only descend into children if this node is expanded.
    if (row->expanded)
    {
      for (const auto& child : node.children)
        flattenNode(child, depth + 1);
    }
  }

  // -- Event handlers --------------------------------------------------------

  void handleRowHover(glint_tree_row* row)
  {
    if (onHover) onHover(row ? row->node : nullptr);
  }

  void handleRowClick(glint_tree_row* row)
  {
    if (mRoot) mRoot->SetFocus(this);
    queuePendingAction(pending_action_type::select_row, row ? row->node->id : 0);
    setDirty(false);
  }

  void handleChevronClick(glint_tree_row* r)
  {
    if (mRoot) mRoot->SetFocus(this);
    queuePendingAction(pending_action_type::toggle_expand, r ? r->node->id : 0);
    setDirty(false);
  }

  void handleEyeClick(glint_tree_row* r)
  {
    if (!r || !r->node) return;
    const uint64_t id = r->node->id;
    const bool nowActive = !r->eyeActive;   // toggle
    // Deactivate any previously active row.
    for (auto* row : mRowPtrs)
      if (row->eyeActive && row != r) row->setEyeActive(false);
    r->setEyeActive(nowActive);
    mEyedId = nowActive ? id : 0;
    if (onEyeToggle) onEyeToggle(mEyedId, nowActive);
    setDirty(false);
  }

  void queuePendingAction(pending_action_type action, uint64_t id)
  {
    if (id == 0) return;
    mPendingAction = action;
    mPendingActionId = id;
  }

  void applyPendingAction()
  {
    const pending_action_type action = mPendingAction;
    const uint64_t id = mPendingActionId;
    mPendingAction = pending_action_type::none;
    mPendingActionId = 0;

    if (action == pending_action_type::none || id == 0) return;

    if (action == pending_action_type::toggle_expand)
    {
      if (mExpandedIds.count(id))
        mExpandedIds.erase(id);
      else
        mExpandedIds.insert(id);

      rebuildRows();
      return;
    }

    if (mSelectedId == id) return;

    mSelectedId = id;
    updateSelectionState();
    scrollToId(id);

    if (onSelect)
    {
      if (const glint_tree_node* node = findNodeById(id, mRootNode))
        onSelect(*node);
    }
  }

  const glint_tree_node* findNodeById(uint64_t id, const glint_tree_node& node) const
  {
    if (node.id == id) return &node;
    for (const auto& child : node.children)
    {
      if (const glint_tree_node* found = findNodeById(id, child))
        return found;
    }
    return nullptr;
  }

  // Returns the mRowPtrs index of the currently selected row, or -1.
  // If nothing is selected, returns 0 (top of list) to allow arrow nav to start.
  int selectedRowIndex() const
  {
    if (mRowPtrs.empty()) return -1;
    if (mSelectedId == 0) return -1;
    for (int i = 0; i < static_cast<int>(mRowPtrs.size()); ++i)
      if (mRowPtrs[i]->node->id == mSelectedId) return i;
    return -1;
  }

  // -- Expand helpers --------------------------------------------------------

  /**
   * Insert node.id into mExpandedIds for every ancestor that lies on the
   * path from the root to the node with the given id.
   * Returns true when the target is found in this subtree.
   */
  bool expandAncestorsOf(uint64_t id, const glint_tree_node& node)
  {
    if (node.id == id) return true;
    for (const auto& child : node.children)
    {
      if (expandAncestorsOf(id, child))
      {
        mExpandedIds.insert(node.id);
        return true;
      }
    }
    return false;
  }

  /**
   * Recursively expand all nodes at depth < maxDepth.
   */
  void collectExpandUpTo(const glint_tree_node& node, int depth, int maxDepth)
  {
    if (!node.children.empty() && depth < maxDepth)
    {
      mExpandedIds.insert(node.id);
      for (const auto& child : node.children)
        collectExpandUpTo(child, depth + 1, maxDepth);
    }
  }

  // -- Selection -------------------------------------------------------------

  void updateSelectionState()
  {
    for (auto* r : mRowPtrs)
      r->setSelected(r->node->id == mSelectedId);
  }
};

// New API names � both refer to the same classes.
using glint_tree     = glint_tree;
namespace { struct _glint_tree_reg { _glint_tree_reg() { glint_element::registerElement("tree", []{ return new glint_tree(); }); glint_element::registerElement("tree-row", []{ return new glint_tree_row(); }); } } _glint_tree_reg_; }

