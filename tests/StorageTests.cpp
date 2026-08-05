// Storage, partitioned.
//
// ADR 0021. Three layers, and the assertions worth reading are at the seams: the area
// enforces its own quota, the partitioned store gives two top-level sites two
// different areas for the same origin, and a page reaches both through an interface
// that cannot name a partition.

#include <optional>
#include <string>
#include <vector>

#include "TestSupport.h"
#include "storage/PartitionedStorage.h"
#include "storage/StorageArea.h"
#include "url/PartitionKey.h"
#include "url/Url.h"

namespace microbrowser::tests {

namespace {

url::PartitionKey KeyFor(const char* top_level, const char* document) {
  const std::optional<url::Url> top = url::Url::Parse(top_level);
  const std::optional<url::Url> doc = url::Url::Parse(document);
  if (!top.has_value() || !doc.has_value()) {
    return url::PartitionKey();
  }
  return url::PartitionKey::ForEmbedded(url::ContainerId::Default(),
                                        url::Site::FromUrl(*top), *doc);
}

}  // namespace

void RegisterStorageTests(std::vector<TestCase>& tests) {
  AddTest(tests, "Storage/KeepsInsertionOrderBecauseKeyNIsPartOfTheApi", [] {
    storage::StorageArea area;
    Expect(area.SetItem("b", "1"), "b");
    Expect(area.SetItem("a", "2"), "a");
    Expect(area.SetItem("c", "3"), "c");
    ExpectEqInt(static_cast<long long>(area.Length()), 3, "three keys");
    // Insertion order, not sorted: `key(0)` is an API a page enumerates with, and a
    // hash map alone cannot answer it at all.
    ExpectEqString(area.KeyAt(0).value_or(""), "b", "first written is first");
    ExpectEqString(area.KeyAt(1).value_or(""), "a", "then the second");
    Expect(!area.KeyAt(3).has_value(), "and past the end is nothing rather than an error");
    // Rewriting a key keeps its position, which is what every other browser does and
    // what a page that renders a list from its own keys depends on.
    Expect(area.SetItem("b", "changed"), "rewrite b");
    ExpectEqString(area.KeyAt(0).value_or(""), "b", "b is still first");
    ExpectEqString(area.GetItem("b").value_or(""), "changed", "with the new value");
  });

  AddTest(tests, "Storage/RemovingAKeyLeavesNoHoleForAnEnumerationToFind", [] {
    storage::StorageArea area;
    area.SetItem("a", "1");
    area.SetItem("b", "2");
    Expect(area.RemoveItem("a"), "removed");
    Expect(!area.RemoveItem("a"), "and removing it twice reports nothing the second time");
    ExpectEqInt(static_cast<long long>(area.Length()), 1, "one key left");
    ExpectEqString(area.KeyAt(0).value_or(""), "b", "and it is at index 0, not index 1");
  });

  AddTest(tests, "Storage/TheQuotaIsEnforcedAtTheStoreAndAFailedWriteChangesNothing", [] {
    // Storage is memory, so unbounded storage from a page is a denial of service
    // against the process -- which makes the quota a security bound, and a security
    // bound with an opt-out is not one. A small quota here so the test is about the
    // rule rather than about allocating five megabytes.
    storage::StorageArea area(32);
    Expect(area.SetItem("key", "0123456789"), "a small value fits");
    Expect(!area.SetItem("key2", "0123456789012345678901234567890"), "a large one does not");
    ExpectEqInt(static_cast<long long>(area.Length()), 1, "and nothing was added");
    ExpectEqString(area.GetItem("key").value_or(""), "0123456789",
                   "the value that was already there is untouched: a page that catches "
                   "QuotaExceededError and retries must not find a half-written value");
    // Rewriting one key with a value of the same size never fails, however many times
    // it happens: the old cost comes off before the new one goes on.
    for (int i = 0; i < 100; ++i) {
      Expect(area.SetItem("key", "9876543210"), "a rewrite of the same size always fits");
    }
    ExpectEqInt(static_cast<long long>(area.Bytes()), 13, "and the size did not grow");
  });

  AddTest(tests, "Storage/TwoTopLevelSitesGiveTheSameOriginDifferentStorage", [] {
    // ADR 0005 applied to storage, and the sentence a user would notice: `example.com`
    // embedded in `a.com` and `example.com` embedded in `b.com` have **different**
    // localStorage. It breaks third-party single sign-on, and under this project's
    // priority order that breakage is a decision.
    storage::PartitionedStorage store;
    store.Lookup(KeyFor("https://a.com/", "https://example.com/")).SetItem("token", "from-a");
    store.Lookup(KeyFor("https://b.com/", "https://example.com/")).SetItem("token", "from-b");
    ExpectEqInt(static_cast<long long>(store.Partitions()), 2, "two partitions, one origin");
    ExpectEqString(
        store.Lookup(KeyFor("https://a.com/", "https://example.com/")).GetItem("token").value_or(""),
        "from-a", "each sees only its own");
    ExpectEqString(
        store.Lookup(KeyFor("https://b.com/", "https://example.com/")).GetItem("token").value_or(""),
        "from-b", "and the other's is not reachable from it");
  });

  AddTest(tests, "Storage/AskingWhetherAPartitionExistsDoesNotCreateIt", [] {
    // `Lookup` creates because a page that reads before it writes must get an empty
    // store rather than an error. That makes a second question necessary: a caller
    // counting partitions would otherwise create the one it asked about.
    storage::PartitionedStorage store;
    const url::PartitionKey key = KeyFor("https://a.com/", "https://a.com/");
    Expect(!store.Has(key), "nothing stored yet");
    ExpectEqInt(static_cast<long long>(store.Partitions()), 0, "and asking created nothing");
    store.Lookup(key);
    Expect(store.Has(key), "reading created it, which is what the API requires");
  });

  AddTest(tests, "Storage/ClearingTheStoreIsTheOnlyWayStateEndsAndItEndsEverywhere", [] {
    // Nothing here reaches a disk, so this is the whole of "what outlives a browser
    // session": ADR 0021 §2 makes persistence a per-site user act that lands together
    // with encryption at rest, and until then the object dying is the end of the data.
    storage::PartitionedStorage store;
    store.Lookup(KeyFor("https://a.com/", "https://a.com/")).SetItem("k", "v");
    store.Lookup(KeyFor("https://b.com/", "https://b.com/")).SetItem("k", "v");
    store.Clear();
    ExpectEqInt(static_cast<long long>(store.Partitions()), 0, "every partition, not just one");
  });
}

}  // namespace microbrowser::tests
