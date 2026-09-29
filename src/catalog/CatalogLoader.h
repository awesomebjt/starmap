#pragma once
// =============================================================================
// CatalogLoader.h — stars.db  ->  entt::registry
// =============================================================================
//
// ROLE IN THE ARCHITECTURE
//   This is the bridge between the offline data pipeline and the real-time
//   program:
//
//     build_star_catalog.py --> stars.db --> load_catalog() --> entt::registry
//     (Python, offline)         (SQLite)    (this file)         (one entity per star)
//
//   After load_catalog() returns, the rest of the program never touches
//   SQLite again. Systems (ColorSystem, SizeSystem, StarRenderSystem, ...)
//   read the components the loader attached, and the database file could
//   even be deleted.
//
// WHY LOAD EVERYTHING UP FRONT?
//   92k stars x ~300 bytes of components is roughly 30 MB, which loads in
//   about 0.3 s. Holding it all in RAM lets every frame iterate packed arrays
//   with no I/O. Streaming or paging would only pay off for catalogs 100x
//   larger (full Gaia is about 1.8 billion rows).
//
// ENTT BACKGROUND FOR READERS NEW TO IT
//   * entt::registry owns all entities and all components.
//   * An entity is only an ID. A "star" is simply an ID that has a Position,
//     Physical, ... component attached.
//   * Each component TYPE lives in its own storage (a sparse set): a packed,
//     contiguous array of T plus index tables mapping entity <-> slot. That
//     is why a system that only needs Position + DisplayColor streams through
//     exactly those two arrays, which is cache-friendly and is the point of
//     an ECS.
//
// HEADER HYGIENE
//   <entt/entity/fwd.hpp> only forward-declares entt::registry (as the
//   basic_registry<> alias). That is enough for a reference parameter, and it
//   keeps this header cheap to include. Only the .cpp pulls in the full
//   (template-heavy) registry.hpp.
// =============================================================================

#include <entt/entity/fwd.hpp>

#include <cstddef>
#include <filesystem>
#include <optional>

namespace starmap::catalog {

// What to load. Aggregate struct with defaults, so call sites can use
// designated initializers: LoadOptions{.db_path = p, .max_dist_ly = 50.0}.
struct LoadOptions {
    std::filesystem::path db_path;             // path to stars.db
    double max_dist_ly = 200.0;                // stars.dist_ly <= this (the db itself goes to ~200 ly)
    // Keep a Gaia-distance star only if parallax / parallax_error >= this.
    // Rows without a Gaia parallax (HYG-only stars, exoplanet-archive distances, Sol) are always kept.
    // std::nullopt means "no filter". It is bound as SQL NULL, and the WHERE
    // clause short-circuits on ":min_snr IS NULL".
    // Why filter at all? Gaia parallaxes with SNR < ~5 have large distance
    // errors, and such stars can appear much closer than they are, cluttering
    // the nearby volume. The app exposes this as --min-snr.
    std::optional<double> min_parallax_snr;
    // Keep a Gaia-distance star only if ruwe <= this (typical cut: 1.4). Rows without ruwe are kept.
    // RUWE (renormalised unit weight error) > 1.4 usually means an unresolved
    // binary or a bad astrometric fit.
    std::optional<double> max_ruwe;
    bool load_names = true;        // star_names -> ctx NameIndex
    bool load_planets = false;     // planets    -> ecs::Planets component on host stars
    bool load_provenance = false;  // *_source columns -> ecs::Provenance component
};

// What happened: counts for logging and tests, plus timing.
struct LoadResult {
    std::size_t stars = 0;          // entities created
    std::size_t names = 0;          // NameIndex entries
    std::size_t planets = 0;        // Planet records attached
    std::size_t duplicate_names = 0;  // exact duplicate star_names rows dropped
    std::size_t skipped_rows = 0;  // star_names / planets rows whose star was filtered out
    double elapsed_ms = 0.0;        // wall time for the whole call
};

// Creates one entity per selected `stars` row with components
//   StarId, Position, StarInfo, Photometry, Physical, CatalogIds, Quality, DisplayColor
//   (+ Provenance, Planets when requested)
// and puts StarIndex (always) and NameIndex (if load_names) into registry.ctx(),
// replacing any previous ones. Intended for an empty registry; entities already
// present are left alone.
// Throws CatalogError if the file is missing, is not a star catalog, or SQLite fails.
// Exception safety: if it throws halfway, the entities created so far stay
// in the registry (the "basic" guarantee). Callers treat a throw as fatal,
// as the app does, or start again from a fresh registry.
LoadResult load_catalog(entt::registry& registry, const LoadOptions& options);

}  // namespace starmap::catalog
