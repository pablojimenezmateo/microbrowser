#include "TestSupport.h"
#include "util/MimeType.h"

#include <string>
#include <vector>

namespace microbrowser::tests {

void RegisterMimeTypeTests(std::vector<TestCase>& tests) {
  AddTest(tests, "MimeType/BasicsLowercaseTypeAndKeepParameterCase", [] {
    ExpectEqString(util::BlobMimeType("TEXT/HTML;CHARSET=GBK"), "text/html;charset=GBK",
                   "type/subtype fold, parameter value does not");
    ExpectEqString(util::BlobMimeType("text/html;charset=gbk"), "text/html;charset=gbk",
                   "already-canonical form is a fixed point");
  });

  AddTest(tests, "MimeType/QuotesValuesThatNeedQuoting", [] {
    ExpectEqString(util::BlobMimeType("text/html;charset=gbk("), "text/html;charset=\"gbk(\"",
                   "a value that is not an HTTP token is quoted");
    ExpectEqString(util::BlobMimeType("text/html;charset= gbk"), "text/html;charset=\" gbk\"",
                   "a leading space in the value is quoted, not trimmed");
  });

  AddTest(tests, "MimeType/DuplicateParametersKeepTheFirst", [] {
    ExpectEqString(util::BlobMimeType("text/html;charset=gbk;charset=windows-1255"),
                   "text/html;charset=gbk", "first charset wins");
  });

  AddTest(tests, "MimeType/SpaceBeforeEqualsDropsTheParameter", [] {
    ExpectEqString(util::BlobMimeType("text/html;charset =gbk"), "text/html",
                   "a space in the parameter name is not a token");
  });

  AddTest(tests, "MimeType/FailureIsTheEmptyString", [] {
    ExpectEqString(util::BlobMimeType(""), "", "empty input");
    ExpectEqString(util::BlobMimeType("text"), "", "no slash");
    ExpectEqString(util::BlobMimeType("\x01/x"), "", "a control in the type");
    ExpectEqString(util::BlobMimeType("/x"), "", "empty type");
  });

  AddTest(tests, "MimeType/QuotedRemainderIsDiscarded", [] {
    ExpectEqString(util::BlobMimeType("text/html;charset=\"shift_jis\"iso-2022-jp"),
                   "text/html;charset=shift_jis",
                   "bytes after the closing quote of a quoted value are ignored");
  });
}

}  // namespace microbrowser::tests
