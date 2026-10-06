// TaskScope: running tasks that nobody awaits.
//
// `co_await` runs a task and waits for it. Sometimes the caller should not
// wait: a server starts a task per connection and goes straight back to
// accepting. Spawn does that. The scope keeps track of the tasks it has
// started, so that the ones still unfinished when the scope ends are cleaned
// up rather than leaked.
//
// A scope is also how to run several tasks at once and wait for all of them:
// Spawn each, then `co_await scope.Join()`.

#ifndef ASYNC_TASK_SCOPE_H_
#define ASYNC_TASK_SCOPE_H_

#include <cstddef>

#include "absl/container/flat_hash_set.h"
#include "async/awaitable.h"
#include "async/task.h"

namespace async_internal {
struct SelfOwned;
}  // namespace async_internal

class TaskScope {
 public:
  TaskScope() = default;
  TaskScope(const TaskScope&) = delete;
  TaskScope& operator=(const TaskScope&) = delete;

  // Abandons the tasks that have not finished: they are destroyed where they
  // are suspended and never continue. Whatever they were waiting for must
  // already have been cancelled, so that nothing wakes them afterwards.
  ~TaskScope();

  // Starts `task` and returns as soon as it first waits (or finishes). From
  // then on it continues whenever what it is waiting for happens, and frees
  // itself when it finishes.
  void Spawn(Task<> task);

  // How many spawned tasks have not finished.
  size_t unfinished() const { return unfinished_.size(); }

  // What Join returns. See async/awaitable.h.
  class Joining : public Awaitable<Joining> {
   public:
    explicit Joining(TaskScope& scope) : scope_(scope) {}
    bool Ready() const { return scope_.unfinished_.empty(); }
    void Start(Waker waker) {
      scope_.joiner_ = waker;
      scope_.joining_ = true;
    }
    void Finish() const {}

   private:
    TaskScope& scope_;
  };

  // Waits until no spawned task is unfinished, including any spawned in the
  // meantime. The tasks run concurrently, taking turns whenever one waits.
  //
  //   scope.Spawn(Fetch(first));
  //   scope.Spawn(Fetch(second));
  //   co_await scope.Join();
  //
  // One Join at a time.
  Joining Join() { return Joining(*this); }

 private:
  friend struct async_internal::SelfOwned;

  // Called when a spawned task has finished and been freed.
  void TaskFinished();

  // The suspended state of each unfinished task, by address.
  absl::flat_hash_set<void*> unfinished_;

  // The task waiting in Join, if `joining_`.
  Waker joiner_;
  bool joining_ = false;
};

#endif  // ASYNC_TASK_SCOPE_H_
