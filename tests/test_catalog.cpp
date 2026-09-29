// Integration tests against the real stars.db.
// DB path: env STARMAP_DB, else the STARMAP_TEST_DB compile definition (CMake cache var).
// If the file is missing the tests print a message and pass (skip).
#include "catalog/CatalogLoader.h"
#include "catalog/CatalogStats.h"
#include "catalog/Indexes.h"
#include "catalog/Sqlite.h"
#include "ecs/Components.h"

#include <doctest/doctest.h>
#include <entt/entity/registry.hpp>
#include <glm/geometric.hpp>

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>

using namespace starmap;
using namespace starmap::catalog;

#ifndef STARMAP_TEST_DB
#define STARMAP_TEST_DB ""
#endif

namespace {

std::filesystem::path db_path() {
    if (const char* env = std::getenv("STARMAP_DB"); env && *env) return env;
    return STARMAP_TEST_DB;
}

bool have_db() {
    std::error_code ec;
    return std::filesystem::is_regular_file(db_path(), ec);
}

#define SKIP_WITHOUT_DB()                                                              \
    if (!have_db()) {                                                                  \
        MESSAGE("stars.db not found at '" << db_path().string()                        \
                << "' - set STARMAP_DB to run the catalog tests; skipping");           \
        return;                                                                        \
    }

struct Loaded {
    entt::registry registry;
    LoadResult result;
};

// The full catalog is loaded once and shared by the read-only tests below.
const Loaded& full_catalog() {
    static const std::unique_ptr<Loaded> loaded = [] {
        auto l = std::make_unique<Loaded>();
        LoadOptions opt;
        opt.db_path = db_path();
        opt.load_planets = true;
        l->result = load_catalog(l->registry, opt);
        return l;
    }();
    return *loaded;
}

std::int64_t count_rows(const std::string& sql) {
    Database db(db_path().string());
    Statement st(db, sql);
    st.step();
    return st.column_int64(0).value_or(-1);
}

entt::entity must_find(const entt::registry& reg, std::string_view name) {
    const auto hit = reg.ctx().get<NameIndex>().find(name);
    REQUIRE_MESSAGE(hit.has_value(), "name not found: " << name);
    return *hit;
}

}  // namespace

TEST_CASE("catalog: every stars row becomes exactly one entity") {
    SKIP_WITHOUT_DB();
    const auto& [reg, res] = full_catalog();
    const auto rows = count_rows("SELECT COUNT(*) FROM stars WHERE dist_ly <= 200");
    CHECK(static_cast<std::int64_t>(res.stars) == rows);
    CHECK(reg.view<ecs::StarId>().size() == res.stars);
    CHECK(reg.ctx().get<StarIndex>().size() == res.stars);
    CHECK(static_cast<std::int64_t>(res.names + res.duplicate_names) ==
          count_rows("SELECT COUNT(*) FROM star_names"));
    CHECK(static_cast<std::int64_t>(res.names) ==
          count_rows("SELECT COUNT(*) FROM (SELECT DISTINCT star_id, name, catalog FROM star_names)"));
    CHECK(res.planets > 1000);
    MESSAGE("loaded " << res.stars << " stars, " << res.names << " names, " << res.planets
                      << " planets in " << res.elapsed_ms << " ms");

    // star_ids are unique and StarIndex maps back to the same entity.
    const auto& index = reg.ctx().get<StarIndex>();
    std::size_t mismatches = 0;
    for (auto [e, id] : reg.view<const ecs::StarId>().each()) {
        if (index.find(id.value) != e) ++mismatches;
    }
    CHECK(mismatches == 0);
}

TEST_CASE("catalog: each entity has exactly one primary name, equal to display_name") {
    SKIP_WITHOUT_DB();
    const auto& reg = full_catalog().registry;
    const auto& names = reg.ctx().get<NameIndex>();
    std::size_t bad_count = 0, bad_display = 0;
    for (auto [e, info] : reg.view<const ecs::StarInfo>().each()) {
        int primaries = 0;
        for (const auto& n : names.names_of(e)) primaries += n.is_primary ? 1 : 0;
        if (primaries != 1) ++bad_count;
        const auto* p = names.primary_of(e);
        if (!p || p->name != info.display_name) ++bad_display;
    }
    CHECK(bad_count == 0);
    CHECK(bad_display == 0);
    CHECK(names.star_count() == full_catalog().result.stars);
}

TEST_CASE("catalog: Sol at the origin with 8 planets") {
    SKIP_WITHOUT_DB();
    const auto& reg = full_catalog().registry;
    const auto sol = must_find(reg, "Sol");
    CHECK(must_find(reg, "sun") == sol);
    CHECK(reg.ctx().get<NameIndex>().names_of(sol).size() == 2);  // 'Sol', 'Sun' (db duplicate dropped)
    const auto& [info, pos, phys] = reg.get<ecs::StarInfo, ecs::Position, ecs::Physical>(sol);
    CHECK(info.is_sol);
    CHECK(info.spectral_class == 'G');
    CHECK(glm::length(pos.galactic_pc) == doctest::Approx(0.0f));
    CHECK(glm::length(pos.equatorial_pc) == doctest::Approx(0.0f));
    CHECK(pos.dist_ly == doctest::Approx(0.0f));
    CHECK(phys.planet_count == 8);
    // The planets table holds exoplanets only (NASA Exoplanet Archive); Sol's
    // eight planets are just a count, so Sol has no Planets component.
    CHECK_FALSE(reg.all_of<ecs::Planets>(sol));
    CHECK(reg.all_of<ecs::DisplayColor>(sol));
    CHECK_FALSE(reg.all_of<ecs::Selected>(sol));
    CHECK_FALSE(reg.all_of<ecs::Provenance>(sol));  // not requested
}

TEST_CASE("catalog: Planets component matches planet_count for exoplanet hosts") {
    SKIP_WITHOUT_DB();
    const auto& reg = full_catalog().registry;
    std::size_t hosts = 0, mismatches = 0;
    // A view over two components visits only entities having both.
    for (auto [e, phys, planets] : reg.view<const ecs::Physical, const ecs::Planets>().each()) {
        ++hosts;
        if (static_cast<std::size_t>(phys.planet_count) != planets.list.size()) ++mismatches;
    }
    CHECK(hosts > 100);
    CHECK(mismatches == 0);
    const auto prox = must_find(reg, "Proxima Centauri");
    REQUIRE(reg.all_of<ecs::Planets>(prox));
    CHECK(reg.get<ecs::Planets>(prox).list.size() == 2);
}

TEST_CASE("catalog: Epsilon Eridani by several names, case-insensitive") {
    SKIP_WITHOUT_DB();
    const auto& reg = full_catalog().registry;
    const auto eps = must_find(reg, "Epsilon Eridani");
    CHECK(must_find(reg, "epsilon eridani") == eps);
    CHECK(must_find(reg, "EPSILON ERIDANI") == eps);
    CHECK(must_find(reg, "Ran") == eps);
    CHECK(must_find(reg, "ran") == eps);
    CHECK(must_find(reg, "HIP 16537") == eps);
    CHECK(must_find(reg, "hip 16537") == eps);
    CHECK(must_find(reg, "HD 22049") == eps);
    const auto& info = reg.get<ecs::StarInfo>(eps);
    REQUIRE(info.bayer_name.has_value());
    CHECK(*info.bayer_name == "Epsilon Eridani");
    CHECK(info.display_name == "Ran");
    CHECK(reg.get<ecs::Position>(eps).dist_ly == doctest::Approx(10.5f).epsilon(0.01));
    CHECK(reg.get<ecs::Physical>(eps).planet_count >= 1);
}

TEST_CASE("catalog: Omicron^2 Eridani (40 Eri / Keid) at ~16.3 ly") {
    SKIP_WITHOUT_DB();
    const auto& reg = full_catalog().registry;
    const auto o2 = must_find(reg, "Omicron^2 Eridani");
    CHECK(must_find(reg, "omicron^2 eridani") == o2);
    CHECK(must_find(reg, "Omicron² Eridani") == o2);  // UTF-8 stored form
    CHECK(must_find(reg, "OMICRON² ERIDANI") == o2);  // ASCII letters fold, '²' passes through
    CHECK(must_find(reg, "40 Eridani") == o2);
    CHECK(must_find(reg, "Keid") == o2);
    CHECK(reg.get<ecs::Position>(o2).dist_ly == doctest::Approx(16.3f).epsilon(0.01));
}

TEST_CASE("catalog: names that should not resolve") {
    SKIP_WITHOUT_DB();
    const auto& names = full_catalog().registry.ctx().get<NameIndex>();
    CHECK_FALSE(names.find("Not A Star").has_value());
    CHECK_FALSE(names.find("").has_value());
    CHECK_FALSE(names.find("Epsilon  Eridani").has_value());  // exact match only (no whitespace folding)
}

TEST_CASE("catalog: min_parallax_snr=20 drops noisy Gaia rows but keeps Sirius and Sol") {
    SKIP_WITHOUT_DB();
    entt::registry reg;
    LoadOptions opt;
    opt.db_path = db_path();
    opt.min_parallax_snr = 20.0;
    const auto res = load_catalog(reg, opt);
    CHECK(res.stars < full_catalog().result.stars);
    CHECK(res.stars > 0);
    CHECK(res.skipped_rows > 0);  // names of the dropped stars
    const auto& names = reg.ctx().get<NameIndex>();
    CHECK(names.find("Sirius").has_value());
    CHECK(names.find("Sol").has_value());
    CHECK(names.find("Proxima Centauri").has_value());

    // Every remaining Gaia-parallax star really has SNR >= 20.
    std::size_t violations = 0;
    for (auto [e, q] : reg.view<const ecs::Quality>().each()) {
        if (auto snr = q.parallax_snr(); snr && *snr < 20.0f) ++violations;
    }
    CHECK(violations == 0);
    MESSAGE("min_parallax_snr=20: " << res.stars << " of " << full_catalog().result.stars << " stars");
}

TEST_CASE("catalog: max_dist_ly=20 gives a small plausible neighbourhood") {
    SKIP_WITHOUT_DB();
    entt::registry reg;
    LoadOptions opt;
    opt.db_path = db_path();
    opt.max_dist_ly = 20.0;
    opt.load_provenance = true;
    const auto res = load_catalog(reg, opt);
    // ~130 known star systems / ~150 stars+brown dwarfs lie within 20 ly.
    CHECK(res.stars >= 80);
    CHECK(res.stars <= 250);
    for (auto [e, pos] : reg.view<const ecs::Position>().each()) {
        CHECK(pos.dist_ly <= 20.0f);
        // dist_ly must agree with the Cartesian position (1 pc = 3.26156 ly).
        CHECK(glm::length(pos.galactic_pc) * 3.26156f == doctest::Approx(pos.dist_ly).epsilon(0.001));
    }
    const auto& names = reg.ctx().get<NameIndex>();
    CHECK(names.find("Sol").has_value());
    CHECK(names.find("Proxima Centauri").has_value());
    CHECK(names.find("Epsilon Eridani").has_value());
    CHECK(names.find("Omicron^2 Eridani").has_value());
    CHECK_FALSE(names.find("Gamma Pavonis").has_value());  // ~30 ly
    CHECK(reg.view<ecs::Provenance>().size() == res.stars);
    CHECK(reg.get<ecs::Provenance>(*names.find("Sol")).dist_source == "sol");
}

TEST_CASE("catalog: load_names=false leaves no NameIndex; StarIndex is always present") {
    SKIP_WITHOUT_DB();
    entt::registry reg;
    LoadOptions opt;
    opt.db_path = db_path();
    opt.max_dist_ly = 50.0;
    opt.load_names = false;
    const auto res = load_catalog(reg, opt);
    CHECK(res.names == 0);
    CHECK_FALSE(reg.ctx().contains<NameIndex>());
    REQUIRE(reg.ctx().contains<StarIndex>());
    CHECK(reg.ctx().get<StarIndex>().find(1).has_value());  // Sol is star_id 1
}

TEST_CASE("catalog: CatalogStats coverage and ranges") {
    SKIP_WITHOUT_DB();
    const auto& reg = full_catalog().registry;
    const auto stats = CatalogStats::compute(reg);
    CHECK(stats.total_stars == full_catalog().result.stars);
    const auto* dist = stats.field("dist_ly");
    REQUIRE(dist);
    CHECK(dist->count == stats.total_stars);
    CHECK(dist->min == doctest::Approx(0.0));
    CHECK(dist->max <= 200.0);
    const auto* teff = stats.field("teff_k");
    REQUIRE(teff);
    CHECK(teff->count > 0);
    CHECK(teff->count < stats.total_stars);
    CHECK(teff->min > 100.0);
    CHECK(teff->max < 100000.0);
    CHECK(teff->p01 <= teff->p99);
    CHECK(stats.spectral_classes.count('G') == 1);
    CHECK(stats.with_planets > 100);
    CHECK(stats.field("nope") == nullptr);
}

TEST_CASE("catalog: clear errors for bad input") {
    entt::registry reg;
    LoadOptions opt;
    opt.db_path = std::filesystem::temp_directory_path() / "starmap_no_such_file.db";
    CHECK_THROWS_AS(load_catalog(reg, opt), CatalogError);

    const auto empty = std::filesystem::temp_directory_path() / "starmap_empty_test.db";
    std::filesystem::remove(empty);
    { Database db(empty.string(), OpenMode::ReadWriteCreate); db.exec("CREATE TABLE x (a)"); }
    opt.db_path = empty;
    CHECK_THROWS_WITH_AS(load_catalog(reg, opt), doctest::Contains("not a star catalog"), CatalogError);
    std::filesystem::remove(empty);
}
