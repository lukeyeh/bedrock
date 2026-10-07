#include "http/cookie.h"

#include <cstddef>
#include <string>
#include <string_view>

#include "absl/strings/ascii.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"

namespace http {

std::string_view FindCookie(std::string_view cookie_header,
                            std::string_view name) {
  for (const std::string_view pair : absl::StrSplit(cookie_header, ';')) {
    const size_t equals = pair.find('=');
    if (equals == std::string_view::npos) continue;

    if (absl::StripAsciiWhitespace(pair.substr(0, equals)) == name) {
      return absl::StripAsciiWhitespace(pair.substr(equals + 1));
    }
  }
  return {};
}

std::string FormatSetCookie(const Cookie& cookie) {
  return absl::StrCat(cookie.name, "=", cookie.value,
                      "; Path=/; Max-Age=", cookie.lifetime.count(),
                      "; HttpOnly; SameSite=Lax",
                      cookie.secure ? "; Secure" : "");
}

}  // namespace http
