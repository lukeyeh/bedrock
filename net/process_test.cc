// net::Process by example: holding a conversation with another program over
// a stream, and ending it.

#include "net/process.h"

#include <benchmark/benchmark.h>

#include <array>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "async/task.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "net/event_loop.h"
#include "net/reader.h"
#include "net/stream.h"

namespace {

using absl_testing::IsOkAndHolds;
using absl_testing::StatusIs;
using testing::Optional;

void RunOnEventLoop(Task<> test) {
  absl::StatusOr<EventLoop> loop = EventLoop::Create();
  ABSL_ASSERT_OK(loop);
  loop->Run(std::move(test));
}

net::Deadline Soon() { return net::After(std::chrono::seconds(5)); }

// A program that answers each line it is given, until there are no more.
absl::StatusOr<std::unique_ptr<net::Process>> StartEcho() {
  return net::Process::Start({
      .arguments =
          {
              "sh",
              "-c",
              "while read line; do echo \"you said $line\"; done; exit 4",
          },
  });
}

// A program is read and written like any other stream, so a Reader gives its
// output a line at a time, and the conversation can go on as long as wanted.
TEST(ProcessTest, IsAStreamToAProgram) {
  RunOnEventLoop([]() -> Task<> {
    const absl::StatusOr<std::unique_ptr<net::Process>> process = StartEcho();
    ABSL_EXPECT_OK(process);
    if (!process.ok()) co_return;
    net::Reader reader(process->get());

    ABSL_EXPECT_OK(co_await (*process)->Write("hello\n"));
    EXPECT_THAT(co_await reader.ReadUntil("\n", 100, Soon()),
                IsOkAndHolds("you said hello"));

    ABSL_EXPECT_OK(co_await (*process)->Write("goodbye\n"));
    EXPECT_THAT(co_await reader.ReadUntil("\n", 100, Soon()),
                IsOkAndHolds("you said goodbye"));
  }());
}

// A program with nothing to say costs the caller no more than the deadline,
// and can still be talked to afterwards.
TEST(ProcessTest, ReadGivesUpAtTheDeadlineAndStaysUsable) {
  RunOnEventLoop([]() -> Task<> {
    const absl::StatusOr<std::unique_ptr<net::Process>> process = StartEcho();
    ABSL_EXPECT_OK(process);
    if (!process.ok()) co_return;
    std::array<char, 64> buffer = {};

    EXPECT_THAT(co_await (*process)->Read(
                    buffer, net::After(std::chrono::milliseconds(10))),
                StatusIs(absl::StatusCode::kDeadlineExceeded));

    ABSL_EXPECT_OK(co_await (*process)->Write("late\n"));
    EXPECT_THAT(co_await (*process)->Read(buffer, Soon()),
                IsOkAndHolds(std::string_view("you said late\n").size()));
  }());
}

// Finish closes the program's input, which is how most programs know to
// stop, and reports how it exited.
TEST(ProcessTest, FinishSeesTheProgramOut) {
  RunOnEventLoop([]() -> Task<> {
    const absl::StatusOr<std::unique_ptr<net::Process>> process = StartEcho();
    ABSL_EXPECT_OK(process);
    if (!process.ok()) co_return;

    // Never read: Finish discards it, so that the program is not left
    // waiting to be heard.
    ABSL_EXPECT_OK(co_await (*process)->Write("unheard\n"));

    EXPECT_THAT(co_await (*process)->Finish(Soon()), Optional(4));
  }());
}

// A program that ignores the end of its input is not waited for beyond the
// deadline.
TEST(ProcessTest, FinishKillsAProgramThatWillNotStop) {
  RunOnEventLoop([]() -> Task<> {
    const absl::StatusOr<std::unique_ptr<net::Process>> process =
        net::Process::Start({
            .arguments =
                {
                    "sleep",
                    "30",
                },
        });
    ABSL_EXPECT_OK(process);
    if (!process.ok()) co_return;

    EXPECT_EQ(
        co_await (*process)->Finish(net::After(std::chrono::milliseconds(100))),
        std::nullopt);
  }());
}

// The program exiting is the end of the stream, not a failure.
TEST(ProcessTest, TheProgramExitingEndsTheStream) {
  RunOnEventLoop([]() -> Task<> {
    const absl::StatusOr<std::unique_ptr<net::Process>> process =
        net::Process::Start({
            .arguments =
                {
                    "true",
                },
        });
    ABSL_EXPECT_OK(process);
    if (!process.ok()) co_return;
    std::array<char, 64> buffer = {};

    EXPECT_THAT(co_await (*process)->Read(buffer, Soon()), IsOkAndHolds(0));
  }());
}

// A program that cannot be started is an error straight away.
TEST(ProcessTest, FailsToStartWhatCannotBeRun) {
  RunOnEventLoop([]() -> Task<> {
    EXPECT_THAT(net::Process::Start({
                    .arguments =
                        {
                            "no-such-program-anywhere",
                        },
                }),
                StatusIs(absl::StatusCode::kNotFound));
    co_return;
  }());
}

// -----------------------------------------------------------------------------
// Benchmarks
// -----------------------------------------------------------------------------
//
//   bazel run -c opt //net:process_test -- --benchmark_filter=all

Task<> PingPong(benchmark::State& state) {
  const absl::StatusOr<std::unique_ptr<net::Process>> process =
      net::Process::Start({
          .arguments =
              {
                  "cat",
              },
      });
  if (!process.ok()) {
    state.SkipWithError("cannot run cat");
    co_return;
  }
  net::Reader reader(process->get());

  for (auto _ : state) {
    (co_await (*process)->Write("ping\n")).IgnoreError();
    benchmark::DoNotOptimize(co_await reader.ReadUntil("\n", 100, Soon()));
  }
}

// One line to a program and the line that comes back: what a message costs
// in a protocol spoken with a child process.
void BM_LineThereAndBack(benchmark::State& state) {
  EventLoop::Create()->Run(PingPong(state));
}
BENCHMARK(BM_LineThereAndBack);

}  // namespace
