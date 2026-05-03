#pragma once

/**
 * glint_list.hpp
 * A scrollable list of glint_list_item rows, with optional header and footer
 * slots.  This is the single public header to include — it pulls in
 * glint_list_item.hpp automatically.
 *
 * Layout:
 *   ┌─ header    ── (flex-col; add children here, e.g. a search glint_input)
 *   ├─ container ── (flex-col, flexGrow=1, overflowY=auto — scrollable items)
 *   │     glint_list_item
 *   │     glint_list_item
 *   │     ...
 *   └─ footer    ── (flex-col; add children here, e.g. action buttons)
 *
 * The header and footer have zero intrinsic height until children are added to
 * them.  Add children directly via their add.* methods post-attachment:
 *   list->header->add.button(...)
 *   list->footer->add.button(...)
 *
 * Usage:
 *   // Create via the builder (sets size / style / callbacks):
 *   auto* list = ctx.add.list([](glint_list& _c) {
 *     _c.style.width  = "100%";
 *     _c.style.height = 260.f;
 *     _c.highlightOnSelect = true;
 *     _c.onItemSelected = [](glint_list_item* item) {
 *       DBGMSG("selected: %s\n", item->innerText.c_str());
 *     };
 *   });
 *
 *   // Add items post-attachment:
 *   list->items.add([](glint_list_item& _c) {
 *     _c.innerText = "Item A";
 *     _c.id       = "a";
 *     _c.userData = std::make_any<int>(1);
 *     _c.selectedStyle.backgroundColor = "#5a9fff";
 *   });
 *
 *   // Selection API:
 *   list->selectItemById("a");
 *   list->selectItemByIdx(0);
 *   list->deselectAll();
 *   list->clear();
 */

#include "glint_list_item.hpp"

#include <algorithm>
#include <cassert>
#include <functional>
#include <string>
#include <vector>

// ─── glint_list ──────────────────────────────────────────────────────────────

class glint_list : public glint_element
{
public:

  // ── Slots ──────────────────────────────────────────────────────────────────
  // Built in the constructor and valid for the life of the list instance.
  glint_element* header    = nullptr;
  glint_element* container = nullptr;
  glint_element* footer    = nullptr;

  // ── Items (public for iteration / external inspection) ─────────────────────
  std::vector<glint_list_item*> mList;

  // ── Behaviour ──────────────────────────────────────────────────────────────
  bool highlightOnSelect = true;  // toggle selection highlight on click

  // ── Callbacks ──────────────────────────────────────────────────────────────
  std::function<void(glint_list_item*)> onItemSelected;   // item selected (highlight → fires)
  std::function<void(glint_list_item*)> onNewItem;        // just after an item is added
  std::function<void(glint_list_item*)> onItemClicked;    // item clicked (after select logic)
  std::function<void(glint_list_item*)> onItemMouseEnter; // mouse entered item
  std::function<void(glint_list_item*)> onItemMouseLeave; // mouse left item

  // ── Constructor ────────────────────────────────────────────────────────────
  glint_list()
    : items{this}
  {
    // Flex column: header / container / footer stacked vertically.
    // overflow:hidden clips children; the container handles its own scroll.
    style.display       = "flex";
    style.flexDirection = "column";
    style.overflow      = "hidden";

    auto* hdr              = new glint_element();
    hdr->style.width       = "100%";
    hdr->style.display     = "flex";
    hdr->style.flexDirection = "column";
    addChild(hdr);
    header = hdr;

    // ── Scrollable container — grows to fill remaining space ─────────────────
    auto* cnt              = new glint_element();
    cnt->style.width       = "100%";
    cnt->style.flexGrow    = 1.f;
    cnt->style.display     = "flex";
    cnt->style.flexDirection = "column";
    cnt->style.overflowY   = "auto";
    addChild(cnt);
    container = cnt;

    // ── Footer slot ───────────────────────────────────────────────────────────
    auto* ftr              = new glint_element();
    ftr->style.width       = "100%";
    ftr->style.display     = "flex";
    ftr->style.flexDirection = "column";
    addChild(ftr);
    footer = ftr;
  }

  const char* typeName() const override { return "list"; }
  const char* tagName()  const override { return "ul"; }

  // ── items accessor — mirrors the reference JS API ──────────────────────────
  struct ItemsAccessor
  {
    glint_list* _owner;

    /**
     * Create and attach a new glint_list_item to the container.
     * `setup` receives a reference to the item for configuration.
     * Returns the created item.
     * Must be called after the list is attached to the scene graph.
     */
    template<typename S>
    glint_list_item* add(S&& setup)
    {
      return add(std::function<void(glint_list_item&)>(std::forward<S>(setup)));
    }

    glint_list_item* add(std::function<void(glint_list_item&)> setup)
    {
      assert(_owner->container && "glint_list::items.add called before constructor slot setup");

      auto* item  = new glint_list_item();
      item->mList = _owner;
      item->idx   = static_cast<int>(_owner->mList.size());

      // Run caller's configuration first so all fields are set before DOM wiring.
      setup(*item);

      // Wire click → select + onItemClicked via DOM "click" (fires after proper
      // mousedown+mouseup on the same target, matching the browser 'click' event).
      item->element.addEventListener("click", [owner = _owner, item](glint_event&) {
        owner->_onItemClick(item);
      });

      // Wire mouseenter/mouseleave (non-bubbling, exact-node events).
      item->element.addEventListener("mouseenter", [owner = _owner, item](glint_event&) {
        if (owner->onItemMouseEnter) owner->onItemMouseEnter(item);
      });
      item->element.addEventListener("mouseleave", [owner = _owner, item](glint_event&) {
        if (owner->onItemMouseLeave) owner->onItemMouseLeave(item);
      });

      _owner->container->addChild(item);
      _owner->mList.push_back(item);

      if (_owner->onNewItem) _owner->onNewItem(item);

      return item;
    }
  } items;

  // ── Selection API ──────────────────────────────────────────────────────────

  /** Deselect every item. */
  void deselectAll()
  {
    for (auto* item : mList) item->setSelected(false);
  }

  /**
   * Select by pointer.
   * If highlightOnSelect, deselects all others and marks this item selected.
   * Always fires onItemSelected.
   */
  void selectItem(glint_list_item* item)
  {
    if (!item) return;

    if (highlightOnSelect)
    {
      deselectAll();
      item->setSelected(true);
    }

    if (onItemSelected) onItemSelected(item);
  }

  /** Select by 0-based index. No-op if out of range. */
  void selectItemByIdx(int idx)
  {
    if (idx >= 0 && idx < static_cast<int>(mList.size()))
      selectItem(mList[static_cast<size_t>(idx)]);
  }

  /** Select by string id. No-op if not found. */
  void selectItemById(const std::string& id)
  {
    selectItem(findItemById(id));
  }

  /** Find the first item whose id matches. Returns nullptr if not found. */
  glint_list_item* findItemById(const std::string& id)
  {
    for (auto* item : mList)
      if (item->id == id) return item;
    return nullptr;
  }

  /**
  * Remove all items.
  * mList is cleared first (invalidating raw pointers) then container children
  * are destroyed.  Safe to call at any time after construction.
   */
  void clear()
  {
    if (!container) return;
    mList.clear();                 // clear raw ptrs before destroying components
    container->clearChildren();    // destroys all glint_list_item instances
  }

  // ── Internal callbacks — called by DOM listeners in items.add ──────────────
  void _onItemClick(glint_list_item* item)
  {
    selectItem(item);
    if (onItemClicked) onItemClicked(item);
  }
};

// New API name — both refer to the same class.
namespace { struct _glint_list_reg { _glint_list_reg() { glint_element::registerElement("list", []{ return new glint_list(); }); } } _glint_list_reg_; }
