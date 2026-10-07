// Cookies: small named values a server asks a browser to keep and send back
// with every later request, which is how a server recognises someone who
// has signed in.

#ifndef HTTP_COOKIE_H_
#define HTTP_COOKIE_H_

#include <chrono>
#include <string>
#include <string_view>

namespace http {

// The value of the cookie called `name` among those a request sent, given
// the request's Cookie header ("a=1; b=2"). Empty if it sent none by that
// name.
std::string_view FindCookie(std::string_view cookie_header,
                            std::string_view name);

// A cookie for a browser to keep.
struct Cookie {
  // Letters, digits and "-_" only: nothing here escapes them.
  std::string name;
  std::string value;

  // How long the browser is to keep it. Zero tells it to forget the cookie
  // now, which is how to sign someone out.
  std::chrono::seconds lifetime{0};

  // Whether the browser is to send it only over HTTPS. Off is for a server
  // being tried on localhost.
  bool secure = true;
};

// The value of a Set-Cookie header that has a browser keep `cookie`. The
// cookie is for the whole site, is kept from the page's scripts, and comes
// back on requests made from the site itself and on links followed into it,
// but not on requests other sites make behind the user's back.
std::string FormatSetCookie(const Cookie& cookie);

}  // namespace http

#endif  // HTTP_COOKIE_H_
