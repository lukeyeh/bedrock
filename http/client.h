// An HTTP client: give it a request, get back the server's response.
//
// The client owns everything between those two: connecting (with TLS for
// https URLs), keeping the connection open for the next request, reconnecting
// when the server has dropped it, and bounding how long any of it takes.
//
// Send hands back the whole response at once, which suits a web API's
// replies. Open hands back the head and leaves the body to be read as it
// arrives, which suits a body produced over time, such as a stream of
// server-sent events (see http/event_stream.h).
//
// Both are asynchronous (see async/task.h) and must be called from a task
// running on an EventLoop.

#ifndef HTTP_CLIENT_H_
#define HTTP_CLIENT_H_

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/statusor.h"
#include "async/task.h"
#include "http/head.h"
#include "net/stream.h"

namespace http {

enum class Method { kGet, kPost, kPut, kPatch, kDelete };

struct Request {
  Method method = Method::kGet;
  // Absolute, http or https.
  std::string url;
  // In addition to Host and Content-Length, which the client supplies.
  std::vector<Header> headers;
  std::string body;
};

struct Response {
  int status = 0;
  std::vector<Header> headers;
  std::string body;

  // The value of header `name`, or an empty string if it was not sent.
  std::string_view Get(std::string_view name) const {
    return FindHeader(headers, name);
  }
};

// The body of a response from Open, read piece by piece. Destroying it before
// its end abandons the rest.
class Body {
 public:
  virtual ~Body() = default;

  // Evaluates to the next piece of the body: whatever has arrived, waiting
  // if nothing has, or an empty view at the end of the body. The view is
  // valid only until the next call. Fails with DeadlineExceeded if nothing
  // arrives in time, after which the body is still readable; with
  // InvalidArgument if what arrives is not HTTP and Unavailable if the
  // server goes away, both of which are final.
  virtual Task<absl::StatusOr<std::string_view>> Next(
      net::Deadline deadline) = 0;
};

// A response whose body is still to be read.
struct OpenResponse {
  int status = 0;
  std::vector<Header> headers;
  // Never null.
  std::unique_ptr<Body> body;

  // The value of header `name`, or an empty string if it was not sent.
  std::string_view Get(std::string_view name) const {
    return FindHeader(headers, name);
  }
};

// A client makes one request at a time: wait for a Send to finish, or for
// the body from an Open to end or be destroyed, before starting the next.
class Client {
 public:
  virtual ~Client() = default;

  // Sends `request` and evaluates to the response, whatever its status: a 404
  // or a 500 is a response, not a failure. Fails only when no response could
  // be had: InvalidArgument for a URL that cannot be requested or a reply
  // that is not HTTP, Unavailable or NotFound if the server cannot be
  // reached, Unauthenticated if it is not who the URL says, DeadlineExceeded
  // if it is too slow.
  virtual Task<absl::StatusOr<Response>> Send(const Request& request) = 0;

  // Sends `request` and evaluates to the response as soon as its head has
  // arrived, which must be by `deadline`. Fails as Send does. How long the
  // body may take is for whoever reads it to say, piece by piece.
  //
  // A client that implements only Send, as a test's stand-in might, answers
  // this by sending and handing over the body as a single piece.
  virtual Task<absl::StatusOr<OpenResponse>> Open(const Request& request,
                                                  net::Deadline deadline);
};

// A client that talks to real servers.
std::unique_ptr<Client> NewClient();

}  // namespace http

#endif  // HTTP_CLIENT_H_
