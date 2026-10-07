// Mutex by example: tasks that would otherwise overlap, taking turns.

#include "async/mutex.h"

#include <benchmark/benchmark.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "async/awaitable.h"
#include "async/task.h"
#include "async/task_scope.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace {

using testing::ElementsAre;

// Something for a task to wait at until the test lets it through.
class Gate {
 public:
  class Pass : public Awaitable<Pass> {
   public:
    explicit Pass(Gate& gate) : gate_(gate) {}
    void Start(Waker waker) { gate_.waiting_ = waker; }
    void Finish() const {}

   private:
    Gate& gate_;
  };

  void Open() const { waiting_.Wake(); }

 private:
  Waker waiting_;
};

// A piece of work that waits in the middle: it notes that it began, waits
// at the gate, and notes that it ended.
Task<> Work(std::string name, Gate& gate, std::vector<std::string>& log) {
  log.push_back(name + " begins");
  co_await Gate::Pass(gate);
  log.push_back(name + " ends");
}

// A piece of work that does not wait.
Task<> Note(std::string name, std::vector<std::string>& log) {
  log.push_back(std::move(name));
  co_return;
}

Task<> RunInTurn(Mutex& mutex, Task<> work) {
  co_await mutex.Run(std::move(work));
}

Task<int> Double(int number) { co_return number * 2; }

Task<> StoreDoubled(Mutex& mutex, int number, int& result) {
  result = co_await mutex.Run(Double(number));
}

// With nobody ahead of it, a task runs at once, as if there were no mutex.
TEST(MutexTest, RunsATaskStraightAwayWhenItIsFree) {
  Mutex mutex;
  int result = 0;
  TaskScope scope;

  scope.Spawn(StoreDoubled(mutex, 21, result));

  EXPECT_EQ(result, 42);
}

// The point of it: the second piece of work does not begin while the first
// is waiting part-way through, as it would without the mutex.
TEST(MutexTest, MakesTasksTakeTurns) {
  Mutex mutex;
  Gate first;
  Gate second;
  std::vector<std::string> log;
  TaskScope scope;

  scope.Spawn(RunInTurn(mutex, Work("first", first, log)));
  scope.Spawn(RunInTurn(mutex, Work("second", second, log)));
  EXPECT_THAT(log, ElementsAre("first begins"));

  first.Open();
  EXPECT_THAT(log, ElementsAre("first begins", "first ends", "second begins"));

  second.Open();
  EXPECT_EQ(scope.unfinished(), 0);
}

// Turns are taken in the order they were asked for.
TEST(MutexTest, ServesTasksInTheOrderTheyAsked) {
  Mutex mutex;
  Gate gate;
  std::vector<std::string> log;
  TaskScope scope;

  scope.Spawn(RunInTurn(mutex, Work("holder", gate, log)));
  scope.Spawn(RunInTurn(mutex, Note("second", log)));
  scope.Spawn(RunInTurn(mutex, Note("third", log)));
  scope.Spawn(RunInTurn(mutex, Note("fourth", log)));
  EXPECT_THAT(log, ElementsAre("holder begins"));

  gate.Open();

  EXPECT_THAT(log, ElementsAre("holder begins", "holder ends", "second",
                               "third", "fourth"));
  EXPECT_EQ(scope.unfinished(), 0);
}

// A task that is abandoned while it queues is forgotten: its turn passes to
// the next, and nothing tries to wake what is no longer there.
TEST(MutexTest, PassesOverATaskAbandonedInTheQueue) {
  Mutex mutex;
  Gate gate;
  std::vector<std::string> log;
  int abandoned = 0;
  int waiting = 0;
  TaskScope scope;
  scope.Spawn(RunInTurn(mutex, Work("holder", gate, log)));

  auto gone = std::make_unique<TaskScope>();
  gone->Spawn(StoreDoubled(mutex, 1, abandoned));
  scope.Spawn(StoreDoubled(mutex, 2, waiting));
  gone.reset();

  gate.Open();

  EXPECT_EQ(abandoned, 0);
  EXPECT_EQ(waiting, 4);
}

// -----------------------------------------------------------------------------
// Benchmarks
// -----------------------------------------------------------------------------
//
//   bazel run -c opt //async:mutex_test -- --benchmark_filter=all

Task<> RunMany(Mutex& mutex, benchmark::State& state) {
  for (auto _ : state) {
    benchmark::DoNotOptimize(co_await mutex.Run(Double(21)));
  }
}

// A turn nobody else wants: what a mutex costs the caller it does not delay.
void BM_UncontendedTurn(benchmark::State& state) {
  Mutex mutex;
  TaskScope scope;
  scope.Spawn(RunMany(mutex, state));
}
BENCHMARK(BM_UncontendedTurn);

}  // namespace
