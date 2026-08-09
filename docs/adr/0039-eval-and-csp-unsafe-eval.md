# ADR 0039 — `eval` / `Function`, and CSP `'unsafe-eval'`

**Status:** accepted · **Date:** 2026-08-09

## Context

ADR 0012 and ADR 0020 refused `eval` and `Function(source)` outright: a missing
name is an honest failure, and CSP `'unsafe-eval'` was called moot because there
was nothing to gate. That held until youtube.com's soft navigation path.

Measured on search→watch (TD-0024): WebPO's context processor waits on BotGuard
(`wne()` / `wpc.f()`). The challenge script at `www.google.com/js/th/…` runs
`(0,eval)(…)`. With no `eval`, that throw left the wait promise unsettled, so
`networkManager.fetch` never issued `/youtubei/v1/player` or `/next`, and SPA
watch never stamped a playable player. Cold `/watch` still worked via
`ytInitialPlayerResponse` in HTML — a different path that never waits on WebPO.

The refusal was therefore not a privacy win in practice: it was an infinite hang
disguised as a ReferenceError on a side script, and it blocked a user-caused
navigation's own subsequent requests.

## Decision

1. **`eval` and `Function` exist** on the global object. `eval` of a non-string
   returns the argument unchanged (the Trusted-Types probe shape BotGuard uses
   before feeding a real source). `eval` of a string runs it in the **global**
   scope — the indirect-eval form `(0,eval)` measured on youtube. Direct-eval
   scope chaining is a follow-up, not required for this path.
2. **`new Function(…)`** builds `(function anonymous(params){ body })` and returns
   the completion value of that expression.
3. **CSP `'unsafe-eval'` is real.** When `script-src` or (failing that)
   `default-src` governs and does not include `'unsafe-eval'`, both entry points
   throw `EvalError`. When nothing governs script, the platform default allows
   them. The gate lives on `js::Interpreter` as a host hook
   (`SetEvalForbidden`); `src/js` still does not include `csp`.
   `DocumentPolicy::AllowsEval` / `PageScript` install the hook from the
   document's policy list.
4. **ADR 0012's "stub worse than absence" still applies** to half-implemented
   eval (e.g. a global that always throws for every string). A working eval
   gated by CSP is the platform; a missing name is not safer once pages hang
   waiting on it.

## Consequences

- Tests that asserted `typeof eval === "undefined"` are updated.
- ADR 0012 / 0020 rows that said these APIs are refused are amended by this ADR.
- Pages with `script-src 'self'` (no `'unsafe-eval'`) keep the strict behaviour.
- youtube.com has no such policy; BotGuard can run; WebPO can mint; SPA player
  fetches can proceed (TD-0024 remainder may still be player stamp / `eue`).
