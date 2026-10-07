// Random bytes by example: as many as asked for, and different every time.

#include "os/random.h"

#include <benchmark/benchmark.h>

#include <set>
#include <string>

#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace {

using absl_testing::IsOkAndHolds;
using testing::IsEmpty;
using testing::SizeIs;

// The count asked for is the count given, including none and more than the
// kernel hands over in one go.
TEST(RandomBytesTest, GivesAsManyBytesAsAskedFor) {
  EXPECT_THAT(os::RandomBytes(0), IsOkAndHolds(IsEmpty()));
  EXPECT_THAT(os::RandomBytes(32), IsOkAndHolds(SizeIs(32)));
  EXPECT_THAT(os::RandomBytes(100000), IsOkAndHolds(SizeIs(100000)));
}

// Two tokens are never the same, which is the whole point of them.
TEST(RandomBytesTest, NeverRepeats) {
  std::set<std::string> seen;
  for (int i = 0; i < 1000; ++i) {
    const absl::StatusOr<std::string> bytes = os::RandomBytes(16);
    ABSL_ASSERT_OK(bytes);
    EXPECT_TRUE(seen.insert(*bytes).second);
  }
}

// Benchmarks
// -----------------------------------------------------------------------------
//
//   bazel run -c opt //os:random_test -- --benchmark_filter=all

// A session token's worth.
void BM_RandomBytes(benchmark::State& state) {
  for (auto _ : state) benchmark::DoNotOptimize(os::RandomBytes(32));
}
BENCHMARK(BM_RandomBytes);

}  // namespace
