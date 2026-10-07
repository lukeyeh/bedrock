// Random bytes from the kernel: unpredictable to anyone, so fit for things
// that are secret because they cannot be guessed, such as a session token.

#ifndef OS_RANDOM_H_
#define OS_RANDOM_H_

#include <cstddef>
#include <string>

#include "absl/status/statusor.h"

namespace os {

// `count` random bytes. Fails with Unavailable if the kernel cannot supply
// them, which on a running system it always can.
absl::StatusOr<std::string> RandomBytes(size_t count);

}  // namespace os

#endif  // OS_RANDOM_H_
