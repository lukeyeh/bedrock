// http::Serve by example, over a loopback port: what a handler is given,
// what goes back, and what the server answers without asking it.

#include "http/server.h"

#include <benchmark/benchmark.h>

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "async/task.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "http/client.h"
#include "http/head.h"
#include "net/event_loop.h"
#include "net/reader.h"
#include "net/stream.h"

namespace {

using testing::HasSubstr;
using testing::StartsWith;

void RunOnEventLoop(Task<> test) {
  absl::StatusOr<EventLoop> loop = EventLoop::Create();
  ABSL_ASSERT_OK(loop);
  loop->Run(std::move(test));
}

net::Deadline Soon() { return net::After(std::chrono::seconds(5)); }

std::string_view NameOf(http::Method method) {
  switch (method) {
    case http::Method::kGet: return "GET";
    case http::Method::kPost: return "POST";
    case http::Method::kPut: return "PUT";
    case http::Method::kPatch: return "PATCH";
    case http::Method::kDelete: return "DELETE";
  }
  return "";
}

// Says back what it was asked: the method, the target and the body.
Task<http::Response> Echo(const http::Request& request) {
  co_return http::Response{
      .status = 200,
      .headers =
          {
              http::Header{
                  .name = "Content-Type",
                  .value = "text/plain",
              },
          },
      .body = absl::StrCat(NameOf(request.method), " ", request.url, " [",
                           request.body, "]"),
  };
}

// A server answering with `handler` on a loopback port, for as long as the
// test runs.
class Served {
 public:
  explicit Served(http::Handler handler) {
    absl::StatusOr<net::Listener> listener = net::Listener::OnLoopback();
    ABSL_EXPECT_OK(listener);
    listener_ = std::make_unique<net::Listener>(std::move(*listener));
    Spawn(Run(*listener_, std::move(handler)));
  }

  std::string Url(std::string_view target) const {
    return absl::StrCat("http://127.0.0.1:", listener_->address().port, target);
  }

  const net::Listener& listener() const { return *listener_; }

 private:
  static Task<> Run(const net::Listener& listener, http::Handler handler) {
    (co_await http::Serve(listener, std::move(handler))).IgnoreError();
  }

  std::unique_ptr<net::Listener> listener_;
};

// Sends `bytes` to the server as they are and returns all it sends back
// before closing the connection.
Task<std::string> Exchange(const Served& server, std::string_view bytes) {
  absl::StatusOr<std::unique_ptr<net::Stream>> stream =
      co_await net::Dial(server.listener().address(), Soon());
  ABSL_EXPECT_OK(stream);
  if (!stream.ok()) co_return "";

  ABSL_EXPECT_OK(co_await (*stream)->Write(bytes));

  net::Reader reader(stream->get());
  const absl::StatusOr<std::string_view> reply =
      co_await reader.ReadToEnd(Soon());
  ABSL_EXPECT_OK(reply);
  co_return reply.ok() ? std::string(*reply) : "";
}

// The handler is given the method, the target and the body, and what it
// returns is what the client receives.
TEST(ServeTest, HasTheHandlerAnswerARequest) {
  RunOnEventLoop([]() -> Task<> {
    const Served server(&Echo);
    const std::unique_ptr<http::Client> client = http::NewClient();

    const absl::StatusOr<http::Response> response =
        co_await client->Send(http::Request{
            .method = http::Method::kPost,
            .url = server.Url("/bets?fresh=1"),
            .body = R"({"gmbux":50})",
        });

    ABSL_EXPECT_OK(response);
    if (!response.ok()) co_return;
    EXPECT_EQ(response->status, 200);
    EXPECT_EQ(response->Get("Content-Type"), "text/plain");
    EXPECT_EQ(response->body, R"(POST /bets?fresh=1 [{"gmbux":50}])");
  }());
}

// The request's headers reach the handler as sent, and any status and
// headers it likes go back: here, a redirect that sets a cookie.
TEST(ServeTest, PassesHeadersBothWays) {
  RunOnEventLoop([]() -> Task<> {
    const Served server([](const http::Request& request)
                            -> Task<http::Response> {
      co_return http::Response{
          .status = 302,
          .headers =
              {
                  http::Header{
                      .name = "Location",
                      .value = "/",
                  },
                  http::Header{
                      .name = "Set-Cookie",
                      .value = absl::StrCat(
                          "seen=", http::FindHeader(request.headers, "X-Name")),
                  },
              },
      };
    });
    const std::unique_ptr<http::Client> client = http::NewClient();

    const absl::StatusOr<http::Response> response =
        co_await client->Send(http::Request{
            .url = server.Url("/auth/callback"),
            .headers =
                {
                    http::Header{
                        .name = "X-Name",
                        .value = "luke",
                    },
                },
        });

    ABSL_EXPECT_OK(response);
    if (!response.ok()) co_return;
    EXPECT_EQ(response->status, 302);
    EXPECT_EQ(response->Get("Location"), "/");
    EXPECT_EQ(response->Get("Set-Cookie"), "seen=luke");
    EXPECT_EQ(response->body, "");
  }());
}

// A connection is kept for the requests that follow, which is what makes a
// page that asks for a dozen things quick.
TEST(ServeTest, AnswersManyRequestsOnOneConnection) {
  RunOnEventLoop([]() -> Task<> {
    const Served server(&Echo);

    // Two requests sent at once, the second asking to be the last.
    const std::string replies = co_await Exchange(
        server,
        "GET /one HTTP/1.1\r\nHost: x\r\n\r\n"
        "POST /two HTTP/1.1\r\nHost: x\r\nContent-Length: 2\r\n"
        "Connection: close\r\n\r\nhi");

    EXPECT_THAT(replies, StartsWith("HTTP/1.1 200 OK\r\n"));
    EXPECT_THAT(replies, HasSubstr("GET /one []"));
    EXPECT_THAT(replies, HasSubstr("Connection: close\r\n"));
    EXPECT_THAT(replies, HasSubstr("POST /two [hi]"));
  }());
}

// A handler that waits holds up nobody else: the request that arrives
// second, on another connection, is answered first.
TEST(ServeTest, AnswersOtherConnectionsWhileAHandlerWaits) {
  RunOnEventLoop([]() -> Task<> {
    const Served server(
        [](const http::Request& request) -> Task<http::Response> {
          if (request.url == "/slow")
            co_await Sleep(std::chrono::milliseconds(50));

          co_return http::Response{
              .status = 200,
              .body = request.url,
          };
        });

    std::string order;
    const auto ask = [](const Served& served, std::string target,
                        std::string& finished) -> Task<> {
      const std::unique_ptr<http::Client> client = http::NewClient();
      const absl::StatusOr<http::Response> response =
          co_await client->Send(http::Request{
              .url = served.Url(target),
          });
      ABSL_EXPECT_OK(response);
      if (response.ok()) finished += response->body;
    };

    // The slow one is asked first and left to it; the quick one is asked
    // while it waits.
    Spawn(ask(server, "/slow", order));
    co_await ask(server, "/quick", order);
    EXPECT_EQ(order, "/quick");

    co_await Sleep(std::chrono::milliseconds(200));
    EXPECT_EQ(order, "/quick/slow");
  }());
}

// What is not HTTP is told so and hung up on, without troubling the
// handler.
TEST(ServeTest, RefusesWhatIsNotHttp) {
  RunOnEventLoop([]() -> Task<> {
    const Served server(&Echo);

    EXPECT_THAT(co_await Exchange(server, "hello there\r\n\r\n"),
                StartsWith("HTTP/1.1 400 Bad Request\r\n"));
    EXPECT_THAT(co_await Exchange(server, "GET nowhere HTTP/1.1\r\n\r\n"),
                StartsWith("HTTP/1.1 400 Bad Request\r\n"));
    EXPECT_THAT(co_await Exchange(
                    server, "POST / HTTP/1.1\r\nContent-Length: lots\r\n\r\n"),
                StartsWith("HTTP/1.1 400 Bad Request\r\n"));
  }());
}

// So is a method the handler has no name for, with the ones it does.
TEST(ServeTest, RefusesAMethodItDoesNotKnow) {
  RunOnEventLoop([]() -> Task<> {
    const Served server(&Echo);

    const std::string reply =
        co_await Exchange(server, "OPTIONS / HTTP/1.1\r\nHost: x\r\n\r\n");

    EXPECT_THAT(reply, StartsWith("HTTP/1.1 405 Method Not Allowed\r\n"));
    EXPECT_THAT(reply, HasSubstr("Allow: GET, POST, PUT, PATCH, DELETE\r\n"));
  }());
}

// A body is bounded, so that nobody can have the server hold more than it
// should; and has to say how long it is.
TEST(ServeTest, RefusesABodyThatIsTooLargeOrOfUnknownLength) {
  RunOnEventLoop([]() -> Task<> {
    const Served server(&Echo);

    EXPECT_THAT(
        co_await Exchange(
            server, "POST / HTTP/1.1\r\nContent-Length: 99999999\r\n\r\n"),
        StartsWith("HTTP/1.1 413 Content Too Large\r\n"));
    EXPECT_THAT(
        co_await Exchange(
            server,
            "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n"),
        StartsWith("HTTP/1.1 501 Not Implemented\r\n"));
  }());
}

// A body of exactly the most allowed is still a request.
TEST(ServeTest, TakesABodyOfAMegabyte) {
  RunOnEventLoop([]() -> Task<> {
    const Served server(
        [](const http::Request& request) -> Task<http::Response> {
          co_return http::Response{
              .status = 200,
              .body = absl::StrCat(request.body.size()),
          };
        });
    const std::unique_ptr<http::Client> client = http::NewClient();

    const absl::StatusOr<http::Response> response =
        co_await client->Send(http::Request{
            .method = http::Method::kPut,
            .url = server.Url("/"),
            .body = std::string(size_t{1} << 20, 'x'),
        });

    ABSL_EXPECT_OK(response);
    if (!response.ok()) co_return;
    EXPECT_EQ(response->body, "1048576");
  }());
}

// Benchmarks
// -----------------------------------------------------------------------------
//
//   bazel run -c opt //http:server_test -- --benchmark_filter=all

Task<> AskRepeatedly(benchmark::State& state) {
  const Served server(&Echo);
  const std::unique_ptr<http::Client> client = http::NewClient();
  const http::Request request{
      .url = server.Url("/api/markets"),
  };

  for (auto _ : state) {
    benchmark::DoNotOptimize(co_await client->Send(request));
  }
}

// A request and its answer over loopback, on a connection already open:
// what the server adds to whatever its handler costs.
void BM_ServeARequest(benchmark::State& state) {
  EventLoop::Create()->Run(AskRepeatedly(state));
}
BENCHMARK(BM_ServeARequest);

}  // namespace
