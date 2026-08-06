#include "text/LineBreak.h"

#include "text/UnicodeProperties.h"
#include "util/PerformanceCounters.h"
#include "util/StringUtil.h"

namespace microbrowser::text {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

// UAX #14's rules, in the order they are numbered, applied to a pair of classes.
//
// Written as a function over the pair rather than as a literal table for one reason: the rules are
// *readable* this way and a 33x33 grid of letters is not. Each block below cites the rule it
// implements, so a wrong answer can be traced to a rule rather than to a cell -- which is what makes
// this maintainable by someone who has the specification open and not this file's history.
BreakAction ActionFor(LineBreakClass before, LineBreakClass after) {
  using C = LineBreakClass;

  // LB4, LB5: a mandatory break after BK, and after CR/LF/NL -- except inside CRLF, which is one
  // break rather than two.
  if (before == C::BK || before == C::LF || before == C::NL) {
    return BreakAction::Mandatory;
  }
  if (before == C::CR) {
    return after == C::LF ? BreakAction::Prohibited : BreakAction::Mandatory;
  }
  // LB6: never break before a mandatory break -- the break belongs after the character, not before.
  if (after == C::BK || after == C::CR || after == C::LF || after == C::NL) {
    return BreakAction::Prohibited;
  }
  // LB7: never break before a space or a zero-width space. A run of spaces stays with the word before
  // it, and the break comes after the run (LB18).
  if (after == C::SP || after == C::ZW) {
    return BreakAction::Prohibited;
  }
  // LB8: break *after* a zero-width space, which is the one character whose entire purpose is to
  // offer a break opportunity in text that has none -- a CJK page that pre-inserted them relies on
  // this working.
  if (before == C::ZW) {
    return BreakAction::Allowed;
  }
  // LB9/LB10 are handled by the caller, which folds a combining mark into the character before it:
  // a mark is never a break opportunity of its own, and treating it as one would separate an accent
  // from its letter.
  // LB11: never break around a word joiner, which is what it is for.
  if (after == C::WJ || before == C::WJ) {
    return BreakAction::Prohibited;
  }
  // LB12, LB12a: never break after a non-breaking glue, and never before one unless a space or a
  // break-after precedes it.
  if (before == C::GL) {
    return BreakAction::Prohibited;
  }
  if (after == C::GL && before != C::SP && before != C::BA && before != C::HY) {
    return BreakAction::Prohibited;
  }
  // LB13: never break before a closing bracket, an exclamation, a separator or an infix separator.
  // This is what keeps a comma with the number before it.
  if (after == C::CL || after == C::CP || after == C::EX || after == C::IS || after == C::SY) {
    return BreakAction::Prohibited;
  }
  // LB14: never break after an opening bracket, however many spaces follow.
  if (before == C::OP) {
    return BreakAction::Prohibited;
  }
  // LB15-LB17: quotes, brackets and the em-dash pair.
  if (before == C::QU && after == C::OP) {
    return BreakAction::Prohibited;
  }
  if (before == C::CL && after == C::NS) {
    return BreakAction::Prohibited;
  }
  if (before == C::B2 && after == C::B2) {
    return BreakAction::Prohibited;
  }
  // LB18: break after a space. This is the rule that makes ordinary English text wrap, and it comes
  // *after* LB7 so that a run of spaces breaks once, at its end.
  if (before == C::SP) {
    return BreakAction::Allowed;
  }
  // LB19: never break around a quotation mark.
  if (after == C::QU || before == C::QU) {
    return BreakAction::Prohibited;
  }
  // LB20: break on either side of an unresolved break-either-side character.
  if (before == C::B2 || after == C::B2) {
    return BreakAction::Allowed;
  }
  // LB21: never break before a hyphen, a non-starter or a break-after; never break after a hyphen.
  if (after == C::BA || after == C::HY || after == C::NS) {
    return BreakAction::Prohibited;
  }
  if (before == C::HY) {
    // A hyphen offers a break *after* itself, which is why a hyphenated word wraps at the hyphen.
    return BreakAction::Allowed;
  }
  if (before == C::BA) {
    return BreakAction::Allowed;
  }
  // LB22: never break before an inseparable (an ellipsis, for instance).
  if (after == C::IN) {
    return BreakAction::Prohibited;
  }
  // LB23, LB23a: digits and letters stick together, and so do numbers and CJK prefixes.
  if ((before == C::AL || before == C::AI || before == C::SA) && after == C::NU) {
    return BreakAction::Prohibited;
  }
  if (before == C::NU && (after == C::AL || after == C::AI || after == C::SA)) {
    return BreakAction::Prohibited;
  }
  if (before == C::PR && (after == C::ID || after == C::EB || after == C::EM)) {
    return BreakAction::Prohibited;
  }
  if ((before == C::ID || before == C::EB || before == C::EM) && after == C::PO) {
    return BreakAction::Prohibited;
  }
  // LB24, LB25: currency and numbers. `$` before a digit, `%` after one, and a number's internal
  // structure -- which is what stops `1,000` breaking after the comma.
  if ((before == C::PR || before == C::PO) && (after == C::AL || after == C::AI ||
                                              after == C::NU)) {
    return BreakAction::Prohibited;
  }
  if ((before == C::AL || before == C::AI || before == C::NU) && after == C::PO) {
    return BreakAction::Prohibited;
  }
  if (before == C::NU && (after == C::NU || after == C::IS || after == C::SY)) {
    return BreakAction::Prohibited;
  }
  if ((before == C::IS || before == C::SY) && after == C::NU) {
    return BreakAction::Prohibited;
  }
  if (before == C::CL && after == C::PO) {
    return BreakAction::Prohibited;
  }
  // LB26, LB27: Hangul syllables are not broken internally. A Korean syllable is two to four code
  // points, and breaking between them produces a jamo cluster no reader recognises.
  if (before == C::JL && (after == C::JL || after == C::JV || after == C::H2 || after == C::H3)) {
    return BreakAction::Prohibited;
  }
  if ((before == C::JV || before == C::H2) && (after == C::JV || after == C::JT)) {
    return BreakAction::Prohibited;
  }
  if ((before == C::JT || before == C::H3) && after == C::JT) {
    return BreakAction::Prohibited;
  }
  // LB28: never break between letters. This is what makes a word a word.
  if ((before == C::AL || before == C::AI || before == C::SA || before == C::CM) &&
      (after == C::AL || after == C::AI || after == C::SA)) {
    return BreakAction::Prohibited;
  }
  // LB29: never break between an infix separator and a letter.
  if (before == C::IS && (after == C::AL || after == C::AI)) {
    return BreakAction::Prohibited;
  }
  // LB30: never break between a letter or number and an opening bracket, or between a closing
  // bracket and a letter or number.
  if ((before == C::AL || before == C::AI || before == C::NU) && after == C::OP) {
    return BreakAction::Prohibited;
  }
  if (before == C::CP && (after == C::AL || after == C::AI || after == C::NU)) {
    return BreakAction::Prohibited;
  }
  // LB30b: an emoji modifier stays with its base, or a skin tone lands on its own line.
  if (before == C::EB && after == C::EM) {
    return BreakAction::Prohibited;
  }
  // LB31: everything else may break -- **and this is the rule that fixes CJK**. Two ideographs are
  // `ID`/`ID`, no rule above prohibits it, so a break is allowed between them: a paragraph of
  // Japanese wraps at almost every character, which is what Japanese typesetting does.
  return BreakAction::Allowed;
}

}  // namespace

BreakAction BreakBetween(std::uint32_t before, std::uint32_t after) {
  LineBreakClass first = LineBreakClassOf(before);
  LineBreakClass second = LineBreakClassOf(after);
  // LB1: resolve the classes the specification says have no behaviour of their own. `AI` is ambiguous
  // and resolves to `AL` outside East Asian context; `SA` is complex-context and resolves to `AL`
  // because there is no dictionary here; `CJ` resolves to `NS`, the strict Japanese behaviour, which
  // is the specification's default.
  const auto resolve = [](LineBreakClass value) {
    if (value == LineBreakClass::CJ) {
      return LineBreakClass::NS;
    }
    return value;
  };
  first = resolve(first);
  second = resolve(second);
  return ActionFor(first, second);
}

std::vector<BreakOpportunity> FindBreakOpportunities(std::string_view utf8) {
  std::vector<BreakOpportunity> opportunities;
  std::size_t at = 0;
  std::uint32_t previous = 0;
  std::size_t previous_end = 0;
  bool have_previous = false;
  std::uint32_t code = 0;
  while (util::DecodeUtf8(utf8, at, code)) {
    if (have_previous) {
      // LB9: a combining mark attaches to what precedes it and is never a break opportunity of its
      // own. Handled here rather than in the pair table because it is a rule about *sequences*: the
      // mark takes the class of its base, so `previous` is left alone and this character contributes
      // nothing.
      if (LineBreakClassOf(code) == LineBreakClass::CM && previous_end != 0) {
        previous_end = at;
        continue;
      }
      const BreakAction action = BreakBetween(previous, code);
      if (action != BreakAction::Prohibited) {
        opportunities.push_back(
            BreakOpportunity{previous_end, action == BreakAction::Mandatory});
      }
    }
    previous = code;
    previous_end = at;
    have_previous = true;
  }
  AddPerformanceCounter(PerfCounterId::TextBreakOpportunities,
                        static_cast<std::uint64_t>(opportunities.size()));
  return opportunities;
}

}  // namespace microbrowser::text
