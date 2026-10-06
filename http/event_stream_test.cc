// http::EventReader by example: turning a response body into the events a
// server sent, wherever the network happened to split it.

#include "http/event_stream.h"

#include <benchmark/benchmark.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "async/task.h"
#include "async/task_scope.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "http/client.h"
#include "http/fake_client.h"
#include "net/stream.h"

namespace {

using absl_testing::StatusIs;
using testing::ElementsAre;
using testing::FieldsAre;

// Nothing here waits on I/O, so a scope is enough to run a task to its end.
void RunToCompletion(Task<> test) {
  TaskScope scope;
  scope.Spawn(std::move(test));
  EXPECT_EQ(scope.unfinished(), 0);
}

// Every event in a body that arrives as `pieces`.
Task<std::vector<http::ServerEvent>> ReadAll(std::vector<std::string> pieces) {
  http::FakeBody body(std::move(pieces));
  http::EventReader reader(&body);

  std::vector<http::ServerEvent> events;
  for (;;) {
    absl::StatusOr<std::optional<http::ServerEvent>> event =
        co_await reader.Next(net::Deadline::max());
    ABSL_EXPECT_OK(event);
    if (!event.ok() || !event->has_value()) co_return events;
    events.push_back(std::move(**event));
  }
}

// An event is a few "name: value" lines ended by a blank one. The type is
// "message" unless the server names another.
TEST(EventReaderTest, ReadsEventsWithTheirTypeAndData) {
  RunToCompletion([]() -> Task<> {
    const std::vector<http::ServerEvent> events = co_await ReadAll({
        "event: greeting\n"
        "data: hello\n"
        "\n"
        "data: untyped\n"
        "\n",
    });

    EXPECT_THAT(events, ElementsAre(FieldsAre("greeting", "hello"),
                                    FieldsAre("message", "untyped")));
  }());
}

// The network splits a body wherever it likes, including in the middle of a
// line ending. The events are the same.
TEST(EventReaderTest, ReassemblesEventsSplitAcrossPieces) {
  RunToCompletion([]() -> Task<> {
    const std::vector<http::ServerEvent> events = co_await ReadAll({
        "event: gree",
        "ting\r",
        "\ndata: hel",
        "lo\r\n\r",
        "\ndata: next\r\n\r\n",
    });

    EXPECT_THAT(events, ElementsAre(FieldsAre("greeting", "hello"),
                                    FieldsAre("message", "next")));
  }());
}

// Several data lines make one payload of several lines, and the space after
// the colon is optional.
TEST(EventReaderTest, JoinsDataLines) {
  RunToCompletion([]() -> Task<> {
    const std::vector<http::ServerEvent> events = co_await ReadAll({
        "data:first\n"
        "data: second\n"
        "\n",
    });

    EXPECT_THAT(events, ElementsAre(FieldsAre("message", "first\nsecond")));
  }());
}

// Servers send comments, and events with no data, to keep an idle connection
// open. Neither is an event, and nor is one the body ends in the middle of.
TEST(EventReaderTest, SkipsWhatIsNotAnEvent) {
  RunToCompletion([]() -> Task<> {
    const std::vector<http::ServerEvent> events = co_await ReadAll({
        ": keep-alive\n"
        "\n"
        "event: ping\n"
        "\n"
        "id: 7\n"
        "data: real\n"
        "\n"
        "data: cut off",
    });

    EXPECT_THAT(events, ElementsAre(FieldsAre("message", "real")));
  }());
}

// A body that fails to arrive is the reader's failure too.
class BrokenBody final : public http::Body {
 public:
  Task<absl::StatusOr<std::string_view>> Next(net::Deadline) override {
    co_return absl::UnavailableError("connection lost");
  }
};

TEST(EventReaderTest, FailsAsTheBodyDoes) {
  RunToCompletion([]() -> Task<> {
    BrokenBody body;
    http::EventReader reader(&body);

    EXPECT_THAT(co_await reader.Next(net::Deadline::max()),
                StatusIs(absl::StatusCode::kUnavailable));
  }());
}

// -----------------------------------------------------------------------------
// Benchmarks
// -----------------------------------------------------------------------------
//
//   bazel run -c opt //http:event_stream_test -- --benchmark_filter=all

Task<> CountEvents(std::vector<std::string> pieces, size_t& count) {
  count = (co_await ReadAll(std::move(pieces))).size();
}

// Reading a body of small events, the shape of a language model's reply as
// it is written: this is paid once per word or so of every reply.
void BM_ReadEvents(benchmark::State& state) {
  std::string body;
  for (int i = 0; i < state.range(0); ++i) {
    body += "event: content_block_delta\ndata: {\"text\":\"word\"}\n\n";
  }

  for (auto _ : state) {
    size_t count = 0;
    TaskScope().Spawn(CountEvents(
        {
            body,
        },
        count));
    benchmark::DoNotOptimize(count);
  }
  state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_ReadEvents)->Arg(1)->Arg(1024);

}  // namespace
