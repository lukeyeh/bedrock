// Server-sent events: a response body that is a series of small messages,
// each sent when the server has something to say. This is how web APIs
// report progress on slow work, such as a language model writing its reply.
//
//   CO_ASSIGN_OR_RETURN(http::OpenResponse response,
//                       co_await client.Open(request, deadline));
//   http::EventReader events(response.body.get());
//   for (;;) {
//     CO_ASSIGN_OR_RETURN(std::optional<http::ServerEvent> event,
//                         co_await events.Next(net::After(kPatience)));
//     if (!event.has_value()) break;
//     ...
//   }

#ifndef HTTP_EVENT_STREAM_H_
#define HTTP_EVENT_STREAM_H_

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "absl/status/statusor.h"
#include "async/task.h"
#include "http/client.h"
#include "net/stream.h"

namespace http {

struct ServerEvent {
  // What kind of event the server says this is; "message" if it does not.
  std::string type = "message";
  // The event's payload. Several lines if the server sent several.
  std::string data;
};

// Reads a body as server-sent events, however the network splits it.
class EventReader {
 public:
  // `body` must outlive the reader, and must not be read directly while the
  // reader is in use.
  explicit EventReader(Body* body) : body_(body) {}

  // Evaluates to the next event, or to nothing at the end of the body.
  // Comments and events without data, which servers send to keep a
  // connection open, are skipped. Fails as Body::Next does, with
  // DeadlineExceeded leaving the reader usable, and with ResourceExhausted
  // if a single event is implausibly large.
  Task<absl::StatusOr<std::optional<ServerEvent>>> Next(net::Deadline deadline);

 private:
  // Removes and returns the next complete line received, without its line
  // ending, or nothing if no complete line has arrived yet.
  std::optional<std::string_view> TakeLine();

  // Adds a line to the event being put together.
  void AddField(std::string_view line);

  Body* body_;
  bool ended_ = false;

  // What has been received. Bytes before consumed_ have been dealt with.
  std::string buffer_;
  size_t consumed_ = 0;
  // The last line ended with a carriage return; a line feed arriving next
  // belongs to that ending.
  bool after_carriage_return_ = false;

  // The event whose lines are arriving.
  ServerEvent pending_;
  bool has_data_ = false;
};

}  // namespace http

#endif  // HTTP_EVENT_STREAM_H_
