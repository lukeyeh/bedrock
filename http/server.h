// An HTTP server: hand it a listener and a handler, and it has the handler
// answer every request that arrives.
//
//   Task<http::Response> Hello(const http::Request& request) {
//     co_return http::Response{
//         .status = 200,
//         .body = absl::StrCat("You asked for ", request.url),
//     };
//   }
//
//   ABSL_ASSIGN_OR_RETURN(const net::Listener listener,
//                         net::Listener::OnLoopback(8080));
//   co_return co_await http::Serve(listener, &Hello);
//
// The server owns everything between a connection arriving and the handler
// being called, and between the handler's answer and the bytes on the wire:
// reading requests however they are framed, keeping a connection open for
// the next one, bounding how long a slow peer may take and how much it may
// send, and answering whatever is not a proper request itself.
//
// It speaks plain HTTP/1.1 on loopback. To be reached from elsewhere, and
// over HTTPS, it sits behind a proxy on the same machine that provides both.
//
// Serve is asynchronous (see async/task.h) and must be called from a task
// running on an EventLoop.

#ifndef HTTP_SERVER_H_
#define HTTP_SERVER_H_

#include <functional>

#include "absl/status/status.h"
#include "async/task.h"
#include "http/client.h"
#include "net/stream.h"

namespace http {

// Answers one request. What it is given differs from a request a client
// sends in one way: `url` is the request's target, the path and query alone
// ("/search?q=gm"), which http/form.h takes apart. The headers are as the
// peer sent them, and the body is all of it.
//
// What it returns is sent as it stands, with Content-Length added.
//
// A handler may wait, on a database or on a request of its own. While it
// does, requests on other connections are answered, so handlers run
// interleaved with one another, though never at the same instant: everything
// is on the event loop's one thread.
using Handler = std::function<Task<Response>(const Request& request)>;

// Serves the connections `listener` accepts for as long as it accepts them,
// and evaluates to why it stopped, which is never OK. `listener` must
// outlive the task.
//
// A handler is only ever given a well-formed request with a method that
// http::Method names and a body of at most a megabyte. Anything else the
// server answers itself: 400 for what is not HTTP, 405 for another method,
// 413 for a larger body, 501 for a body sent in chunks.
Task<absl::Status> Serve(const net::Listener& listener, Handler handler);

}  // namespace http

#endif  // HTTP_SERVER_H_
