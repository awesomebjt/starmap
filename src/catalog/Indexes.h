#pragma once
// =============================================================================
// Indexes.h — lookup tables that live in the registry CONTEXT
// =============================================================================
//
// ROLE IN THE ARCHITECTURE
//   CatalogLoader creates one entity per star and attaches components
//   (Position, Physical, ...; see ecs/Components.h). Components answer
//   "given an entity, what are its properties?". Some questions go the other
//   way:
//     * "which entity is star_id 1?"            -> StarIndex
//     * "which entity is called 'Sirius'?"      -> NameIndex::find
//     * "what are all the names of entity 42?"  -> NameIndex::names_of
//   Those reverse lookups live here. The loader stores one instance of each
//   in the registry context; the app uses them to find Sol (for the ring
//   marker and F-to-focus), to resolve `--focus "Sirius"`, and later for
//   search and tooltips.
//
// ENTT IDIOM: registry.ctx()
//   An entt::registry holds entities and their components. It also has a
//   "context": a small type-indexed bag of objects that belong to the
//   registry as a whole rather than to any single entity. Think of it as
//   registry-scoped singletons:
//       registry.ctx().emplace<NameIndex>();          // create (once, in the loader)
//       auto& idx = registry.ctx().get<NameIndex>();  // fetch from any system
//       registry.ctx().find<NameIndex>();             // pointer, or nullptr if absent
//   Why not globals? Two registries (say a test fixture next to the real
//   one) each get their own index, lifetime is tied to the registry, and
//   every dependency is visible in the code that asks for it. The app puts
//   its camera, colour settings and view settings in ctx() for the same
//   reasons.
//   Alternative considered: storing the names as a component on each entity
//   (e.g. std::vector<std::string> per star). That works for names_of(), but
//   a lookup by name would then have to scan all 92k entities, and
//   per-entity vectors scatter ~117k small heap allocations across memory.
//   One flat table plus a hash map is faster and simpler.
//
// ABOUT entt::entity
//   An entity is just an identifier: a 32-bit integer by default, packing an
//   index plus a "version" that is bumped when the slot is recycled. So a
//   stale handle to a destroyed entity does not silently alias a new one
//   (registry.valid(e) is false). It is cheap to copy and hash, which is
//   why it can be used as a map key below.
// =============================================================================

#include <entt/entity/entity.hpp>  // entt::entity only; the full registry header is heavier and not needed here

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace starmap::catalog {

// -----------------------------------------------------------------------------
// StarIndex: stars.star_id (the database primary key) -> entity.
// Entity ids are assigned by EnTT and are NOT equal to star_id, so anything
// that knows a database id (e.g. "Sol is star_id 1", or planets.star_id)
// goes through this map.
// -----------------------------------------------------------------------------
class StarIndex {
public:
    void reserve(std::size_t n) { map_.reserve(n); }  // one allocation up front instead of repeated rehashing
    // insert_or_assign: a repeated id overwrites (the schema makes star_id unique anyway).
    void add(std::int64_t star_id, entt::entity e) { map_.insert_or_assign(star_id, e); }
    // std::optional instead of entt::null makes "not found" impossible to ignore by accident.
    [[nodiscard]] std::optional<entt::entity> find(std::int64_t star_id) const;
    [[nodiscard]] std::size_t size() const noexcept { return map_.size(); }

private:
    std::unordered_map<std::int64_t, entt::entity> map_;
};

// One row of the star_names table.
struct NameEntry {
    std::string name;     // as stored, e.g. 'Omicron² Eridani', 'HIP 16537', 'Gaia DR3 5164707970261890560'
    std::string catalog;  // 'IAU', 'proper', 'bayer', 'bayer_unicode', 'flamsteed', 'HD', 'HIP', ...
    bool is_primary = false;  // the display name the catalog chose for this star (one per star)
};

// -----------------------------------------------------------------------------
// NameIndex: all names of all loaded stars.
//  * find("epsilon eridani")  -> entity   (ASCII case-insensitive, exact otherwise)
//  * names_of(entity)         -> span of NameEntry (primary first)
//
// DATA LAYOUT (why it looks like this)
//   entries_ is one flat vector holding every NameEntry, grouped by entity:
//       [ Sol's names | Proxima's names | ... ]
//   ranges_ maps entity -> (begin, count) inside entries_, so names_of() can
//   return a std::span, a zero-copy view of a contiguous slice.
//   by_name_ maps the case-folded name -> index into entries_. It is a
//   MULTImap because a few names are shared (binary components, or
//   duplicated designations across catalogs).
//   Indices are uint32_t rather than size_t or pointers: half the memory,
//   and indices stay valid when the vector reallocates (pointers would not).
//
// CASE FOLDING IS ASCII-ONLY
//   'OMICRON² ERIDANI' matches 'Omicron² Eridani' because only the ASCII
//   letters change case, but a Greek capital 'Ε Eri' does NOT match 'ε Eri'.
//   Full Unicode case folding needs ICU or similar, which is a big
//   dependency for a search box. The catalog also stores ASCII spellings
//   ('epsilon Eridani'), so users can type those.
// -----------------------------------------------------------------------------
class NameIndex {
public:
    void reserve(std::size_t names, std::size_t stars);

    // Entries for one entity must be added consecutively. The loader reads
    // star_names ORDER BY star_id, so each entity's names form one contiguous
    // range. Out-of-order adds throw CatalogError.
    // Returns false (and stores nothing) for an exact duplicate (same entity,
    // name and catalog); stars.db has 'Sol'/proper twice, for example. If the
    // duplicate was flagged primary, the flag is merged into the existing entry.
    // `name`/`catalog` are taken BY VALUE and moved into place: callers can
    // pass a temporary (moved, no copy) or an lvalue (one copy), the usual
    // "sink argument" idiom.
    bool add(entt::entity e, std::string name, std::string catalog, bool is_primary);

    // Best match for a name. A star whose *primary* name it is wins; otherwise
    // the first one loaded, which is the lowest star_id, so results are
    // deterministic.
    [[nodiscard]] std::optional<entt::entity> find(std::string_view name) const;
    // Every star carrying this name (a few designations are shared by binary components), without duplicates.
    [[nodiscard]] std::vector<entt::entity> find_all(std::string_view name) const;

    // All names of `e`, primary first. The span points into entries_ and is
    // valid until the next add(), which may reallocate. Empty for unknown entities.
    [[nodiscard]] std::span<const NameEntry> names_of(entt::entity e) const;
    [[nodiscard]] const NameEntry* primary_of(entt::entity e) const;  // nullptr if the star has no primary name

    [[nodiscard]] std::size_t name_count() const noexcept { return entries_.size(); }

    // Raw, index-based access to every entry, for whole-index scans such as
    // the substring search in NameSearch (a hash map can only answer "exact
    // key?", never "contains 'eri'?"). entry(i) and owner(i) are parallel:
    // owner(i) is the entity entries()[i] belongs to. Valid until the next add().
    [[nodiscard]] std::span<const NameEntry> entries() const noexcept { return entries_; }
    [[nodiscard]] entt::entity owner(std::size_t i) const { return owner_.at(i); }
    [[nodiscard]] std::size_t star_count() const noexcept { return ranges_.size(); }

    // ASCII lower-casing used for keys (exposed for tests / UI search boxes).
    [[nodiscard]] static std::string fold(std::string_view s);

private:
    // Swap entry i into position `front` (both belong to the same entity) and
    // keep by_name_ consistent. Used to maintain the "primary first" invariant.
    void move_to_front(std::uint32_t front, std::uint32_t i);

    struct Range {
        std::uint32_t begin = 0;  // index of the entity's first entry in entries_
        std::uint32_t count = 0;  // number of entries
    };
    std::vector<NameEntry> entries_;
    std::vector<entt::entity> owner_;  // owner_[i] = entity of entries_[i] (parallel array, for find())
    std::unordered_map<entt::entity, Range> ranges_;
    std::unordered_multimap<std::string, std::uint32_t> by_name_;  // folded name -> entry index
};

}  // namespace starmap::catalog
