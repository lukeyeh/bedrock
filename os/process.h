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
//
// RunProcess is for a program that is run and then read. A program that is
// talked to while it runs is a Process:
//
//   ABSL_ASSIGN_OR_RETURN(os::Process process, os::Process::Start({
//                                                  .arguments = {"bc"},
//                                              }));
//   co_await os::Send(process.io(), "6 * 7\n", {});
//   co_await os::Receive(process.io(), buffer);  // "42\n"

#ifndef OS_PROCESS_H_
#define OS_PROCESS_H_

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "async/task.h"
#include "os/io.h"
#include "os/socket.h"

namespace os_internal {
class Child;
}  // namespace os_internal

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

// What a Process runs.
struct Program {
  // The program, then the arguments to give it. A program named without a
  // slash is looked for in the directories of PATH.
  std::vector<std::string> arguments;

  // The directory the program starts in. Empty means this process's own.
  std::string directory;
};

// A program that is talked to while it runs: what is sent to it is its
// standard input, and what is received from it is what it writes to its
// standard output. What it writes to its standard error goes wherever this
// process's own does.
//
// Destroying a Process kills the program, and everything the program
// started, unless Exit has seen it out. It must not be destroyed while an
// operation on io() is in progress.
class Process {
 public:
  // Starts `program`. Fails with InvalidArgument if it names none, NotFound
  // if there is no such program or directory, and PermissionDenied if it may
  // not be run.
  static absl::StatusOr<Process> Start(const Program& program);

  Process(Process&&);
  Process& operator=(Process&&);
  ~Process();

  // The program's standard input and output, for os::Send and os::Receive.
  // Receiving nothing means that the program, and whatever it started, has
  // closed its output, which exiting does.
  const Socket& io() const;

  // Tells the program there is no more input: it reads the end of its
  // standard input once it has read what was sent. Its output can still be
  // received. Nothing may be sent afterwards.
  void CloseInput();

  // Waits for the program to exit, and evaluates to what it exited with,
  // where 0 conventionally means success. If `time_limit` runs out first the
  // program is killed, along with everything it started. Evaluates to
  // nothing if the program did not exit of its own accord: it was killed,
  // for running out of time or by a signal.
  //
  // A program that writes more than it is read from waits to be read, so
  // receive its output to the end before waiting here.
  Task<std::optional<int>> Exit(std::optional<Duration> time_limit = {});

 private:
  explicit Process(std::unique_ptr<os_internal::Child> child);

  std::unique_ptr<os_internal::Child> child_;
};

}  // namespace os

#endif  // OS_PROCESS_H_
