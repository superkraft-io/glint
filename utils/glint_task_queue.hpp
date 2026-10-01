#pragma once

/**
 * glint_task_queue.hpp
 * Thread-safe queue of tasks to run on a document's owning (window) thread.
 *
 * Every glint window runs on its own thread.  Popup windows (color / date /
 * time pickers, the gradient editor's picker) deliver their callbacks on the
 * popup's thread, so those callbacks must not touch the owning document's
 * elements directly.  Instead they post a task here; the owning host is woken
 * through the document's redraw callback and glint_document::DrawToCanvas
 * drains the queue on the owning thread before layout.
 *
 * Usually reached through glint_element::ownerThreadPoster(), which also skips
 * tasks whose element was destroyed before they ran.
 */

#include <functional>
#include <mutex>
#include <utility>
#include <vector>

class glint_task_queue
{
public:
  /** Queue `task` and wake the owning thread.  Safe from any thread. */
  void post(std::function<void()> task)
  {
    if (!task) return;
    std::lock_guard<std::mutex> lk(mMutex);
    mTasks.push_back(std::move(task));
    // Called under the lock so setWake(nullptr) (document teardown) cannot
    // return while a wake into the departing host is still running.
    if (mWake) mWake();
  }

  /** Set how to wake the owning thread (e.g. request a redraw).  The callback
   *  must be safe to call from any thread and must not post to this queue. */
  void setWake(std::function<void()> wake)
  {
    std::lock_guard<std::mutex> lk(mMutex);
    mWake = std::move(wake);
  }

  /** Run every queued task.  Owning thread only. */
  void drain()
  {
    std::vector<std::function<void()>> tasks;
    {
      std::lock_guard<std::mutex> lk(mMutex);
      tasks.swap(mTasks);
    }
    // Tasks posted while these run are picked up by the next drain (they also
    // wake the host again).
    for (auto& task : tasks)
      task();
  }

private:
  std::mutex                          mMutex;
  std::vector<std::function<void()>>  mTasks;
  std::function<void()>               mWake;
};

/** Runs a task on an element's owning thread; see glint_element::ownerThreadPoster(). */
using glint_owner_poster = std::function<void(std::function<void()>)>;
