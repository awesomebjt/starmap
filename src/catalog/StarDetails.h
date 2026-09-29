#pragma once
// =============================================================================
// catalog/StarDetails.h — everything about ONE star, fetched lazily from SQLite
// =============================================================================
//
// WHY LAZY?
//   The details panel shows ~40 fields, per-field provenance strings and a
//   planets table — but only for the ONE selected star. Loading all of that
//   into components for all 91,998 stars at start-up would:
//     * add tens of MB of mostly-never-read strings (every *_source column is
//       a heap-allocated std::string per star), and cache-polluting padding
//       to components the render loop iterates every frame;
//     * add load time (the catalog already takes ~240 ms) for data that a
//       typical session looks at for a handful of stars.
//   A primary-key lookup in SQLite ("WHERE star_id = ?") walks one B-tree:
//   well under a millisecond. So we query on selection and cache the result
//   (see scene/Selection.h). The same reasoning applies to planets.
//   Names are the exception: NameIndex already holds all ~117k names in
//   memory because the SEARCH needs every one of them anyway, so the details
//   panel reads names from there instead of querying again.
//
//   The general principle: components hold what SYSTEMS iterate (positions,
//   colours, the physical values that drive colour schemes and filters);
//   rarely-read, per-entity "document" data stays in the database.
// =============================================================================

#include "catalog/Sqlite.h"
#include "ecs/Components.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace starmap::catalog {

// A value together with the catalogue it came from (the *_source column).
struct SourcedValue {
    std::optional<double> value;
    std::string source;  // e.g. 'gaia_gspphot', 'exoplanet_archive', 'hypatia'; empty if unknown
};

struct StarDetails {
    std::int64_t star_id = 0;
    std::string display_name;
    std::optional<std::string> bayer_name;
    // position
    double dist_pc = 0.0, dist_ly = 0.0;
    std::string dist_source;               // gaia_parallax | hyg | exoplanet_archive | sol
    std::optional<double> ra_deg, dec_deg; // ICRS degrees (epoch J2016 for Gaia rows)
    std::optional<double> parallax_mas, parallax_error_mas, ruwe;
    // photometry
    std::optional<double> app_mag, abs_mag, vmag, gaia_g_mag, bp_rp, ci_bv;
    std::string app_mag_band;              // 'V' or 'G'
    // classification + physics (each with its source)
    std::optional<std::string> spectral_type;
    std::string spectral_type_source;
    std::optional<std::string> spectral_class;
    SourcedValue teff_k, radius_sol, mass_sol, luminosity_sol, age_gyr, metallicity;
    // identifiers
    std::optional<std::int64_t> gaia_source_id, hip, hd, hyg_id;
    // planets
    int planet_count = 0;
    std::optional<int> system_planet_count;  // sy_pnum: planets in the whole system (all components)
    std::vector<ecs::Planet> planets;        // ordered by orbital period (innermost first)
    std::string sources;                     // comma list of contributing catalogues
    bool is_sol = false;
};

// One star by primary key. Throws CatalogError if the star_id does not exist.
// Cost: two indexed queries (stars by PRIMARY KEY, planets by idx_planets_star).
[[nodiscard]] StarDetails load_star_details(Database& db, std::int64_t star_id);

}  // namespace starmap::catalog
