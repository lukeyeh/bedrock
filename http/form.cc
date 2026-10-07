#include "http/form.h"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "absl/strings/ascii.h"
#include "absl/strings/escaping.h"
#include "absl/strings/str_split.h"

namespace http {
namespace {

// Bytes that mean only themselves wherever they appear in a URL.
bool IsUnreserved(char c) {
  return absl::ascii_isalnum(static_cast<unsigned char>(c)) || c == '-' ||
         c == '.' || c == '_' || c == '~';
}

// `text` with its escapes undone: "%41" is "A". In a form, though not in a
// path, "+" is a space.
std::string Decode(std::string_view text, bool plus_is_space) {
  std::string decoded;
  decoded.reserve(text.size());

  for (size_t i = 0; i < text.size(); ++i) {
    std::string byte;
    if (text[i] == '%' && i + 2 < text.size() &&
        absl::HexStringToBytes(text.substr(i + 1, 2), &byte)) {
      decoded += byte;
      i += 2;
    } else if (text[i] == '+' && plus_is_space) {
      decoded += ' ';
    } else {
      decoded += text[i];
    }
  }

  return decoded;
}

std::string Encode(std::string_view text) {
  constexpr std::string_view kHex = "0123456789ABCDEF";

  std::string encoded;
  for (const char c : text) {
    if (IsUnreserved(c)) {
      encoded += c;
    } else {
      const unsigned char byte = static_cast<unsigned char>(c);
      encoded += '%';
      encoded += kHex[byte >> 4];
      encoded += kHex[byte & 0xF];
    }
  }

  return encoded;
}

}  // namespace

std::vector<Field> ParseForm(std::string_view encoded) {
  std::vector<Field> fields;
  for (const std::string_view pair :
       absl::StrSplit(encoded, '&', absl::SkipEmpty())) {
    const size_t equals = pair.find('=');
    const std::string_view name = pair.substr(0, equals);
    const std::string_view value = equals == std::string_view::npos
                                       ? std::string_view()
                                       : pair.substr(equals + 1);

    fields.emplace_back(Decode(name, true), Decode(value, true));
  }

  return fields;
}

std::string FormatForm(std::span<const Field> fields) {
  std::string encoded;
  for (const Field& field : fields) {
    if (!encoded.empty()) encoded += '&';

    encoded += Encode(field.first);
    encoded += '=';
    encoded += Encode(field.second);
  }

  return encoded;
}

std::string_view FindField(std::span<const Field> fields,
                           std::string_view name) {
  for (const Field& field : fields) {
    if (field.first == name) return field.second;
  }
  return {};
}

Target ParseTarget(std::string_view target) {
  const size_t question = target.find('?');

  return Target{
      .path = Decode(target.substr(0, question), false),
      .query = question == std::string_view::npos
                   ? std::vector<Field>()
                   : ParseForm(target.substr(question + 1)),
  };
}

}  // namespace http
