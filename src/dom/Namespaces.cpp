#include "dom/Namespaces.h"

#include <array>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace microbrowser::dom {

namespace {

// The fixed URIs, indexed by `NamespaceRef::Known`. Order must match the enum;
// the static_assert below is what says so.
constexpr std::array<std::string_view, NamespaceRef::kFirstInterned> kKnownUris = {
    "",
    "http://www.w3.org/1999/xhtml",
    "http://www.w3.org/2000/svg",
    "http://www.w3.org/1998/Math/MathML",
    "http://www.w3.org/1999/xlink",
    "http://www.w3.org/XML/1998/namespace",
    "http://www.w3.org/2000/xmlns/",
};
static_assert(kKnownUris[NamespaceRef::kHtml] == "http://www.w3.org/1999/xhtml");
static_assert(kKnownUris[NamespaceRef::kXmlns] == "http://www.w3.org/2000/xmlns/");

// One interned URI and how many elements and attributes are in it.
//
// Held behind a `unique_ptr` rather than by value, because the index below
// holds a `string_view` into the URI and a `vector` that reallocates would
// move a short (SSO) string's characters with it. A stable address is what
// makes the view outlive a growth.
struct Entry {
  std::string uri;
  std::size_t refs = 0;
};

struct Table {
  std::vector<std::unique_ptr<Entry>> entries;  // index = id - kFirstInterned
  std::unordered_map<std::string_view, std::uint32_t> by_uri;
  std::vector<std::uint32_t> free_slots;
  std::size_t live = 0;
};

// Function-local rather than a file-scope object: a namespace interned during
// the construction of another static would otherwise depend on link order.
Table& Interned() {
  static Table table;
  return table;
}

}  // namespace

NamespaceRef::NamespaceRef(std::string_view uri) {
  if (uri.empty()) {
    return;  // step 1 of every namespace-taking method: "" is no namespace
  }
  for (std::uint32_t known = kHtml; known < kFirstInterned; ++known) {
    if (kKnownUris[known] == uri) {
      id_ = known;
      return;
    }
  }
  Table& table = Interned();
  if (const auto found = table.by_uri.find(uri); found != table.by_uri.end()) {
    id_ = found->second;
    ++table.entries[id_ - kFirstInterned]->refs;
    return;
  }
  auto entry = std::make_unique<Entry>();
  entry->uri = std::string(uri);
  entry->refs = 1;
  const std::string_view stable = entry->uri;
  if (table.free_slots.empty()) {
    table.entries.push_back(std::move(entry));
    id_ = static_cast<std::uint32_t>(table.entries.size() - 1) + kFirstInterned;
  } else {
    id_ = table.free_slots.back();
    table.free_slots.pop_back();
    table.entries[id_ - kFirstInterned] = std::move(entry);
  }
  table.by_uri.emplace(stable, id_);
  ++table.live;
}

NamespaceRef::NamespaceRef(const NamespaceRef& other) : id_(other.id_) {
  if (id_ >= kFirstInterned) {
    ++Interned().entries[id_ - kFirstInterned]->refs;
  }
}

NamespaceRef& NamespaceRef::operator=(const NamespaceRef& other) {
  if (this == &other) {
    return *this;
  }
  NamespaceRef copy(other);  // count up before down: self-URI aliasing is free
  *this = std::move(copy);
  return *this;
}

NamespaceRef& NamespaceRef::operator=(NamespaceRef&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  Release();
  id_ = other.id_;
  other.id_ = kNone;
  return *this;
}

NamespaceRef::~NamespaceRef() { Release(); }

void NamespaceRef::Release() {
  if (id_ < kFirstInterned) {
    return;
  }
  Table& table = Interned();
  std::unique_ptr<Entry>& entry = table.entries[id_ - kFirstInterned];
  if (--entry->refs > 0) {
    return;
  }
  // The last holder is gone, so the slot goes back. This is the difference
  // between a table and a leak: without it a page that makes ten million
  // one-off namespaces keeps every one of them for the life of the process.
  table.by_uri.erase(entry->uri);
  table.free_slots.push_back(id_);
  entry.reset();
  --table.live;
  id_ = kNone;
}

std::string_view NamespaceRef::Uri() const {
  if (id_ < kFirstInterned) {
    return kKnownUris[id_];
  }
  return Interned().entries[id_ - kFirstInterned]->uri;
}

bool IsScriptElement(const NamespaceRef& name_space, std::string_view local_name) {
  return local_name == "script" &&
         (name_space.IsHtml() || name_space == NamespaceRef(NamespaceRef::kSvg));
}

std::size_t InternedNamespaceCount() { return Interned().live; }

}  // namespace microbrowser::dom
