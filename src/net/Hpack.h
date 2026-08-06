#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace microbrowser::net::hpack {

// HPACK — HTTP/2's header compression (RFC 7541).
//
// **This is the most dangerous parser in the network stack and it is worth
// saying why before the first declaration.** HPACK is a *stateful* compressor:
// the decoder holds a dynamic table that the peer's encoder writes to, one
// entry at a time, across every header block on the connection. Its CVE history
// is not buffer overruns — it is the two ends disagreeing about that table, so
// that a header the sender never wrote appears at the receiver. Every bound
// below is therefore enforced on the *decode* side against what arrives, never
// against what the peer says it will send.
//
// Three consequences shape the interface:
//
//  - A decoder that has failed stays failed. A header block half-applied has
//    already left the dynamic table out of step with the peer's, so there is no
//    such thing as recovering and carrying on: the connection is finished.
//  - The encoder never writes to *its* dynamic table. Indexing would mean
//    tracking what the peer evicted, and the only thing it buys is bytes on a
//    request this browser already keeps small. A table that is always empty
//    cannot fall out of step with anything.
//  - Nothing here allocates in proportion to a length the peer declared.
//    Lengths are checked against the bytes that actually arrived first.

// One header field. Names are lowercase on the wire in HTTP/2 and an uppercase
// byte in a name is a protocol error rather than something to normalize —
// see `Decoder::Decode`.
struct Header {
  std::string name;
  std::string value;
};

// The dynamic table size this browser advertises in SETTINGS_HEADER_TABLE_SIZE,
// and the ceiling a "dynamic table size update" may not exceed.
//
// 4096 is the protocol default and also the answer to "how much attacker-chosen
// state may one connection make us hold": the table costs at most this many
// bytes by RFC 7541's own accounting (§4.1), which charges 32 bytes of overhead
// per entry precisely so that a flood of empty headers is bounded too.
inline constexpr std::size_t kDynamicTableBytes = 4096;

// The total decoded size of one header list. A peer can send a header block
// that is small on the wire and enormous decoded — that is what a compression
// bomb is — and the ratio available through the static table alone is over a
// hundred to one. Checked as each field is produced, not after.
inline constexpr std::size_t kMaxHeaderListBytes = 64 * 1024;

// How many fields one header list may have. A response with more than this is
// not a response any page needs; it is a way to make the receiver do work.
inline constexpr std::size_t kMaxHeaderCount = 200;

// The longest single name or value. Enforced before the string is built.
inline constexpr std::size_t kMaxHeaderStringBytes = 16 * 1024;

// Decodes header blocks for one connection, in order.
//
// The instance *is* the connection state: two blocks decoded by two decoders
// are not the same thing as two blocks decoded by one, and a session that
// created a fresh decoder per block would silently be speaking a different
// protocol from the server.
class Decoder {
 public:
  // Appends the block's fields to `out`. False means the connection is over:
  // either the block was malformed or it exceeded a bound, and both are
  // COMPRESSION_ERROR at the HTTP/2 layer because neither leaves a table the
  // peer would agree with.
  //
  // `out` is appended to rather than cleared, because a header list may be
  // split across a HEADERS frame and its CONTINUATIONs — but those are
  // reassembled into one block before they reach here, since a decoder cannot
  // be paused mid-field.
  bool Decode(std::span<const std::byte> block, std::vector<Header>& out);

  bool Failed() const { return error_ != nullptr; }
  // Null until something has gone wrong. A literal, so it costs nothing to
  // carry and can be put straight into a GOAWAY's debug data.
  const char* Error() const { return error_; }

  // Bytes the dynamic table currently accounts for, by RFC 7541 §4.1. Exposed
  // for the tests that assert eviction happened, and for a counter.
  std::size_t TableBytes() const { return table_bytes_; }
  std::size_t TableCount() const { return table_.size(); }

 private:
  // A cursor over the block. Every read goes through it, so "ran off the end"
  // is one check in one place rather than a bound at every call site.
  struct Cursor {
    std::span<const std::byte> data;
    std::size_t at = 0;
    bool Empty() const { return at >= data.size(); }
    std::size_t Remaining() const { return data.size() - at; }
  };

  bool ReadInteger(Cursor& cursor, unsigned prefix_bits, std::uint64_t& out);
  bool ReadString(Cursor& cursor, std::string& out);
  bool Lookup(std::uint64_t index, Header& out) const;
  void Insert(Header field);
  void SetCapacity(std::size_t capacity);
  bool Fail(const char* reason);

  // Front is the most recently inserted, which is what makes index 62 the
  // newest entry without anything having to be renumbered on eviction.
  std::deque<Header> table_;
  std::size_t table_bytes_ = 0;
  std::size_t capacity_ = kDynamicTableBytes;
  const char* error_ = nullptr;
};

// Encodes a request's header list.
//
// A free function rather than a class because there is no state: this encoder
// never writes to a dynamic table (see the note at the top of this file), so
// two calls are independent and the compiler can prove it.
//
// `sensitive` names the fields that must be sent as "literal never indexed" —
// `cookie` and `authorization`. That flag is not a hint: it tells every
// intermediary on the path that the field may not be put in *its* table
// either, which is what stops a compression side channel from recovering a
// session cookie one guess at a time (the CRIME family, and BREACH after it).
void Encode(std::span<const Header> headers, std::string& out);

// True for a field this browser refuses to let an encoder index. Exposed
// because the test that asserts the cookie went out never-indexed has to name
// the same rule the encoder uses rather than a copy of it.
bool IsSensitiveHeader(std::string_view name);

// Huffman, RFC 7541 Appendix B. Exposed for the fuzzer and the round-trip test;
// the encoder and decoder above are the only production callers.
//
// The table in the source is 257 *code lengths* rather than 257 codes, because
// the code is canonical — the codes follow from the lengths, and a length table
// can be checked against Kraft's equality, which a code table cannot. A test
// does exactly that, so a single mistyped entry fails the build rather than
// garbling one header in a thousand.
void HuffmanEncode(std::string_view text, std::string& out);
// False on a padding that is not all ones, on padding longer than seven bits,
// and on an encoded EOS — all three are "malformed" by §5.2 rather than
// something to tolerate, because each is a way of making two implementations
// disagree about where a string ended.
bool HuffmanDecode(std::span<const std::byte> data, std::size_t max_output, std::string& out);
// How many bytes `text` would take Huffman-coded. Used to pick between the
// coded and literal forms, which is a decision per string.
std::size_t HuffmanEncodedSize(std::string_view text);

// The code length for one symbol, 0 through 256 with 256 being EOS. Exposed
// for one reason: it is what lets a test sum 2^-length over the whole alphabet
// and assert the result is exactly one. That is Kraft's equality, it holds for
// a complete prefix code and fails for any single mistyped length, and it is
// the only check that can be made against a table nobody can eyeball.
unsigned HuffmanCodeLength(std::uint16_t symbol);

}  // namespace microbrowser::net::hpack
