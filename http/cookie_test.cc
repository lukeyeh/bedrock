// Cookies by example: finding one a browser sent, and writing one for it to
// keep.

#include "http/cookie.h"

#include <benchmark/benchmark.h>

#include <chrono>

#include "gtest/gtest.h"

namespace {

// A request's cookies come in one header, separated by semicolons.
TEST(FindCookieTest, FindsACookieByName) {
  EXPECT_EQ(http::FindCookie("session=abc123; theme=dark", "session"),
            "abc123");
  EXPECT_EQ(http::FindCookie("session=abc123; theme=dark", "theme"), "dark");
  EXPECT_EQ(http::FindCookie("theme=dark;session=abc123", "session"), "abc123");
}

// One that was not sent reads as empty, as does one whose name is only the
// end of another's.
TEST(FindCookieTest, FindsNothingForANameThatIsNotThere) {
  EXPECT_EQ(http::FindCookie("", "session"), "");
  EXPECT_EQ(http::FindCookie("theme=dark", "session"), "");
  EXPECT_EQ(http::FindCookie("oldsession=abc", "session"), "");
  EXPECT_EQ(http::FindCookie("nonsense; session=abc", "session"), "abc");
}

// A value may itself contain "=", as base64 does.
TEST(FindCookieTest, KeepsEqualsSignsInAValue) {
  EXPECT_EQ(http::FindCookie("session=YWJj==", "session"), "YWJj==");
}

// A cookie is kept for as long as it says, out of reach of scripts, and
// sent over HTTPS alone.
TEST(FormatSetCookieTest, WritesACookieToKeep) {
  EXPECT_EQ(http::FormatSetCookie(http::Cookie{
                .name = "session",
                .value = "abc123",
                .lifetime = std::chrono::hours(24),
            }),
            "session=abc123; Path=/; Max-Age=86400; HttpOnly; SameSite=Lax; "
            "Secure");
}

// A lifetime of nothing has the browser forget it; a server on localhost,
// which has no HTTPS, leaves Secure off.
TEST(FormatSetCookieTest, WritesACookieToForget) {
  EXPECT_EQ(http::FormatSetCookie(http::Cookie{
                .name = "session",
                .secure = false,
            }),
            "session=; Path=/; Max-Age=0; HttpOnly; SameSite=Lax");
}

// Benchmarks
// -----------------------------------------------------------------------------
//
//   bazel run -c opt //http:cookie_test -- --benchmark_filter=all

// Done for every request from someone signed in.
void BM_FindCookie(benchmark::State& state) {
  for (auto _ : state) {
    benchmark::DoNotOptimize(http::FindCookie(
        "theme=dark; session=4f7c2a9e1b3d5f60718293a4b5c6d7e8", "session"));
  }
}
BENCHMARK(BM_FindCookie);

}  // namespace
