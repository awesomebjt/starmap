// =============================================================================
// Indexes.cpp — StarIndex / NameIndex implementation (see Indexes.h for the
// data layout and for why these tables live in registry.ctx()).
// =============================================================================
#include "catalog/Indexes.h"

#include "catalog/Sqlite.h"  // CatalogError

#include <algorithm>
#include <utility>

namespace starmap::catalog {

std::optional<entt::entity> StarIndex::find(std::int64_t star_id) const {
    // C++17 "if with initializer": `it` is scoped to the if statement, and a
    // single hash lookup serves both the test and the result (unlike
    // count() followed by at()).
    if (auto it = map_.find(star_id); it != map_.end()) return it->second;
    return std::nullopt;
}

// Reserving up front matters at this scale (~117k names). Without it the
// vectors grow by doubling (copying/moving all elements each time), and the
// hash maps rehash every bucket each time they grow.
void NameIndex::reserve(std::size_t names, std::size_t stars) {
    entries_.reserve(names);
    owner_.reserve(names);
    by_name_.reserve(names);
    ranges_.reserve(stars);
}

// Why not std::tolower? It depends on the global C locale (surprising
// results, and it is not thread-safe to change), and passing it a negative
// char (any UTF-8 byte >= 0x80 on platforms where char is signed) is
// undefined behaviour. The explicit A-Z range check has neither problem and
// leaves multi-byte UTF-8 sequences intact.
std::string NameIndex::fold(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');  // bytes >= 0x80 (UTF-8) untouched
    }
    return out;
}

bool NameIndex::add(entt::entity e, std::string name, std::string catalog, bool is_primary) {
    const auto idx = static_cast<std::uint32_t>(entries_.size());  // where the new entry WOULD go
    // try_emplace inserts {idx, 0} only if `e` is new, and returns an
    // iterator to the existing or new Range plus a flag saying which. This is
    // a single hash lookup (find() followed by emplace() would take two).
    auto [it, inserted] = ranges_.try_emplace(e, Range{idx, 0});
    Range& r = it->second;
    if (!inserted) {
        // The entity already has names. The new one must extend that range
        // directly; anything else would break the contiguous-slice
        // invariant that names_of() relies on.
        if (r.begin + r.count != idx) {
            throw CatalogError("NameIndex::add: names for an entity must be added consecutively");
        }
        for (std::uint32_t i = r.begin; i < idx; ++i) {  // a star has ~5-15 names: linear scan is fine
            if (entries_[i].name == name && entries_[i].catalog == catalog) {
                entries_[i].is_primary = entries_[i].is_primary || is_primary;
                return false;
            }
        }
    }
    ++r.count;
    // fold() is called before `name` is moved from below. Argument
    // evaluation order would not save us if both happened in one expression,
    // which is why they are separate statements.
    by_name_.emplace(fold(name), idx);
    entries_.push_back(NameEntry{std::move(name), std::move(catalog), is_primary});
    owner_.push_back(e);
    if (is_primary && idx != r.begin) move_to_front(r.begin, idx);  // keep "primary first"
    return true;
}

void NameIndex::move_to_front(std::uint32_t front, std::uint32_t i) {
    // Swap two entries of the same entity and patch their slots in the name map.
    // owner_ needs no update: both slots belong to the same entity.
    // `slot` finds the by_name_ element that points at entry `idx`. With a
    // multimap, equal_range gives every element with that key, and we pick
    // the one whose value is idx.
    auto slot = [this](std::uint32_t idx) {
        auto [first, last] = by_name_.equal_range(fold(entries_[idx].name));
        for (auto it = first; it != last; ++it) {
            if (it->second == idx) return it;
        }
        return by_name_.end();  // unreachable: every entry is in the map
    };
    // Look up both slots BEFORE modifying anything: slot() searches by the
    // entry's name, and the swap below changes which name sits at each index.
    const auto a = slot(front);
    const auto b = slot(i);
    a->second = i;
    b->second = front;
    std::swap(entries_[front], entries_[i]);
}

std::optional<entt::entity> NameIndex::find(std::string_view name) const {
    // Unordered containers iterate equal keys in unspecified order, so the
    // tie-break is written out explicitly: a primary name wins immediately,
    // otherwise the smallest entry index wins, i.e. the lowest star_id,
    // because the loader adds in star_id order.
    auto [first, last] = by_name_.equal_range(fold(name));
    std::optional<entt::entity> best;
    std::uint32_t best_idx = UINT32_MAX;
    for (auto it = first; it != last; ++it) {
        const std::uint32_t i = it->second;
        if (entries_[i].is_primary) return owner_[i];
        if (i < best_idx) {  // earliest loaded (= lowest star_id) wins among non-primary hits
            best_idx = i;
            best = owner_[i];
        }
    }
    return best;
}

std::vector<entt::entity> NameIndex::find_all(std::string_view name) const {
    std::vector<entt::entity> out;
    auto [first, last] = by_name_.equal_range(fold(name));
    for (auto it = first; it != last; ++it) {
        const entt::entity e = owner_[it->second];
        // De-duplicate: one star can carry the same name in two catalogs.
        // Result sets have 1-3 elements, so a linear std::find beats building a set.
        if (std::find(out.begin(), out.end(), e) == out.end()) out.push_back(e);
    }
    return out;
}

std::span<const NameEntry> NameIndex::names_of(entt::entity e) const {
    if (auto it = ranges_.find(e); it != ranges_.end()) {
        // std::span(pointer, count): a non-owning view of the entity's slice.
        return {entries_.data() + it->second.begin, it->second.count};
    }
    return {};
}

// Thanks to the "primary first" invariant this loop normally stops at the
// first element. The loop still covers stars that have no primary name.
const NameEntry* NameIndex::primary_of(entt::entity e) const {
    for (const NameEntry& n : names_of(e)) {
        if (n.is_primary) return &n;
    }
    return nullptr;
}

}  // namespace starmap::catalog
