// Internal to //discord: Discord's HTTP API, through which a bot acts.
//
// Each function here is one API call. What they share is in one place: the
// address, authentication, encoding, waiting out rate limits, and turning
// Discord's refusals into Statuses.
//
// Calls may be made by several tasks at once. They are carried out one at a
// time, in the order they were made.

#ifndef DISCORD_REST_H_
#define DISCORD_REST_H_

#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "async/mutex.h"
#include "async/task.h"
#include "discord/model.h"
#include "http/client.h"
#include "json/json.h"

namespace discord_internal {

class Rest {
 public:
  // `http` must outlive this object. `token` is the bot's token, as issued by
  // the Discord Developer Portal.
  Rest(http::Client* http, std::string token)
      : http_(http), token_(std::move(token)) {}

  // Every call fails with:
  //   Unauthenticated     Discord does not accept the token.
  //   PermissionDenied    the bot may not do this here.
  //   NotFound            the channel, message or the like does not exist.
  //   InvalidArgument     Discord found the request malformed.
  //   ResourceExhausted   the bot is rate limited beyond what waiting fixes.
  //   Unavailable         Discord could not be reached, or is having trouble.

  // Where to connect for events: a wss:// URL.
  Task<absl::StatusOr<std::string>> GatewayUrl();

  // Posts `content` as a message in `channel`, and evaluates to the id of
  // the message that made.
  Task<absl::StatusOr<discord::MessageId>> CreateMessage(
      discord::ChannelId channel, std::string_view content);

  // Makes `content` what `message` in `channel`, which the bot posted, says.
  Task<absl::Status> EditMessage(discord::ChannelId channel,
                                 discord::MessageId message,
                                 std::string_view content);

  // Shows the bot as typing in `channel`, for about ten seconds or until it
  // next posts there, whichever comes first.
  Task<absl::Status> TriggerTyping(discord::ChannelId channel);

  // Adds `emoji`, a Unicode emoji such as "🌅", to `message` in `channel` as
  // a reaction from the bot.
  Task<absl::Status> AddReaction(discord::ChannelId channel,
                                 discord::MessageId message,
                                 std::string_view emoji);

  // Which server `channel` is in. Fails with FailedPrecondition if it is not
  // in one, as a direct message is not.
  Task<absl::StatusOr<discord::GuildId>> GuildOf(discord::ChannelId channel);

  // Makes `commands` the slash commands that the bot whose application this
  // is offers in `guild`, in place of whatever it offered there before. They
  // can be used at once.
  //
  // With no guild, sets instead the commands it offers in every server,
  // which Discord takes up to an hour to show people.
  Task<absl::Status> SetCommands(discord::ApplicationId application,
                                 std::optional<discord::GuildId> guild,
                                 std::span<const discord::Command> commands);

  // Answers the use of a command that `interaction` and `token` identify with
  // a message saying `content`. Mentions in it notify nobody.
  Task<absl::Status> Respond(discord::InteractionId interaction,
                             std::string_view token, std::string_view content);

 private:
  // Makes one API call, once those made before it are done, and evaluates to
  // the JSON Discord answered with, which is null when it answered with
  // nothing.
  Task<absl::StatusOr<json::Value>> Call(http::Method method, std::string path,
                                         std::optional<json::Value> body);

  // The call itself. Must not overlap another, which is Call's business.
  Task<absl::StatusOr<json::Value>> CallNow(http::Method method,
                                            std::string path,
                                            std::optional<json::Value> body);

  http::Client* http_;
  std::string token_;
  // An http::Client makes one request at a time.
  Mutex one_at_a_time_;
};

}  // namespace discord_internal

#endif  // DISCORD_REST_H_
