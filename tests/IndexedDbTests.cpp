// IndexedDB's memory tier, partitioned.
//
// ADR 0038. Same split as StorageTests.cpp / StorageScriptTests.cpp: the assertions here
// are about the store itself -- a key encodes and orders the way EntityStore's own use
// needs, a put/delete keeps an index in step with its store, and the quota is enforced at
// the partition. What a page's own script sees through `indexedDB` is IndexedDbScriptTests.cpp.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "TestSupport.h"
#include "storage/IndexedDbKey.h"
#include "storage/IndexedDbObjectStore.h"
#include "storage/PartitionedIndexedDb.h"
#include "url/PartitionKey.h"
#include "url/Url.h"

namespace microbrowser::tests {

namespace {

url::PartitionKey TopLevelKey(const char* url) {
  const std::optional<url::Url> parsed = url::Url::Parse(url);
  return parsed.has_value()
             ? url::PartitionKey::ForTopLevel(url::ContainerId::Default(), *parsed)
             : url::PartitionKey();
}

}  // namespace

void RegisterIndexedDbTests(std::vector<TestCase>& tests) {
  AddTest(tests, "IndexedDb/KeyEncodingIsStableAndDistinguishesShapes", [] {
    // The same value encodes the same way twice, which is what makes it usable
    // as a map key at all.
    const storage::IndexedDbKey a = storage::IndexedDbKey::OfString("abc");
    const storage::IndexedDbKey b = storage::IndexedDbKey::OfString("abc");
    ExpectEqString(a.Encode(), b.Encode(), "two equal strings encode identically");
    // A number and the string of its digits must not collide -- EntityStore
    // mixes numeric and string entity keys in the same store.
    const storage::IndexedDbKey number = storage::IndexedDbKey::OfNumber(123.0);
    const storage::IndexedDbKey text = storage::IndexedDbKey::OfString("123");
    Expect(number.Encode() != text.Encode(), "a number and its digit-string are different keys");
    // A compound key is distinct from the same parts joined into one string --
    // EntityStore's own `["parentEntityKey", "childEntityKey"]`.
    const storage::IndexedDbKey compound =
        storage::IndexedDbKey::OfArray({storage::IndexedDbKey::OfString("a"),
                                       storage::IndexedDbKey::OfString("b")});
    const storage::IndexedDbKey joined = storage::IndexedDbKey::OfString("ab");
    Expect(compound.Encode() != joined.Encode(),
           "['a','b'] is not the same key as 'ab'");
    Expect(compound.Encode() != storage::IndexedDbKey::OfString("a").Encode(),
           "and not the same as either part alone");
  });

  AddTest(tests, "IndexedDb/NumberKeysOrderByIeeeValueNotByEncodedBytes", [] {
    // `Query` walks a `std::map<std::string, ...>` keyed by `Encode()`, so the
    // byte order of the encoding is the iteration order a cursor sees. Negative
    // numbers, and numbers whose magnitude differs, must still come out low to
    // high.
    storage::IndexedDbObjectStore store;
    std::int64_t delta = 0;
    for (double n : {5.0, -3.0, 0.0, 100.0, -100.0}) {
      store.Put(storage::IndexedDbKey::OfNumber(n), {}, {}, delta);
    }
    const std::vector<storage::IndexedDbObjectStore::QueryEntry> entries =
        store.Query("", std::nullopt);
    std::vector<double> order;
    for (const auto& entry : entries) {
      order.push_back(entry.key.number);
    }
    ExpectEqInt(static_cast<long long>(order.size()), 5, "five distinct numeric keys");
    for (std::size_t i = 1; i < order.size(); ++i) {
      Expect(order[i - 1] < order[i], "numeric keys iterate low to high");
    }
  });

  AddTest(tests, "IndexedDb/PutGetDeleteRoundTripOnTheStoreItself", [] {
    storage::IndexedDbObjectStore store;
    std::int64_t delta = 0;
    const storage::IndexedDbKey key = storage::IndexedDbKey::OfString("k1");
    const auto put_result = store.Put(key, {1, 2, 3}, {}, delta);
    Expect(put_result == storage::IndexedDbObjectStore::PutResult::Stored, "the first write stores");
    ExpectEqInt(delta, 3 + static_cast<std::int64_t>(key.Encode().size()),
               "the delta charges the key and the value");
    const std::optional<std::vector<std::uint8_t>> read = store.Get(key);
    Expect(read.has_value() && read->size() == 3 && (*read)[1] == 2, "the write reads back");
    Expect(!store.Get(storage::IndexedDbKey::OfString("missing")).has_value(),
           "a key that was never written reads back nothing");
    Expect(store.Delete(key, delta), "deleting an existing key reports it");
    Expect(delta < 0, "and the delta gives the bytes back");
    Expect(!store.Delete(key, delta), "deleting it again finds nothing");
    Expect(!store.Get(key).has_value(), "and it is really gone");
  });

  AddTest(tests, "IndexedDb/AUniqueIndexRefusesASecondKeyAndTheWriteDoesNotHappen", [] {
    storage::IndexedDbObjectStore store;
    Expect(store.CreateIndex("byEmail", /*unique=*/true), "the index is created");
    Expect(!store.CreateIndex("byEmail", true), "and not created twice");
    std::int64_t delta = 0;
    const auto first = store.Put(storage::IndexedDbKey::OfString("user1"), {1},
                                 {{"byEmail", storage::IndexedDbKey::OfString("a@example.com")}},
                                 delta);
    Expect(first == storage::IndexedDbObjectStore::PutResult::Stored, "the first record stores");
    const auto second = store.Put(storage::IndexedDbKey::OfString("user2"), {2},
                                  {{"byEmail", storage::IndexedDbKey::OfString("a@example.com")}},
                                  delta);
    Expect(second == storage::IndexedDbObjectStore::PutResult::ConstraintError,
           "a second primary key under the same unique index value is refused");
    Expect(!store.Get(storage::IndexedDbKey::OfString("user2")).has_value(),
           "and the refused write never happened at all");
    // A record is allowed to keep its own index value -- replacing `user1`
    // under the email it already owns is not a collision with itself.
    const auto replace = store.Put(storage::IndexedDbKey::OfString("user1"), {9},
                                   {{"byEmail", storage::IndexedDbKey::OfString("a@example.com")}},
                                   delta);
    Expect(replace == storage::IndexedDbObjectStore::PutResult::Stored,
           "a record may keep the unique value it already held");
  });

  AddTest(tests, "IndexedDb/QueryByIndexFindsEveryPrimaryKeyAndOnlyFiltersOne", [] {
    // EntityStore's own use: a non-unique index over a parent id, walked to
    // delete every child of one parent.
    storage::IndexedDbObjectStore store;
    store.CreateIndex("byParent", /*unique=*/false);
    std::int64_t delta = 0;
    store.Put(storage::IndexedDbKey::OfString("child1"), {1},
             {{"byParent", storage::IndexedDbKey::OfString("parentA")}}, delta);
    store.Put(storage::IndexedDbKey::OfString("child2"), {2},
             {{"byParent", storage::IndexedDbKey::OfString("parentA")}}, delta);
    store.Put(storage::IndexedDbKey::OfString("child3"), {3},
             {{"byParent", storage::IndexedDbKey::OfString("parentB")}}, delta);

    const std::vector<storage::IndexedDbObjectStore::QueryEntry> every_parent_a =
        store.Query("byParent", storage::IndexedDbKey::OfString("parentA"));
    ExpectEqInt(static_cast<long long>(every_parent_a.size()), 2, "two children under parentA");

    const std::vector<storage::IndexedDbObjectStore::QueryEntry> everything =
        store.Query("byParent", std::nullopt);
    ExpectEqInt(static_cast<long long>(everything.size()), 3, "no filter means every record");

    // Deleting the primary keys an index query named removes them from the
    // index too -- RemoveFromIndexes runs on every delete, not just a put's
    // replacement path.
    for (const auto& entry : every_parent_a) {
      store.Delete(entry.primary_key, delta);
    }
    const std::vector<storage::IndexedDbObjectStore::QueryEntry> after_delete =
        store.Query("byParent", storage::IndexedDbKey::OfString("parentA"));
    Expect(after_delete.empty(), "parentA's children are gone from its own index");
    ExpectEqInt(static_cast<long long>(store.Query("byParent", std::nullopt).size()), 1,
               "and parentB's is untouched");
  });

  AddTest(tests, "IndexedDb/TwoTopLevelSitesGetTwoPartitionsForTheSameBrowser", [] {
    storage::PartitionedIndexedDb databases;
    const url::PartitionKey a = TopLevelKey("https://a.example/");
    const url::PartitionKey b = TopLevelKey("https://b.example/");
    Expect(!databases.Has(a), "nothing opened yet");
    databases.Lookup(a).try_emplace("store", storage::IndexedDbDatabase("store"));
    Expect(databases.Has(a), "opening a database created the partition");
    Expect(!databases.Has(b), "and a different top-level site has none of it");
    ExpectEqInt(static_cast<long long>(databases.Lookup(b).size()), 0,
               "b's own (empty) registry, not a's");
  });

  AddTest(tests, "IndexedDb/TheQuotaIsPerPartitionAndARefusalChargesNothing", [] {
    storage::PartitionedIndexedDb databases(/*quota_per_partition=*/16);
    const url::PartitionKey a = TopLevelKey("https://a.example/");
    const url::PartitionKey b = TopLevelKey("https://b.example/");
    Expect(databases.ChargeQuota(a, 10), "a small charge fits under 16 bytes");
    ExpectEqInt(static_cast<long long>(databases.BytesUsed(a)), 10, "and is recorded");
    Expect(!databases.ChargeQuota(a, 10), "a second charge that would exceed 16 is refused");
    ExpectEqInt(static_cast<long long>(databases.BytesUsed(a)), 10,
               "and the refused charge changed nothing");
    Expect(databases.ChargeQuota(b, 10), "a different partition has its own, untouched, quota");
    databases.ChargeQuota(a, -10);
    ExpectEqInt(static_cast<long long>(databases.BytesUsed(a)), 0, "freeing gives the bytes back");
    Expect(databases.ChargeQuota(a, 16), "so a full-size charge now fits again");
  });

  AddTest(tests, "IndexedDb/ClearingTheStoreIsTheOnlyWayStateEndsAndItEndsEverywhere", [] {
    // Same rule StorageTests.cpp asserts for `PartitionedStorage`: nothing here
    // reaches a disk, so the object dying is the whole of "the session ended".
    storage::PartitionedIndexedDb databases;
    const url::PartitionKey a = TopLevelKey("https://a.example/");
    databases.Lookup(a).try_emplace("store", storage::IndexedDbDatabase("store"));
    databases.ChargeQuota(a, 5);
    databases.Clear();
    Expect(!databases.Has(a), "every partition, not just its databases");
    ExpectEqInt(static_cast<long long>(databases.BytesUsed(a)), 0, "and its quota reset too");
  });
}

}  // namespace microbrowser::tests
