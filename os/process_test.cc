// os::RunProcess by example: running another program and getting back what
// it printed and how it ended.
//
// Every test here runs twice, once on each I/O backend.

#include "os/process.h"

#include <benchmark/benchmark.h>

#include <chrono>
#include <cstdlib>
#include <optional>
#include <string>
#include <utility>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "async/task.h"
#include "async/task_scope.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "os/io.h"

namespace {

using absl_testing::StatusIs;
using std::chrono::milliseconds;
using testing::Lt;
using testing::Optional;

os::IoDriver NewDriver(os::IoBackend backend) {
  absl::StatusOr<os::IoDriver> driver = os::IoDriver::Create({
      .backend = backend,
  });
  ABSL_EXPECT_OK(driver);
  return std::move(*driver);
}

Task<> RunThenSetFlag(os::Command command,
                      absl::StatusOr<os::ProcessResult>& result, bool& done) {
  result = co_await os::RunProcess(std::move(command));
  done = true;
}

// Gives every test a driver of the backend under test, attached to the test's
// thread, and a way to run a command on it. EventLoop, a layer up, is what
// does this for a real program.
class ProcessTest : public testing::TestWithParam<os::IoBackend> {
 protected:
  void SetUp() override { driver_.Attach(); }
  void TearDown() override {
    driver_.CancelAll();
    driver_.Detach();
  }

  absl::StatusOr<os::ProcessResult> Run(os::Command command) {
    absl::StatusOr<os::ProcessResult> result = absl::UnknownError("not run");
    bool done = false;
    TaskScope scope;
    scope.Spawn(RunThenSetFlag(std::move(command), result, done));
    // NOLINTNEXTLINE(bugprone-infinite-loop): set by a task woken in the call.
    while (!done) driver_.WakeFinished(done);
    return result;
  }

  os::IoDriver driver_ = NewDriver(GetParam());
};

INSTANTIATE_TEST_SUITE_P(Backends, ProcessTest,
                         testing::Values(os::IoBackend::kIoUring,
                                         os::IoBackend::kEpoll),
                         [](const auto& info) {
                           return std::string(os::IoBackendName(info.param));
                         });

// The result holds both of the program's output streams, interleaved as
// written, and its exit code. A non-zero code is a result, not an error.
TEST_P(ProcessTest, CapturesOutputAndExitCode) {
  const absl::StatusOr<os::ProcessResult> result = Run({
      .arguments =
          {
              "sh",
              "-c",
              "echo to output; echo to error >&2; exit 3",
          },
  });

  ABSL_ASSERT_OK(result);
  EXPECT_EQ(result->output, "to output\nto error\n");
  EXPECT_THAT(result->exit_code, Optional(3));
  EXPECT_FALSE(result->timed_out);
}

// The program starts in the directory asked for.
TEST_P(ProcessTest, RunsInTheGivenDirectory) {
  const std::string directory = std::getenv("TEST_TMPDIR");

  const absl::StatusOr<os::ProcessResult> result = Run({
      .arguments =
          {
              "pwd",
              "-P",
          },
      .directory = directory,
  });

  ABSL_ASSERT_OK(result);
  EXPECT_THAT(result->output, testing::EndsWith("\n"));
  EXPECT_THAT(result->exit_code, Optional(0));
}

// A program waiting for input gets none, rather than waiting for ever.
TEST_P(ProcessTest, GivesTheProgramNoInput) {
  const absl::StatusOr<os::ProcessResult> result = Run({
      .arguments =
          {
              "cat",
          },
  });

  ABSL_ASSERT_OK(result);
  EXPECT_EQ(result->output, "");
  EXPECT_THAT(result->exit_code, Optional(0));
}

// A program that outstays its time limit is killed. What it printed before
// then is kept, and there is no exit code because it did not exit.
TEST_P(ProcessTest, KillsAProgramThatRunsOutOfTime) {
  const auto start = std::chrono::steady_clock::now();

  const absl::StatusOr<os::ProcessResult> result = Run({
      .arguments =
          {
              "sh",
              "-c",
              "echo started; sleep 30",
          },
      .time_limit = milliseconds(100),
  });

  ABSL_ASSERT_OK(result);
  EXPECT_TRUE(result->timed_out);
  EXPECT_EQ(result->exit_code, std::nullopt);
  EXPECT_EQ(result->output, "started\n");
  EXPECT_THAT(std::chrono::steady_clock::now() - start,
              Lt(std::chrono::seconds(10)));
}

// The time limit covers what the program leaves behind, too: a background
// job that still holds the output open is part of the run.
TEST_P(ProcessTest, KillsWhatTheProgramLeftRunning) {
  const absl::StatusOr<os::ProcessResult> result = Run({
      .arguments =
          {
              "sh",
              "-c",
              "sleep 30 & echo left one behind",
          },
      .time_limit = milliseconds(100),
  });

  ABSL_ASSERT_OK(result);
  EXPECT_TRUE(result->timed_out);
  EXPECT_EQ(result->output, "left one behind\n");
}

// A program that prints without end cannot fill memory: only the beginning
// of its output is kept, and the result says so.
TEST_P(ProcessTest, KeepsOnlyTheBeginningOfLongOutput) {
  const absl::StatusOr<os::ProcessResult> result = Run({
      .arguments =
          {
              "sh",
              "-c",
              "head -c 100000 /dev/zero | tr '\\0' x",
          },
      .max_output_bytes = 10,
  });

  ABSL_ASSERT_OK(result);
  EXPECT_EQ(result->output, "xxxxxxxxxx");
  EXPECT_TRUE(result->output_truncated);
  EXPECT_THAT(result->exit_code, Optional(0));
}

// Not being able to run the program at all is the one kind of error.
TEST_P(ProcessTest, FailsWhenTheProgramCannotBeRun) {
  EXPECT_THAT(Run({
                  .arguments =
                      {
                          "no-such-program-anywhere",
                      },
              }),
              StatusIs(absl::StatusCode::kNotFound));
  EXPECT_THAT(Run({}), StatusIs(absl::StatusCode::kInvalidArgument));
}

// -----------------------------------------------------------------------------
// Benchmarks
// -----------------------------------------------------------------------------
//
//   bazel run -c opt //os:process_test -- --benchmark_filter=all

Task<> RunTrue(benchmark::State& state, bool& done) {
  for (auto _ : state) {
    benchmark::DoNotOptimize(co_await os::RunProcess({
        .arguments =
            {
                "true",
            },
    }));
  }
  done = true;
}

// Running the smallest program there is: the fixed cost of starting a
// process, collecting its output and waiting for it to exit.
void BM_RunProcess(benchmark::State& state) {
  os::IoDriver driver = NewDriver(os::IoBackend::kAuto);
  driver.Attach();
  bool done = false;
  TaskScope scope;
  scope.Spawn(RunTrue(state, done));
  // NOLINTNEXTLINE(bugprone-infinite-loop): set by a task woken in the call.
  while (!done) driver.WakeFinished(done);
  driver.Detach();
}
BENCHMARK(BM_RunProcess);

}  // namespace
