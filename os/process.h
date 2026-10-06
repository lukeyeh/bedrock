// Other programs, run as child processes.
//
//   CO_ASSIGN_OR_RETURN(const os::ProcessResult result,
//                       co_await os::RunProcess({
//                           .arguments = {"git", "status", "--short"},
//                           .time_limit = std::chrono::seconds(10),
//                       }));
//   if (result.exit_code == 0) Use(result.output);
//
// Running a program waits, for as long as the program takes, without
// blocking the thread: other tasks carry on meanwhile, and several programs
// can be run side by side. Like the operations in io.h, RunProcess needs an
// IoDriver attached to the thread, which a task on an EventLoop has.

#ifndef OS_PROCESS_H_
#define OS_PROCESS_H_

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "async/task.h"
#include "os/io.h"

namespace os {

struct Command {
  // The program, then the arguments to give it. A program named without a
  // slash is looked for in the directories of PATH.
  std::vector<std::string> arguments;

  // The directory the program starts in. Empty means this process's own.
  std::string directory;

  // How long the program may run. When the time is up it is killed, along
  // with everything it started. Unset means for as long as it likes.
  std::optional<Duration> time_limit;

  // How much of the program's output to keep. The rest is discarded.
  size_t max_output_bytes = size_t{1024} * 1024;
};

struct ProcessResult {
  // What the program wrote to its standard output and standard error,
  // together, in the order it wrote them.
  std::string output;
  // The program wrote more than Command::max_output_bytes; `output` is the
  // beginning of it.
  bool output_truncated = false;

  // What the program exited with, where 0 conventionally means success.
  // Unset if it did not exit of its own accord: it was killed by a signal
  // or for running out of time.
  std::optional<int> exit_code;
  // The program was killed because Command::time_limit ran out.
  bool timed_out = false;
};

// Runs `command` to its end and evaluates to what came of it. The program
// reads nothing: its standard input is empty.
//
// A program that fails has still been run: a non-zero exit code or running
// out of time is a result, not an error. Fails only when the program could
// not be run: with InvalidArgument if `command` names none, NotFound if
// there is no such program or directory, and PermissionDenied if it may not
// be run.
//
// The program is finished when nothing holds its output open any more, so a
// program that leaves something running in the background is waited for
// until that ends too, or the time limit does away with both. Abandoning the
// task kills the program.
Task<absl::StatusOr<ProcessResult>> RunProcess(Command command);

}  // namespace os

#endif  // OS_PROCESS_H_
