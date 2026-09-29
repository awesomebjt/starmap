#pragma once
// =============================================================================
// Components.h — the ECS components that make up one star
// =============================================================================
//
// ROLE IN THE ARCHITECTURE
//   CatalogLoader creates one entt::entity per row of the `stars` table and
//   attaches the components below. Every system in the app reads and writes
//   these:
//     ColorSystem       reads  Physical / StarInfo    -> writes DisplayColor
//     SizeSystem        reads  Physical / Photometry  -> writes DisplaySize (render/RenderComponents.h)
//     StarRenderSystem  reads  Position + DisplayColor + DisplaySize -> GPU instance buffer
//     CatalogStats      reads  everything             -> ranges for colormaps
//   Render-only components (DisplaySize, the Marked tag) are declared in
//   scene/RenderComponents.h instead, so this catalog library has no
//   dependency on rendering.
//
// ENTT IDIOM: COMPONENTS ARE PLAIN DATA
//   A component is any movable type: no base class, no registration, no
//   virtual functions. Behaviour lives in SYSTEMS, i.e. free functions that
//   iterate views. The registry stores each component type in its own
//   densely packed array (a "storage", implemented as a sparse set).
//
// WHY MANY SMALL STRUCTS INSTEAD OF ONE `struct Star`?
//   Data-oriented design. The per-frame render loop only needs Position +
//   DisplayColor + DisplaySize, about 36 bytes per star. If everything lived
//   in one ~300-byte Star struct (strings, optionals, IDs...), iterating
//   positions would drag all the unused bytes through the CPU cache too.
//   Split storages mean each system streams only the arrays it asks for.
//   Grouping follows ACCESS PATTERNS (what is read together), not the order
//   of database columns.
//
// std::optional<float> FOR "UNKNOWN"
//   Coverage varies widely: nearly every star has a position, only about
//   60% have Teff and far fewer have an age. std::optional makes "unknown"
//   explicit and type-checked. Sentinels such as NaN or -1 are easy to forget
//   and poison averages. Cost: optional<float> is 8 bytes (value plus flag,
//   padded) instead of 4. At 92k stars that is irrelevant.
//
// AGGREGATES AND emplace
//   None of these structs declares a constructor, so they are aggregates.
//   registry.emplace<T>(e, a, b, c) brace-initialises T{a, b, c} in member
//   order (see CatalogLoader.cpp). Keep that in mind when reordering members.
// =============================================================================

#include <glm/vec3.hpp>  // GLM: header-only maths library whose types mirror GLSL (vec3, mat4, ...)
#include <glm/vec4.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace starmap::ecs {

// stars.star_id, the database primary key (Sol is always 1). Kept on the
// entity so a system holding an entity can refer back to the database row.
struct StarId {
    std::int64_t value = 0;
};

// Positions in parsecs (1 pc = 3.2616 ly), Sun at the origin.
// float is plenty for rendering: ~7 significant digits over 61 pc
// (200 ly) is sub-AU precision.
//
// GALACTIC FRAME (what the renderer uses):
//   +x points to the Galactic Centre (l = 0°, b = 0°, in Sagittarius),
//   +y points along Galactic rotation (l = 90°, towards Cygnus),
//   +z points to the North Galactic Pole (b = +90°, in Coma Berenices).
//   It is right-handed (x cross y = z), and the galactic plane is z = 0.
//   The camera uses +z as "up" (see scene/OrbitCamera.h), so the disc of the
//   Milky Way appears horizontal on screen.
struct Position {
    glm::vec3 galactic_pc{0.0f};    // stars.x, y, z   : +x galactic centre, +z north galactic pole
    glm::vec3 equatorial_pc{0.0f};  // stars.x_eq ...  : +x RA 0h, +z north celestial pole (Earth's axis)
    float dist_ly = 0.0f;           // stars.dist_ly (kept separately: the UI shows ly, and length() is not free)
};

// Identity and classification: mostly for UI text, plus spectral_class for
// the "spectral class" colour scheme.
struct StarInfo {
    std::string display_name;                  // stars.display_name (never empty)
    std::optional<std::string> bayer_name;     // 'Epsilon Eridani', 'Omicron^2 Eridani'
    std::optional<std::string> spectral_type;  // 'G2V', 'M5.5 V', ...
    char spectral_class = '?';                 // O B A F G K M L T Y D C S W, '?' if unknown (first letter only)
    bool is_sol = false;
};

// Brightness and colour indices as observed.
// Magnitudes are logarithmic and "backwards": smaller = brighter, and 5 mag = 100x flux.
struct Photometry {
    std::optional<float> app_mag;  // apparent: as seen from Earth. V if known, else Gaia G (see Provenance::app_mag_band)
    std::optional<float> abs_mag;  // absolute: as seen from 10 pc, i.e. intrinsic brightness (Sol: +4.83)
    std::optional<float> bp_rp;    // Gaia colour (blue minus red photometer), bigger = redder
    std::optional<float> ci_bv;    // B-V colour (HYG), bigger = redder
};

// The physical quantities the colour schemes map to colours.
struct Physical {
    std::optional<float> teff_k;          // effective surface temperature, kelvin (Sol: 5772)  -> Temperature scheme
    std::optional<float> radius_sol;      // radius in solar radii                                -> Diameter scheme, billboard size
    std::optional<float> mass_sol;        // mass in solar masses
    std::optional<float> luminosity_sol;  // total power output in solar luminosities            -> billboard size
    std::optional<float> age_gyr;         // age in billions of years (often very uncertain)     -> Age scheme
    std::optional<float> metallicity;     // [Fe/H] or [M/H], dex: log10 of iron/hydrogen relative to Sol -> Metallicity scheme
    int planet_count = 0;                 // known exoplanets (0 = none known; never NULL)      -> Planets scheme
};

// Cross-identifications with the big catalogs.
struct CatalogIds {
    std::optional<std::int64_t> gaia_source_id;  // 19 digits: must stay int64, never float/double (a double only holds ~15.9 digits exactly)
    std::optional<std::int64_t> hip;             // Hipparcos
    std::optional<std::int64_t> hd;              // Henry Draper
    std::optional<std::int64_t> hyg_id;          // HYG database row
};

// Astrometric quality, for filtering out spurious Gaia sources.
struct Quality {
    std::optional<float> parallax_mas;        // milliarcseconds; distance_pc = 1000 / parallax_mas
    std::optional<float> parallax_error_mas;
    std::optional<float> ruwe;  // > 1.4 often means an unresolved binary / bad astrometry

    // Signal-to-noise of the parallax. SNR 5 means roughly 20% distance error.
    // A member function on a component is fine as long as it is a pure,
    // derived computation. The ECS rule of thumb is "no behaviour that
    // touches other entities or systems".
    [[nodiscard]] std::optional<float> parallax_snr() const {
        if (parallax_mas && parallax_error_mas && *parallax_error_mas > 0.0f)
            return *parallax_mas / *parallax_error_mas;
        return std::nullopt;
    }
};

// Written by systems::update_colors (ColorSystem). The loader only emplaces
// the default grey, so the storage exists from the start and colour updates
// are plain writes rather than 92k component insertions.
//   rgb = linear-ish display colour in 0..1
//   a   = INTENSITY multiplier, not opacity. Blending is additive (see
//         StarRenderer), so "alpha" as transparency means nothing there.
//         The shader multiplies the glow by it. Values > 1 emphasise a star
//         (planet hosts), values < 1 fade it (no data).
struct DisplayColor {
    glm::vec4 rgba{0.5f, 0.5f, 0.5f, 1.0f};
};

// Tag component (empty struct): EnTT stores no data for it, only membership,
// and views can filter on it (registry.view<Position, Selected>()).
// Not added by the loader; a future UI will add or remove it on click.
struct Selected {};

// Only present when LoadOptions::load_provenance is true (keeps memory small
// otherwise). Where each value came from, for tooltips and debugging data issues.
struct Provenance {
    std::string dist_source;           // gaia_parallax | hyg | exoplanet_archive | sol
    std::string app_mag_band;          // 'V' | 'G'
    std::string spectral_type_source;
    std::string teff_source;
    std::string radius_source;
    std::string mass_source;
    std::string luminosity_source;
    std::string age_source;
    std::string metallicity_source;
    std::string sources;               // comma list of contributing catalogs
};

// One row of the `planets` table.
struct Planet {
    std::string name;                         // pl_name, e.g. 'eps Eri b'
    std::optional<int> disc_year;
    std::string discovery_method;             // e.g. 'Radial Velocity'
    std::optional<float> orbital_period_days; // pl_orbper_days
    std::optional<float> semi_major_axis_au;  // pl_orbsmax_au
    std::optional<float> eccentricity;        // pl_orbeccen
    std::optional<float> radius_earth;        // pl_rade
    std::optional<float> mass_earth;          // pl_bmasse (mass or M sin i)
    std::optional<float> eq_temp_k;           // pl_eqt_k
};

// Only present when LoadOptions::load_planets is true, and only on stars with
// planets. Optional components are an ECS strength: "stars with planets" is
// just registry.view<Planets>(), with no bool flag to test on every star.
// (The Planets colour scheme uses Physical::planet_count instead, which
// exists on every star and needs no planet table loaded.)
struct Planets {
    std::vector<Planet> list;  // innermost first (ordered by orbital period in the loader)
};

}  // namespace starmap::ecs
