#include "os/random.h"

#include <sys/random.h>

#include <cerrno>
#include <cstddef>
#include <string>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

namespace os {

absl::StatusOr<std::string> RandomBytes(const size_t count) {
  std::string bytes(count, '\0');

  size_t filled = 0;
  while (filled < count) {
    // May supply fewer than asked for, or be interrupted by a signal before
    // supplying any.
    const ssize_t got = getrandom(bytes.data() + filled, count - filled, 0);
    if (got < 0) {
      if (errno == EINTR) continue;

      return absl::ErrnoToStatus(errno, "getrandom");
    }

    filled += static_cast<size_t>(got);
  }

  return bytes;
}

}  // namespace os
