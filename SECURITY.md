# Security Policy

## Status: pre-release, and not yet safe to browse with

**microbrowser does not currently have a network stack, an HTML parser, or a sandbox.** It opens a
window and paints a placeholder page. There is no version of it that should be pointed at the open
web, and there is nothing here to report a vulnerability in yet.

This document exists now rather than later so the policy is decided before the first report arrives,
which is the only time it can be decided calmly.

## Reporting a Vulnerability

> **Contact channel: not yet established.** Fill this in before the first release that can load a
> remote page. A security policy whose reporting address is a guess is worse than none, so this is
> left blank deliberately rather than filled with a plausible-looking one.

When it exists, the process is:

- **Report privately first.** Do not open a public issue for a memory-safety bug, a sandbox escape,
  or an origin-confusion bug.
- Include what you did, what you expected, and what happened. A crashing input file or a small
  reproducer HTML page is worth more than a description of either.
- You will get an acknowledgement that a human read it. If a report goes unacknowledged, the failure
  is ours and escalating publicly is reasonable.

## Disclosure

- **90 days**, or the day a fix ships, whichever comes first.
- If a bug is being exploited, the fix ships as fast as it can be written and the disclosure follows
  it immediately. Coordinating a quiet timeline while users are being attacked is not a service to
  them.
- Reporters are credited unless they ask not to be.
- **There is no bug bounty**, and there will not be one for the foreseeable future. This is a
  single-maintainer project with no funding. Saying so plainly is more useful than leaving it
  ambiguous.

## Scope

In scope once there is code to attack:

- Memory-safety bugs reachable from page content — the HTML, CSS, and JavaScript parsers, the image
  and font decoders, content decompression, and the display-list executor.
- Anything that crosses an origin or site boundary: a document reading another origin's data, a
  cookie or storage key resolving to the wrong partition, a bypass of the same-origin policy or CORS.
- Sandbox escapes, and anything a compromised renderer can reach that it should not — a file path, a
  socket, a window-system connection, another site's data.
- Bugs in the IPC layer: a message from a renderer that the browser process trusts too far.
- Privacy failures with a security shape: a request sent without passing the privacy layer, state
  written to disk when the user did not opt in, a partitioning key that collapses.
- TLS handling, certificate validation, and HTTPS-only bypasses.

Out of scope:

- Anything requiring a compromised operating system or physical access to an unlocked machine.
- Missing hardening that has no exploit path, reported without one. "Compiled without `-fstack-clash-
  protection`" is a fine patch and not a vulnerability report.
- Denial of service by a page consuming memory or CPU. A page can always do that; the mitigation is
  the tab-level resource accounting on the roadmap, not a security fix.
- Phishing and social engineering, except where the browser's own UI can be spoofed or occluded by
  page content. That case *is* in scope.
- Findings from an automated scanner with no analysis attached.

## What We Do Instead of Promising

- The threat model, the trust boundaries, and the containment design are written down in
  `guidelines/security.md` and `docs/adr/0004-process-model-and-site-isolation.md`, including what
  they explicitly do not protect against.
- Unsafe C functions, manual heap ownership, uncentralized environment reads, and cross-module
  reaching are rejected by a lint in the test suite, and every rule ships with a fixture proving it
  can fail.
- ASan, UBSan, and TSan run clean. Every parser that touches network bytes gets a fuzz target on the
  commit it lands.
- There is no telemetry, no auto-update, and no remote configuration, so there is no channel from
  this project into a running browser — and therefore none for anyone who compromises this project
  either.
