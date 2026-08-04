# ADR 0009 — Bounds on attacker-controlled script

**Status:** accepted · **Date:** 2026-08-04

## Context

`src/js` has a parse depth bound, `kMaxParseDepth`, and the comment on it is correct about why it
exists: the parser is recursive descent, `((((((` nests as deeply as the input is long, and without
a bound a page can overflow the C++ stack. That is memory safety, not tidiness, and the bound stays.

What was wrong was the number, and more importantly the *reasoning behind the number*. It was 256,
which is a round number rather than a measured one, and nothing recorded what it was supposed to
protect against or how much headroom it left. Aiming at youtube.com found the cost of that: the
application bundle failed to parse, and the construct that broke it was not exotic. It was a region
of generated code 97 brackets deep — roughly what a compiler emits for a moderately nested data
structure.

The number was also being read in the wrong unit. `Depth` is incremented at several points in the
expression grammar, so one level of source nesting costs about three units. A reader who sees 256
reasonably assumes "256 levels of nesting" and would be wrong by 3x, which is exactly the kind of
misunderstanding that makes a security bound get raised carelessly later.

Three measurements, all on the default 8MB thread stack, `microbrowser_jsshell -p`:

| What | Result |
|---|---|
| Where the C++ stack actually overflows | between 5,000 and 6,000 nested parentheses |
| Cost of one nesting level | ~1.4KB of stack, ~3.4 `Depth` units |
| What youtube.com's bundle needs | between 300 and 400 `Depth` units |
| Its deepest bracket nesting | 97 |

So the bound was set at roughly 5% of the point where the thing it guards against actually happens,
and real code sat just the wrong side of it.

## Decision

**`kMaxParseDepth` becomes 1024, and the reasoning is written down in units that can be checked.**

1024 `Depth` units is about 300 levels of source nesting and about 0.4MB of stack — a **12x margin**
against the measured overflow point, on the smallest stack the project runs on. It admits
youtube.com's bundle with roughly 3x headroom over what that bundle needs.

**The bound is stated in stack, not in taste.** Anyone changing it has to answer two questions that
now have measured answers: how much stack does one unit cost, and how far is the new number from
where it segfaults. A round number with neither answer beside it is not a bound, it is a guess that
happens to be small.

**Refusing is the correct behaviour and stays.** Beyond the bound the parser reports a SyntaxError
and stops. It does not truncate the program, and it does not try to recover and parse the rest: a
half-understood program is worse than a refused one, because the page then runs something nobody
wrote.

**The bound covers the whole pipeline, not just the parser.** The parser is the first recursion over
a deep tree but not the only one — the bytecode compiler and the tree-walker both walk it. The
constraint is therefore that any tree the parser *accepts* must survive being compiled and run, and
that is what is tested: 250 nested calls, the shape the bundle actually contains, parse, compile and
execute on both engines.

### What this does not change

The runtime bound on call depth (4,096 frames) is a different mechanism protecting a different
thing: the machine's frames are heap data, so that number is about memory rather than the C++ stack,
and it is unaffected. Nothing here relaxes the rule that script is attacker-controlled.

## Consequences

- A page can now build a parse tree about 3x deeper than before, and about 0.4MB of stack goes with
  it in the worst case. That is the price, it is bounded, and it is paid only by a page that
  actually nests that deeply.
- The 8MB figure is an assumption about the thread the parser runs on. When the engine moves into
  its own process and its own threads (ADR 0004), whoever sets those stack sizes has to check this
  bound against them. That is a real coupling and it is written here so it is found rather than
  discovered.
- Fuzzing keeps its job. `js_parser_fuzzer` explores exactly this space, and it is the thing that
  would catch a recursion that escapes the `Depth` guard entirely — a guard covers the productions
  it is written into and nothing else.

## Alternatives considered

**Leave it at 256 and treat the bundle as out of scope.** Rejected, and it is worth being clear
why: the bound was not protecting anything at 256 that it does not protect at 1024, because the
failure it guards against is 20x further away. Keeping it would have traded a real compatibility
loss for no security gain at all.

**Remove the bound and make the parser iterative.** An explicit stack in the parser removes the
C++ recursion and with it the reason for a depth bound. That is the right long-term answer and it
is a rewrite of the expression grammar, not a change to a constant. It also does not remove the
need for *a* bound — an iterative parser with no limit trades a stack overflow for unbounded heap
growth, which is a denial of service rather than a memory-safety bug, but still a page taking the
browser down.

**Raise the bound until the target site passes.** Rejected as the reasoning that produced 256 in the
first place: a number chosen against one input is a number that gets raised again for the next
input. The measurement is what makes 1024 defensible, not the fact that youtube.com fits under it.
