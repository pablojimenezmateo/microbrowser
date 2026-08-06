#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace microbrowser::net {

// `text/event-stream`, parsed. ADR 0020 §5.
//
// A pure function over a growing buffer, for the reason the WebSocket codec is one: the
// bytes are a server's, the parse has rules whose corners are load-bearing, and a parser
// separated from its socket is one that can be fuzzed and tested without one.
//
// The rules worth knowing, each of which a naive reader gets wrong:
//
//   * **A blank line dispatches.** Fields accumulate until then, so an event is not "a
//     line" -- a `data:` field with three lines is one event with two newlines in it.
//   * **A line with no colon is a field with an empty value**, not a syntax error, and a
//     line starting with a colon is a comment. Servers send `:keep-alive` comments
//     precisely because they cost nothing -- and this is where a stream that "cannot be
//     parsed" would break a connection that a real browser holds for hours.
//   * **One optional space after the colon is stripped, and only one.** `data:  x` is
//     " x". Getting that wrong corrupts every payload that is indented.
//   * **`data` with no value dispatches an event with an empty payload**, but a
//     dispatch with *no* data field at all fires nothing. That is the difference
//     between a keep-alive and a message.
//   * **`retry:` changes the reconnect delay** and is the server's only say over it.
struct ServerSentEvent {
  // `event:`, or empty for the default -- which the binding turns into "message".
  std::string type;
  std::string data;
  // `id:`, remembered by the caller and sent back as `Last-Event-ID` on a reconnect. An
  // id is *sticky*: it survives events that do not carry one, which is what lets a
  // server resume a stream from where a client stopped.
  std::optional<std::string> id;
};

// What one parse pass produced.
struct EventStreamResult {
  std::vector<ServerSentEvent> events;
  // A `retry:` field, in milliseconds, if the stream set one.
  std::optional<std::uint32_t> retry_ms;
  // How much of `input` was consumed. What remains is a partial event and must be kept.
  std::size_t consumed = 0;
};

// Every complete event in `input`. Bytes after the last blank line are not consumed:
// they are a partial event, and a parser that dispatched one would deliver half a
// message whenever a packet boundary fell inside it.
//
// `max_event` bounds a single event's accumulated data. A server that sends `data:`
// forever without a blank line would otherwise grow this without limit, and that is a
// bound on memory a peer controls -- so it is enforced here rather than by a caller. Over
// it, the event is dropped and its bytes are consumed: dropping one message is better
// than closing a stream, and better than holding it in memory forever.
EventStreamResult ParseEventStream(std::string_view input, std::size_t max_event = 4u * 1024u * 1024u);

}  // namespace microbrowser::net
