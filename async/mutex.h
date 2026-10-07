// Mutex: making tasks take turns.
//
// Tasks on one thread already run one at a time, but only between waits: a
// task that waits part-way through something lets another begin the same
// thing. Where that would not do, as with a connection that can carry one
// request at a time, a Mutex has them queue:
//
//   Task<Response> Client::Send(Request request) {
//     co_return co_await one_at_a_time_.Run(SendOnConnection(request));
//   }
//
// Whichever task asks first goes first, and the rest follow in the order
// they asked.

#ifndef ASYNC_MUTEX_H_
#define ASYNC_MUTEX_H_

#include <algorithm>
#include <deque>
#include <utility>

#include "async/awaitable.h"
#include "async/task.h"

class Mutex {
 public:
  Mutex() = default;
  Mutex(const Mutex&) = delete;
  Mutex& operator=(const Mutex&) = delete;

  // Runs `task` once every task given to Run before it has finished, and
  // evaluates to its result. The mutex must outlive the wait.
  //
  // A task abandoned while waiting for its turn simply leaves the queue.
  // One abandoned during its turn never ends it, and the tasks behind it
  // wait for good: abandon those too.
  template <typename T>
  Task<T> Run(Task<T> task) {
    co_await Turn(*this);
    T result = co_await task;
    EndTurn();

    co_return result;
  }

  Task<> Run(Task<> task) {
    co_await Turn(*this);
    co_await task;
    EndTurn();
  }

 private:
  // Waits until it is the awaiting task's turn.
  class Turn : public Awaitable<Turn> {
   public:
    explicit Turn(Mutex& mutex) : mutex_(mutex) {}
    Turn(const Turn&) = delete;
    Turn& operator=(const Turn&) = delete;
    // The task was abandoned in the queue, if it is still there.
    ~Turn() { std::erase(mutex_.waiting_, this); }

    bool Ready() {
      if (mutex_.taken_) return false;

      mutex_.taken_ = true;
      return true;
    }
    void Start(Waker waker) {
      waker_ = waker;
      mutex_.waiting_.push_back(this);
    }
    void Finish() const {}

   private:
    friend class Mutex;

    Mutex& mutex_;
    Waker waker_;
  };

  // Hands the turn to whoever has waited longest, if anyone is waiting.
  void EndTurn() {
    if (waiting_.empty()) {
      taken_ = false;
      return;
    }

    const Turn* const next = waiting_.front();
    waiting_.pop_front();
    next->waker_.Wake();
  }

  // Whether some task is having its turn.
  bool taken_ = false;
  // The tasks waiting for theirs, longest first.
  std::deque<Turn*> waiting_;
};

#endif  // ASYNC_MUTEX_H_
