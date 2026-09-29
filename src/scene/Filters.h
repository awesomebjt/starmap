#pragma once
// =============================================================================
// scene/Filters.h — the star filter model: settings, domains and predicates
// =============================================================================
//
// ROLE IN THE ARCHITECTURE
//   The user trims the star population per colour scheme ("only 5000-6500 K",
//   "only G and K stars", "hide stars without an age"). This header holds the
//   DATA of that feature and the pure LOGIC deciding whether one star passes.
//   It knows nothing about ImGui or bgfx, so the logic is unit-tested without
//   a window or GPU (tests/test_filters.cpp).
//
//     ui::FilterPanel (ImGui)  --edits-->  FilterSettings (registry.ctx(), dirty flag)
//     systems::update_filters  --reads settings, runs star_visible() per star,
//                                adds/removes the FilteredOut tag-->  registry
//     systems::StarRenderSystem --view<...>(entt::exclude<FilteredOut>)--> GPU
//
//   Same "UI writes plain state, a system derives data when dirty" pattern as
//   ColorSettings/ColorSystem.
//
// SEMANTICS (the rules the user sees, all enforced by the functions below)
//   * One SchemeFilter per colour scheme; switching schemes keeps each one.
//   * Default: only the ACTIVE scheme's filter applies. With `combine` on,
//     every scheme whose filter differs from its default applies, ANDed.
//   * "Hide stars with no <field> data" removes stars lacking the value.
//     With it OFF, stars without data stay visible even when the range is
//     trimmed — the range only judges stars that HAVE a value. This keeps the
//     two controls independent: one control, one concept.
//   * Range: keep lo <= value <= hi. A range covering the full domain is
//     "untrimmed" and tests nothing (so float rounding at the ends can never
//     drop a star by accident).
//   * Spectral class (categorical): keep stars whose class checkbox is on.
//     Unknown class ('?') is NOT a checkbox; it is governed by the same
//     "hide stars with no data" toggle as every other scheme. Alternative
//     considered: an "Unknown" checkbox kept in sync with the toggle — two
//     widgets for one flag invite confusion, so there is only the toggle.
//   * Planets: planet_count == 0 means "no known planets" and is treated as
//     NO DATA (the absence of a detection is not a measurement of zero). So
//     "hide no data" shows only hosts, and the 1..8 range judges hosts only.
//   * Distance from Sol (light years) is NOT a colour scheme. It is one
//     range that always applies, ANDed with the scheme filters, because
//     "only stars within 25 ly" describes the map, not the colours on it.
//     A range that covers the whole domain tests nothing. Every star has a
//     distance (Sol is 0), so there is no "hide missing" toggle for it.
//   * only_tagged hides stars that carry no scene::StarTag. While it is on,
//     ColorSystem paints tagged stars from the 16-colour palette instead of
//     the active scheme. The flag lives here, next to the other visibility
//     switches; the recolour is ColorSettings::dirty, which the widget sets.
//     FilterSystem writes FilteredOut only, never DisplayColor.
//   * Sol is ALWAYS visible. It is the origin of the coordinate system, the
//     centre of the distance rings and the "you are here" marker; hiding it
//     would leave the map without its reference point, and one star adds no
//     clutter. (star_visible() short-circuits on StarInfo::is_sol.)
// =============================================================================

#include "scene/ColorScheme.h"

#include <entt/entity/entity.hpp>

#include <array>
#include <bitset>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace starmap::catalog { struct CatalogStats; }
namespace starmap::ecs { struct StarInfo; struct Physical; }

namespace starmap::scene {

// Tag component: present on every star currently hidden by the filters.
// Empty struct => EnTT stores only membership (a sparse set), no payload.
// See systems/FilterSystem.h for why a tag (and not a bool or a group).
struct FilteredOut {};

// Static description of a scheme's value, for labels and formatting.
struct FilterFieldInfo {
    const char* noun;    // "temperature" -> "Hide stars with no temperature data"
    const char* unit;    // "K", "R\u2609" (solar radius), "Gyr", "dex", "planets"
    const char* format;  // printf format for one value, e.g. "%.0f"
};
[[nodiscard]] const FilterFieldInfo& filter_field_info(ColorScheme s);

// The slider's domain for one range scheme, measured once from CatalogStats.
struct FilterDomain {
    float min = 0.0f, max = 1.0f;  // full data range (value units, e.g. kelvin)
    float p01 = 0.0f, p99 = 1.0f;  // 1st/99th percentiles: markers + "Clamp to 1-99 %"
    bool log_scale = false;        // slider maps log10(value) linearly (radius, planets)
    bool integer = false;          // only whole numbers make sense (planet count)
    std::size_t with_data = 0;     // number of stars that have a value
};

// The user's choices for one scheme.
struct SchemeFilter {
    bool hide_missing = false;
    float lo = 0.0f, hi = 0.0f;   // selected range (value units); == domain when untouched
    // Spectral class only: enabled classes, indexed by the ASCII code of the
    // class letter ('G' = 71). A bitset of 128 is 16 bytes and makes the
    // per-star test one bit lookup — cheaper than a std::set<char> search.
    std::bitset<128> classes;
};

// One spectral class present in the catalog (for the checkbox list).
struct SpectralClassEntry {
    char letter = '?';
    std::size_t count = 0;
};

// Everything the filter feature needs, stored as ONE registry context
// variable: registry.ctx().get<FilterSettings>().
struct FilterSettings {
    static constexpr std::size_t kSchemes = static_cast<std::size_t>(ColorScheme::Count);

    std::array<FilterDomain, kSchemes> domains{};   // fixed after load
    std::array<SchemeFilter, kSchemes> filters{};   // edited by the UI / CLI
    std::vector<SpectralClassEntry> classes;        // present classes in canonical order OBAFGKM LTY D ...
    std::size_t total_stars = 0;
    std::size_t unknown_class = 0;                  // stars with spectral class '?'

    bool combine = false;  // AND the non-default filters of ALL schemes

    // Distance from Sol, in light years. Independent of `combine` and of the
    // active scheme: if the handles are not at the ends of distance_domain,
    // every applied scheme filter AND this range must pass. Filled by
    // make_filter_settings from CatalogStats ("dist_ly"); lo/hi start equal
    // to the domain, i.e. "show every distance".
    FilterDomain distance_domain{};
    float distance_lo = 0.0f;
    float distance_hi = 0.0f;

    // Tagged-only view. See the semantics comment above and scene/Tags.h.
    // Whoever sets this also sets ColorSettings::dirty so the palette (or
    // the scheme colours, when turning it off) is applied the same frame.
    bool only_tagged = false;

    bool dirty = true;     // set by anyone who edits the fields above; cleared by FilterSystem

    // Results of the last filter pass (written by FilterSystem, shown by the UI).
    std::size_t visible = 0;
    double last_pass_ms = 0.0;
    std::optional<ColorScheme> applied_scheme;  // active scheme the last pass used

    // Milestone 3: the SELECTED star is exempt from filtering, like Sol. If
    // you search for a star the filters would hide, it is still selected and
    // drawn (with a notice + "Clear filters" button in the details view)
    // rather than silently selecting something invisible. The pass re-runs
    // when the selection changes (applied_selection != current selection).
    entt::entity applied_selection = entt::null;
    bool selection_filtered = false;  // the selected star would be hidden if it weren't selected

    [[nodiscard]] FilterDomain& domain(ColorScheme s) { return domains[static_cast<std::size_t>(s)]; }
    [[nodiscard]] const FilterDomain& domain(ColorScheme s) const { return domains[static_cast<std::size_t>(s)]; }
    [[nodiscard]] SchemeFilter& filter(ColorScheme s) { return filters[static_cast<std::size_t>(s)]; }
    [[nodiscard]] const SchemeFilter& filter(ColorScheme s) const { return filters[static_cast<std::size_t>(s)]; }
};

// Builds domains, the class list and default (= "show everything") filters.
[[nodiscard]] FilterSettings make_filter_settings(const catalog::CatalogStats& stats);

// Range schemes have a slider; SpectralClass has checkboxes.
[[nodiscard]] bool is_range_scheme(ColorScheme s);

// The value a range scheme filters on, or nullopt when the star has no data.
// (Mirrors the "no data" rules of ColorSystem::color_for, so a star that is
// drawn grey is exactly a star that "hide no data" removes.)
[[nodiscard]] std::optional<float> scheme_value(ColorScheme s, const ecs::StarInfo& info, const ecs::Physical& phys);
[[nodiscard]] bool has_data(ColorScheme s, const ecs::StarInfo& info, const ecs::Physical& phys);

// True if the selected range is narrower than the domain.
[[nodiscard]] bool range_is_trimmed(const SchemeFilter& f, const FilterDomain& d);
// True if the distance handles are in from the ends of distance_domain.
[[nodiscard]] bool distance_is_trimmed(const FilterSettings& settings);
// Back to the full distance domain ("show every distance").
void reset_distance(FilterSettings& settings);
// True if the filter would not hide anything (so combine-mode can skip it).
[[nodiscard]] bool is_default(ColorScheme s, const FilterSettings& settings);
// Back to "show everything" for one scheme.
void reset_filter(ColorScheme s, FilterSettings& settings);
// Enables every present class (all = true) or none.
void set_all_classes(FilterSettings& settings, bool all);

// Does one star pass ONE scheme's filter?
[[nodiscard]] bool passes(ColorScheme s, const SchemeFilter& f, const FilterDomain& d,
                          const ecs::StarInfo& info, const ecs::Physical& phys);

// The schemes whose filters apply right now (active only, or all non-default
// ones in combine mode). Computed once per filter pass, not once per star.
[[nodiscard]] std::vector<ColorScheme> applied_filters(const FilterSettings& settings, ColorScheme active);

// The full rule for one star: Sol always visible, otherwise every applied
// scheme filter AND the distance range AND (when only_tagged) "has a tag"
// must pass. `dist_ly` is Position::dist_ly. `tagged` is "this star has a
// StarTag". Both are ignored unless that part of the rule is active, so
// scheme-filter tests can omit them.
[[nodiscard]] bool star_visible(const FilterSettings& settings, const std::vector<ColorScheme>& applied,
                                const ecs::StarInfo& info, const ecs::Physical& phys, float dist_ly = 0.0f,
                                bool tagged = false);

// Short human-readable summary, e.g. "5000 - 6500 K, no-data hidden" or
// "classes G K (2 of 14)". Empty for a default filter.
[[nodiscard]] std::string describe_filter(ColorScheme s, const FilterSettings& settings);

// ----------------------------------------------------------------------------
// Command-line filter requests (parsed by app/CommandLine, applied after the
// catalog is loaded, when the domains are known).
struct FilterRequest {
    ColorScheme scheme = ColorScheme::Temperature;
    std::optional<float> lo, hi;         // --filter temperature=3000:6000 (either side may be empty)
};
// --filter distance=LO:HI, in light years. Not a ColorScheme: it always
// applies, whether or not --combine is set. Either side may be empty.
struct DistanceRequest {
    std::optional<float> lo, hi;
};
struct FilterOptions {
    std::vector<FilterRequest> ranges;
    std::optional<DistanceRequest> distance;
    std::vector<ColorScheme> hide_missing;   // --hide-missing[=scheme]
    bool hide_missing_active = false;        // bare --hide-missing: the --scheme one
    std::optional<std::string> classes;      // --classes G,K
    bool combine = false;                    // --combine
};
// Applies CLI requests; values are clamped into each domain. Returns an error
// message (empty on success), e.g. for an unknown spectral class letter.
std::string apply_filter_options(FilterSettings& settings, const FilterOptions& options, ColorScheme active);

}  // namespace starmap::scene
