// =============================================================================
// catalog/StarDetails.cpp — see StarDetails.h for why this is loaded lazily
// =============================================================================
#include "catalog/StarDetails.h"

namespace starmap::catalog {

namespace {
// Column order of the SELECT below. Naming the indices (instead of sprinkling
// magic numbers through the reads) keeps the SQL and the C++ in step: add a
// column in both places or the static_assert-free code reads garbage.
enum Col : int {
    kDisplayName, kBayer, kGaia, kHip, kHd, kHyg, kDistPc, kDistLy, kDistSource, kRa, kDec,
    kPlx, kPlxErr, kRuwe, kAppMag, kBand, kAbsMag, kVmag, kGaiaG, kBpRp, kCiBv,
    kSpType, kSpSrc, kSpClass, kTeff, kTeffSrc, kRad, kRadSrc, kMass, kMassSrc, kLum, kLumSrc,
    kAge, kAgeSrc, kMet, kMetSrc, kPlanetCount, kSysPlanets, kIsSol, kSources
};

constexpr const char* kStarSql =
    "SELECT display_name, bayer_name, gaia_source_id, hip, hd, hyg_id, dist_pc, dist_ly, dist_source, ra, dec,"
    " parallax_mas, parallax_error_mas, ruwe, app_mag, app_mag_band, abs_mag, vmag, gaia_g_mag, bp_rp, ci_bv,"
    " spectral_type, spectral_type_source, spectral_class, teff_k, teff_source, radius_sol, radius_source,"
    " mass_sol, mass_source, luminosity_sol, luminosity_source, age_gyr, age_source, metallicity,"
    " metallicity_source, planet_count, system_planet_count, is_sol, sources"
    " FROM stars WHERE star_id = ?";

// ORDER BY with "IS NULL" first: planets without a known period sort last
// instead of first (SQLite sorts NULL before any number by default).
constexpr const char* kPlanetSql =
    "SELECT pl_name, disc_year, discoverymethod, pl_orbper_days, pl_orbsmax_au, pl_orbeccen, pl_rade,"
    " pl_bmasse, pl_eqt_k FROM planets WHERE star_id = ?"
    " ORDER BY pl_orbper_days IS NULL, pl_orbper_days, pl_name";

SourcedValue sourced(const Statement& st, int value_col, int source_col) {
    return {st.column_double(value_col), st.column_text(source_col).value_or("")};
}
}  // namespace

namespace {
// stars.db says Sol has planet_count = 8, but the planets table comes from the
// NASA Exoplanet Archive, which (by definition) lists no Solar System
// planets. Rather than show "8 planets" next to an empty table, the details
// for Sol get the eight planets from the NASA planetary fact sheet
// (https://nssdc.gsfc.nasa.gov/planetary/factsheet/), in the same units as
// the archive columns: days, AU, Earth masses, Earth radii.
std::vector<ecs::Planet> solar_system_planets() {
    struct Row {
        const char* name;
        float period_d, a_au, ecc, mass_e, radius_e;
        int year;  // 0 = known since antiquity
    };
    static constexpr Row kRows[] = {
        {"Mercury", 87.969f, 0.387f, 0.206f, 0.0553f, 0.383f, 0},
        {"Venus", 224.701f, 0.723f, 0.007f, 0.815f, 0.949f, 0},
        {"Earth", 365.256f, 1.000f, 0.017f, 1.0f, 1.0f, 0},
        {"Mars", 686.980f, 1.524f, 0.094f, 0.107f, 0.532f, 0},
        {"Jupiter", 4332.59f, 5.203f, 0.049f, 317.8f, 11.21f, 0},
        {"Saturn", 10759.22f, 9.537f, 0.057f, 95.2f, 9.45f, 0},
        {"Uranus", 30685.4f, 19.19f, 0.046f, 14.5f, 4.01f, 1781},
        {"Neptune", 60189.0f, 30.07f, 0.011f, 17.1f, 3.88f, 1846},
    };
    std::vector<ecs::Planet> out;
    for (const Row& r : kRows) {
        ecs::Planet p;
        p.name = r.name;
        p.orbital_period_days = r.period_d;
        p.semi_major_axis_au = r.a_au;
        p.eccentricity = r.ecc;
        p.mass_earth = r.mass_e;
        p.radius_earth = r.radius_e;
        p.discovery_method = r.year ? "Telescope" : "Naked eye";
        if (r.year) p.disc_year = r.year;
        out.push_back(std::move(p));
    }
    return out;
}
}  // namespace

StarDetails load_star_details(Database& db, std::int64_t star_id) {
    StarDetails d;
    d.star_id = star_id;
    {
        // A prepared statement per call: preparing costs a few microseconds,
        // negligible next to a human clicking. Caching the Statement would
        // save that but tie its lifetime to the Database (one more thing to
        // get wrong at shutdown).
        Statement st(db, kStarSql);
        st.bind(1, star_id);  // SQLite parameters are 1-based
        if (!st.step()) throw CatalogError("star_id " + std::to_string(star_id) + " not found in stars");
        d.display_name = st.column_text(kDisplayName).value_or("");
        d.bayer_name = st.column_text(kBayer);
        d.gaia_source_id = st.column_int64(kGaia);
        d.hip = st.column_int64(kHip);
        d.hd = st.column_int64(kHd);
        d.hyg_id = st.column_int64(kHyg);
        d.dist_pc = st.column_double(kDistPc).value_or(0.0);
        d.dist_ly = st.column_double(kDistLy).value_or(0.0);
        d.dist_source = st.column_text(kDistSource).value_or("");
        d.ra_deg = st.column_double(kRa);
        d.dec_deg = st.column_double(kDec);
        d.parallax_mas = st.column_double(kPlx);
        d.parallax_error_mas = st.column_double(kPlxErr);
        d.ruwe = st.column_double(kRuwe);
        d.app_mag = st.column_double(kAppMag);
        d.app_mag_band = st.column_text(kBand).value_or("");
        d.abs_mag = st.column_double(kAbsMag);
        d.vmag = st.column_double(kVmag);
        d.gaia_g_mag = st.column_double(kGaiaG);
        d.bp_rp = st.column_double(kBpRp);
        d.ci_bv = st.column_double(kCiBv);
        d.spectral_type = st.column_text(kSpType);
        d.spectral_type_source = st.column_text(kSpSrc).value_or("");
        d.spectral_class = st.column_text(kSpClass);
        d.teff_k = sourced(st, kTeff, kTeffSrc);
        d.radius_sol = sourced(st, kRad, kRadSrc);
        d.mass_sol = sourced(st, kMass, kMassSrc);
        d.luminosity_sol = sourced(st, kLum, kLumSrc);
        d.age_gyr = sourced(st, kAge, kAgeSrc);
        d.metallicity = sourced(st, kMet, kMetSrc);
        d.planet_count = static_cast<int>(st.column_int64(kPlanetCount).value_or(0));
        if (auto n = st.column_int64(kSysPlanets)) d.system_planet_count = static_cast<int>(*n);
        d.is_sol = st.column_int64(kIsSol).value_or(0) != 0;
        d.sources = st.column_text(kSources).value_or("");
    }
    if (db.has_table("planets")) {
        Statement st(db, kPlanetSql);
        st.bind(1, star_id);
        while (st.step()) {
            ecs::Planet p;
            p.name = st.column_text(0).value_or("");
            if (auto y = st.column_int64(1)) p.disc_year = static_cast<int>(*y);
            p.discovery_method = st.column_text(2).value_or("");
            p.orbital_period_days = st.column_float(3);
            p.semi_major_axis_au = st.column_float(4);
            p.eccentricity = st.column_float(5);
            p.radius_earth = st.column_float(6);
            p.mass_earth = st.column_float(7);
            p.eq_temp_k = st.column_float(8);
            d.planets.push_back(std::move(p));
        }
    }
    if (d.is_sol && d.planets.empty()) d.planets = solar_system_planets();
    return d;
}

}  // namespace starmap::catalog
