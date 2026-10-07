#include "os/process.h"

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/status_macros.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "async/task.h"
#include "os/io.h"
#include "os/socket.h"

// The environment the child inherits.
extern char** environ;  // NOLINT(readability-redundant-declaration)

namespace os_internal {

// A running child process and the socket it is reached by. Destroying it
// kills the child, and whatever the child started, if it has not been seen
// to exit.
class Child {
 public:
  Child(pid_t process, os::Socket socket)
      : process_(process), socket_(std::move(socket)) {}
  Child(Child&& other)
      : process_(std::exchange(other.process_, -1)),
        socket_(std::move(other.socket_)) {}
  ~Child() {
    if (process_ > 0) Kill();
  }

  const os::Socket& socket() const { return socket_; }

  // How the child ended, if it has: its exit code, or nothing for a signal.
  // The outer optional is unset while it is still running.
  std::optional<std::optional<int>> Exited() {
    return Wait(WNOHANG);  // NOLINT(misc-include-cleaner): from sys/wait.h.
  }

  // Ends the child and everything in its process group, now.
  void Kill() {
    // The child leads a process group of its own, which a negative process
    // number addresses.
    kill(-process_, SIGKILL);
    Wait(0);
  }

 private:
  std::optional<std::optional<int>> Wait(int options) {
    int status = 0;
    pid_t waited = 0;
    do {
      waited = waitpid(process_, &status, options);
    } while (waited < 0 && errno == EINTR);
    if (waited == 0) return std::nullopt;

    process_ = -1;
    // NOLINTBEGIN(misc-include-cleaner): both are from sys/wait.h.
    if (waited > 0 && WIFEXITED(status)) {
      return std::optional<int>(WEXITSTATUS(status));
    }
    // NOLINTEND(misc-include-cleaner)
    return std::optional<int>();
  }

  // The kernel's number for the child, or -1 once it has been waited for.
  pid_t process_;
  os::Socket socket_;
};

}  // namespace os_internal

namespace os {
namespace {

using Clock = std::chrono::steady_clock;
using os_internal::Child;

// How often a program that has closed its output is checked for having
// exited. Nearly always it has by the first look.
constexpr Duration kExitPollInterval = std::chrono::milliseconds(1);

// The longest a program that is taking its time to exit goes unchecked.
constexpr Duration kSlowestExitPollInterval = std::chrono::milliseconds(50);

// Which of the program's standard streams the socket carries.
enum class Wiring : uint8_t {
  // Its output and its errors, together. It is given no input.
  kOutputAndErrors,
  // Its input and its output. Its errors go where this process's do.
  kInputAndOutput,
};

// A descriptor that is closed unless released.
class Descriptor {
 public:
  explicit Descriptor(int descriptor) : descriptor_(descriptor) {}
  Descriptor(const Descriptor&) = delete;
  Descriptor& operator=(const Descriptor&) = delete;
  ~Descriptor() {
    if (descriptor_ >= 0) close(descriptor_);
  }

  int get() const { return descriptor_; }
  int Release() { return std::exchange(descriptor_, -1); }

 private:
  int descriptor_;
};

// Starts the program, with one end of a socket pair as its standard streams
// and the other end kept here. A socket rather than a pipe, because sockets
// are what the operations in io.h work on.
absl::StatusOr<Child> StartChild(const std::vector<std::string>& arguments,
                                 const std::string& directory, Wiring wiring) {
  if (arguments.empty()) {
    return absl::InvalidArgumentError("no program to run");
  }
  const std::string& program = arguments.front();

  std::array<int, 2> ends = {};
  if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, ends.data()) != 0) {
    return absl::ErrnoToStatus(errno, "cannot create a socket pair");
  }
  Descriptor ours(ends[0]);
  const Descriptor theirs(ends[1]);

  // What the child does to its descriptors between being created and
  // becoming the program.
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  switch (wiring) {
    case Wiring::kOutputAndErrors:
      posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null",
                                       O_RDONLY, 0);
      posix_spawn_file_actions_adddup2(&actions, theirs.get(), STDOUT_FILENO);
      posix_spawn_file_actions_adddup2(&actions, theirs.get(), STDERR_FILENO);
      break;
    case Wiring::kInputAndOutput:
      posix_spawn_file_actions_adddup2(&actions, theirs.get(), STDIN_FILENO);
      posix_spawn_file_actions_adddup2(&actions, theirs.get(), STDOUT_FILENO);
      break;
  }
  if (!directory.empty()) {
    posix_spawn_file_actions_addchdir_np(&actions, directory.c_str());
  }

  // A process group of its own, so that it and its descendants can be
  // killed together.
  posix_spawnattr_t attributes;
  posix_spawnattr_init(&attributes);
  posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
  posix_spawnattr_setpgroup(&attributes, 0);

  std::vector<char*> pointers;
  pointers.reserve(arguments.size() + 1);
  for (const std::string& argument : arguments) {
    // posix_spawn takes them as non-const but does not modify them.
    pointers.push_back(const_cast<char*>(argument.c_str()));
  }
  pointers.push_back(nullptr);

  pid_t process = -1;
  const int failure = posix_spawnp(&process, program.c_str(), &actions,
                                   &attributes, pointers.data(), environ);
  posix_spawnattr_destroy(&attributes);
  posix_spawn_file_actions_destroy(&actions);
  if (failure != 0) {
    return absl::ErrnoToStatus(failure, absl::StrCat("cannot run ", program));
  }

  return Child(process, os_internal::SocketFromDescriptor(ours.Release()));
}

}  // namespace

Task<absl::StatusOr<ProcessResult>> RunProcess(Command command) {
  absl::StatusOr<Child> child = StartChild(command.arguments, command.directory,
                                           Wiring::kOutputAndErrors);
  if (!child.ok()) co_return child.status();

  const std::optional<Clock::time_point> deadline =
      command.time_limit.has_value()
          ? std::optional(Clock::now() + *command.time_limit)
          : std::nullopt;
  // The time left, which is none once the deadline has passed.
  const auto remaining = [&deadline]() -> std::optional<Duration> {
    if (!deadline.has_value()) return std::nullopt;
    return std::max<Duration>(*deadline - Clock::now(), Duration::zero());
  };

  ProcessResult result;
  std::array<char, 16384> buffer = {};
  for (;;) {
    const std::optional<Duration> left = remaining();
    // Not left to Receive to notice, which reads what there is to read
    // without looking at the clock: a program that never stops writing would
    // never be stopped.
    if (left == Duration::zero()) {
      result.timed_out = true;
      break;
    }

    const absl::StatusOr<size_t> received =
        co_await Receive(child->socket(), buffer, left);
    if (absl::IsDeadlineExceeded(received.status())) {
      result.timed_out = true;
      break;
    }
    if (!received.ok()) co_return received.status();
    // Everything that had the output open has closed it.
    if (*received == 0) break;

    const size_t kept =
        std::min(*received, command.max_output_bytes - result.output.size());
    result.output.append(buffer.data(), kept);
    if (kept < *received) result.output_truncated = true;
  }

  // A program can close its output and carry on, so wait for it to exit as
  // well, for as long as it is allowed.
  while (!result.timed_out) {
    const std::optional<std::optional<int>> exited = child->Exited();
    if (exited.has_value()) {
      result.exit_code = *exited;
      co_return result;
    }

    if (remaining() == Duration::zero()) {
      result.timed_out = true;
    } else {
      co_await Sleep(kExitPollInterval);
    }
  }

  child->Kill();
  co_return result;
}

absl::StatusOr<Process> Process::Start(const Program& program) {
  ABSL_ASSIGN_OR_RETURN(Child child,
                        StartChild(program.arguments, program.directory,
                                   Wiring::kInputAndOutput));

  return Process(std::make_unique<Child>(std::move(child)));
}

Process::Process(std::unique_ptr<Child> child) : child_(std::move(child)) {}
Process::Process(Process&&) = default;
Process& Process::operator=(Process&&) = default;
Process::~Process() = default;

const Socket& Process::io() const { return child_->socket(); }

void Process::CloseInput() {
  // Fails only if the program has already gone, which comes to the same.
  shutdown(os_internal::DescriptorOf(child_->socket()), SHUT_WR);
}

Task<std::optional<int>> Process::Exit(std::optional<Duration> time_limit) {
  const std::optional<Clock::time_point> deadline =
      time_limit.has_value() ? std::optional(Clock::now() + *time_limit)
                             : std::nullopt;

  // Looked for often at first, since a program that has been told to stop
  // usually has, and then less and less often.
  Duration interval = kExitPollInterval;
  for (;;) {
    const std::optional<std::optional<int>> exited = child_->Exited();
    if (exited.has_value()) co_return *exited;

    if (deadline.has_value() && Clock::now() >= *deadline) break;

    co_await Sleep(interval);
    interval = std::min(interval * 2, kSlowestExitPollInterval);
  }

  child_->Kill();
  co_return std::nullopt;
}

}  // namespace os
