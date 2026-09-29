// =============================================================================
// CatalogLoader.cpp — reads stars.db and builds the ECS world
// =============================================================================
//
// Structure of load_catalog():
//   0. validate the file and open it read-only inside one transaction
//   1. COUNT(*) with the same filters, then reserve() every component storage
//   2. stream the `stars` rows: one entity per row, ~8 components each
//   3. stream `star_names` into NameIndex (ctx)
//   4. optionally stream `planets` into a Planets component on host stars
//   5. store StarIndex in ctx, report counts and timing
//
// The SQL is kept in this file, in plain sight, on purpose. When the
// database schema changes (a Python-side change), this file and
// ecs/Components.h are the only C++ that needs to follow.
// =============================================================================
#include "catalog/CatalogLoader.h"

#include "catalog/Indexes.h"
#include "catalog/Sqlite.h"
#include "ecs/Components.h"

#include <entt/entity/registry.hpp>

#include <array>
#include <chrono>
#include <string>
#include <string_view>

namespace starmap::catalog {
// An unnamed namespace gives everything inside it internal linkage: the
// helpers are private to this .cpp and cannot clash with same-named
// functions elsewhere at link time. It is the modern replacement for `static`
// free functions.
namespace {

using namespace starmap::ecs;

// Columns read from `stars`, in SELECT order. col("teff_k") turns a name into
// its index at compile time, so the SQL and the reading code cannot drift apart
// (a typo in a name is a compile error).
constexpr std::array<std::string_view, 41> kStarColumns = {
    "star_id", "display_name", "bayer_name", "spectral_type", "spectral_class", "is_sol",
    "x", "y", "z", "x_eq", "y_eq", "z_eq", "dist_ly",
    "app_mag", "abs_mag", "bp_rp", "ci_bv",
    "teff_k", "radius_sol", "mass_sol", "luminosity_sol", "age_gyr", "metallicity", "planet_count",
    "gaia_source_id", "hip", "hd", "hyg_id",
    "parallax_mas", "parallax_error_mas", "ruwe",
    "dist_source", "app_mag_band", "spectral_type_source", "teff_source", "radius_source",
    "mass_source", "luminosity_source", "age_source", "metallicity_source", "sources",
};

// `consteval` (C++20) means the function MUST be evaluated at compile
// time; any call that can't be is an error. Inside, `throw` is not allowed in
// a constant expression, so reaching it (an unknown name) turns into a
// compile error that points at the offending col("...") call. At runtime
// col("teff_k") is just the integer 17, with zero lookup cost in the hot loop.
consteval int col(std::string_view name) {
    for (std::size_t i = 0; i < kStarColumns.size(); ++i) {
        if (kStarColumns[i] == name) return static_cast<int>(i);
    }
    throw "unknown column";  // not a constant expression -> compile error
}

// Builds the SELECT for `stars`. The COUNT(*) variant shares the exact same
// WHERE clause, so the pre-count and the real query can never disagree about
// which rows qualify.
std::string star_select_sql(bool count_only) {
    std::string sql = "SELECT ";
    if (count_only) {
        sql += "COUNT(*)";
    } else {
        for (std::size_t i = 0; i < kStarColumns.size(); ++i) {
            if (i) sql += ", ";
            sql += kStarColumns[i];
        }
    }
    // Quality filters apply only to rows whose distance came from a Gaia parallax,
    // so HYG-only stars, exoplanet-archive distances and Sol are never dropped.
    // The ":x IS NULL OR ..." pattern makes each filter optional with ONE
    // prepared statement: binding NULL switches the filter off. The
    // alternative, building different SQL strings per option combination,
    // multiplies code paths.
    // "parallax_error_mas <= 0" guards the division against bad rows.
    // R"(...)" is a raw string literal: newlines and quotes need no escaping.
    sql += R"(
 FROM stars
 WHERE dist_ly <= :max_ly
   AND (:min_snr IS NULL OR dist_source <> 'gaia_parallax'
        OR parallax_mas IS NULL OR parallax_error_mas IS NULL OR parallax_error_mas <= 0
        OR parallax_mas / parallax_error_mas >= :min_snr)
   AND (:max_ruwe IS NULL OR dist_source <> 'gaia_parallax' OR ruwe IS NULL OR ruwe <= :max_ruwe))";
    // ORDER BY star_id makes entity creation order deterministic (Sol, star_id 1,
    // is created first) and matches the order in which names are read later.
    if (!count_only) sql += "\n ORDER BY star_id";
    return sql;
}

// Named parameters (":max_ly") instead of ?1/?2/?3: the SQL is readable and
// immune to reordering. parameter_index() throws on a typo. The
// std::optional overload of bind() turns nullopt into NULL.
void bind_filters(Statement& st, const LoadOptions& o) {
    st.bind(st.parameter_index(":max_ly"), o.max_dist_ly);
    st.bind(st.parameter_index(":min_snr"), o.min_parallax_snr);
    st.bind(st.parameter_index(":max_ruwe"), o.max_ruwe);
}

// Pitfall on Windows: path::string() converts to the ANSI code page and
// mangles non-ASCII user names (C:\Users\Zoë\...). u8string() is always UTF-8,
// which is what sqlite3_open_v2 expects. In C++20 it returns std::u8string
// (char8_t), hence the byte-wise copy into a std::string.
std::string path_to_utf8(const std::filesystem::path& p) {
    const std::u8string s = p.u8string();  // SQLite wants UTF-8 file names on every platform
    return {s.begin(), s.end()};
}

// For TEXT columns where "missing" and "empty" mean the same thing to the app.
std::string text_or_empty(const Statement& st, int c) { return std::string(st.column_text_view(c)); }

std::optional<int> opt_int(const Statement& st, int c) {
    if (auto v = st.column_int64(c)) return static_cast<int>(*v);
    return std::nullopt;
}

}  // namespace

LoadResult load_catalog(entt::registry& registry, const LoadOptions& options) {
    const auto t0 = std::chrono::steady_clock::now();
    LoadResult result;

    const std::string path = path_to_utf8(options.db_path);
    // Check existence ourselves first. A read-only open of a missing file
    // fails anyway, but "star catalog not found: <path>" is a friendlier
    // message than SQLite's "unable to open database file". The error_code
    // overload doesn't throw (e.g. on a permission error while probing).
    std::error_code ec;
    if (!std::filesystem::is_regular_file(options.db_path, ec)) {
        throw CatalogError("star catalog not found: " + path);
    }
    // Declaration order matters: db, then tx, then the Statements. Locals are
    // destroyed in reverse, so every Statement is finalized and the
    // transaction closed before the connection closes.
    Database db(path, OpenMode::ReadOnly);
    if (!db.has_table("stars") || !db.has_table("star_names")) {
        throw CatalogError("not a star catalog (missing stars/star_names tables): " + path);
    }
    // One read transaction = one consistent snapshot and a single shared lock.
    Transaction tx(db);

    // ---- 1. count first so every storage can be sized exactly once -------------
    Statement count(db, star_select_sql(true));
    bind_filters(count, options);
    count.step();  // COUNT(*) always yields exactly one row
    const auto n = static_cast<std::size_t>(count.column_int64(0).value_or(0));

    // EnTT idiom: each component type lives in its own storage (a sparse set +
    // packed array). reserve() pre-allocates the packed arrays so the loop below
    // never reallocates. storage<entt::entity>() is the entity pool itself.
    // Why bother? 92k emplace calls into growing vectors would reallocate
    // (and move every component) about 17 times per storage. Reserving is
    // cheap and makes load time predictable.
    registry.storage<entt::entity>().reserve(n);
    registry.storage<StarId>().reserve(n);
    registry.storage<Position>().reserve(n);
    registry.storage<StarInfo>().reserve(n);
    registry.storage<Photometry>().reserve(n);
    registry.storage<Physical>().reserve(n);
    registry.storage<CatalogIds>().reserve(n);
    registry.storage<Quality>().reserve(n);
    registry.storage<DisplayColor>().reserve(n);
    if (options.load_provenance) registry.storage<Provenance>().reserve(n);

    StarIndex star_index;
    star_index.reserve(n);

    // ---- 2. stars -------------------------------------------------------------
    Statement st(db, star_select_sql(false));
    bind_filters(st, options);
    while (st.step()) {
        // create() returns a fresh entity ID with no components yet.
        const entt::entity e = registry.create();
        const std::int64_t star_id = st.column_int64(col("star_id")).value_or(0);
        star_index.add(star_id, e);

        // emplace<T>(e, args...) constructs the component in place in T's storage.
        // For aggregates (plain structs without constructors, like all our
        // components), EnTT uses brace-initialisation T{args...}, so the
        // arguments must follow the member declaration order in Components.h.
        // Pitfall: adding a member in the middle of a component silently
        // shifts the meaning of these arguments if the types happen to match.
        registry.emplace<StarId>(e, star_id);

        // x/y/z are NOT NULL in the schema, so value_or(0) is just a formality.
        // Captured by reference: the lambda is only used within this iteration.
        auto f = [&st](int c) { return st.column_float(c).value_or(0.0f); };  // NOT NULL columns
        registry.emplace<Position>(e,
            glm::vec3{f(col("x")), f(col("y")), f(col("z"))},
            glm::vec3{f(col("x_eq")), f(col("y_eq")), f(col("z_eq"))},
            f(col("dist_ly")));

        // column_text_view is valid until the next step(). Using .front()
        // immediately (copying one char) is safe.
        const std::string_view cls = st.column_text_view(col("spectral_class"));
        registry.emplace<StarInfo>(e,
            text_or_empty(st, col("display_name")),
            st.column_text(col("bayer_name")),
            st.column_text(col("spectral_type")),
            cls.empty() ? '?' : cls.front(),
            st.column_int64(col("is_sol")).value_or(0) != 0);

        registry.emplace<Photometry>(e,
            st.column_float(col("app_mag")), st.column_float(col("abs_mag")),
            st.column_float(col("bp_rp")), st.column_float(col("ci_bv")));

        registry.emplace<Physical>(e,
            st.column_float(col("teff_k")), st.column_float(col("radius_sol")),
            st.column_float(col("mass_sol")), st.column_float(col("luminosity_sol")),
            st.column_float(col("age_gyr")), st.column_float(col("metallicity")),
            opt_int(st, col("planet_count")).value_or(0));

        registry.emplace<CatalogIds>(e,
            st.column_int64(col("gaia_source_id")), st.column_int64(col("hip")),
            st.column_int64(col("hd")), st.column_int64(col("hyg_id")));

        registry.emplace<Quality>(e,
            st.column_float(col("parallax_mas")), st.column_float(col("parallax_error_mas")),
            st.column_float(col("ruwe")));

        // Every star gets a DisplayColor slot now, even though ColorSystem
        // fills it later. Adding a component to 92k entities at runtime would
        // mean 92k insertions into the storage. Writing into an existing one
        // is a plain store.
        registry.emplace<DisplayColor>(e);  // default grey until the colour system runs

        if (options.load_provenance) {
            registry.emplace<Provenance>(e,
                text_or_empty(st, col("dist_source")), text_or_empty(st, col("app_mag_band")),
                text_or_empty(st, col("spectral_type_source")), text_or_empty(st, col("teff_source")),
                text_or_empty(st, col("radius_source")), text_or_empty(st, col("mass_source")),
                text_or_empty(st, col("luminosity_source")), text_or_empty(st, col("age_source")),
                text_or_empty(st, col("metallicity_source")), text_or_empty(st, col("sources")));
        }
        ++result.stars;
    }

    // ---- 3. names -------------------------------------------------------------
    if (options.load_names) {
        std::size_t total_names = 0;
        {
            Statement c(db, "SELECT COUNT(*) FROM star_names");
            c.step();
            total_names = static_cast<std::size_t>(c.column_int64(0).value_or(0));
        }
        NameIndex names;
        names.reserve(total_names, result.stars);
        // ORDER BY star_id keeps each star's names contiguous. (star_id, name_id) is
        // exactly the order of idx_star_names_star, so SQLite needs no sort step;
        // NameIndex itself moves the primary name to the front of each star's range.
        Statement ns(db,
            "SELECT star_id, name, catalog, is_primary FROM star_names ORDER BY star_id, name_id");
        while (ns.step()) {
            const auto e = star_index.find(ns.column_int64(0).value_or(0));
            if (!e) {  // star filtered out by distance / quality
                ++result.skipped_rows;
                continue;
            }
            if (!names.add(*e, text_or_empty(ns, 1), text_or_empty(ns, 2),
                           ns.column_int64(3).value_or(0) != 0)) {
                ++result.duplicate_names;
            }
        }
        result.names = names.name_count();
        // std::move: NameIndex holds ~117k strings. Moving hands over the
        // buffers (a few pointer swaps) instead of copying every string.
        // insert_or_assign replaces a NameIndex left over from a previous load;
        // ctx().emplace<T>() would keep the old one.
        registry.ctx().insert_or_assign(std::move(names));
    }

    // ---- 4. planets (optional) --------------------------------------------------
    // Ordering puts each star's planets innermost-first (by orbital period),
    // with unknown periods last ("IS NULL" sorts false=0 before true=1).
    if (options.load_planets && db.has_table("planets")) {
        Statement ps(db,
            "SELECT star_id, pl_name, disc_year, discoverymethod, pl_orbper_days, pl_orbsmax_au,"
            " pl_orbeccen, pl_rade, pl_bmasse, pl_eqt_k"
            " FROM planets ORDER BY star_id, pl_orbper_days IS NULL, pl_orbper_days, pl_name");
        while (ps.step()) {
            const auto e = star_index.find(ps.column_int64(0).value_or(0));
            if (!e) {
                ++result.skipped_rows;
                continue;
            }
            // get_or_emplace: add the component on the first planet of a star, reuse it afterwards.
            registry.get_or_emplace<Planets>(*e).list.push_back(Planet{
                text_or_empty(ps, 1), opt_int(ps, 2), text_or_empty(ps, 3),
                ps.column_float(4), ps.column_float(5), ps.column_float(6),
                ps.column_float(7), ps.column_float(8), ps.column_float(9)});
            ++result.planets;
        }
    }

    // Nothing was written, so COMMIT versus ROLLBACK makes no difference to
    // the data. Committing explicitly just ends the read transaction cleanly
    // and releases the shared lock.
    tx.commit();
    registry.ctx().insert_or_assign(std::move(star_index));

    // steady_clock, not system_clock: it is monotonic, so it can't jump if
    // the wall clock is adjusted mid-load. duration<double, milli> gives
    // fractional milliseconds.
    result.elapsed_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return result;
}

}  // namespace starmap::catalog
