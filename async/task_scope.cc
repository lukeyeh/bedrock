#include "async/task_scope.h"

#include <coroutine>
#include <utility>
#include <vector>

#include "async/task.h"

namespace async_internal {

// A coroutine that owns itself: it starts immediately and frees its state
// when it finishes, since nothing awaits it. While it lives, its address is
// in the set it was started with.
struct SelfOwned {
  struct promise_type {
    // Receives the arguments of the coroutine function, RunToCompletion.
    promise_type(TaskScope& scope, Task<>&) : scope_(scope) {
      scope_.unfinished_.insert(Address());
    }
    ~promise_type() { scope_.unfinished_.erase(Address()); }

    SelfOwned get_return_object() const { return {}; }
    std::suspend_never initial_suspend() const { return {}; }

    // When the task finishes it frees itself and then tells the scope, in
    // that order, so that whoever the scope wakes may destroy the scope.
    // noexcept is required by the language here.
    auto final_suspend() const noexcept {
      struct FreeThenTellScope {
        TaskScope& scope;
        bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<> finished) const noexcept {
          // This object is part of what destroy() frees.
          TaskScope& told = scope;
          finished.destroy();
          told.TaskFinished();
        }
        void await_resume() const noexcept {}
      };
      return FreeThenTellScope{
          scope_,
      };
    }
    void return_void() const {}

    void* Address() {
      return std::coroutine_handle<promise_type>::from_promise(*this).address();
    }

    TaskScope& scope_;
  };

  static SelfOwned RunToCompletion(TaskScope& scope, Task<> task) {
    co_await task;
  }
};

}  // namespace async_internal

TaskScope::~TaskScope() {
  // Copied because destroying a task removes it from the set.
  const std::vector<void*> abandoned(unfinished_.begin(), unfinished_.end());
  for (void* const task : abandoned) {
    std::coroutine_handle<>::from_address(task).destroy();
  }
}

void TaskScope::Spawn(Task<> task) {
  async_internal::SelfOwned::RunToCompletion(*this, std::move(task));
}

void TaskScope::TaskFinished() {
  if (!joining_ || !unfinished_.empty()) return;

  joining_ = false;
  // The woken task may destroy this scope, so nothing may follow.
  joiner_.Wake();
}
