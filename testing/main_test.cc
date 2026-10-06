// A test file as this main expects it: tests and benchmarks side by side.
//
//   bazel test //testing:main_test
//       Runs the test.
//
//   bazel run -c opt //testing:main_test -- --benchmark_filter=all
//       Runs the benchmark instead.

#include <benchmark/benchmark.h>

#include <numeric>
#include <vector>

#include "gtest/gtest.h"

namespace {

int Sum(const std::vector<int>& numbers) {
  return std::accumulate(numbers.begin(), numbers.end(), 0);
}

// What the code does: run by default.
TEST(MainTest, RunsTestsByDefault) {
  const std::vector<int> numbers = {
      1,
      2,
      3,
  };

  EXPECT_EQ(Sum(numbers), 6);
}

// -----------------------------------------------------------------------------
// Benchmarks
// -----------------------------------------------------------------------------
//
//   bazel run -c opt //testing:main_test -- --benchmark_filter=all

// What the code costs: run only when a --benchmark flag is given.
void BM_Sum(benchmark::State& state) {
  const std::vector<int> numbers(1000, 1);
  for (auto _ : state) benchmark::DoNotOptimize(Sum(numbers));
}
BENCHMARK(BM_Sum);

}  // namespace
