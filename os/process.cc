#include "os/process.h"

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "async/task.h"
#include "os/io.h"
#include "os/socket.h"

// The environment the child inherits.
extern char** environ;  // NOLINT(readability-redundant-declaration)

namespace os {
namespace {

using Clock = std::chrono::steady_clock;

// How often a program that has closed its output is checked for having
// exited. Nearly always it has by the first look.
constexpr Duration kExitPollInterval = std::chrono::milliseconds(1);

// A running child process and the socket its output arrives on. Destroying
// it kills the child, and whatever the child started, if it has not been
// seen to exit.
class Child {
 public:
  Child(pid_t process, Socket output)
      : process_(process), output_(std::move(output)) {}
  Child(Child&& other)
      : process_(std::exchange(other.process_, -1)),
        output_(std::move(other.output_)) {}
  ~Child() {
    if (process_ > 0) Kill();
  }

  const Socket& output() const { return output_; }

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
  Socket output_;
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

// Starts the program, with its output going to one end of a socket pair and
// the other end kept here. A socket rather than a pipe, because sockets are
// what the operations in io.h work on.
absl::StatusOr<Child> Start(const Command& command) {
  if (command.arguments.empty()) {
    return absl::InvalidArgumentError("no program to run");
  }
  const std::string& program = command.arguments.front();

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
  posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null",
                                   O_RDONLY, 0);
  posix_spawn_file_actions_adddup2(&actions, theirs.get(), STDOUT_FILENO);
  posix_spawn_file_actions_adddup2(&actions, theirs.get(), STDERR_FILENO);
  if (!command.directory.empty()) {
    posix_spawn_file_actions_addchdir_np(&actions, command.directory.c_str());
  }

  // A process group of its own, so that it and its descendants can be
  // killed together.
  posix_spawnattr_t attributes;
  posix_spawnattr_init(&attributes);
  posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
  posix_spawnattr_setpgroup(&attributes, 0);

  std::vector<char*> arguments;
  arguments.reserve(command.arguments.size() + 1);
  for (const std::string& argument : command.arguments) {
    // posix_spawn takes them as non-const but does not modify them.
    arguments.push_back(const_cast<char*>(argument.c_str()));
  }
  arguments.push_back(nullptr);

  pid_t process = -1;
  const int failure = posix_spawnp(&process, program.c_str(), &actions,
                                   &attributes, arguments.data(), environ);
  posix_spawnattr_destroy(&attributes);
  posix_spawn_file_actions_destroy(&actions);
  if (failure != 0) {
    return absl::ErrnoToStatus(failure, absl::StrCat("cannot run ", program));
  }

  return Child(process, os_internal::SocketFromDescriptor(ours.Release()));
}

}  // namespace

Task<absl::StatusOr<ProcessResult>> RunProcess(Command command) {
  absl::StatusOr<Child> child = Start(command);
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
        co_await Receive(child->output(), buffer, left);
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

}  // namespace os
