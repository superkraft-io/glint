#pragma once

/**
 * glint_element_layout.hpp
 * Layout engine implementation for glint_element.
 *
 * Included INSIDE the glint_element class body by glint_element.hpp.
 * Contains CSS flex/block/inline layout algorithms, scroll helpers,
 * bottom-up intrinsic measurement, and self-sizing helpers.
 *
 * STATUS: LIVE -- migrated from glint_element.hpp (web-refactor.md step 3).
 */
  // ── Layout helpers ────────────────────────────────────────────────────────

  // Returns true if the child has an explicit cross-axis size (not auto-sized).
  static bool childHasExplicitH(const glint_element& c)
  {
    const auto& r = c.computedStyle.height.raw;
    return !r.empty() && r != "fit-content" && r != "auto";
  }
  static bool childHasExplicitW(const glint_element& c)
  {
    const auto& r = c.computedStyle.width.raw;
    return !r.empty() && r != "fit-content" && r != "auto";
  }
  
  static bool _hasInFlowChildren(const glint_element& c)
  {
    for (const auto& child : c.mChildren)
    {
      if (child->computedStyle.display == "none") continue;
      if (child->computedStyle.position == "absolute") continue;
      return true;
    }
    return false;
  }

  // Returns true when the component has explicit fractional position/size or a
  // non-trivial transform — meaning sub-pixel placement is intentional and we
  // should NOT snap its layout rect to integer pixels.
  static bool _hasSubpixelIntent(const glint_element& c, float parentW, float parentH)
  {
    if (!c.computedStyle.transform.empty() && c.computedStyle.transform != "none") return true;
    auto isFrac    = [](float v)             { return std::abs(v - std::round(v)) > 0.001f; };
    auto isPercent = [](const std::string& r){ return !r.empty() && r.back() == '%'; };
    // Only treat a dimension as intentionally fractional when it is a literal px/raw
    // value — NOT when it is a % that happens to resolve non-integer due to an odd
    // parent size (e.g. "50%" of 13px = 6.5 should still be snapped).
    if (!c.computedStyle.left.raw.empty()   && !isPercent(c.computedStyle.left.raw)   && isFrac(c.computedStyle.left.resolve(parentW)))   return true;
    if (!c.computedStyle.top.raw.empty()    && !isPercent(c.computedStyle.top.raw)    && isFrac(c.computedStyle.top.resolve(parentH)))    return true;
    if (!c.computedStyle.right.raw.empty()  && !isPercent(c.computedStyle.right.raw)  && isFrac(c.computedStyle.right.resolve(parentW)))  return true;
    if (!c.computedStyle.bottom.raw.empty() && !isPercent(c.computedStyle.bottom.raw) && isFrac(c.computedStyle.bottom.resolve(parentH))) return true;
    const auto& wr = c.computedStyle.width.raw;
    if (!wr.empty() && wr != "fit-content" && wr != "auto" && !isPercent(wr)
        && isFrac(c.computedStyle.width.resolve(parentW)))  return true;
    const auto& hr = c.computedStyle.height.raw;
    if (!hr.empty() && hr != "fit-content" && hr != "auto" && !isPercent(hr)
        && isFrac(c.computedStyle.height.resolve(parentH))) return true;
    return false;
  }

  // Snap an LTRB rect so that all four edges fall on integer pixel boundaries.
  // Each edge is rounded independently (matching browser sub-pixel layout behaviour
  // where adjacent elements may differ by 1px due to independent rounding).
  static glint_rect _snapRect(float l, float t, float r, float b)
  {
    return glint_rect(std::round(l), std::round(t), std::round(r), std::round(b));
  }

  // Returns true when this element is "positioned" per CSS spec (position:relative /
  // absolute / fixed / sticky).  Only positioned elements act as a containing block
  // for position:absolute descendants.  "" / "static" are NOT positioned.
  static bool _isPositioned(const glint_element& e)
  {
    const auto& p = e.computedStyle.position;
    return p == "relative" || p == "absolute" || p == "fixed" || p == "sticky";
  }

  static bool _isTableDisplay(const std::string& d)
  {
    return d == "table";
  }

  static bool _isTableRowDisplay(const std::string& d)
  {
    return d == "table-row";
  }

  static bool _isTableCellDisplay(const std::string& d)
  {
    return d == "table-cell";
  }

  template <typename T>
  struct _TableRowInfo
  {
    T* rowBox = nullptr;
    std::vector<T*> cells;
  };

  struct _TableCellMetrics
  {
    bool hasBounds = false;
    float top = 0.f;
    float bottom = 0.f;
    bool hasBaseline = false;
    float baseline = 0.f;
  };

  static void _refreshLayoutStyle(glint_element* node)
  {
    if (!node) return;
    node->computedStyle = node->mergedStyleForLayout();
  }

  static void _refreshLayoutStyle(const glint_element* node)
  {
    if (!node) return;
    auto* mutableNode = const_cast<glint_element*>(node);
    mutableNode->computedStyle = mutableNode->mergedStyleForLayout();
  }

  template <typename T>
  static std::vector<_TableRowInfo<T>> _collectTableRows(T& table)
  {
    std::vector<_TableRowInfo<T>> rows;
    _TableRowInfo<T>* anon = nullptr;

    auto ensureAnon = [&]() -> _TableRowInfo<T>& {
      if (!anon)
      {
        rows.push_back({nullptr, {}});
        anon = &rows.back();
      }
      return *anon;
    };

    for (auto& childPtr : table.mChildren)
    {
      T* child = childPtr.get();
      _refreshLayoutStyle(child);
      if (child->computedStyle.display == "none" || child->computedStyle.position == "absolute")
        continue;

      if (_isTableRowDisplay(child->computedStyle.display))
      {
        anon = nullptr;
        rows.push_back({child, {}});
        auto& row = rows.back();
        for (auto& cellPtr : child->mChildren)
        {
          T* cell = cellPtr.get();
          _refreshLayoutStyle(cell);
          if (cell->computedStyle.display == "none" || cell->computedStyle.position == "absolute")
            continue;
          row.cells.push_back(cell);
        }
        continue;
      }

      ensureAnon().cells.push_back(child);
    }

    return rows;
  }

  static void _translateRect(glint_rect& r, float dx, float dy)
  {
    r = glint_rect(r.L + dx, r.T + dy, r.R + dx, r.B + dy);
  }

  static void _translateSubtree(glint_element* node, float dx, float dy)
  {
    if (!node || (dx == 0.f && dy == 0.f)) return;
    _translateRect(node->mRect, dx, dy);
    _translateRect(node->mPaintRECT, dx, dy);
    for (auto& line : node->mInlineTextRenderLines)
    {
      line.x += dx;
      line.top += dy;
      line.baselineY += dy;
  line.drawBaselineY += dy;
      line.lineBoxTop += dy;
      line.lineBoxBottom += dy;
      line.inkTop += dy;
      line.inkBottom += dy;
    }
    for (auto& child : node->mChildren)
    {
      _refreshLayoutStyle(child.get());
      if (child->computedStyle.display == "none" || child->computedStyle.position == "absolute") continue;
      _translateSubtree(child.get(), dx, dy);
    }
  }

  // Applies a CSS position:relative visual offset to child->mRect / mPaintRECT.
  // top / left shift the box down / right; bottom / right shift it up / left.
  // Must be called AFTER the flow rect is already set.
  static void _applyRelativeOffset(glint_element* child, float parentW, float parentH)
  {
    float ox = 0.f, oy = 0.f;
    // Builder-injected top/left are stale cursor snapshots, not user CSS offsets — skip them.
    if      (!child->computedStyle.left.raw.empty()   && !child->computedStyle.left.builderInjected)   ox =  child->computedStyle.left.resolve(parentW);
    else if (!child->computedStyle.right.raw.empty())                                                   ox = -child->computedStyle.right.resolve(parentW);
    if      (!child->computedStyle.top.raw.empty()    && !child->computedStyle.top.builderInjected)    oy =  child->computedStyle.top.resolve(parentH);
    else if (!child->computedStyle.bottom.raw.empty())                                                  oy = -child->computedStyle.bottom.resolve(parentH);
    if (ox != 0.f || oy != 0.f)
    {
      const glint_rect r = child->mRect;
      child->mRect = child->mPaintRECT = glint_rect(r.L + ox, r.T + oy, r.R + ox, r.B + oy);
    }
  }

  // Returns the padding-box rect of the nearest positioned ancestor of absChild.
  // Per the CSS spec, the containing block for position:absolute is the padding
  // edge of the nearest positioned ancestor — padding is NOT subtracted.
  // Matches Chrome: if no positioned ancestor exists, falls back to the root
  // element (initial containing block = viewport), NOT the direct parent.
  static glint_rect _containingBlockContent(const glint_element* absChild)
  {
    const glint_element* p    = absChild->mParent;
    const glint_element* root = p;  // tracks the topmost ancestor
    while (p)
    {
      if (_isPositioned(*p)) return p->GetPaddingBox();
      root = p;
      p = p->mParent;
    }
    // No positioned ancestor found: initial containing block is the root.
    return root ? root->GetPaddingBox() : glint_rect();
  }

  // Computes the CSS "static position" for an absolutely-positioned child whose
  // direct parent is a flex container, for one axis.
  //
  // Implements the Flexbox spec §9.2 static-position rectangle:
  //   https://www.w3.org/TR/css-flexbox-1/#abspos-items
  //
  // When neither left/right (horizontal=true) nor top/bottom (horizontal=false)
  // is set, Chrome positions the element as if it were the sole in-flow flex
  // item drawn at the parent's alignment.  This replicates that behaviour.
  //
  // Returns the absolute pixel coordinate of the near edge (L or T).
  static float _absFlexStaticPos(const glint_element* child, const glint_rect& cb,
                                  bool horizontal, float size)
  {
    const glint_element* p = child->mParent;
    if (!p) return horizontal ? cb.L : cb.T;

    // Only applies when the direct parent is a flex container.
    if (p->computedStyle.display != "flex") return horizontal ? cb.L : cb.T;

    const bool isRow = (p->computedStyle.flexDirection != "column" &&
                        p->computedStyle.flexDirection != "column-reverse");

    // For horizontal axis: justifyContent in row, alignItems in column.
    // For vertical axis:   alignItems in row,     justifyContent in column.
    const std::string& alignment = horizontal
      ? (isRow ? p->computedStyle.justifyContent : p->computedStyle.alignItems)
      : (isRow ? p->computedStyle.alignItems     : p->computedStyle.justifyContent);

    const glint_rect pc = p->getContent();

    float offset;
    if (alignment == "center")
      offset = (horizontal ? pc.W() : pc.H()) * 0.5f - size * 0.5f;
    else if (alignment == "flex-end")
      offset = (horizontal ? pc.W() : pc.H()) - size;
    else  // flex-start / space-between / space-around / default
      offset = 0.f;

    // Convert parent-content-relative offset to containing-block-absolute coord.
    const float base = horizontal ? (pc.L - cb.L) : (pc.T - cb.T);
    return (horizontal ? cb.L : cb.T) + base + offset;
  }

  // ── Intrinsic (bottom-up) measurement ─────────────────────────────────────
  // When a component has no explicit height/width, measure it from its children.
  // flex-column / block: sum child heights + gaps + vertical padding.
  // flex-row:           max child height  +         vertical padding.

  static float measureIntrinsicH(const glint_element& c, float refW, float refH)
  {
    // Use computedStyle for padding+border — same source getContent() reads.
    const float padT = static_cast<float>(c.computedStyle.paddingTop);
    const float padB = static_cast<float>(c.computedStyle.paddingBottom);
    const float padL = static_cast<float>(c.computedStyle.paddingLeft);
    const float padR = static_cast<float>(c.computedStyle.paddingRight);
    const float brdT = c.computedStyle.resolvedBorderWidth(0);
    const float brdB = c.computedStyle.resolvedBorderWidth(2);
    const float brdL = c.computedStyle.resolvedBorderWidth(3);
    const float brdR = c.computedStyle.resolvedBorderWidth(1);
    const float insetT = padT + brdT;
    const float insetB = padB + brdB;
    const float insetL = padL + brdL;
    const float insetR = padR + brdR;
    const float gap  = c.computedStyle.gap.toFloat();
    const bool  isRow = (c.computedStyle.display == "flex" &&
                         c.computedStyle.flexDirection != "column" &&
                         c.computedStyle.flexDirection != "column-reverse");
    // The content area available to percentage-height children is refH minus
    // this component's own padding+border.  Pass that as parentH so grandchildren
    // with height="50%" resolve against THIS component's content height, not
    // the grandparent's.
    const float contentRefH = std::max(0.f, refH - insetT - insetB);
    // Available content width for this container's children (for text-wrap measurement).
    const float contentRefW = std::max(0.f, refW - insetL - insetR);

    if (_isTableDisplay(c.computedStyle.display))
    {
      const auto rows = _collectTableRows(c);
      const auto cols = _tableColumnWidths(rows, contentRefW, contentRefH, false);
      const auto hs   = _tableRowHeights(rows, cols, contentRefH, false);
      float totalH = 0.f;
      for (float h : hs) totalH += h;
      return insetT + totalH + insetB;
    }

    // Inline formatting context: if ALL in-flow children are inline-level
    // (display:inline / inline-block / inline-flex), Chrome sizes the block to
    // the tallest element in the single line box — NOT the sum of all children.
    // Summing would be wrong and diverge from Chrome.
    if (!isRow) {
      bool allInlineLevel = true;
      int  inFlowCount    = 0;
      for (const auto& child : c.mChildren) {
        if (child->computedStyle.display  == "none")     continue;
        if (child->computedStyle.position == "absolute") continue;
        ++inFlowCount;
        const auto& d = child->computedStyle.display;
        if (d != "inline" && d != "inline-block" && d != "inline-flex") {
          allInlineLevel = false;
          break;
        }
      }
      if (allInlineLevel && inFlowCount > 0) {
        float lineH = 0.f;
        for (const auto& child : c.mChildren) {
          if (child->computedStyle.display  == "none")     continue;
          if (child->computedStyle.position == "absolute") continue;
          const float ch = childPrefH(*child, contentRefH, contentRefW, false);
          lineH = std::max(lineH, ch);
        }
        return insetT + lineH + insetB;
      }
    }

    float result = 0.f;
    int   n      = 0;
    for (const auto& child : c.mChildren)
    {
      if (child->computedStyle.display  == "none")     continue;
      if (child->computedStyle.position == "absolute") continue;
      // Row-flex: child's available width = its own preferred width.
      // Column-flex / block: child stretches to the full container content width.
      const float childW = isRow ? childPrefW(*child, refW) : contentRefW;
      // parentHDefinite=false: the container's height is auto/intrinsic, so per
      // CSS spec §10.5 any percentage height on the child resolves to auto.
      const float ch = childPrefH(*child, contentRefH, childW, false);
      if (isRow)
      {
        // For a row, the cross-axis size (height) contribution includes vertical margins.
        const float mT = child->computedStyle.marginTop.resolve(refW);
        const float mB = child->computedStyle.marginBottom.resolve(refW);
        result = std::max(result, mT + ch + mB);
      }
      else
      {
        // For a column / block, each child stacks: gap (after first) + vertical margins + height.
        const float mT = child->computedStyle.marginTop.resolve(refW);
        const float mB = child->computedStyle.marginBottom.resolve(refW);
        if (n > 0) result += gap;
        result += mT + ch + mB;
      }
      ++n;
    }
    return insetT + result + insetB;
  }

  template <typename T>
  static std::vector<float> _tableColumnWidths(const std::vector<_TableRowInfo<T>>& rows,
                                               float availW, float parentH,
                                               bool expandToAvail)
  {
    std::size_t colCount = 0;
    for (const auto& row : rows)
      colCount = std::max(colCount, row.cells.size());

    std::vector<float> widths(colCount, 0.f);
    for (const auto& row : rows)
    {
      for (std::size_t i = 0; i < row.cells.size(); ++i)
      {
        const float pref = std::max(0.f, childPrefW(*row.cells[i], availW > 0.f ? availW : 0.f));
        widths[i] = std::max(widths[i], pref);
      }
    }

    float total = 0.f;
    for (float w : widths) total += w;
    if (expandToAvail && colCount > 0 && availW > total)
    {
      const float extra = (availW - total) / static_cast<float>(colCount);
      for (float& w : widths) w += extra;
    }
    return widths;
  }

  template <typename T>
  static std::vector<float> _tableRowHeights(const std::vector<_TableRowInfo<T>>& rows,
                                             const std::vector<float>& colWidths,
                                             float parentH,
                                             bool parentHDefinite)
  {
    std::vector<float> heights(rows.size(), 0.f);
    for (std::size_t r = 0; r < rows.size(); ++r)
    {
      float rowH = 0.f;
      for (std::size_t c = 0; c < rows[r].cells.size(); ++c)
      {
        const float cw = c < colWidths.size() ? colWidths[c] : 0.f;
        rowH = std::max(rowH, std::max(0.f, childPrefH(*rows[r].cells[c], parentH, cw, parentHDefinite)));
      }
      heights[r] = rowH;
    }
    return heights;
  }

  static _TableCellMetrics _measureTableCellMetrics(glint_element* cell)
  {
    _TableCellMetrics m;
    if (!cell) return m;

    if (!cell->innerText.empty())
    {
      const float sz = cell->computedStyle.fontSize.toFloat() > 0.f ? cell->computedStyle.fontSize.toFloat() : 12.f;
      SkFont font = skFont(sz,
                           cell->computedStyle.fontFamily.c_str(),
                           cell->computedStyle.fontWeight,
                           cell->computedStyle.fontStyle.c_str());
      // _buildRenderLines self-caches and short-circuits when nothing has
      // changed; the assignment here just keeps cell->mInlineTextRenderLines
      // in sync for the metric/baseline reads below.
      cell->mInlineTextRenderLines = cell->_buildRenderLines(font);
    }

    for (const auto& line : cell->mInlineTextRenderLines)
    {
      const float lt = line.top;
      const float lb = line.top + line.lineHeight;
      if (!m.hasBounds)
      {
        m.top = lt; m.bottom = lb; m.hasBounds = true;
      }
      else
      {
        m.top = std::min(m.top, lt);
        m.bottom = std::max(m.bottom, lb);
      }
      if (!m.hasBaseline)
      {
        m.hasBaseline = true;
        m.baseline = line.baselineY;
      }
    }

    for (auto& child : cell->mChildren)
    {
      _refreshLayoutStyle(child.get());
      if (child->computedStyle.display == "none" || child->computedStyle.position == "absolute") continue;
      const glint_rect childRect = child->GetPaintRECT();
      if (!m.hasBounds)
      {
        m.top = childRect.T;
        m.bottom = childRect.B;
        m.hasBounds = true;
      }
      else
      {
        m.top = std::min(m.top, childRect.T);
        m.bottom = std::max(m.bottom, childRect.B);
      }

      if (!m.hasBaseline && !child->mInlineTextRenderLines.empty())
      {
        m.hasBaseline = true;
        m.baseline = child->mInlineTextRenderLines.front().baselineY;
      }
      if (!m.hasBaseline)
      {
        m.hasBaseline = true;
        m.baseline = childRect.B;
      }
    }

    if (!m.hasBaseline && m.hasBounds)
    {
      m.hasBaseline = true;
      m.baseline = m.bottom;
    }

    return m;
  }

  static void _alignTableCellContent(glint_element* cell, float rowBaselineY = 0.f, bool useBaseline = false)
  {
    if (!cell) return;

    const auto metrics = _measureTableCellMetrics(cell);
    if (!metrics.hasBounds) return;

    const glint_rect content = cell->getContent();

    std::string v = cell->computedStyle.verticalAlign;
    std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    float dy = 0.f;
    if (useBaseline && (v.empty() || v == "baseline"))
      dy = rowBaselineY - metrics.baseline;
    else if (v == "middle")
      dy = (content.T + content.B - metrics.top - metrics.bottom) * 0.5f;
    else if (v == "bottom" || v == "text-bottom")
      dy = content.B - metrics.bottom;
    else
      dy = content.T - metrics.top;

    if (std::abs(dy) < 0.01f) return;

    for (auto& line : cell->mInlineTextRenderLines)
    {
      line.top += dy;
      line.baselineY += dy;
  line.drawBaselineY += dy;
      line.lineBoxTop += dy;
      line.lineBoxBottom += dy;
      line.inkTop += dy;
      line.inkBottom += dy;
    }
    for (auto& child : cell->mChildren)
    {
      _refreshLayoutStyle(child.get());
      if (child->computedStyle.display == "none" || child->computedStyle.position == "absolute") continue;
      _translateSubtree(child.get(), 0.f, dy);
    }
  }

  static float measureIntrinsicW(const glint_element& c, float refW, float refH,
                                 bool refWDefinite = true)
  {
    // Use computedStyle for padding+border — same source getContent() reads.
    const float padL = static_cast<float>(c.computedStyle.paddingLeft);
    const float padR = static_cast<float>(c.computedStyle.paddingRight);
    const float brdL = c.computedStyle.resolvedBorderWidth(3);
    const float brdR = c.computedStyle.resolvedBorderWidth(1);
    const float insetL = padL + brdL;
    const float insetR = padR + brdR;
    const float gap  = c.computedStyle.gap.toFloat();
    const bool  isRow = (c.computedStyle.display == "flex" &&
                         c.computedStyle.flexDirection != "column" &&
                         c.computedStyle.flexDirection != "column-reverse");
    const float contentRefW = std::max(0.f, refW - insetL - insetR);

    if (_isTableDisplay(c.computedStyle.display))
    {
      const auto rows = _collectTableRows(c);
      const auto cols = _tableColumnWidths(rows, contentRefW, refH, false);
      float totalW = 0.f;
      for (float w : cols) totalW += w;
      return insetL + totalW + insetR;
    }

    float result = 0.f;
    int   n      = 0;
    for (const auto& child : c.mChildren)
    {
      if (child->computedStyle.display  == "none")     continue;
      if (child->computedStyle.position == "absolute") continue;
      const float cw = childPrefW(*child, contentRefW, refWDefinite);
      const float mL = child->computedStyle.marginLeft.resolve(contentRefW);
      const float mR = child->computedStyle.marginRight.resolve(contentRefW);
      if (isRow)
      {
        if (n > 0) result += gap;
        result += mL + cw + mR;
      }
      else
        result = std::max(result, mL + cw + mR);
      ++n;
    }
    return insetL + result + insetR;
  }

  // Preferred child height: explicit style.height if set, else intrinsic from
  // children (recursive), else preferredH() virtual, else last-known mRect.H().
  //
  // CSS border-box model for ABSOLUTE sizes (px / raw float) — matches Chrome:
  //   declared height = total box. Content = max(0, height - padT - padB).
  //   When padding >= declared height the box grows to exactly padT+padB
  //   (content clamps to 0), replicating the Chrome overflow behaviour.
  // Percentage sizes are also border-box (% already means "fraction of parent"):
  //   total = % resolved, content = total - padding (no extra expansion).
  //
  // parentHDefinite: true  → the containing block has an explicit/definite height;
  //                           percentage heights resolve normally (layout pass).
  //                  false → the containing block's height is auto/intrinsic
  //                           (measureIntrinsicH pass); per CSS spec §10.5
  //                           percentage heights resolve to 'auto' to avoid the
  //                           circular dependency that would inflate the parent.
  static float childPrefH(const glint_element& c, float parentH, float parentW = 0.f,
                           bool parentHDefinite = true)
  {
    float h;
    if (childHasExplicitH(c))
    {
      const float resolved = c.computedStyle.height.resolve(parentH);
      const auto& raw = c.computedStyle.height.raw;
      const bool isPercent = (!raw.empty() && raw.back() == '%');
      if (!isPercent)
      {
        // Absolute size: border-box — declared height is the total box.
        // Padding+border squeeze content; box grows only when they overflow.
        const float padT = static_cast<float>(c.computedStyle.paddingTop);
        const float padB = static_cast<float>(c.computedStyle.paddingBottom);
        const float brdT = c.computedStyle.resolvedBorderWidth(0);
        const float brdB = c.computedStyle.resolvedBorderWidth(2);
        h = std::max(resolved, padT + padB + brdT + brdB);
      }
      else if (parentHDefinite)
        h = resolved;  // Percentage: declared value = total box height (border-box).
      else
      {
        // CSS spec §10.5: if the containing block's height is not explicit
        // (auto/intrinsic), a percentage height resolves to 'auto'.  Fall
        // through to intrinsic / preferredH() sizing below.
        goto computeIntrinsic;
      }
    }
    else
    {
      computeIntrinsic:
      if (!c.mChildren.empty() && _hasInFlowChildren(c))
      {
        // Resolve the container's OWN width first so that text inside a column-flex
        // card wraps at the card's actual pixel width, not at the full grandparent
        // width.  childPrefW honours explicit px/% widths; for auto/fit-content it
        // returns the intrinsic content width, which is the best pre-flex estimate.
        const float grandW = parentW > 0.f ? parentW : c.mParentW;
        const float selfW  = grandW > 0.f  ? childPrefW(c, grandW) : grandW;
        h = measureIntrinsicH(c, selfW > 0.f ? selfW : grandW, parentH);
      }
      else
      {
        // Leaf node: ask the component (e.g. glint_element measures its text).
        // preferredH() returns the *content* height — add padding so the returned
        // total height matches what measureIntrinsicH() does for non-leaf nodes.
        // Compute available content width so word-wrapping is accounted for.
        float availW = 0.f;
        if (parentW > 0.f)
        {
          const float padL = static_cast<float>(c.computedStyle.paddingLeft);
          const float padR = static_cast<float>(c.computedStyle.paddingRight);
          const float brdL = c.computedStyle.resolvedBorderWidth(3);
          const float brdR = c.computedStyle.resolvedBorderWidth(1);
          const auto& wr = c.computedStyle.width.raw;
          if (!wr.empty() && wr != "auto" && wr != "fit-content")
            availW = std::max(0.f, c.computedStyle.width.resolve(parentW) - padL - padR - brdL - brdR);
          else
            availW = std::max(0.f, parentW - padL - padR - brdL - brdR);
        }
        const float pref = c.preferredH(availW);
        if (pref > 0.f)
        {
          const float padT = static_cast<float>(c.computedStyle.paddingTop);
          const float padB = static_cast<float>(c.computedStyle.paddingBottom);
          const float brdT = c.computedStyle.resolvedBorderWidth(0);
          const float brdB = c.computedStyle.resolvedBorderWidth(2);
          h = padT + brdT + pref + padB + brdB;
        }
        else
          // Stale fallback: use the tight paint rect, not the filter-inflated
          // visual rect, so filter spread never feeds back into intrinsic layout.
          h = c.GetPaintRECT().H();
      }
    }
    // Apply CSS min-height / max-height clamping (border-box sizing, matches Chrome).
    // % values on min/max-height resolve against parentH (same axis as height).
    const auto& mnhr = c.computedStyle.minHeight.raw;
    const auto& mxhr = c.computedStyle.maxHeight.raw;
    if (!mnhr.empty()) h = std::max(h, c.computedStyle.minHeight.resolve(parentH));
    if (!mxhr.empty()) h = std::min(h, c.computedStyle.maxHeight.resolve(parentH));
    return h;
  }

  // Preferred child width: explicit style.width if set, else intrinsic from
  // children (recursive), else preferredW() virtual, else last-known mRect.W().
  //
  // CSS border-box model for ABSOLUTE sizes (px / raw float) — matches Chrome:
  //   declared width = total box. Content = max(0, width - padL - padR).
  //   When padding >= declared width the box grows to exactly padL+padR.
  // Percentage sizes are also border-box (% already means "fraction of parent"):
  //   total = % resolved, content = total - padding (no extra expansion).
  static float childPrefW(const glint_element& c, float parentW,
                          bool parentWDefinite = true)
  {
    float w;
    const auto& r = c.computedStyle.width.raw;
    if (!r.empty() && r != "fit-content" && r != "auto")
    {
      const float resolved = c.computedStyle.width.resolve(parentW);
      const bool isPercent = (r.back() == '%');
      if (!isPercent)
      {
        // Absolute size: border-box — declared width is the total box.
        // Padding+border squeeze content; box grows only when they overflow.
        const float padL = static_cast<float>(c.computedStyle.paddingLeft);
        const float padR = static_cast<float>(c.computedStyle.paddingRight);
        const float brdL = c.computedStyle.resolvedBorderWidth(3);
        const float brdR = c.computedStyle.resolvedBorderWidth(1);
        w = std::max(resolved, padL + padR + brdL + brdR);
      }
      else if (parentWDefinite)
        w = resolved;  // Percentage: declared value = total box width (border-box).
      else
      {
        goto computeIntrinsic;
      }
    }
    else
    {
      computeIntrinsic:
      if (!c.mChildren.empty() && _hasInFlowChildren(c))
      {
        // CSS width percentages behave like auto when the containing block width
        // is itself intrinsic/indefinite, avoiding circular inflation during
        // flex-item shrink-to-fit measurement.
        w = measureIntrinsicW(c, parentW, c.mParentH, false);
      }
      else
      {
        // Leaf node: ask the component (e.g. glint_element measures its text).
        // preferredW() returns the *content* width — add padding so the returned
        // total width matches what measureIntrinsicW() does for non-leaf nodes.
        const float pref = c.preferredW();
        if (pref > 0.f)
        {
          const float padL = static_cast<float>(c.computedStyle.paddingLeft);
          const float padR = static_cast<float>(c.computedStyle.paddingRight);
          const float brdL = c.computedStyle.resolvedBorderWidth(3);
          const float brdR = c.computedStyle.resolvedBorderWidth(1);
          w = padL + brdL + pref + padR + brdR;
        }
        else
          // Stale fallback: use the tight paint rect, not the filter-inflated
          // visual rect, so filter spread never feeds back into intrinsic layout.
          w = c.GetPaintRECT().W();
      }
    }
    // Apply CSS min-width / max-width clamping (border-box sizing, matches Chrome).
    // % values on min/max-width resolve against parentW (same axis as width).
    const auto& mnwr = c.computedStyle.minWidth.raw;
    const auto& mxwr = c.computedStyle.maxWidth.raw;
    if (!mnwr.empty()) w = std::max(w, c.computedStyle.minWidth.resolve(parentW));
    if (!mxwr.empty()) w = std::min(w, c.computedStyle.maxWidth.resolve(parentW));
    return w;
  }

  void layoutFlex(glint_canvas* g, const glint_rect& content, float cW, float cH)
  {
    // Note: text-line caches on children are NOT cleared here. _buildRenderLines
    // self-invalidates when its inputs (text, font, content rect) change.

    const bool  isRow     = (computedStyle.flexDirection != "column" && computedStyle.flexDirection != "column-reverse");
    const float mainAvail = isRow ? cW : cH;
    const float crsAvail  = isRow ? cH : cW;
    const float gap       = computedStyle.gap.toFloat();
    const bool  doStretch = (computedStyle.alignItems == "stretch");

    struct CI { glint_element* node; float main, cross, m1, m2, c1, c2, grow, extra; };
    std::vector<CI> infos;

    for (auto& child : mChildren)
    {
      _refreshLayoutStyle(child.get());
      if (child->computedStyle.display   == "none")     continue;
      if (child->computedStyle.position  == "absolute") continue;
      CI ci;
      ci.node  = child.get();
      ci.extra = 0.f;
      ci.grow  = child->computedStyle.flexGrow;
      const float w = childPrefW(*child, cW);
      // For height, pass the child's available width so text wrapping is accounted for:
      // row-flex: child's own preferred width; column-flex: full container content width.
      const float h = childPrefH(*child, cH, isRow ? w : cW);
      if (isRow) {
        ci.m1 = child->computedStyle.marginLeft.resolve(cW);  ci.m2 = child->computedStyle.marginRight.resolve(cW);
        ci.c1 = child->computedStyle.marginTop.resolve(cW);   ci.c2 = child->computedStyle.marginBottom.resolve(cW);
        ci.main  = w;
        // stretch: fill cross axis if child has no explicit height
        ci.cross = (doStretch && !childHasExplicitH(*child))
                   ? std::max(0.f, crsAvail - ci.c1 - ci.c2)
                   : h;
      } else {
        ci.m1 = child->computedStyle.marginTop.resolve(cW);   ci.m2 = child->computedStyle.marginBottom.resolve(cW);
        ci.c1 = child->computedStyle.marginLeft.resolve(cW);  ci.c2 = child->computedStyle.marginRight.resolve(cW);
        ci.main  = h;
        // stretch: fill cross axis if child has no explicit width
        ci.cross = (doStretch && !childHasExplicitW(*child))
                   ? std::max(0.f, crsAvail - ci.c1 - ci.c2)
                   : w;
      }
      infos.push_back(ci);
    }

    // Total fixed main size (before flex-grow)
    float totalMain = 0.f;
    for (const auto& ci : infos) totalMain += ci.m1 + ci.main + ci.m2;
    if (!infos.empty()) totalMain += gap * (float)(infos.size() - 1);

    // Distribute flex-grow
    float totalGrow = 0.f;
    for (const auto& ci : infos) totalGrow += ci.grow;
    if (totalGrow > 0.f)
    {
      // Distribute surplus (grow) or deficit (shrink) among flex-grow items.
      // Negative freeSpace means the container is too small; items with
      // flexGrow > 0 shrink proportionally (using their grow weight).
      const float freeSpace = mainAvail - totalMain;
      for (auto& ci : infos)
        ci.extra = ci.grow / totalGrow * freeSpace;
    }

    // justify-content: cursor start
    const float originMain = isRow ? content.L : content.T;
    const float originCrs  = isRow ? content.T : content.L;
    float cursor = originMain, itemSpacing = gap;
    if (totalGrow == 0.f)
    {
      const auto& jc = computedStyle.justifyContent;
      const float  n = (float)infos.size();
      if (jc == "center")
        cursor = originMain + (mainAvail - totalMain) * 0.5f;
      else if (jc == "flex-end")
        cursor = originMain + mainAvail - totalMain;
      else if (jc == "space-between" && infos.size() > 1)
        itemSpacing = (mainAvail - totalMain + gap * (n - 1.f)) / (n - 1.f);
      else if (jc == "space-around") {
        const float e = (mainAvail - totalMain) / n;
        cursor = originMain + e * 0.5f; itemSpacing = gap + e;
      }
      else if (jc == "space-evenly") {
        const float e = (mainAvail - totalMain) / (n + 1.f);
        cursor = originMain + e; itemSpacing = gap + e;
      }
    }

    // Position and recurse
    const auto& ai = computedStyle.alignItems;
    for (auto& ci : infos)
    {
      cursor += ci.m1;
      float crossPos = originCrs + ci.c1;
      // align-items: center — center the margin box, then offset by the cross-start margin.
      // Per CSS Flexbox spec §9.6.2: free space = crsAvail - (c1 + cross + c2);
      // margin-box-start = originCrs + free/2; content-box-start = margin-box-start + c1.
      // Simplified: originCrs + (crsAvail - cross + c1 - c2) / 2
      if (ai == "center")        crossPos = originCrs + (crsAvail - ci.cross + ci.c1 - ci.c2) * 0.5f;
      else if (ai == "flex-end") crossPos = originCrs + crsAvail - ci.cross - ci.c2;
      // "stretch": crossPos stays at originCrs + ci.c1 (already the default)
      const float mx = isRow ? cursor   : crossPos;
      const float my = isRow ? crossPos : cursor;
      const float mainSz = std::max(0.f, ci.main + ci.extra);  // clamp: shrink can't go negative
      const float mw = std::max(0.f, isRow ? mainSz : ci.cross);
      const float mh = std::max(0.f, isRow ? ci.cross : mainSz);
      if (_hasSubpixelIntent(*ci.node, cW, cH))
        ci.node->mRect = ci.node->mPaintRECT = glint_rect(mx, my, mx + mw, my + mh);
      else
        ci.node->mRect = ci.node->mPaintRECT = _snapRect(mx, my, mx + mw, my + mh);
      // Apply position:relative visual offset — shifts box from its flex slot
      // without affecting the flex cursor (siblings see the original slot size).
      if (ci.node->computedStyle.position == "relative")
        _applyRelativeOffset(ci.node, cW, cH);
      cursor += mainSz + ci.m2 + itemSpacing;
      ci.node->Layout(g);
    }

    // Absolute children — positioned against their CSS containing block
    // (nearest positioned ancestor, per CSS spec; falls back to direct parent).
    // Browser CSS rules: left wins over right; top wins over bottom.
    // Scrollbar children are excluded — _positionScrollbars() owns their rects.
    for (auto& child : mChildren)
    {
      _refreshLayoutStyle(child.get());
      if (child->computedStyle.position != "absolute") continue;
      if (child.get() == mScrollbarV || child.get() == mScrollbarH || child.get() == mScrollCorner) continue;
      const glint_rect cb  = _containingBlockContent(child.get());
      const float cbW = cb.W(), cbH = cb.H();
      const float w = std::max(0.f, childPrefW(*child, cbW));
      const float h = std::max(0.f, childPrefH(*child, cbH, cbW));
      // X: left → right → static-position (Chrome flex behaviour)
      float l;
      if      (!child->computedStyle.left.raw.empty())  l = cb.L + child->computedStyle.left.resolve(cbW);
      else if (!child->computedStyle.right.raw.empty()) l = cb.L + cbW - child->computedStyle.right.resolve(cbW) - w;
      else                                       l = _absFlexStaticPos(child.get(), cb, true,  w);
      // Y: top → bottom → static-position (Chrome flex behaviour)
      float t;
      if      (!child->computedStyle.top.raw.empty())    t = cb.T + child->computedStyle.top.resolve(cbH);
      else if (!child->computedStyle.bottom.raw.empty()) t = cb.T + cbH - child->computedStyle.bottom.resolve(cbH) - h;
      else                                        t = _absFlexStaticPos(child.get(), cb, false, h);
      if (_hasSubpixelIntent(*child, cbW, cbH))
        child->mRect = child->mPaintRECT = glint_rect(l, t, l + w, t + h);
      else
        child->mRect = child->mPaintRECT = _snapRect(l, t, l + w, t + h);
      child->Layout(g);
    }
  }

  // ── Inline formatting context ─────────────────────────────────────────────
  // Called from layoutBlock() when any in-flow child has display:"inline" or
  // carries innerText. Implements a greedy line-breaking algorithm:
  //   • Inline children (display:"inline" or empty display with innerText) are
  //     packed left-to-right on the current line. When a child's width would
  //     overflow the container it wraps to the next line.
  //   • For children with `innerText` (and whiteSpace != "nowrap") the text is
  //     split at word boundaries; each word is measured with Skia and packed
  //     individually. The parent's mRect for that child covers all its lines
  //     (full width × totalLineHeight); drawContent() renders word-wrapped text.
  //   • Block-level children (display:"block"|"flex", or display:"" with child
  //     elements) flush the current line and are placed at full container width.
  //   • Absolute children are positioned last, same as layoutBlock/Flex.
  void layoutInline(glint_canvas* g, const glint_rect& rect, float rW, float rH)
  {
    float cursorX  = 0.f;
    float cursorY  = 0.f;
    float lineMaxH = 0.f;

    struct _InlineLineItem
    {
      glint_element* node = nullptr;
      float x = 0.f;
      float w = 0.f;
      float h = 0.f;           // line-box contribution height (lineHeight for display:inline; full box for inline-block)
      float baselineFromTop = 0.f;
      float drawBaselineFromTop = 0.f;
      float lineHeightRef = 0.f;
      bool  subpixel = false;
      std::string verticalAlign;
      bool  isTextFragment = false;
      std::string text;
      int byteStart = 0;
      int byteEnd = 0;
      bool  hasInkBounds = false;
      float inkTopAboveBaseline = 0.f;
      float inkBottomBelowBaseline = 0.f;
      // For display:inline elements with padding/border: the paint rect extends
      // above/below the line-box rect by these amounts (per CSS2.1 §10.8.1).
      // display:inline-block keeps these at 0 (padding is included in h).
      float inlinePadT = 0.f;
      float inlinePadB = 0.f;
    };
    std::vector<_InlineLineItem> lineItems;

    struct _InlineTextBounds
    {
      bool  hasBounds = false;
      bool  subpixel = false;
      float l = 0.f;
      float t = 0.f;
      float r = 0.f;
      float b = 0.f;
    };
    std::map<glint_element*, _InlineTextBounds> textBounds;

    for (auto& child : mChildren)
      child->_clearInlineTextRenderLines();

    struct _InlineFontMetrics
    {
      float lineHeight = 0.f;
      float textTopAbove = 0.f;
      float textBottomBelow = 0.f;
      float xHalf = 0.f;
      float inkAscent = 0.f;
    };

    struct _InlineInkMetrics
    {
      bool hasBounds = false;
      float topAboveBaseline = 0.f;
      float bottomBelowBaseline = 0.f;
    };

    auto skWordW = [&](const std::string& word, float sz) -> float {
      if (word.empty()) return 0.f;
      SkFont font = skFont(sz);
      SkRect bounds;
      return font.measureText(word.c_str(), word.size(), SkTextEncoding::kUTF8, &bounds);
    };

    auto skTextW = [&](const SkFont& font, const std::string& word) -> float {
      if (word.empty()) return 0.f;
      SkRect bounds;
      return font.measureText(word.c_str(), word.size(), SkTextEncoding::kUTF8, &bounds);
    };

    auto resolveFontMetrics = [&](const glint_style& s, const glint_style* fallbackStyle) -> _InlineFontMetrics {
      const glint_style& fallback = fallbackStyle ? *fallbackStyle : s;
      const float sz = s.fontSize.toFloat() > 0.f ? s.fontSize.toFloat()
                                                  : (fallback.fontSize.toFloat() > 0.f ? fallback.fontSize.toFloat() : 12.f);
      const float weight = s.fontWeight > 0.f ? s.fontWeight : fallback.fontWeight;
      const std::string family = !s.fontFamily.empty() ? s.fontFamily : fallback.fontFamily;
      const std::string style  = !s.fontStyle.empty()  ? s.fontStyle  : fallback.fontStyle;

      SkFont font = skFont(sz, family.c_str(), weight, style.c_str());
      SkFontMetrics metrics;
      font.getMetrics(&metrics);

      const float ascent  = std::max(0.f, -metrics.fAscent);
      const float descent = std::max(0.f,  metrics.fDescent);
      const float leading = std::max(0.f, metrics.fLeading);
      float lineHeight = s.lineHeight > 0.f ? sz * s.lineHeight
                                            : (fallbackStyle && fallback.lineHeight > 0.f
                                                 ? sz * fallback.lineHeight
                     : ascent + descent + leading);
      if (lineHeight <= 0.f) lineHeight = ascent + descent + leading;
      const float halfLeading = std::max(0.f, lineHeight - (ascent + descent)) * 0.5f;

      SkRect inkBounds;
      const std::string probe = "Mg";
      font.measureText(probe.c_str(), probe.size(), SkTextEncoding::kUTF8, &inkBounds);

      _InlineFontMetrics fm;
      fm.lineHeight      = lineHeight;
      fm.textTopAbove    = halfLeading + ascent;
      fm.textBottomBelow = std::max(0.f, lineHeight - fm.textTopAbove);
      fm.xHalf           = (metrics.fXHeight > 0.f ? metrics.fXHeight : sz * 0.5f) * 0.5f;
      fm.inkAscent       = inkBounds.height() > 0.f ? -inkBounds.top() : fm.textTopAbove;
      return fm;
    };

    // Resolve the parent element's effective inherited font properties via CSS
    // inheritance.  Chrome defaults to 16px and inherits font-size, font-family,
    // font-weight and font-style down from ancestors; we replicate that here.
    // These values drive the line-box STRUT and are also the fallback metrics
    // used for any child that has no explicit font properties of its own.
    float       _inheritedSz     = computedStyle.fontSize.toFloat();
    std::string _inheritedFamily = computedStyle.fontFamily;
    float       _inheritedWeight = computedStyle.fontWeight;
    std::string _inheritedFStyle = computedStyle.fontStyle;
    if (_inheritedSz <= 0.f || _inheritedFamily.empty() || _inheritedWeight <= 0)
    {
      for (const glint_element* _p = mParent; _p; _p = _p->mParent)
      {
        if (_inheritedSz <= 0.f)
        {
          const float _psz = _p->computedStyle.fontSize.toFloat();
          if (_psz > 0.f) _inheritedSz = _psz;
        }
        if (_inheritedFamily.empty() && !_p->computedStyle.fontFamily.empty())
          _inheritedFamily = _p->computedStyle.fontFamily;
        if (_inheritedWeight <= 0.f && _p->computedStyle.fontWeight > 0.f)
          _inheritedWeight = _p->computedStyle.fontWeight;
        if (_inheritedFStyle.empty() && !_p->computedStyle.fontStyle.empty())
          _inheritedFStyle = _p->computedStyle.fontStyle;
        // Stop walking when all four are resolved
        if (_inheritedSz > 0.f && !_inheritedFamily.empty() &&
            _inheritedWeight > 0.f && !_inheritedFStyle.empty())
          break;
      }
    }
    if (_inheritedSz <= 0.f) _inheritedSz = 16.f; // CSS browser default

    glint_style _parentStyleForStrut = computedStyle;
    if (computedStyle.fontSize.toFloat() <= 0.f)
      _parentStyleForStrut.fontSize = _inheritedSz;
    if (computedStyle.fontFamily.empty() && !_inheritedFamily.empty())
      _parentStyleForStrut.fontFamily = _inheritedFamily;
    if (computedStyle.fontWeight <= 0.f && _inheritedWeight > 0.f)
      _parentStyleForStrut.fontWeight = _inheritedWeight;
    if (computedStyle.fontStyle.empty() && !_inheritedFStyle.empty())
      _parentStyleForStrut.fontStyle = _inheritedFStyle;
    const _InlineFontMetrics parentFontMetrics = resolveFontMetrics(_parentStyleForStrut, nullptr);

    auto resolveChildFont = [&](const glint_element* child, float sz) -> SkFont {
      const std::string family = !child->computedStyle.fontFamily.empty()
                                 ? child->computedStyle.fontFamily : _inheritedFamily;
      const float weight = child->computedStyle.fontWeight > 0.f
                         ? child->computedStyle.fontWeight : _inheritedWeight;
      const std::string style = !child->computedStyle.fontStyle.empty()
                                ? child->computedStyle.fontStyle : _inheritedFStyle;
      return skFont(sz, family.c_str(), weight, style.c_str());
    };

    auto measureInkMetrics = [&](const SkFont& font,
                                 const std::string& text,
                                 const _InlineFontMetrics& fm) -> _InlineInkMetrics {
      if (text.empty()) return {};
      SkRect bounds;
      font.measureText(text.c_str(), text.size(), SkTextEncoding::kUTF8, &bounds);
      _InlineInkMetrics ink;
      ink.hasBounds = true;
      if (bounds.height() > 0.f)
      {
        ink.topAboveBaseline = -bounds.top();
        ink.bottomBelowBaseline = bounds.bottom();
      }
      else
      {
        ink.topAboveBaseline = fm.textTopAbove;
        ink.bottomBelowBaseline = fm.textBottomBelow;
      }
      return ink;
    };

    auto centeredBaselineFromTop = [&](float boxH, const _InlineInkMetrics& ink, float fallbackBaselineFromTop) -> float {
      if (!ink.hasBounds) return fallbackBaselineFromTop;
      const float inkSpan = std::max(0.f, ink.topAboveBaseline + ink.bottomBelowBaseline);
      const float topLead = std::max(0.f, boxH - inkSpan) * 0.5f;
      return topLead + ink.topAboveBaseline;
    };

    auto lower = [](std::string v) {
      std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      return v;
    };

    auto resolveShiftY = [&](const _InlineLineItem& item) -> std::pair<bool, float> {
      const std::string v = lower(item.verticalAlign);
      if (v.empty() || v == "baseline") return { true, 0.f };
      if (v == "super") return { true, -std::max(1.f, item.lineHeightRef * 0.34f) };
      if (v == "sub")   return { true,  std::max(1.f, item.lineHeightRef * 0.20f) };

      if (!v.empty())
      {
        try
        {
          if (v.size() > 1 && v.substr(v.size() - 2) == "px")
            return { true, -std::stof(v.substr(0, v.size() - 2)) };
          if (v.back() == '%')
            return { true, -(std::stof(v.substr(0, v.size() - 1)) * 0.01f * item.lineHeightRef) };
          const char last = v.back();
          if ((last >= '0' && last <= '9') || last == '.')
            return { true, -std::stof(v) };
        }
        catch (...) {}
      }
      return { false, 0.f };
    };

    auto flushLine = [&]() {
      if (lineItems.empty()) return;

      float lineAbove = std::max(parentFontMetrics.textTopAbove, 0.f);
      float lineBelow = std::max(parentFontMetrics.textBottomBelow, 0.f);

      for (const auto& item : lineItems)
      {
        const std::string v = lower(item.verticalAlign);
        if (v == "top" || v == "bottom") continue;

        float above = item.baselineFromTop;
        float below = std::max(0.f, item.h - item.baselineFromTop);

        if (v == "middle")
        {
          above = std::max(0.f, item.h * 0.5f - parentFontMetrics.xHalf);
          below = std::max(0.f, item.h - above);
        }
        else if (v == "text-top")
        {
          above = std::max(0.f, parentFontMetrics.textTopAbove);
          below = std::max(0.f, item.h - above);
        }
        else if (v == "text-bottom")
        {
          below = std::max(0.f, parentFontMetrics.textBottomBelow);
          above = std::max(0.f, item.h - below);
        }
        else
        {
          const auto shift = resolveShiftY(item);
          if (shift.first)
          {
            above = std::max(0.f, item.baselineFromTop - shift.second);
            below = std::max(0.f, item.h - item.baselineFromTop + shift.second);
          }
        }

        lineAbove = std::max(lineAbove, above);
        lineBelow = std::max(lineBelow, below);
      }

      for (int i = 0; i < 3; ++i)
      {
        bool changed = false;
        for (const auto& item : lineItems)
        {
          const std::string v = lower(item.verticalAlign);
          if (v == "top")
          {
            const float needBelow = std::max(0.f, item.h - lineAbove);
            if (needBelow > lineBelow) { lineBelow = needBelow; changed = true; }
          }
          else if (v == "bottom")
          {
            const float needAbove = std::max(0.f, item.h - lineBelow);
            if (needAbove > lineAbove) { lineAbove = needAbove; changed = true; }
          }
        }
        if (!changed) break;
      }

      const float lineTop      = cursorY;
      const float lineBaseline = lineTop + lineAbove;
      const float lineBottom   = lineBaseline + lineBelow;

      for (auto& item : lineItems)
      {
        const std::string v = lower(item.verticalAlign);
        float top = lineBaseline - item.baselineFromTop;

        if (v == "top")
          top = lineTop;
        else if (v == "bottom")
          top = lineBottom - item.h;
        else if (v == "middle")
          top = lineBaseline + parentFontMetrics.xHalf - item.h * 0.5f;
        else if (v == "text-top")
          top = lineBaseline - parentFontMetrics.textTopAbove;
        else if (v == "text-bottom")
          top = lineBaseline + parentFontMetrics.textBottomBelow - item.h;
        else
        {
          const auto shift = resolveShiftY(item);
          if (shift.first)
            top += shift.second;
        }

        const float l = rect.L + item.x;
        const float t = rect.T + top;
        if (item.isTextFragment)
        {
          const float absLineBoxTop = rect.T + lineTop;
          const float absLineBoxBottom = rect.T + lineBottom;
          const float baselineY = t + item.baselineFromTop;
          const float drawBaselineY = t + item.drawBaselineFromTop;
          const float inkTop = item.hasInkBounds
            ? (drawBaselineY - item.inkTopAboveBaseline)
            : t;
          const float inkBottom = item.hasInkBounds
            ? (drawBaselineY + item.inkBottomBelowBaseline)
            : (t + item.h);
          item.node->mInlineTextRenderLines.push_back({
            item.text,
            item.byteStart,
            item.byteEnd,
            l,
            t,
            baselineY,
            drawBaselineY,
            item.h,
            item.w,
            absLineBoxTop,
            absLineBoxBottom,
            inkTop,
            inkBottom
          });
          auto& bounds = textBounds[item.node];
          const float boundsTop = rect.T + lineTop;
          const float boundsBottom = rect.T + lineBottom;
          if (!bounds.hasBounds)
          {
            bounds.hasBounds = true;
            bounds.subpixel = item.subpixel;
            bounds.l = l; bounds.t = boundsTop; bounds.r = l + item.w; bounds.b = boundsBottom;
          }
          else
          {
            bounds.subpixel = bounds.subpixel || item.subpixel;
            bounds.l = std::min(bounds.l, l);
            bounds.t = std::min(bounds.t, boundsTop);
            bounds.r = std::max(bounds.r, l + item.w);
            bounds.b = std::max(bounds.b, boundsBottom);
          }
        }
        else
        {
          // For display:inline elements with padding/border, the paint rect extends
          // above/below the line-box rect — padding overflows without widening the
          // line box (CSS2.1 §10.8.1).  inlinePadT/B carry that extra extent.
          const float paintT = t - item.inlinePadT;
          const float paintH = item.h + item.inlinePadT + item.inlinePadB;
          if (item.subpixel)
            item.node->mRect = item.node->mPaintRECT = glint_rect(l, paintT, l + item.w, paintT + paintH);
          else
            item.node->mRect = item.node->mPaintRECT = _snapRect(l, paintT, l + item.w, paintT + paintH);
          if (item.node->computedStyle.position == "relative") _applyRelativeOffset(item.node, rW, rH);
          item.node->Layout(g);
        }
      }

      cursorY = lineBottom;
      cursorX = 0.f;
      lineMaxH = 0.f;
      lineItems.clear();
    };

    for (auto& childPtr : mChildren)
    {
      glint_element* child = childPtr.get();
      _refreshLayoutStyle(child);
      if (child->computedStyle.display == "none") continue;
      if (child->computedStyle.position == "absolute") continue;

      const auto& disp = child->computedStyle.display;

      const bool isBlockLevel =
        (disp == "block" || disp == "flex") ||
        (disp != "inline" && child->innerText.empty() && !child->mChildren.empty());

      if (isBlockLevel)
      {
        flushLine();
        const float cw = std::max(0.f, childPrefW(*child, rW));
        const float ch = std::max(0.f, childPrefH(*child, rH, rW));
        child->mRect = child->mPaintRECT =
          _snapRect(rect.L, rect.T + cursorY, rect.L + rW, rect.T + cursorY + ch);
        if (child->computedStyle.position == "relative") _applyRelativeOffset(child, rW, rH);
        cursorY += ch;
        child->Layout(g);
        continue;
      }

      const bool childNowrap =
        (child->computedStyle.whiteSpace == "nowrap" || child->computedStyle.whiteSpace == "pre" ||
         computedStyle.whiteSpace == "nowrap" || computedStyle.whiteSpace == "pre");
      const bool childHasInlineBoxChromeBits =
        static_cast<float>(child->computedStyle.paddingTop) > 0.f ||
        static_cast<float>(child->computedStyle.paddingRight) > 0.f ||
        static_cast<float>(child->computedStyle.paddingBottom) > 0.f ||
        static_cast<float>(child->computedStyle.paddingLeft) > 0.f ||
        child->computedStyle.resolvedBorderWidth(0) > 0.f ||
        child->computedStyle.resolvedBorderWidth(1) > 0.f ||
        child->computedStyle.resolvedBorderWidth(2) > 0.f ||
        child->computedStyle.resolvedBorderWidth(3) > 0.f ||
        child->computedStyle.backgroundColor.value.A > 0 ||
        !child->computedStyle.width.raw.empty() ||
        !child->computedStyle.height.raw.empty();
      const float sz = child->computedStyle.fontSize.toFloat() > 0.f
                       ? child->computedStyle.fontSize.toFloat()
                       : (_inheritedSz > 0.f ? _inheritedSz : 16.f);
      const float lh = sz * (child->computedStyle.lineHeight > 0.f
                             ? child->computedStyle.lineHeight : computedStyle.lineHeight);
      // display:inline-block creates an atomic inline-level block container.
      // It must NEVER be split into word-wrapped text fragments even when it
      // has innerText, and it ALWAYS contributes its full box (including
      // padding/border/margin) to line-box height — unlike display:inline.
      const bool isInlineBlock = (child->computedStyle.display == "inline-block");

      if (!child->innerText.empty() && !childNowrap && !childHasInlineBoxChromeBits && !isInlineBlock)
      {
        struct Tok {
          std::string text;
          int byteOff = 0;
          int byteEnd = 0;
          bool isNL = false;
          bool isWord = false;
        };

        SkFont childFont = resolveChildFont(child, sz);
        const auto fm = resolveFontMetrics(child->computedStyle, &_parentStyleForStrut);
        const auto childWSMode = child->_whiteSpaceMode();
        const bool collapseWS = (childWSMode == _WhiteSpaceMode::Normal || childWSMode == _WhiteSpaceMode::NoWrap || childWSMode == _WhiteSpaceMode::PreLine);
        const bool preserveNL = (childWSMode == _WhiteSpaceMode::Pre || childWSMode == _WhiteSpaceMode::PreLine || childWSMode == _WhiteSpaceMode::PreWrap || childWSMode == _WhiteSpaceMode::BreakSpaces);
        const bool breakSpaces = (childWSMode == _WhiteSpaceMode::BreakSpaces);

        std::vector<Tok> toks;
        if (collapseWS)
        {
          std::string cur;
          int curOff = 0;
          int i = 0;
          while (i < static_cast<int>(child->innerText.size()))
          {
            const char c = child->innerText[static_cast<std::size_t>(i)];
            if (child->_isWhiteSpaceChar(c))
            {
              if (!cur.empty())
              {
                toks.push_back({cur, curOff, i, false, true});
                cur.clear();
              }
              if (preserveNL && c == '\n')
                toks.push_back({"", i, i + 1, true, false});
              ++i;
              continue;
            }

            if (cur.empty()) curOff = i;
            cur.push_back(c);
            ++i;
          }
          if (!cur.empty())
            toks.push_back({cur, curOff, static_cast<int>(child->innerText.size()), false, true});
        }
        else
        {
          std::string cur;
          int curOff = 0;
          bool curSpace = false;

          auto flushTok = [&](int endOff) {
            if (cur.empty()) return;
            toks.push_back({cur, curOff, endOff, false, !curSpace});
            cur.clear();
          };

          int i = 0;
          while (i < static_cast<int>(child->innerText.size()))
          {
            char c = child->innerText[static_cast<std::size_t>(i)];
            if (c == '\r') { ++i; continue; }
            if (c == '\t') c = ' ';
            if (preserveNL && c == '\n')
            {
              flushTok(i);
              toks.push_back({"", i, i + 1, true, false});
              ++i;
              continue;
            }

            const bool isSpace = (c == ' ');
            if (breakSpaces && isSpace)
            {
              flushTok(i);
              toks.push_back({" ", i, i + 1, false, false});
              ++i;
              continue;
            }

            if (cur.empty())
            {
              curOff = i;
              curSpace = isSpace;
              cur.push_back(c);
            }
            else if (isSpace == curSpace)
            {
              cur.push_back(c);
            }
            else
            {
              flushTok(i);
              curOff = i;
              curSpace = isSpace;
              cur.push_back(c);
            }
            ++i;
          }
          flushTok(static_cast<int>(child->innerText.size()));
        }

        std::string line;
        int lineByteStart = 0;
        int lineByteEnd = 0;
        bool lineStarted = false;
        const bool childSubpixel = _hasSubpixelIntent(*child, rW, rH);

        auto emitFragment = [&]() {
          if (!lineStarted) return;
          const float lineW = skTextW(childFont, line);
          const auto ink = measureInkMetrics(childFont, line, fm);
          lineItems.push_back({
            child,
            cursorX,
            lineW,
            fm.lineHeight,
            fm.textTopAbove,
            centeredBaselineFromTop(fm.lineHeight, ink, fm.textTopAbove),
            fm.lineHeight,
            childSubpixel,
            child->computedStyle.verticalAlign,
            true,
            line,
            lineByteStart,
            lineByteEnd
          });
          lineItems.back().hasInkBounds = ink.hasBounds;
          lineItems.back().inkTopAboveBaseline = ink.topAboveBaseline;
          lineItems.back().inkBottomBelowBaseline = ink.bottomBelowBaseline;
          cursorX += lineW;
          lineMaxH = std::max(lineMaxH, fm.lineHeight);
          line.clear();
          lineStarted = false;
        };

        for (const Tok& tok : toks)
        {
          if (tok.isNL)
          {
            emitFragment();
            flushLine();
            lineByteStart = lineByteEnd = tok.byteEnd;
            continue;
          }

          std::string probe;
          if (collapseWS)
          {
            if (line.empty()) probe = tok.text;
            else if (tok.isWord) probe = line + " " + tok.text;
            else probe = line;
          }
          else
          {
            probe = line + tok.text;
          }

          float probeW = skTextW(childFont, probe);
          if (line.empty() && cursorX > 0.f && probeW > (rW - cursorX))
            flushLine();

          probeW = skTextW(childFont, probe);
          if (!line.empty() && cursorX + probeW > rW)
          {
            emitFragment();
            flushLine();
            line = tok.text;
            lineByteStart = tok.byteOff;
            lineByteEnd = tok.byteEnd;
            lineStarted = true;
          }
          else
          {
            if (!lineStarted)
            {
              lineByteStart = tok.byteOff;
              lineStarted = true;
            }
            line = std::move(probe);
            lineByteEnd = tok.byteEnd;
          }
        }

        emitFragment();
        continue;
      }

      const float cw = std::max(0.f, childPrefW(*child, rW));
      float usedH = std::max(0.f, childPrefH(*child, rH, rW));
      if (!childNowrap && cursorX + cw > rW && cursorX > 0.f)
        flushLine();

      float baselineFromTop = usedH;
      float drawBaselineFromTop = baselineFromTop;
      float lineHeightRef   = std::max(usedH, 1.f);
      float itemInlinePadT  = 0.f;  // extra paint extent above line-box rect (display:inline only)
      float itemInlinePadB  = 0.f;  // extra paint extent below line-box rect (display:inline only)
      bool  itemHasInkBounds = false;
      float itemInkTopAboveBaseline = 0.f;
      float itemInkBottomBelowBaseline = 0.f;
      if (!child->innerText.empty())
      {
        const auto fm = resolveFontMetrics(child->computedStyle, &_parentStyleForStrut);
        const auto childFont = resolveChildFont(child, sz);
        const auto ink = measureInkMetrics(childFont, child->innerText, fm);
        const float padT = static_cast<float>(child->computedStyle.paddingTop);
        const float padB = static_cast<float>(child->computedStyle.paddingBottom);
        const float brdT = child->computedStyle.resolvedBorderWidth(0);
        const float brdB = child->computedStyle.resolvedBorderWidth(2);
        if (isInlineBlock)
        {
          // display:inline-block — full padded box participates in the line box
          // (CSS2.1 §10.8.1: "replaced elements, inline-block elements …
          //  the height used is their margin-box height").
          usedH           = std::max(usedH, padT + brdT + fm.lineHeight + padB + brdB);
          baselineFromTop = std::min(usedH, std::max(0.f, padT + brdT + fm.textTopAbove));
          drawBaselineFromTop = std::min(usedH, std::max(0.f,
            padT + brdT + centeredBaselineFromTop(fm.lineHeight, ink, fm.textTopAbove)));
        }
        else
        {
          // display:inline — only line-height contributes to line-box height;
          // padding/border overflow visually above/below without displacing other
          // items (CSS2.1 §10.8.1, inline non-replaced element rule).
          usedH           = fm.lineHeight;
          baselineFromTop = fm.textTopAbove;
          drawBaselineFromTop = centeredBaselineFromTop(fm.lineHeight, ink, fm.textTopAbove);
          itemInlinePadT  = padT + brdT;
          itemInlinePadB  = padB + brdB;
        }
        lineHeightRef = std::max(fm.lineHeight, 1.f);
        itemHasInkBounds = ink.hasBounds;
        itemInkTopAboveBaseline = ink.topAboveBaseline;
        itemInkBottomBelowBaseline = ink.bottomBelowBaseline;
      }

      lineItems.push_back({
        child,
        cursorX,
        cw,
        usedH,
        baselineFromTop,
        drawBaselineFromTop,
        lineHeightRef,
        _hasSubpixelIntent(*child, rW, rH),
        child->computedStyle.verticalAlign
      });
      if (itemHasInkBounds)
      {
        lineItems.back().hasInkBounds = true;
        lineItems.back().inkTopAboveBaseline = itemInkTopAboveBaseline;
        lineItems.back().inkBottomBelowBaseline = itemInkBottomBelowBaseline;
      }
      if (itemInlinePadT > 0.f || itemInlinePadB > 0.f)
      {
        lineItems.back().inlinePadT = itemInlinePadT;
        lineItems.back().inlinePadB = itemInlinePadB;
      }
      cursorX += cw;
      lineMaxH = std::max(lineMaxH, usedH);
    }

    flushLine();

    for (auto& it : textBounds)
    {
      glint_element* child = it.first;
      const auto& bounds = it.second;
      if (!bounds.hasBounds) continue;
      if (bounds.subpixel)
        child->mRect = child->mPaintRECT = glint_rect(bounds.l, bounds.t, bounds.r, bounds.b);
      else
        child->mRect = child->mPaintRECT = _snapRect(bounds.l, bounds.t, bounds.r, bounds.b);
      if (child->computedStyle.position == "relative") _applyRelativeOffset(child, rW, rH);
      child->Layout(g);
    }

    // Absolute-positioned children — positioned against their CSS containing block.
    for (auto& childPtr : mChildren)
    {
      glint_element* child = childPtr.get();
      _refreshLayoutStyle(child);
      if (child->computedStyle.display == "none" || child->computedStyle.position != "absolute") continue;
      const glint_rect cb  = _containingBlockContent(child);
      const float cbW = cb.W(), cbH = cb.H();
      const float cw = std::max(0.f, childPrefW(*child, cbW));
      const float ch = std::max(0.f, childPrefH(*child, cbH, cbW));
      float l, t;
      if      (!child->computedStyle.left.raw.empty())   l = cb.L + child->computedStyle.left.resolve(cbW);
      else if (!child->computedStyle.right.raw.empty())  l = cb.L + cbW - child->computedStyle.right.resolve(cbW) - cw;
      else                                        l = cb.L;
      if      (!child->computedStyle.top.raw.empty())    t = cb.T + child->computedStyle.top.resolve(cbH);
      else if (!child->computedStyle.bottom.raw.empty()) t = cb.T + cbH - child->computedStyle.bottom.resolve(cbH) - ch;
      else                                        t = cb.T;
      child->mRect = child->mPaintRECT =
        _snapRect(l, t, l + cw, t + ch);
      child->Layout(g);
    }
  }

  void layoutTable(glint_canvas* g, const glint_rect& rect, float rW, float rH)
  {
    // Note: text-line caches on children are NOT cleared here. _buildRenderLines
    // self-invalidates when its inputs (text, font, content rect) change.

    const auto rows = _collectTableRows(*this);
    const auto colWidths = _tableColumnWidths(rows, rW, rH, true);
    const auto rowHeights = _tableRowHeights(rows, colWidths, rH, true);

    auto lower = [](std::string v) {
      std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      return v;
    };

    float y = rect.T;
    for (std::size_t ri = 0; ri < rows.size(); ++ri)
    {
      float rowH = ri < rowHeights.size() ? rowHeights[ri] : 0.f;

      auto layoutRow = [&](float currentRowH) {
        if (rows[ri].rowBox)
        {
          rows[ri].rowBox->mRect = rows[ri].rowBox->mPaintRECT =
            _snapRect(rect.L, y, rect.L + rW, y + currentRowH);
          if (rows[ri].rowBox->computedStyle.position == "relative")
            _applyRelativeOffset(rows[ri].rowBox, rW, rH);
        }

        float x = rect.L;
        for (std::size_t ci = 0; ci < rows[ri].cells.size(); ++ci)
        {
          glint_element* cell = rows[ri].cells[ci];
          const float cellW = ci < colWidths.size() ? colWidths[ci] : 0.f;
          if (_hasSubpixelIntent(*cell, rW, rH))
            cell->mRect = cell->mPaintRECT = glint_rect(x, y, x + cellW, y + currentRowH);
          else
            cell->mRect = cell->mPaintRECT = _snapRect(x, y, x + cellW, y + currentRowH);
          if (cell->computedStyle.position == "relative") _applyRelativeOffset(cell, rW, rH);
          cell->Layout(g);
          x += cellW;
        }
      };

      layoutRow(rowH);

      float rowBaselineY = y;
      float maxBelowBaseline = 0.f;
      float maxOccupiedHeight = 0.f;
      bool hasBaselineCells = false;

      for (glint_element* cell : rows[ri].cells)
      {
        const auto metrics = _measureTableCellMetrics(cell);
        if (!metrics.hasBounds) continue;
        maxOccupiedHeight = std::max(maxOccupiedHeight, metrics.bottom - y);

        const std::string v = lower(cell->computedStyle.verticalAlign);
        const bool wantsBaseline = (v.empty() || v == "baseline");
        if (wantsBaseline && metrics.hasBaseline)
        {
          hasBaselineCells = true;
          rowBaselineY = std::max(rowBaselineY, metrics.baseline);
        }
      }

      if (hasBaselineCells)
      {
        for (glint_element* cell : rows[ri].cells)
        {
          const auto metrics = _measureTableCellMetrics(cell);
          const std::string v = lower(cell->computedStyle.verticalAlign);
          const bool wantsBaseline = (v.empty() || v == "baseline");
          if (wantsBaseline && metrics.hasBaseline)
            maxBelowBaseline = std::max(maxBelowBaseline, metrics.bottom - metrics.baseline);
        }
        rowH = std::max(rowH, (rowBaselineY - y) + maxBelowBaseline);
      }

      rowH = std::max(rowH, maxOccupiedHeight);

      layoutRow(rowH);

      if (hasBaselineCells)
      {
        rowBaselineY = y;
        for (glint_element* cell : rows[ri].cells)
        {
          const auto metrics = _measureTableCellMetrics(cell);
          const std::string v = lower(cell->computedStyle.verticalAlign);
          const bool wantsBaseline = (v.empty() || v == "baseline");
          if (wantsBaseline && metrics.hasBaseline)
            rowBaselineY = std::max(rowBaselineY, metrics.baseline);
        }
      }

      for (glint_element* cell : rows[ri].cells)
      {
        const std::string v = lower(cell->computedStyle.verticalAlign);
        const bool wantsBaseline = hasBaselineCells && (v.empty() || v == "baseline");
        _alignTableCellContent(cell, rowBaselineY, wantsBaseline);
      }

      y += rowH;
    }

    for (auto& childPtr : mChildren)
    {
      glint_element* child = childPtr.get();
      _refreshLayoutStyle(child);
      if (child->computedStyle.display == "none" || child->computedStyle.position != "absolute") continue;
      const glint_rect cb  = _containingBlockContent(child);
      const float cbW = cb.W(), cbH = cb.H();
      const float cw = std::max(0.f, childPrefW(*child, cbW));
      const float ch = std::max(0.f, childPrefH(*child, cbH, cbW));
      float l, t;
      if      (!child->computedStyle.left.raw.empty())   l = cb.L + child->computedStyle.left.resolve(cbW);
      else if (!child->computedStyle.right.raw.empty())  l = cb.L + cbW - child->computedStyle.right.resolve(cbW) - cw;
      else                                               l = cb.L;
      if      (!child->computedStyle.top.raw.empty())    t = cb.T + child->computedStyle.top.resolve(cbH);
      else if (!child->computedStyle.bottom.raw.empty()) t = cb.T + cbH - child->computedStyle.bottom.resolve(cbH) - ch;
      else                                               t = cb.T;
      child->mRect = child->mPaintRECT = _snapRect(l, t, l + cw, t + ch);
      child->Layout(g);
    }
  }

  void layoutBlock(glint_canvas* g, const glint_rect& rect, float rW, float rH)
  {
    // Note: text-line caches on children are NOT cleared here. _buildRenderLines
    // self-invalidates when its inputs (text, font, content rect) change.

    // If any in-flow child has display:"inline", delegate to the inline
    // formatting context (greedy word-wrap, horizontal flow).
    // display:"inline-block" does NOT trigger the IFC — inside a block
    // container it is treated as a block-level box (per CSS2.1 §9.4.1).
    // Note: innerText alone does NOT trigger inline layout — glint_element (and
    // any other block-level element) may have innerText set without being inline.
    for (const auto& ch : mChildren)
    {
      _refreshLayoutStyle(ch.get());
      if (ch->computedStyle.display == "none" || ch->computedStyle.position == "absolute") continue;
      if (ch->computedStyle.display == "inline")
      { layoutInline(g, rect, rW, rH); return; }
    }

    float cursorY = 0.f;
    for (auto& child : mChildren)
    {
      _refreshLayoutStyle(child.get());
      if (child->computedStyle.display == "none") continue;
      const bool  isAbs = (child->computedStyle.position == "absolute");
      // Scrollbar children are excluded — _positionScrollbars() owns their rects.
      if (isAbs && (child.get() == mScrollbarV || child.get() == mScrollbarH || child.get() == mScrollCorner)) continue;
      const bool  isRel = (child->computedStyle.position == "relative");
      const float mL    = isAbs ? 0.f : child->computedStyle.marginLeft.resolve(rW);
      const float mT    = isAbs ? 0.f : child->computedStyle.marginTop.resolve(rW);
      const float mB    = isAbs ? 0.f : child->computedStyle.marginBottom.resolve(rW);

      if (isAbs)
      {
        // position:absolute — resolve against nearest positioned ancestor (containing block).
        const glint_rect cb  = _containingBlockContent(child.get());
        const float cbW = cb.W(), cbH = cb.H();
        const float w   = std::max(0.f, childPrefW(*child, cbW));
        const float h   = std::max(0.f, childPrefH(*child, cbH, cbW));
        // X: left → right → static-position (Chrome flex behaviour)
        float l;
        if      (!child->computedStyle.left.raw.empty())  l = cb.L + child->computedStyle.left.resolve(cbW);
        else if (!child->computedStyle.right.raw.empty()) l = cb.L + cbW - child->computedStyle.right.resolve(cbW) - w;
        else                                       l = _absFlexStaticPos(child.get(), cb, true,  w);
        // Y: top → bottom → static-position (Chrome flex behaviour)
        float t;
        if      (!child->computedStyle.top.raw.empty())    t = cb.T + child->computedStyle.top.resolve(cbH);
        else if (!child->computedStyle.bottom.raw.empty()) t = cb.T + cbH - child->computedStyle.bottom.resolve(cbH) - h;
        else                                        t = _absFlexStaticPos(child.get(), cb, false, h);
        if (_hasSubpixelIntent(*child, cbW, cbH))
          child->mRect = child->mPaintRECT = glint_rect(l, t, l + w, t + h);
        else
          child->mRect = child->mPaintRECT = _snapRect(l, t, l + w, t + h);
      }
      else
      {
        float w = std::max(0.f, childPrefW(*child, rW));
        const float h = std::max(0.f, childPrefH(*child, rH, rW));
        // Chrome block layout: block-level children with auto/unset width fill
        // the parent content width minus their own horizontal margins.
        // Block-level: display "block", "flex", or "" (default = block in CSS).
        // Inline-level: "inline", "inline-block", "inline-flex" keep fit-content.
        // We check computedStyle.display (merged CSS + inline layer) to correctly
        // handle display values set via CSS classes as well as inline style.
        // Buttons set style.display = "inline-block" in their constructor, so they
        // are correctly excluded and keep their intrinsic / fit-content width.
        {
          const auto& wraw  = child->computedStyle.width.raw;
          const bool  isAutoW = wraw.empty() || wraw == "auto";
          const auto& cDisp = child->computedStyle.display;
          const bool  isBlockChild = (cDisp != "inline" &&
                                      cDisp != "inline-block" &&
                                      cDisp != "inline-flex");
          if (isAutoW && isBlockChild)
          {
            const float mR = child->computedStyle.marginRight.resolve(rW);
            w = std::max(0.f, rW - mL - mR);
          }
        }
        // Flow position.  For position:relative top/left are applied as visual
        // deltas below, so we always put the box at the cursor flow position.
        // Chrome: top/left have NO effect on position:static elements — the
        // cursor always drives vertical position for static block children.
        // Builder-injected tops are stale build-time snapshots; ignoring them
        // here is both Chrome-correct AND required for live reflowing.
        const float flowL = rect.L + (isRel ? 0.f : child->computedStyle.left.resolve(rW)) + mL;
        const float flowT = rect.T + cursorY + mT;
        if (_hasSubpixelIntent(*child, rW, rH))
          child->mRect = child->mPaintRECT = glint_rect(flowL, flowT, flowL + w, flowT + h);
        else
          child->mRect = child->mPaintRECT = _snapRect(flowL, flowT, flowL + w, flowT + h);
        // position:relative: shift box visually by top/left without moving siblings.
        if (isRel) _applyRelativeOffset(child.get(), rW, rH);
        // Cursor advances by FLOW height — ignores any relative visual offset.
        cursorY = (flowT - rect.T) + h + mB;
      }
      child->Layout(g);
    }
  }

  // ─── Scroll layout helpers ───────────────────────────────────────────────────

  /**
  * Wire up element.scrollTop / element.scrollLeft reactive setters.
  * Called once during subtree finalization. The lambdas capture `this` so they stay valid
   * as long as the component exists (guaranteed by the tree ownership model).
   */
  void _initScrollElement()
  {
    element._bindScroll(
      // scrollTop getter
      [this]() -> float { return mScrollTop; },
      // scrollTop setter — clamps, fires "scroll" event, requests redraw
      [this](float v)
      {
        const float sbW   = computedStyle.scrollbarWidth;
        const bool  hasSH = mScrollbarH && mScrollbarH->style.display != "none";
        const float viewH = GetPaintRECT().H() - (hasSH ? sbW : 0.f);
        mScrollTop = std::max(0.f, std::min(v, std::max(0.f, mScrollHeight - viewH)));
        glint_event se; se.type = "scroll"; se.bubbles = false; se.cancelable = false;
        dispatchDOMEvent(se);
        _refreshRootHoverFromPointer();
        setDirty(false);
      },
      // scrollLeft getter
      [this]() -> float { return mScrollLeft; },
      // scrollLeft setter — clamps, fires "scroll" event, requests redraw
      [this](float v)
      {
        const float sbW   = computedStyle.scrollbarWidth;
        const bool  hasSV = mScrollbarV && mScrollbarV->style.display != "none";
        const float viewW = GetPaintRECT().W() - (hasSV ? sbW : 0.f);
        mScrollLeft = std::max(0.f, std::min(v, std::max(0.f, mScrollWidth - viewW)));
        glint_event se; se.type = "scroll"; se.bubbles = false; se.cancelable = false;
        dispatchDOMEvent(se);
        _refreshRootHoverFromPointer();
        setDirty(false);
      }
    );
  }

  /**
   * Ensure vertical/horizontal scrollbar children exist.
    * DECLARED here; DEFINED at the bottom of
    * components/glint_scrollbar/glint_scrollbar.hpp (needs the full glint_scrollbar type).
   */
  void _ensureScrollbars(bool needsX, bool needsY);

  /**
   * Measure the total W/H extent of non-scrollbar, non-absolute children
   * relative to `base.L / base.T`.  Written into outW / outH.
   */
  void _measureContentExtent(const glint_rect& base, float& outW, float& outH) const
  {
    outW = 0.f; outH = 0.f;
    for (const auto& child : mChildren)
    {
      if (child.get() == mScrollbarV || child.get() == mScrollbarH || child.get() == mScrollCorner)
        continue;
      if (child->computedStyle.position == "absolute") continue;
      if (child->computedStyle.display  == "none")     continue;
      const glint_rect childRect = child->GetPaintRECT();
      outW = std::max(outW, childRect.R - base.L);
      outH = std::max(outH, childRect.B - base.T);
    }
  }

  /** Clamp mScrollTop / mScrollLeft to [0, maxScroll] based on measured content dims. */
  void _clampScroll(bool showX, bool showY)
  {
    const glint_rect pr  = GetPaintRECT();
    const float sbW = computedStyle.scrollbarWidth;
    const float viewW = pr.W() - (showY ? sbW : 0.f);
    const float viewH = pr.H() - (showX ? sbW : 0.f);
    const float maxX = std::max(0.f, mScrollWidth  - viewW);
    const float maxY = std::max(0.f, mScrollHeight - viewH);

    // Preserve end-pinning only when there was already a real scroll range.
    // When the prior max scroll was 0, a newly overflowing element starts at
    // scrollTop/Left == 0 by default and must stay at the origin rather than
    // being treated as intentionally pinned to the far edge.
    if (mLastScrollMaxX > 0.f && maxX > mLastScrollMaxX && std::fabs(mScrollLeft - mLastScrollMaxX) <= 0.5f)
      mScrollLeft = maxX;
    if (mLastScrollMaxY > 0.f && maxY > mLastScrollMaxY && std::fabs(mScrollTop - mLastScrollMaxY) <= 0.5f)
      mScrollTop = maxY;

    mScrollLeft = std::max(0.f, std::min(mScrollLeft, maxX));
    mScrollTop  = std::max(0.f, std::min(mScrollTop,  maxY));
    mLastScrollMaxX = maxX;
    mLastScrollMaxY = maxY;
  }

  /**
   * Returns the content clip rect (component paint rect minus active scrollbar strips).
   */
  glint_rect _getContentClipRect() const
  {
    glint_rect r = GetPaintRECT();
    const float sbW = computedStyle.scrollbarWidth;
    if (mScrollbarV && mScrollbarV->style.display != "none") r.R -= sbW;
    if (mScrollbarH && mScrollbarH->style.display != "none") r.B -= sbW;
    return r;
  }

  /**
   * Position (and Layout()) the scrollbar and corner children at the
   * correct screen-space rects on the component's edges.
   */
  void _positionScrollbars(glint_canvas* g, bool showX, bool showY)
  {
    const glint_rect pr  = GetPaintRECT();
    const float sbW = computedStyle.scrollbarWidth;

    if (mScrollbarV)
    {
      if (showY)
      {
        const float vB = pr.B - (showX ? sbW : 0.f);
        mScrollbarV->mRect = mScrollbarV->mPaintRECT = glint_rect(pr.R - sbW, pr.T, pr.R, vB);
        mScrollbarV->style.display = "";
      }
      else
      {
        mScrollbarV->style.display = "none";
      }
      mScrollbarV->Layout(g);
    }

    if (mScrollbarH)
    {
      if (showX)
      {
        const float hR = pr.R - (showY ? sbW : 0.f);
        mScrollbarH->mRect = mScrollbarH->mPaintRECT = glint_rect(pr.L, pr.B - sbW, hR, pr.B);
        mScrollbarH->style.display = "";
      }
      else
      {
        mScrollbarH->style.display = "none";
      }
      mScrollbarH->Layout(g);
    }

    if (mScrollCorner)
    {
      if (showX && showY)
      {
        mScrollCorner->mRect = mScrollCorner->mPaintRECT = glint_rect(pr.R - sbW, pr.B - sbW, pr.R, pr.B);
        mScrollCorner->style.backgroundColor = computedStyle.scrollbarTrackColor;
        mScrollCorner->style.display = "";
      }
      else
      {
        mScrollCorner->style.display = "none";
      }
      mScrollCorner->Layout(g);
    }
  }

  /**
   * Full scroll-aware layout pass. Called from Layout() when overflow-x or
   * overflow-y is "scroll" or "auto".
   */
  void _layoutScroll(glint_canvas* g, bool scrollX, bool scrollY)
  {
    const float sbW = computedStyle.scrollbarWidth;

    // Initial scrollbar visibility: "scroll" = always visible; "auto" = measure first.
    bool showSbarY = (computedStyle.overflowY == "scroll");
    bool showSbarX = (computedStyle.overflowX == "scroll");

    // ── "auto" overflow measurement — iterate to a stable X/Y decision ─────
    if (computedStyle.overflowY == "auto" || computedStyle.overflowX == "auto")
    {
      auto layoutForRect = [&](const glint_rect& r) {
        if (computedStyle.display == "flex")
          layoutFlex(g, r, r.W(), r.H());
        else if (_isTableDisplay(computedStyle.display))
          layoutTable(g, r, r.W(), r.H());
        else
          layoutBlock(g, r, r.W(), r.H());
      };

      for (int i = 0; i < 3; ++i)
      {
        const bool prevShowSbarX = showSbarX;
        const bool prevShowSbarY = showSbarY;

        glint_rect probe = getContent();
        if (showSbarY) probe.R -= sbW;
        if (showSbarX) probe.B -= sbW;
        probe.R = std::max(probe.L, probe.R);
        probe.B = std::max(probe.T, probe.B);

        layoutForRect(probe);

        float cW = 0.f, cH = 0.f;
        _measureContentExtent(probe, cW, cH);

        if (computedStyle.overflowY == "auto") showSbarY = (cH > probe.H());
        if (computedStyle.overflowX == "auto") showSbarX = (cW > probe.W());

        if (showSbarX == prevShowSbarX && showSbarY == prevShowSbarY)
          break;
      }
    }

    // ── Create scrollbar children if they don't exist yet ────────────────────
    _ensureScrollbars(showSbarX, showSbarY);

    // ── Final content rect: getContent() minus reserved scrollbar strips ─────
    glint_rect contentR = getContent();
    if (showSbarY) contentR.R -= sbW;
    if (showSbarX) contentR.B -= sbW;
    contentR.R = std::max(contentR.L, contentR.R);
    contentR.B = std::max(contentR.T, contentR.B);

    // ── Main layout pass in the reduced content area ─────────────────────────
    if (computedStyle.display == "flex")
      layoutFlex(g, contentR, contentR.W(), contentR.H());
    else if (_isTableDisplay(computedStyle.display))
      layoutTable(g, contentR, contentR.W(), contentR.H());
    else
      layoutBlock(g, contentR, contentR.W(), contentR.H());

    // ── Measure final content extent ─────────────────────────────────────────
    float cW = 0.f, cH = 0.f;
    _measureContentExtent(contentR, cW, cH);

    // Children are positioned relative to contentR (the padding-inset rect), but
    // _clampScroll uses GetPaintRECT() as the viewport basis. Add padding and border
    // back so measured scroll extents live in the same border-box coordinate space.
    const float pT = (float)computedStyle.paddingTop,  pB = (float)computedStyle.paddingBottom;
    const float pL = (float)computedStyle.paddingLeft,  pR = (float)computedStyle.paddingRight;
    const float bT = computedStyle.resolvedBorderWidth(0), bR = computedStyle.resolvedBorderWidth(1);
    const float bB = computedStyle.resolvedBorderWidth(2), bL = computedStyle.resolvedBorderWidth(3);
    mScrollWidth  = cW + pL + pR + bL + bR;
    mScrollHeight = cH + pT + pB + bT + bB;

    // ── Clamp scroll position to valid range ─────────────────────────────────
    _clampScroll(showSbarX, showSbarY);

    // ── Position scrollbar children at component edges ────────────────────────
    _positionScrollbars(g, showSbarX, showSbarY);

    // ── Sync element read-only scroll properties ──────────────────────────────
    element.scrollWidth  = mScrollWidth;
    element.scrollHeight = mScrollHeight;
  }

  void EnsureFilterPad()
  {
    const bool rectsEqual = (mRect.L == mPaintRECT.L && mRect.T == mPaintRECT.T &&
                              mRect.R == mPaintRECT.R && mRect.B == mPaintRECT.B);
    float desiredPad = 0.f;
    if (!computedStyle.filter.empty() && computedStyle.filter != "none")
      desiredPad = glint_filter::ComputeExpansion(computedStyle.filter);

    if (mFilterPadApplied && !rectsEqual)
    {
      if (std::abs(mFilterPad - desiredPad) <= 0.001f)
        return;

      // The active filter expansion changed; collapse back to the tight rect
      // before recomputing the new padded bounds for the current frame.
      mRect = mPaintRECT;
    }

    mPaintRECT = mRect;
    mFilterPad = desiredPad;
    mFilterPadApplied = (mFilterPad > 0.f);
    if (mFilterPad > 0.f)
      mRect = mRect.GetPadded(mFilterPad);
  }

  /**
  * Re-resolves mRect from style.width / style.height against mParentW / mParentH.
  * Called during subtree finalization after ApplyAlign() has written "100%" etc. into style.
   */
  void ApplySelfSizing()
  {
    bool changed = false;
    const auto& rw = style.width.raw;
    const auto& rh = style.height.raw;
    if (!rw.empty() && rw != "fit-content" && rw != "auto")
    {
      const float w = style.width.resolve(mParentW);
      mRect   = glint_rect(mRect.L, mRect.T, mRect.L + w, mRect.B);
      changed = true;
    }
    if (!rh.empty() && rh != "fit-content" && rh != "auto")
    {
      const float h = style.height.resolve(mParentH);
      mRect   = glint_rect(mRect.L, mRect.T, mRect.R, mRect.T + h);
      changed = true;
    }
    if (changed)
      mPaintRECT = mRect;
  }

