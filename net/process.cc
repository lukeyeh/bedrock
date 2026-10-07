#include "net/process.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

#include "absl/status/status.h"
#include "absl/status/status_macros.h"
#include "absl/status/statusor.h"
#include "async/status_macros.h"
#include "async/task.h"
#include "net/stream.h"
#include "os/io.h"
#include "os/process.h"

namespace net {
namespace {

// How long a program may refuse to take our bytes before we call it stuck.
constexpr std::chrono::seconds kWriteTimeout(30);

// The time left until `deadline`, as the limit to give one I/O operation.
// Never quite nothing, so that an operation the kernel can finish at once is
// allowed to, even when the deadline has just passed.
os::Duration Remaining(Deadline deadline) {
  return std::max<os::Duration>(deadline - std::chrono::steady_clock::now(),
                                std::chrono::milliseconds(1));
}

}  // namespace

absl::StatusOr<std::unique_ptr<Process>> Process::Start(
    const os::Program& program) {
  ABSL_ASSIGN_OR_RETURN(os::Process process, os::Process::Start(program));

  // Not make_unique, which cannot reach the private constructor.
  return std::unique_ptr<Process>(new Process(std::move(process)));
}

Task<absl::StatusOr<size_t>> Process::Read(std::span<char> buffer,
                                           Deadline deadline) {
  co_return co_await os::Receive(process_.io(), buffer, Remaining(deadline));
}

Task<absl::Status> Process::Write(std::string_view data) {
  while (!data.empty()) {
    CO_ASSIGN_OR_RETURN(
        const size_t sent,
        co_await os::Send(process_.io(), data, {}, kWriteTimeout));

    // Normally everything was sent. If not, go round again with the rest.
    data.remove_prefix(sent);
  }

  co_return absl::OkStatus();
}

Task<std::optional<int>> Process::Finish(Deadline deadline) {
  process_.CloseInput();

  // A program with more to say than fits in the kernel's buffers cannot exit
  // until it has been heard out.
  std::array<char, 16384> discarded = {};
  for (;;) {
    const absl::StatusOr<size_t> read = co_await Read(discarded, deadline);
    if (!read.ok() || *read == 0) break;
  }

  co_return co_await process_.Exit(Remaining(deadline));
}

}  // namespace net
