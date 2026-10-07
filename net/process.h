// Another program as a stream. What is written to the stream is the
// program's standard input, and what is read from it is what the program
// writes to its standard output, so a protocol spoken with a program is read
// and written exactly as one spoken with a server is.
//
//   ABSL_ASSIGN_OR_RETURN(const std::unique_ptr<net::Process> process,
//                         net::Process::Start({.arguments = {"bc"}}));
//   CO_RETURN_IF_ERROR(co_await process->Write("6 * 7\n"));
//
//   net::Reader reader(process.get());
//   co_await reader.ReadUntil("\n", 100, deadline);  // "42"

#ifndef NET_PROCESS_H_
#define NET_PROCESS_H_

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "async/task.h"
#include "net/stream.h"
#include "os/process.h"

namespace net {

// A running program. The end of the stream is the program closing its
// output, which exiting does. What the program writes to its standard error
// goes wherever this process's own does.
//
// Destroying it kills the program, and everything the program started,
// unless Finish has seen it out.
class Process final : public Stream {
 public:
  // Starts `program`. Fails with InvalidArgument if it names none, NotFound
  // if there is no such program or directory, and PermissionDenied if it may
  // not be run. Must be called from a task running on an EventLoop.
  static absl::StatusOr<std::unique_ptr<Process>> Start(
      const os::Program& program);

  Task<absl::StatusOr<size_t>> Read(std::span<char> buffer,
                                    Deadline deadline) override;
  Task<absl::Status> Write(std::string_view data) override;

  // Ends the conversation politely: tells the program there is no more
  // input, discards whatever it still has to say, and waits for it to exit.
  // Evaluates to what it exited with, where 0 conventionally means success.
  // A program still running at `deadline` is killed; the result is then
  // empty, as it is for a program killed by a signal.
  //
  // The stream is finished afterwards. No read or write may be in progress.
  Task<std::optional<int>> Finish(Deadline deadline);

 private:
  explicit Process(os::Process process) : process_(std::move(process)) {}

  os::Process process_;
};

}  // namespace net

#endif  // NET_PROCESS_H_
