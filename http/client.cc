#include "http/client.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
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
#include "http/head.h"
#include "net/reader.h"
#include "net/stream.h"
#include "net/url.h"

namespace http {
namespace {

// How long one request may take from start to finish.
constexpr std::chrono::seconds kTimeout(30);

// More than any API response should be; a bound on what a server can make
// the client hold in memory.
constexpr uint64_t kMaxBodyBytes = uint64_t{64} * 1024 * 1024;

// Longer than any line of a well-behaved chunked body.
constexpr size_t kMaxLineBytes = size_t{16} * 1024;

std::string_view Name(Method method) {
  switch (method) {
    case Method::kGet: return "GET";
    case Method::kPost: return "POST";
    case Method::kPut: return "PUT";
    case Method::kPatch: return "PATCH";
    case Method::kDelete: return "DELETE";
  }
  return "GET";
}

absl::Status TooLarge() {
  return absl::ResourceExhaustedError("HTTP response body too large");
}

std::string Format(const Request& request, const net::Url& url) {
  Head head{
      .start_line =
          absl::StrCat(Name(request.method), " ", url.target, " HTTP/1.1"),
      .headers = request.headers,
  };
  head.headers.push_back(Header{
      .name = "Host",
      .value = url.authority,
  });

  // Stated even when zero for methods that could have had a body, because
  // some servers insist.
  if (request.method != Method::kGet || !request.body.empty()) {
    head.headers.push_back(Header{
        .name = "Content-Length",
        .value = absl::StrCat(request.body.size()),
    });
  }

  return absl::StrCat(FormatHead(head), request.body);
}

// How a response says where its body ends.
enum class Framing : uint8_t {
  // There is no body.
  kNone,
  // Content-Length said how many bytes.
  kCounted,
  // A series of "<size in hex>CRLF<bytes>CRLF", ending with a chunk of size
  // zero and optional trailing headers.
  kChunked,
  // The body is everything until the server closes the connection.
  kUntilClose,
};

// The head of a response, and what it says about the body that follows.
struct Opening {
  int status = 0;
  std::vector<Header> headers;
  Framing framing = Framing::kNone;
  // For kCounted, the length of the body.
  uint64_t bytes = 0;
  // Whether another request may follow this response on the connection.
  bool reusable = false;
};

// A connection to one server, kept open between requests.
class Connection {
 public:
  Connection(std::unique_ptr<net::Stream> stream, net::Address address)
      : stream_(std::move(stream)),
        reader_(stream_.get()),
        address_(std::move(address)) {}

  bool IsTo(const net::Address& address) const {
    return address_.host == address.host && address_.port == address.port &&
           address_.security == address.security;
  }

  // True once any part of a response has arrived. Until then a failure may
  // just mean the server had closed an idle connection, and it is safe to try
  // again on a new one.
  bool response_started() const { return response_started_; }

  // Where the body of the response is read from.
  net::Reader& reader() { return reader_; }

  // Sends `request` and reads the head of the response.
  Task<absl::StatusOr<Opening>> Begin(const Request& request,
                                      const net::Url& url,
                                      net::Deadline deadline) {
    response_started_ = false;

    const std::string wire = Format(request, url);
    CO_RETURN_IF_ERROR(co_await stream_->Write(wire));
    CO_RETURN_IF_ERROR(co_await reader_.Fill(1, deadline));
    response_started_ = true;

    Opening opening;
    // Responses numbered 1xx are progress reports ahead of the real one.
    while (opening.status < 200) {
      CO_ASSIGN_OR_RETURN(Head head, co_await ReadHead(reader_, deadline));

      // "HTTP/1.1 200 OK"
      const std::vector<std::string_view> parts =
          absl::StrSplit(head.start_line, absl::MaxSplits(' ', 2));
      if (parts.size() < 2 || !parts[0].starts_with("HTTP/1.") ||
          !absl::SimpleAtoi(parts[1], &opening.status)) {
        co_return absl::InvalidArgumentError(
            absl::StrCat("malformed HTTP status line: ", head.start_line));
      }

      opening.headers = std::move(head.headers);
    }

    CO_RETURN_IF_ERROR(FindFraming(opening));
    opening.reusable = opening.framing != Framing::kUntilClose &&
                       !absl::EqualsIgnoreCase(
                           FindHeader(opening.headers, "Connection"), "close");
    co_return opening;
  }

 private:
  // Works out which of the three ways the response frames its body.
  static absl::Status FindFraming(Opening& opening) {
    if (opening.status == 204 || opening.status == 304) {
      return absl::OkStatus();
    }

    if (absl::StrContainsIgnoreCase(
            FindHeader(opening.headers, "Transfer-Encoding"), "chunked")) {
      opening.framing = Framing::kChunked;
      return absl::OkStatus();
    }

    const std::string_view length =
        FindHeader(opening.headers, "Content-Length");
    if (length.empty()) {
      opening.framing = Framing::kUntilClose;
      return absl::OkStatus();
    }

    if (!absl::SimpleAtoi(length, &opening.bytes)) {
      return absl::InvalidArgumentError(
          absl::StrCat("malformed Content-Length: ", length));
    }
    if (opening.bytes > 0) opening.framing = Framing::kCounted;
    return absl::OkStatus();
  }

  std::unique_ptr<net::Stream> stream_;
  net::Reader reader_;
  net::Address address_;
  bool response_started_ = false;
};

// Where a client keeps the connection it is not using, for its next request.
// Shared with the bodies the client hands out, which return their connection
// here when they have read it to a point where another request may follow.
using IdleConnection = std::shared_ptr<std::unique_ptr<Connection>>;

// The body of a response, read off its connection. The only code that knows
// how bodies are framed.
//
// A deadline may pass in the middle of any step here. The reader consumes
// nothing when it does, so every step leaves this object describing exactly
// where in the body the reader is, and Next can be called again.
class NetBody final : public Body {
 public:
  NetBody(std::unique_ptr<Connection> connection, IdleConnection idle,
          const Opening& opening)
      : connection_(std::move(connection)),
        idle_(std::move(idle)),
        framing_(opening.framing),
        remaining_(opening.bytes),
        reusable_(opening.reusable) {
    if (framing_ == Framing::kNone) End();
  }

  Task<absl::StatusOr<std::string_view>> Next(net::Deadline deadline) override {
    if (connection_ == nullptr) co_return std::string_view();

    if (framing_ == Framing::kUntilClose) {
      // The reader reports the end of the stream as a failure to fill.
      const absl::Status filled =
          co_await connection_->reader().Fill(1, deadline);
      if (absl::IsUnavailable(filled)) {
        End();
        co_return std::string_view();
      }
      CO_RETURN_IF_ERROR(filled);

      co_return co_await connection_->reader().Read(
          connection_->reader().Peek().size(), deadline);
    }

    if (remaining_ == 0) {
      bool more = false;
      if (framing_ == Framing::kChunked) {
        CO_ASSIGN_OR_RETURN(more, co_await NextChunk(deadline));
      }
      if (!more) {
        End();
        co_return std::string_view();
      }
    }

    CO_RETURN_IF_ERROR(co_await connection_->reader().Fill(1, deadline));
    const size_t bytes = static_cast<size_t>(
        std::min<uint64_t>(connection_->reader().Peek().size(), remaining_));
    CO_ASSIGN_OR_RETURN(const std::string_view piece,
                        co_await connection_->reader().Read(bytes, deadline));

    // The end is noticed on the next call rather than here, because ending
    // may close the connection, and `piece` refers to its memory.
    remaining_ -= bytes;
    if (remaining_ == 0) after_chunk_ = framing_ == Framing::kChunked;
    co_return piece;
  }

 private:
  // Reads up to the bytes of the next chunk and sets remaining_ to how many
  // there are. Evaluates to false if there is no next chunk: the body, and
  // the trailing headers after it, have been read.
  Task<absl::StatusOr<bool>> NextChunk(net::Deadline deadline) {
    net::Reader& reader = connection_->reader();

    // Each chunk is followed by a CRLF that is not part of it.
    if (after_chunk_) {
      CO_ASSIGN_OR_RETURN(const std::string_view end,
                          co_await reader.Read(2, deadline));
      if (end != "\r\n") {
        co_return absl::InvalidArgumentError("malformed HTTP chunk ending");
      }
      after_chunk_ = false;
    }

    if (!in_trailers_) {
      CO_ASSIGN_OR_RETURN(
          const std::string_view line,
          co_await reader.ReadUntil("\r\n", kMaxLineBytes, deadline));

      uint64_t bytes = 0;
      // Anything after a semicolon is an extension, to be ignored.
      if (!absl::SimpleHexAtoi(line.substr(0, line.find(';')), &bytes)) {
        co_return absl::InvalidArgumentError(
            absl::StrCat("malformed HTTP chunk size: ", line));
      }
      if (bytes > 0) {
        remaining_ = bytes;
        co_return true;
      }
      in_trailers_ = true;
    }

    for (;;) {
      CO_ASSIGN_OR_RETURN(
          const std::string_view trailer,
          co_await reader.ReadUntil("\r\n", kMaxLineBytes, deadline));
      if (trailer.empty()) co_return false;
    }
  }

  // The body has been read to its end: gives the connection back to the
  // client if it can carry another request.
  void End() {
    if (reusable_ && *idle_ == nullptr) {
      *idle_ = std::move(connection_);
    }
    connection_.reset();
  }

  // Null once the body has ended.
  std::unique_ptr<Connection> connection_;
  IdleConnection idle_;
  Framing framing_;

  // How many bytes are left of the body (kCounted) or of the chunk being
  // read (kChunked).
  uint64_t remaining_;
  // kChunked: the CRLF after the last chunk's bytes has yet to be read.
  bool after_chunk_ = false;
  // kChunked: the last chunk has been seen; trailing headers follow.
  bool in_trailers_ = false;
  bool reusable_;
};

class NetClient final : public Client {
 public:
  Task<absl::StatusOr<Response>> Send(const Request& request) override {
    const net::Deadline deadline = net::After(kTimeout);
    CO_ASSIGN_OR_RETURN(OpenResponse open, co_await Open(request, deadline));

    Response response{
        .status = open.status,
        .headers = std::move(open.headers),
    };
    for (;;) {
      CO_ASSIGN_OR_RETURN(const std::string_view piece,
                          co_await open.body->Next(deadline));
      if (piece.empty()) co_return response;

      if (response.body.size() + piece.size() > kMaxBodyBytes) {
        co_return TooLarge();
      }
      response.body.append(piece);
    }
  }

  Task<absl::StatusOr<OpenResponse>> Open(const Request& request,
                                          net::Deadline deadline) override {
    CO_ASSIGN_OR_RETURN(const net::Url url, net::ParseUrl(request.url));

    std::unique_ptr<Connection> connection = std::move(*idle_);
    if (connection != nullptr && connection->IsTo(url.address)) {
      absl::StatusOr<Opening> opening =
          co_await connection->Begin(request, url, deadline);
      if (opening.ok() || connection->response_started()) {
        co_return Finish(std::move(connection), std::move(opening));
      }
      // The server had closed the idle connection; start again on a new one.
    }

    CO_ASSIGN_OR_RETURN(std::unique_ptr<net::Stream> stream,
                        co_await net::Dial(url.address, deadline));
    connection = std::make_unique<Connection>(std::move(stream), url.address);

    absl::StatusOr<Opening> opening =
        co_await connection->Begin(request, url, deadline);
    co_return Finish(std::move(connection), std::move(opening));
  }

 private:
  // Hands the connection on to the body that will read the rest of the
  // response from it.
  absl::StatusOr<OpenResponse> Finish(std::unique_ptr<Connection> connection,
                                      absl::StatusOr<Opening> opening) {
    ABSL_RETURN_IF_ERROR(opening.status());

    return OpenResponse{
        .status = opening->status,
        .headers = std::move(opening->headers),
        .body =
            std::make_unique<NetBody>(std::move(connection), idle_, *opening),
    };
  }

  // The connection left open by the last request, if any.
  IdleConnection idle_ = std::make_shared<std::unique_ptr<Connection>>();
};

// A body that was received whole.
class WholeBody final : public Body {
 public:
  explicit WholeBody(std::string body) : body_(std::move(body)) {}

  Task<absl::StatusOr<std::string_view>> Next(net::Deadline) override {
    co_return std::exchange(unread_, false) ? std::string_view(body_)
                                            : std::string_view();
  }

 private:
  std::string body_;
  bool unread_ = true;
};

}  // namespace

Task<absl::StatusOr<OpenResponse>> Client::Open(const Request& request,
                                                net::Deadline) {
  CO_ASSIGN_OR_RETURN(Response response, co_await Send(request));

  co_return OpenResponse{
      .status = response.status,
      .headers = std::move(response.headers),
      .body = std::make_unique<WholeBody>(std::move(response.body)),
  };
}

std::unique_ptr<Client> NewClient() { return std::make_unique<NetClient>(); }

}  // namespace http
