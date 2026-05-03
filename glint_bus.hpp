#pragma once

/**
 * glint_bus.hpp
 * Lightweight typed publish/subscribe event bus for glint.
 *
 * Pure C++ / STL — no OS or host dependency.  Works identically on all
 * platforms.
 *
 * Usage:
 *   // Subscribe (returns an ID for later removal):
 *   int id = glint_bus::subscribe<glint_tree_changed_event>(
 *       [this](const glint_tree_changed_event& e) {
 *           if (e.root != mMainRoot) return;   // filter by root pointer
 *           refreshTree();
 *       });
 *
 *   // Unsubscribe:
 *   glint_bus::unsubscribe(id);
 *
 *   // Publish (called from glint_document internally):
 *   glint_bus::publish(glint_tree_changed_event{ this });
 *
 * Thread safety:
 *   subscribe/unsubscribe/publish all acquire the same mutex and are safe to
 *   call from any thread.  Handler lists are copied before dispatch so it is
 *   safe to subscribe/unsubscribe from within a handler without deadlock.
 *
 * Filtering:
 *   Events carry a `root` pointer.  Subscribers filter on it.  No window IDs
 *   or global registry is required — the root pointer is already a stable
 *   identity valid for the subscriber's lifetime.
 */

#include <functional>
#include <map>
#include <mutex>
#include <typeindex>
#include <typeinfo>
#include <vector>
#include <cstdint>

// Forward declaration — glint_document is defined in glint_document.hpp which includes
// this file.  Events only hold a pointer so a forward declaration suffices.
class glint_document;

// ── Bus event types ───────────────────────────────────────────────────────────

/** Fired when any child is added/removed in the scene graph rooted at `root`. */
struct glint_tree_changed_event
{
  glint_document* root;
};

/** Fired when notifyStyleChanged() is called on a node.
 *  `id` is the mId of the node whose style was mutated. */
struct glint_node_style_changed_event
{
  glint_document* root;
  uint64_t    id;
};

/** Fired when the hovered node changes in inspect mode.
 *  `id == 0` means nothing is currently hovered. */
struct glint_hovered_node_changed_event
{
  glint_document* root;
  uint64_t    id;
};

/** Fired when the user clicks a node in inspect mode — persistent selection.
 *  `id` is the mId of the newly selected node. */
struct glint_selected_node_changed_event
{
  glint_document* root;
  uint64_t    id;
};

// ── glint_bus ─────────────────────────────────────────────────────────────────

class glint_bus
{
public:
  /** Subscribe to an event type.
   *  Returns a subscription ID to pass to unsubscribe() later.
   *  The handler is called on the same thread that calls publish().
   */
  template<typename EventT>
  static int subscribe(std::function<void(const EventT&)> handler)
  {
    std::lock_guard<std::mutex> lock(mutex());
    const int id = nextId()++;
    handlers()[std::type_index(typeid(EventT))][id] =
        [h = std::move(handler)](const void* e) {
          h(*static_cast<const EventT*>(e));
        };
    return id;
  }

  /** Unsubscribe by the ID returned from subscribe().
   *  No-op if the ID is not found (safe to call after the window is gone). */
  static void unsubscribe(int id)
  {
    std::lock_guard<std::mutex> lock(mutex());
    for (auto& [type, map] : handlers())
      map.erase(id);
  }

  /** Publish an event.  All matching subscribers are called synchronously.
   *  Handlers are copied before dispatch so subscribe/unsubscribe from within
   *  a handler is safe (no deadlock). */
  template<typename EventT>
  static void publish(const EventT& event)
  {
    std::vector<std::function<void(const void*)>> toCall;
    {
      std::lock_guard<std::mutex> lock(mutex());
      auto it = handlers().find(std::type_index(typeid(EventT)));
      if (it != handlers().end())
        for (auto& [id, fn] : it->second)
          toCall.push_back(fn);
    }
    for (auto& fn : toCall)
      fn(&event);
  }

private:
  // Meyers-singleton storage avoids static-init-order issues.
  using HandlerFn  = std::function<void(const void*)>;
  using HandlerMap = std::map<int, HandlerFn>;
  using TypeMap    = std::map<std::type_index, HandlerMap>;

  static TypeMap&    handlers() { static TypeMap    m;  return m;  }
  static std::mutex& mutex()    { static std::mutex mx; return mx; }
  static int&        nextId()   { static int        n = 0; return n; }
};
