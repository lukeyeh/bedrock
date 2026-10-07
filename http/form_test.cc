// Form encoding by example: reading a query or a posted form, writing one,
// and taking a request's target apart.

#include "http/form.h"

#include <benchmark/benchmark.h>

#include <string>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace {

using http::Field;
using testing::ElementsAre;
using testing::IsEmpty;
using testing::Pair;

// Fields are separated by "&", and each is a name, "=", and a value.
TEST(ParseFormTest, ReadsNamesAndValues) {
  EXPECT_THAT(http::ParseForm("code=abc123&state=xyz"),
              ElementsAre(Pair("code", "abc123"), Pair("state", "xyz")));
  EXPECT_THAT(http::ParseForm(""), IsEmpty());
}

// "+" is a space and "%XX" is the byte with that code, in names as in
// values.
TEST(ParseFormTest, UndoesEscapes) {
  EXPECT_THAT(http::ParseForm("q=good+morning%21&a%20b=%E2%98%80"),
              ElementsAre(Pair("q", "good morning!"), Pair("a b", "☀")));
}

// What arrives is whatever someone sent, so anything is read as something:
// a field without "=" has no value, and a stray "%" is itself.
TEST(ParseFormTest, IsLenientWithWhatItIsGiven) {
  EXPECT_THAT(http::ParseForm("flag&x=1&&y="),
              ElementsAre(Pair("flag", ""), Pair("x", "1"), Pair("y", "")));
  EXPECT_THAT(
      http::ParseForm("p=100%&q=%zz&r=%4"),
      ElementsAre(Pair("p", "100%"), Pair("q", "%zz"), Pair("r", "%4")));
  EXPECT_THAT(http::ParseForm("a=b=c"), ElementsAre(Pair("a", "b=c")));
}

// Writing escapes everything that could be taken for punctuation, so that
// any value reads back as it was.
TEST(FormatFormTest, WritesWhatParseFormReads) {
  const std::vector<Field> fields = {
      {"redirect_uri", "https://example.com/auth/callback?x=1&y=2"},
      {"scope", "identify guilds"},
      {"sun", "☀"},
      {"empty", ""},
  };

  const std::string encoded = http::FormatForm(fields);

  EXPECT_EQ(encoded,
            "redirect_uri=https%3A%2F%2Fexample.com%2Fauth%2Fcallback%3Fx%3D1"
            "%26y%3D2&scope=identify%20guilds&sun=%E2%98%80&empty=");
  EXPECT_EQ(http::ParseForm(encoded), fields);
  EXPECT_EQ(http::FormatForm({}), "");
}

// The first field of a name is the one found; a name that is not there
// reads as empty.
TEST(FindFieldTest, FindsTheFirstFieldOfAName) {
  const std::vector<Field> fields = http::ParseForm("a=1&b=2&a=3");

  EXPECT_EQ(http::FindField(fields, "a"), "1");
  EXPECT_EQ(http::FindField(fields, "b"), "2");
  EXPECT_EQ(http::FindField(fields, "c"), "");
}

// A target is a path, and after a "?" the fields of a query.
TEST(ParseTargetTest, SplitsThePathFromTheQuery) {
  const http::Target target =
      http::ParseTarget("/auth/callback?code=abc&state=x+y");

  EXPECT_EQ(target.path, "/auth/callback");
  EXPECT_THAT(target.query,
              ElementsAre(Pair("code", "abc"), Pair("state", "x y")));

  EXPECT_EQ(http::ParseTarget("/").path, "/");
  EXPECT_THAT(http::ParseTarget("/").query, IsEmpty());
}

// A path's escapes are undone, but its "+" is a plus: only forms write a
// space that way.
TEST(ParseTargetTest, DecodesThePath) {
  EXPECT_EQ(http::ParseTarget("/a%20b/c+d?x=1").path, "/a b/c+d");
}

// Benchmarks
// -----------------------------------------------------------------------------
//
//   bazel run -c opt //http:form_test -- --benchmark_filter=all

// The target of a request to a small API.
void BM_ParseTarget(benchmark::State& state) {
  for (auto _ : state) {
    benchmark::DoNotOptimize(
        http::ParseTarget("/api/markets/204455667788990011?fresh=1"));
  }
}
BENCHMARK(BM_ParseTarget);

void BM_FormatForm(benchmark::State& state) {
  const std::vector<Field> fields = {
      {"grant_type", "authorization_code"},
      {"code", "NhhvTDYsFcdgNLnnLijcl7Ku7bEEeee"},
      {"redirect_uri", "https://example.com/auth/callback"},
  };
  for (auto _ : state) benchmark::DoNotOptimize(http::FormatForm(fields));
}
BENCHMARK(BM_FormatForm);

}  // namespace
