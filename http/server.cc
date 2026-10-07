#include "http/server.h"

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"
#include "async/status_macros.h"
#include "async/task.h"
#include "http/client.h"
#include "http/head.h"
#include "net/event_loop.h"
#include "net/reader.h"
#include "net/stream.h"

namespace http {
namespace {

// The most a request's body may be. Requests to an API are small; this is
// far more than any of them, and little enough to hold in memory for as many
// connections as will ever be open.
constexpr size_t kMaxBodyBytes = size_t{1} << 20;

// How long a connection may sit between requests before it is closed, and
// how long a peer has to finish sending a request it has begun.
constexpr std::chrono::seconds kIdleTime{60};
constexpr std::chrono::seconds kRequestTime{30};

std::optional<Method> MethodNamed(std::string_view name) {
  if (name == "GET") return Method::kGet;
  if (name == "POST") return Method::kPost;
  if (name == "PUT") return Method::kPut;
  if (name == "PATCH") return Method::kPatch;
  if (name == "DELETE") return Method::kDelete;
  return std::nullopt;
}

// What a status is called on the status line. Nothing depends on the words,
// so only the statuses a small server is likely to send have any.
std::string_view ReasonFor(int status) {
  switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 302: return "Found";
    case 303: return "See Other";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 413: return "Content Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 503: return "Service Unavailable";
    default: return "";
  }
}

// An answer the server gives of its own accord, saying `why` in plain text.
Response Refusal(int status, std::string_view why) {
  return Response{
      .status = status,
      .headers =
          {
              Header{
                  .name = "Content-Type",
                  .value = "text/plain; charset=utf-8",
              },
          },
      .body = absl::StrCat(why, "\n"),
  };
}

// The bytes that put `response` on the wire. `last` is whether the
// connection closes after it, which the peer is told.
std::string Format(const Response& response, bool last) {
  Head head{
      .start_line = absl::StrCat("HTTP/1.1 ", response.status, " ",
                                 ReasonFor(response.status)),
      .headers = response.headers,
  };
  head.headers.push_back(Header{
      .name = "Content-Length",
      .value = absl::StrCat(response.body.size()),
  });
  if (last) {
    head.headers.push_back(Header{
        .name = "Connection",
        .value = "close",
    });
  }

  return absl::StrCat(FormatHead(head), response.body);
}

// What came of reading a request: the request, for the handler, or the
// answer to give in its place. After a refusal the connection is closed,
// since where the next request would begin is no longer known.
struct Arrival {
  std::optional<Request> request;
  Response refusal;
  // Whether the peer asked for the connection to be closed after this.
  bool last = false;
};

// Reads the next request from `reader`. Fails, as the reader does, if the
// peer goes away or sends nothing in time, in which case there is nobody to
// answer.
Task<absl::StatusOr<Arrival>> ReadRequest(net::Reader& reader) {
  absl::StatusOr<Head> head = co_await ReadHead(reader, net::After(kIdleTime));
  if (absl::IsInvalidArgument(head.status()) ||
      absl::IsResourceExhausted(head.status())) {
    co_return Arrival{
        .refusal = Refusal(400, "That is not an HTTP request."),
    };
  }
  CO_RETURN_IF_ERROR(head.status());

  // "GET /path HTTP/1.1".
  const std::vector<std::string_view> words =
      absl::StrSplit(head->start_line, ' ');
  if (words.size() != 3 || !words[1].starts_with('/') ||
      !words[2].starts_with("HTTP/1.")) {
    co_return Arrival{
        .refusal = Refusal(400, "That is not an HTTP request."),
    };
  }

  // The body has to be read, or refused, before anything else is decided:
  // until it is, the next request cannot be found.
  if (!FindHeader(head->headers, "Transfer-Encoding").empty()) {
    co_return Arrival{
        .refusal = Refusal(501, "Send the body with a Content-Length."),
    };
  }

  size_t length = 0;
  const std::string_view declared = FindHeader(head->headers, "Content-Length");
  if (!declared.empty() && !absl::SimpleAtoi(declared, &length)) {
    co_return Arrival{
        .refusal = Refusal(400, "That is not an HTTP request."),
    };
  }
  if (length > kMaxBodyBytes) {
    co_return Arrival{
        .refusal = Refusal(413, "That is too large."),
    };
  }

  CO_ASSIGN_OR_RETURN(const std::string_view body,
                      co_await reader.Read(length, net::After(kRequestTime)));

  // HTTP/1.0 closes after every request unless told otherwise, and 1.1
  // keeps the connection unless told otherwise.
  const std::string_view connection = FindHeader(head->headers, "Connection");
  const bool last = words[2] == "HTTP/1.0"
                        ? !absl::EqualsIgnoreCase(connection, "keep-alive")
                        : absl::EqualsIgnoreCase(connection, "close");

  const std::optional<Method> method = MethodNamed(words[0]);
  if (!method.has_value()) {
    Response refusal = Refusal(405, "That method is not supported.");
    refusal.headers.push_back(Header{
        .name = "Allow",
        .value = "GET, POST, PUT, PATCH, DELETE",
    });

    co_return Arrival{
        .refusal = std::move(refusal),
        .last = true,
    };
  }

  co_return Arrival{
      .request =
          Request{
              .method = *method,
              .url = std::string(words[1]),
              .headers = std::move(head->headers),
              .body = std::string(body),
          },
      .last = last,
  };
}

// Answers the requests that arrive on `stream`, one after another, until
// the peer is done with it or stops making sense.
Task<> ServeConnection(std::unique_ptr<net::Stream> stream,
                       std::shared_ptr<const Handler> handler) {
  net::Reader reader(stream.get());

  for (;;) {
    absl::StatusOr<Arrival> arrival = co_await ReadRequest(reader);
    if (!arrival.ok()) co_return;

    if (!arrival->request.has_value()) {
      (co_await stream->Write(Format(arrival->refusal, true))).IgnoreError();
      co_return;
    }

    const Response response = co_await (*handler)(*arrival->request);
    const absl::Status written =
        co_await stream->Write(Format(response, arrival->last));
    if (!written.ok() || arrival->last) co_return;
  }
}

}  // namespace

Task<absl::Status> Serve(const net::Listener& listener, Handler handler) {
  // Shared with every connection, each of which may outlast this task.
  const std::shared_ptr<const Handler> shared =
      std::make_shared<const Handler>(std::move(handler));

  for (;;) {
    CO_ASSIGN_OR_RETURN(std::unique_ptr<net::Stream> stream,
                        co_await listener.Accept());

    Spawn(ServeConnection(std::move(stream), shared));
  }
}

}  // namespace http
