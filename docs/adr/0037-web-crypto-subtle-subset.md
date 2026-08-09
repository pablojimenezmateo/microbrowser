# ADR 0037 — Web Crypto subset for `crypto.subtle`

**Status:** accepted · **Date:** 2026-08-09

## Context

ADR 0029 already ships `crypto.getRandomValues` and `crypto.randomUUID`. youtube's
player (`au()` in `base.js`) also requires `window.crypto.subtle` with
`importKey`, `encrypt`, and `sign` before it constructs the PES (offline)
encoder. Without those methods `au()` returns undefined, Woffle logs
`PES is undefined`, and the facade can stick on `fmt.unplayable` even while MSE
buffers a playable stream.

The survey counted **2** `crypto.subtle` sites across Gate targets — small, but
on the watch playback path. A full Web Crypto implementation is large (RSA, ECDSA,
PBKDF2, AES-GCM, …). A stub that exists but rejects every operation would send
the player down a path that assumes keys exist.

OpenSSL already links for TLS (`net` only). Putting it in `util` would break
util's dependency-free contract (Sha2 lives in util *because* CSP cannot see
`net`). Hand-rolled AES-128-CTR and HMAC-SHA256 over existing `Sha2` keep the
seam and stay auditable.

## Decision

1. **Own AES-128-CTR and HMAC-SHA256 in `util`** (`AesCtr`, `Hmac`), with NIST /
   RFC vectors in tests. No OpenSSL in util.
2. **Expose a narrow `crypto.subtle` in bindings**: raw `importKey` for AES-CTR
   (128-bit) and HMAC-SHA-256; `encrypt` (AES-CTR, counter length 128); `sign`
   (HMAC-SHA-256). Everything else stays **absent**.
3. **Promises settle synchronously** when the bytes are in hand — same shape as
   `Response.text()` — because the work is CPU-bound and tiny.

## Consequences

- youtube's `au()` feature-detects a usable subtle and can build PES keys.
- Expanding the algorithm set is a deliberate MODULE / ADR change, not a silent
  kitchen-sink growth of Sha2.
- IndexedDB and `BroadcastChannel` (Woffle `g.D8`) remain separate gaps on ADR
  0021's schedule.
