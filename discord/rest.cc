#include "discord/rest.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "async/status_macros.h"
#include "async/task.h"
#include "discord/model.h"
#include "discord/wire.h"
#include "http/client.h"
#include "http/head.h"
#include "json/json.h"
#include "net/event_loop.h"

namespace discord_internal {
namespace {

constexpr std::string_view kApi = "https://discord.com/api/v10";

// Discord asks that bots say what they are.
constexpr std::string_view kUserAgent =
    "DiscordBot (https://github.com/lukeyeh/bedrock, 0.1)";

// How many times a call is made before a rate limit is reported instead of
// waited out, and the longest single wait.
constexpr int kMaxAttempts = 4;
constexpr std::chrono::seconds kMaxRateLimitWait(30);

// Discord's number for answering an interaction with a message.
constexpr int64_t kReplyWithMessage = 4;

// `text` made safe to use as one segment of a URL path.
std::string PercentEncode(std::string_view text) {
  std::string encoded;
  for (const char c : text) {
    const bool unreserved = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                            (c >= '0' && c <= '9') || c == '-' || c == '.' ||
                            c == '_' || c == '~';

    if (unreserved) {
      encoded.push_back(c);
    } else {
      absl::StrAppendFormat(&encoded, "%%%02X", static_cast<unsigned char>(c));
    }
  }
  return encoded;
}

// What a response that is not a success means. `error` is the body, in which
// Discord explains itself.
absl::Status Refusal(int status, const json::Value& error,
                     std::string_view path) {
  const std::string message =
      absl::StrCat("Discord answered ", status, " to ", path, ": ",
                   error["message"].AsString());
  switch (status) {
    case 400: return absl::InvalidArgumentError(message);
    case 401: return absl::UnauthenticatedError(message);
    case 403: return absl::PermissionDeniedError(message);
    case 404: return absl::NotFoundError(message);
    case 429: return absl::ResourceExhaustedError(message);
    default:
      return status >= 500 ? absl::UnavailableError(message)
                           : absl::UnknownError(message);
  }
}

}  // namespace

Task<absl::StatusOr<json::Value>> Rest::Call(http::Method method,
                                             std::string path,
                                             std::optional<json::Value> body) {
  co_return co_await one_at_a_time_.Run(
      CallNow(method, std::move(path), std::move(body)));
}

Task<absl::StatusOr<json::Value>> Rest::CallNow(
    http::Method method, std::string path, std::optional<json::Value> body) {
  http::Request request{
      .method = method,
      .url = absl::StrCat(kApi, path),
      .headers =
          {
              http::Header{
                  .name = "Authorization",
                  .value = absl::StrCat("Bot ", token_),
              },
              http::Header{
                  .name = "User-Agent",
                  .value = std::string(kUserAgent),
              },
          },
  };

  if (body.has_value()) {
    request.headers.push_back(http::Header{
        .name = "Content-Type",
        .value = "application/json",
    });
    request.body = json::Serialize(*body);
  }

  for (int attempt = 1;; ++attempt) {
    const absl::StatusOr<http::Response> response =
        co_await http_->Send(request);
    if (!response.ok()) {
      // Whatever kept the request from being answered, it is the network's
      // doing rather than anything about the request.
      co_return absl::UnavailableError(
          absl::StrCat("cannot reach Discord: ", response.status().message()));
    }

    // Bodies are JSON when there is one; an error page from something in
    // between is not, and reads as null.
    const json::Value answer =
        json::Parse(response->body).value_or(json::Value());
    if (response->status >= 200 && response->status < 300) co_return answer;
    if (response->status != 429 || attempt == kMaxAttempts) {
      co_return Refusal(response->status, answer, path);
    }

    // Rate limited: Discord says how many seconds to stay away.
    const std::chrono::duration<double> wait(answer["retry_after"].AsDouble());
    co_await Sleep(std::min<std::chrono::nanoseconds>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(wait),
        kMaxRateLimitWait));
  }
}

Task<absl::StatusOr<std::string>> Rest::GatewayUrl() {
  CO_ASSIGN_OR_RETURN(
      const json::Value answer,
      co_await Call(http::Method::kGet, "/gateway/bot", std::nullopt));

  const std::string& url = answer["url"].AsString();
  if (url.empty()) {
    co_return absl::UnavailableError(
        "Discord did not say where its gateway is");
  }

  co_return url;
}

namespace {

// Discord's flag for a message under which links are not previewed.
constexpr int64_t kSuppressEmbeds = 1 << 2;

// A message that says `content`, as Discord wants one described.
json::Value MessageBody(std::string_view content,
                        discord::LinkPreviews previews) {
  json::Value body = json::Value().Set("content", content);
  if (previews == discord::LinkPreviews::kHidden) {
    body.Set("flags", kSuppressEmbeds);
  }

  return body;
}

}  // namespace

Task<absl::StatusOr<discord::MessageId>> Rest::CreateMessage(
    discord::ChannelId channel, std::string_view content,
    discord::LinkPreviews previews) {
  CO_ASSIGN_OR_RETURN(
      const json::Value answer,
      co_await Call(http::Method::kPost,
                    absl::StrCat("/channels/", channel.value, "/messages"),
                    MessageBody(content, previews)));

  co_return ParseMessageId(answer);
}

Task<absl::Status> Rest::EditMessage(discord::ChannelId channel,
                                     discord::MessageId message,
                                     std::string_view content,
                                     discord::LinkPreviews previews) {
  co_return (co_await Call(http::Method::kPatch,
                           absl::StrCat("/channels/", channel.value,
                                        "/messages/", message.value),
                           MessageBody(content, previews)))
      .status();
}

Task<absl::Status> Rest::TriggerTyping(discord::ChannelId channel) {
  co_return (co_await Call(http::Method::kPost,
                           absl::StrCat("/channels/", channel.value, "/typing"),
                           std::nullopt))
      .status();
}

Task<absl::Status> Rest::AddReaction(discord::ChannelId channel,
                                     discord::MessageId message,
                                     std::string_view emoji) {
  co_return (
      co_await Call(
          http::Method::kPut,
          absl::StrCat("/channels/", channel.value, "/messages/", message.value,
                       "/reactions/", PercentEncode(emoji), "/@me"),
          std::nullopt))
      .status();
}

Task<absl::StatusOr<discord::GuildId>> Rest::GuildOf(
    discord::ChannelId channel) {
  CO_ASSIGN_OR_RETURN(
      const json::Value answer,
      co_await Call(http::Method::kGet,
                    absl::StrCat("/channels/", channel.value), std::nullopt));

  const discord::GuildId guild = ParseGuildOfChannel(answer);
  if (guild.value == 0) {
    co_return absl::FailedPreconditionError(
        absl::StrCat("channel ", channel.value, " is not in a server"));
  }

  co_return guild;
}

Task<absl::Status> Rest::SetCommands(
    discord::ApplicationId application, std::optional<discord::GuildId> guild,
    std::span<const discord::Command> commands) {
  std::string path = absl::StrCat("/applications/", application.value);
  if (guild.has_value()) absl::StrAppend(&path, "/guilds/", guild->value);
  absl::StrAppend(&path, "/commands");

  co_return (co_await Call(http::Method::kPut, std::move(path),
                           FormatCommands(commands)))
      .status();
}

Task<absl::Status> Rest::Respond(discord::InteractionId interaction,
                                 std::string_view token,
                                 std::string_view content) {
  // A reply may name people, for instance in a leaderboard, and none of them
  // asked to be notified about it.
  json::Value message =
      json::Value()
          .Set("content", content)
          .Set("allowed_mentions",
               json::Value().Set("parse", json::Value::Array()));

  co_return (co_await Call(http::Method::kPost,
                           absl::StrCat("/interactions/", interaction.value,
                                        "/", token, "/callback"),
                           json::Value()
                               .Set("type", kReplyWithMessage)
                               .Set("data", std::move(message))))
      .status();
}

}  // namespace discord_internal
