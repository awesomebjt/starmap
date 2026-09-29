#pragma once
// =============================================================================
// catalog/NameSearch.h — "type a few letters, get the right star" search
// =============================================================================
//
// ROLE IN THE ARCHITECTURE
//   NameIndex (Indexes.h) answers EXACT questions fast: "which star is called
//   'Sirius'?" is one hash-map lookup. A search box needs FUZZY questions:
//   "which names start with 'ran'?", "which contain 'eri'?". A hash map can't
//   answer those (hashing destroys ordering and substrings), so NameSearch
//   keeps its own copy of every name in a SCAN-FRIENDLY form and simply looks
//   at all ~117,000 of them on each keystroke. That sounds wasteful, but:
//     * the keys live in ONE contiguous buffer ("blob"), so the scan streams
//       through ~3 MB of memory front to back — the access pattern CPUs and
//       their prefetchers love;
//     * std::string_view::find on short strings is a tight memchr-based loop;
//     * measured: ~1–3 ms per query in Release for the full catalog, i.e.
//       invisible at typing speed. (See the README for the numbers.)
//   Fancier structures (a suffix array, a trie, an n-gram index) would make
//   each query microseconds but cost memory, build time and a lot of code —
//   not worth it at this size. The rule of thumb: measure the brute force
//   first; build an index only when the measurement says you must.
//
// NORMALISATION (both names and queries go through normalize())
//   * ASCII letters are lower-cased: search is case-insensitive.
//   * ASCII punctuation becomes a space and runs of spaces collapse, so
//     "DENIS-P J0823" and "denis p j0823" are the same key.
//   * The Gliese-catalogue prefixes "Gliese", "Gl" and "GJ" are all mapped to
//     "gj": stars.db stores "Gl 581" and "GJ 581", people type "Gliese 581".
//   * Non-ASCII bytes (UTF-8 for ε, ², ¹ ...) are kept verbatim; stars.db
//     also carries ASCII spellings of those names ("eps Eri", "Alpha2 ...").
//   A second, space-free "compact" key lets "hip16537" find "HIP 16537".
//
// RANKING (best first)
//   1. match tier: Exact > Prefix > WordPrefix (a word inside the name starts
//      with the query, e.g. "eri" in "eps eri") > Substring;
//   2. the matched name is the star's PRIMARY name (what the map displays);
//   3. brighter apparent magnitude (famous stars tend to be bright);
//   4. nearer distance.
//   Each star appears at most once, represented by its best-matching name.
// =============================================================================

#include "catalog/Indexes.h"

#include <entt/entity/fwd.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace starmap::catalog {

enum class MatchTier : std::uint8_t { Exact = 0, Prefix = 1, WordPrefix = 2, Substring = 3 };

struct SearchHit {
    entt::entity entity{};     // the star
    std::uint32_t entry = 0;   // index into NameIndex::entries(): the name that matched
    MatchTier tier = MatchTier::Substring;
    bool primary = false;      // the matched name is the star's display name
    float app_mag = 99.0f;     // apparent magnitude used for ranking (99 = unknown)
    float dist_ly = 0.0f;
};

class NameSearch {
public:
    NameSearch() = default;
    // Copies and normalises every name of `names` (a one-off ~30 ms at start-up).
    explicit NameSearch(const NameIndex& names);

    // Up to `limit` best hits for `query`. `registry` supplies Photometry and
    // Position for the brightness/distance tie-breaks. `elapsed_ms`, if given,
    // receives the wall-clock time of the call (for the UI and the README).
    [[nodiscard]] std::vector<SearchHit> search(std::string_view query, const entt::registry& registry,
                                                std::size_t limit = 20, double* elapsed_ms = nullptr) const;

    [[nodiscard]] std::size_t size() const noexcept { return owner_.empty() ? 0 : owner_.size(); }

    // Exposed for tests and for anyone who wants the same key rules.
    [[nodiscard]] static std::string normalize(std::string_view s);
    [[nodiscard]] static std::string compact(std::string_view normalized);  // normalize() minus spaces

private:
    // All keys back to back in one string; key i is blob_[offset_[i] .. offset_[i+1]).
    // Offsets instead of std::vector<std::string>: one allocation instead of
    // 117k, and the scan touches consecutive memory.
    std::string keys_, compact_;
    std::vector<std::uint32_t> key_offset_, compact_offset_;
    std::vector<entt::entity> owner_;  // parallel to NameIndex::entries()
    std::vector<std::uint8_t> primary_;  // uint8_t rather than vector<bool>: plain bytes, no bit proxies

    [[nodiscard]] std::string_view key(std::size_t i) const {
        return std::string_view(keys_).substr(key_offset_[i], key_offset_[i + 1] - key_offset_[i]);
    }
    [[nodiscard]] std::string_view compact_key(std::size_t i) const {
        return std::string_view(compact_).substr(compact_offset_[i], compact_offset_[i + 1] - compact_offset_[i]);
    }
};

}  // namespace starmap::catalog
