#pragma once
// =============================================================================
// CatalogStats.h — per-field coverage and value ranges, computed from the registry
// =============================================================================
//
// ROLE IN THE ARCHITECTURE
//   The app computes this once after loading (App.cpp) and turns it into
//   scene::ColorRanges (see scene/ColorScheme.cpp), which fixes the ends of
//   each continuous colormap. It is also what the loader's CLI tool prints
//   as a coverage report.
//
// WHY PERCENTILES (p01 / p99) RATHER THAN min / max?
//   Real catalogs have outliers: a few stars with radius 1000 Rsun (red
//   giants, or bad fits) or ages of 0.001 Gyr. Scaling a colormap from min
//   to max would squash 99% of stars into a sliver of the colour range.
//   Clipping at the 1st and 99th percentile spends the colours where the
//   data actually is. Values outside the range just saturate at the ends.
//   (Diameter goes one step further and uses a log scale on top.)
//
// WHY COMPUTE FROM THE REGISTRY, NOT WITH SQL?
//   SQL could compute the same numbers, but the registry is the source of
//   truth once loaded. If the user applied --max-ly or --min-snr filters,
//   the stats describe exactly the stars that are actually shown.
// =============================================================================

#include <entt/entity/fwd.hpp>

#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace starmap::catalog {

// Statistics of one optional field (e.g. teff_k) over all loaded stars.
struct FieldStats {
    std::string name;       // 'teff_k', 'age_gyr', ... (same names as the db columns)
    std::size_t count = 0;  // stars with a value
    double min = 0.0, max = 0.0;  // meaningful only if count > 0
    double p01 = 0.0, p99 = 0.0;  // 1st / 99th percentile (nearest-rank), robust colormap limits
    // Fraction of stars that have this field: count / total, in 0..1.
    [[nodiscard]] double coverage(std::size_t total) const {
        return total ? static_cast<double>(count) / static_cast<double>(total) : 0.0;
    }
};

struct CatalogStats {
    std::size_t total_stars = 0;
    std::size_t with_spectral_type = 0;
    std::size_t with_planets = 0;  // planet_count > 0
    std::vector<FieldStats> fields;  // dist_ly, magnitudes, colours, physicals, planet_count, quality
    // std::map (ordered) rather than unordered_map, so printing it gives a
    // stable, alphabetical listing.
    std::map<char, std::size_t> spectral_classes;  // 'G' -> n, '?' = unknown

    // Iterates views over the components, so it reflects whatever is in the
    // registry now (e.g. after the user filtered or deleted stars).
    [[nodiscard]] static CatalogStats compute(const entt::registry& registry);

    // Lookup by column name, e.g. stats.field("teff_k"). nullptr if unknown.
    // Linear search over ~15 entries is faster than any map at this size.
    [[nodiscard]] const FieldStats* field(std::string_view name) const;
};

}  // namespace starmap::catalog
