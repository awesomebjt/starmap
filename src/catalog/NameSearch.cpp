// =============================================================================
// catalog/NameSearch.cpp — see NameSearch.h for the design
// =============================================================================
#include "catalog/NameSearch.h"

#include "ecs/Components.h"

#include <entt/entity/registry.hpp>

#include <algorithm>
#include <chrono>
#include <tuple>

namespace starmap::catalog {

namespace {

bool is_ascii_alnum(unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

// Tie-break key: smaller is better. std::tuple compares element by element
// (lexicographically), which is exactly "tier first, then primary, then ...".
auto rank_key(const SearchHit& h) {
    return std::make_tuple(static_cast<int>(h.tier), h.primary ? 0 : 1, h.app_mag, h.dist_ly,
                           static_cast<std::uint32_t>(h.entity));  // last: deterministic order
}

}  // namespace

std::string NameSearch::normalize(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    bool pending_space = false;
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        if (c >= 0x80 || is_ascii_alnum(c)) {
            // Emit a single space between words, never at the start.
            if (pending_space && !out.empty()) out.push_back(' ');
            pending_space = false;
            out.push_back(c < 0x80 ? static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c) : ch);
        } else {
            pending_space = true;  // space, punctuation, control: all word separators
        }
    }
    // Gliese synonyms: the first word "gliese" / "gl" / "gj" becomes "gj".
    // (Only as a whole first word: "glow" must stay "glow".)
    for (std::string_view prefix : {"gliese", "gl"}) {
        if (out.size() >= prefix.size() && out.compare(0, prefix.size(), prefix) == 0 &&
            (out.size() == prefix.size() || out[prefix.size()] == ' ')) {
            out.replace(0, prefix.size(), "gj");
            break;
        }
    }
    return out;
}

std::string NameSearch::compact(std::string_view normalized) {
    std::string out;
    out.reserve(normalized.size());
    for (char c : normalized) {
        if (c != ' ') out.push_back(c);
    }
    return out;
}

NameSearch::NameSearch(const NameIndex& names) {
    const auto entries = names.entries();
    owner_.reserve(entries.size());
    primary_.reserve(entries.size());
    key_offset_.reserve(entries.size() + 1);
    compact_offset_.reserve(entries.size() + 1);
    key_offset_.push_back(0);
    compact_offset_.push_back(0);
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const std::string k = normalize(entries[i].name);
        keys_ += k;
        compact_ += compact(k);
        key_offset_.push_back(static_cast<std::uint32_t>(keys_.size()));
        compact_offset_.push_back(static_cast<std::uint32_t>(compact_.size()));
        owner_.push_back(names.owner(i));
        primary_.push_back(entries[i].is_primary ? 1 : 0);
    }
}

std::vector<SearchHit> NameSearch::search(std::string_view query, const entt::registry& registry, std::size_t limit,
                                          double* elapsed_ms) const {
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<SearchHit> hits;
    const std::string q = normalize(query);
    const std::string qc = compact(q);
    const std::string word_q = " " + q;  // "a word inside the key starts with q"
    if (!q.empty() && limit > 0) {
        // Pass 1: classify every name, keep the best one per star. NameIndex
        // stores each star's names CONTIGUOUSLY (its documented invariant), so
        // "best per star" only needs to compare with the previous hit: no
        // hash map of entity -> best hit required.
        for (std::size_t i = 0; i < owner_.size(); ++i) {
            const std::string_view k = key(i);
            const std::string_view kc = compact_key(i);
            MatchTier tier;
            if (k == q || kc == qc) {
                tier = MatchTier::Exact;
            } else if (k.starts_with(q) || kc.starts_with(qc)) {
                tier = MatchTier::Prefix;
            } else if (k.find(word_q) != std::string_view::npos) {
                tier = MatchTier::WordPrefix;
            } else if (k.find(q) != std::string_view::npos ||
                       (qc.size() >= 2 && kc.find(qc) != std::string_view::npos)) {
                tier = MatchTier::Substring;
            } else {
                continue;
            }
            SearchHit h;
            h.entity = owner_[i];
            h.entry = static_cast<std::uint32_t>(i);
            h.tier = tier;
            h.primary = primary_[i] != 0;
            if (!hits.empty() && hits.back().entity == h.entity) {
                // Same star as the previous hit: keep whichever name ranks better.
                const SearchHit& prev = hits.back();
                if (std::make_tuple(static_cast<int>(h.tier), h.primary ? 0 : 1) <
                    std::make_tuple(static_cast<int>(prev.tier), prev.primary ? 0 : 1))
                    hits.back() = h;
                continue;
            }
            hits.push_back(h);
        }
        // Pass 2: fill in brightness and distance, only for the candidates.
        // try_get returns nullptr instead of asserting when a component is
        // missing (e.g. a registry built by a unit test without Photometry).
        for (SearchHit& h : hits) {
            if (const auto* ph = registry.try_get<ecs::Photometry>(h.entity); ph && ph->app_mag) h.app_mag = *ph->app_mag;
            if (const auto* pos = registry.try_get<ecs::Position>(h.entity)) h.dist_ly = pos->dist_ly;
        }
        // Pass 3: partial_sort orders only the first `limit` elements
        // (O(n log limit)) — for a one-letter query n can be ~90,000 stars
        // while we only show 20.
        const std::size_t n = std::min(limit, hits.size());
        std::partial_sort(hits.begin(), hits.begin() + static_cast<std::ptrdiff_t>(n), hits.end(),
                          [](const SearchHit& a, const SearchHit& b) { return rank_key(a) < rank_key(b); });
        hits.resize(n);
    }
    if (elapsed_ms) {
        *elapsed_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }
    return hits;
}

}  // namespace starmap::catalog
