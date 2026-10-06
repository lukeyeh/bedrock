#include "http/event_stream.h"

#include <cstddef>
#include <optional>
#include <string_view>
#include <utility>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "async/status_macros.h"
#include "async/task.h"
#include "net/stream.h"

namespace http {
namespace {

// More than any one event should be; a bound on what a server can make the
// reader hold in memory.
constexpr size_t kMaxEventBytes = size_t{16} * 1024 * 1024;

}  // namespace

Task<absl::StatusOr<std::optional<ServerEvent>>> EventReader::Next(
    net::Deadline deadline) {
  for (;;) {
    while (const std::optional<std::string_view> line = TakeLine()) {
      if (!line->empty()) {
        AddField(*line);
        continue;
      }

      // A blank line ends an event.
      ServerEvent event = std::exchange(pending_, ServerEvent());
      if (std::exchange(has_data_, false)) co_return event;
    }

    // An event cut short by the end of the body is not an event.
    if (ended_) co_return std::nullopt;

    CO_ASSIGN_OR_RETURN(const std::string_view piece,
                        co_await body_->Next(deadline));
    if (piece.empty()) {
      ended_ = true;
      continue;
    }

    buffer_.erase(0, std::exchange(consumed_, 0));
    if (buffer_.size() + piece.size() > kMaxEventBytes) {
      co_return absl::ResourceExhaustedError("server-sent event too large");
    }
    buffer_.append(piece);
  }
}

std::optional<std::string_view> EventReader::TakeLine() {
  if (after_carriage_return_ && consumed_ < buffer_.size()) {
    if (buffer_[consumed_] == '\n') ++consumed_;
    after_carriage_return_ = false;
  }

  // A line ends with CR, LF or both.
  const size_t end = buffer_.find_first_of("\r\n", consumed_);
  if (end == std::string_view::npos) return std::nullopt;

  const std::string_view line =
      std::string_view(buffer_).substr(consumed_, end - consumed_);
  after_carriage_return_ = buffer_[end] == '\r';
  consumed_ = end + 1;
  return line;
}

void EventReader::AddField(std::string_view line) {
  // "name: value", where the space is optional and a line with no colon is
  // a name alone. A line starting with a colon is a comment.
  const size_t colon = line.find(':');
  const std::string_view name = line.substr(0, colon);
  std::string_view value =
      colon == std::string_view::npos ? "" : line.substr(colon + 1);
  if (value.starts_with(' ')) value.remove_prefix(1);

  if (name == "event") {
    pending_.type = value;
  } else if (name == "data") {
    // Each further line of data starts a new line of the payload.
    if (has_data_) pending_.data.push_back('\n');
    pending_.data.append(value);
    has_data_ = true;
  }
  // "id" and "retry" are for resuming a broken stream, which this does not.
}

}  // namespace http
