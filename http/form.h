// Names and values written the way HTML forms send them
// (application/x-www-form-urlencoded): "name=luke&q=good+morning". The body
// of a posted form is written this way, and so is the query of a URL, the
// part after its "?".

#ifndef HTTP_FORM_H_
#define HTTP_FORM_H_

#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace http {

// A field's name and its value, as they are meant: decoded.
using Field = std::pair<std::string, std::string>;

// The fields that `encoded` holds, in order. Reading is lenient, since what
// arrives is whatever someone sent: a field with no "=" has an empty value,
// and a "%" that does not begin an escape stands for itself.
std::vector<Field> ParseForm(std::string_view encoded);

// `fields` written as a form: every byte but letters, digits and "-._~"
// escaped, so that any value survives the trip.
std::string FormatForm(std::span<const Field> fields);

// The value of the first field called `name`, or an empty string if there
// is none.
std::string_view FindField(std::span<const Field> fields,
                           std::string_view name);

// What a request asks for: the path, and the fields of its query.
struct Target {
  // Decoded, so "/a%20b" is "/a b".
  std::string path;
  std::vector<Field> query;
};

// Splits a request's target, such as "/search?q=good+morning", in two.
Target ParseTarget(std::string_view target);

}  // namespace http

#endif  // HTTP_FORM_H_
