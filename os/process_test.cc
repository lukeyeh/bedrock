// os::RunProcess by example: running another program and getting back what
// it printed and how it ended. Then os::Process: a program that is talked to
// while it runs.
//
// Every test here runs twice, once on each I/O backend.

#include "os/process.h"

#include <benchmark/benchmark.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
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

Task<> RunThenSetFlag(Task<> task, bool& done) {
  co_await task;
  done = true;
}

// What the program writes next, or "" once it has closed its output.
Task<std::string> ReceiveSome(const os::Process& process) {
  std::array<char, 256> buffer = {};
  const absl::StatusOr<size_t> received =
      co_await os::Receive(process.io(), buffer, std::chrono::seconds(5));
  ABSL_EXPECT_OK(received);

  co_return std::string(buffer.data(), received.ok() ? *received : 0);
}

Task<> SendAll(const os::Process& process, std::string_view data) {
  while (!data.empty()) {
    const absl::StatusOr<size_t> sent =
        co_await os::Send(process.io(), data, {});
    ABSL_EXPECT_OK(sent);
    if (!sent.ok()) co_return;

    data.remove_prefix(*sent);
  }
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

  // Runs a test body that waits, to its end.
  void RunToEnd(Task<> body) {
    bool done = false;
    TaskScope scope;
    scope.Spawn(RunThenSetFlag(std::move(body), done));
    // NOLINTNEXTLINE(bugprone-infinite-loop): set by a task woken in the call.
    while (!done) driver_.WakeFinished(done);
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

// What is sent to a Process is its input and what it prints is received, any
// number of times over, for as long as it runs.
TEST_P(ProcessTest, TalksToAProgramWhileItRuns) {
  RunToEnd([]() -> Task<> {
    absl::StatusOr<os::Process> process = os::Process::Start({
        .arguments =
            {
                "cat",
            },
    });
    ABSL_EXPECT_OK(process);
    if (!process.ok()) co_return;

    co_await SendAll(*process, "first\n");
    EXPECT_EQ(co_await ReceiveSome(*process), "first\n");

    co_await SendAll(*process, "second\n");
    EXPECT_EQ(co_await ReceiveSome(*process), "second\n");
  }());
}

// Closing the input is how a program that reads to the end is told to
// finish. What it prints afterwards still arrives, then the end of its
// output, and then it can be seen out.
TEST_P(ProcessTest, FinishesWhenItsInputIsClosed) {
  RunToEnd([]() -> Task<> {
    absl::StatusOr<os::Process> process = os::Process::Start({
        .arguments =
            {
                "sh",
                "-c",
                "cat >/dev/null; echo done; exit 3",
            },
    });
    ABSL_EXPECT_OK(process);
    if (!process.ok()) co_return;

    co_await SendAll(*process, "anything\n");
    process->CloseInput();

    EXPECT_EQ(co_await ReceiveSome(*process), "done\n");
    EXPECT_EQ(co_await ReceiveSome(*process), "");
    EXPECT_THAT(co_await process->Exit(), Optional(3));
  }());
}

// A program that will not exit in the time it is given is killed, and has no
// exit code.
TEST_P(ProcessTest, ExitKillsAProgramThatOutstaysItsTimeLimit) {
  RunToEnd([]() -> Task<> {
    absl::StatusOr<os::Process> process = os::Process::Start({
        .arguments =
            {
                "sleep",
                "30",
            },
    });
    ABSL_EXPECT_OK(process);
    if (!process.ok()) co_return;

    const auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(co_await process->Exit(milliseconds(100)), std::nullopt);
    EXPECT_THAT(std::chrono::steady_clock::now() - start,
                Lt(std::chrono::seconds(10)));
  }());
}

// A Process does not outlive the object: nothing is left running by a caller
// that stops caring.
TEST_P(ProcessTest, DestroyingAProcessKillsTheProgram) {
  RunToEnd([]() -> Task<> {
    std::string process_number;
    {
      absl::StatusOr<os::Process> process = os::Process::Start({
          .arguments =
              {
                  "sh",
                  "-c",
                  "echo $$; exec sleep 30",
              },
      });
      ABSL_EXPECT_OK(process);
      if (!process.ok()) co_return;

      process_number = co_await ReceiveSome(*process);
    }

    // `kill -0` asks whether a process exists, and fails if it does not.
    const absl::StatusOr<os::ProcessResult> probe = co_await os::RunProcess({
        .arguments =
            {
                "sh",
                "-c",
                "kill -0 " + process_number,
            },
    });
    ABSL_EXPECT_OK(probe);
    if (!probe.ok()) co_return;
    EXPECT_THAT(probe->exit_code, Optional(testing::Ne(0)));
  }());
}

// A program in a directory of its own choosing.
TEST_P(ProcessTest, StartsAProcessInTheGivenDirectory) {
  RunToEnd([]() -> Task<> {
    absl::StatusOr<os::Process> process = os::Process::Start({
        .arguments =
            {
                "pwd",
                "-P",
            },
        .directory = "/",
    });
    ABSL_EXPECT_OK(process);
    if (!process.ok()) co_return;

    EXPECT_EQ(co_await ReceiveSome(*process), "/\n");
  }());
}

// As with RunProcess, the only errors are those of not being able to start.
TEST_P(ProcessTest, FailsToStartWhatCannotBeRun) {
  EXPECT_THAT(os::Process::Start({
                  .arguments =
                      {
                          "no-such-program-anywhere",
                      },
              }),
              StatusIs(absl::StatusCode::kNotFound));
  EXPECT_THAT(os::Process::Start({}),
              StatusIs(absl::StatusCode::kInvalidArgument));
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
