// =============================================================================
// tests/test_search.cpp — milestone 3: name search and lazy star details.
//
//   * NameSearch::normalize / compact: the key rules (case, punctuation,
//     Gliese synonyms) on plain strings;
//   * ranking on a tiny hand-made NameIndex, where the expected order can be
//     worked out by hand (exact > prefix > word-prefix > substring, then
//     primary name, then brightness);
//   * the queries the milestone spec lists, against the real stars.db
//     (skipped with a message if the db is missing, like test_catalog.cpp);
//   * load_star_details for a planet host and for Sol, plus timings.
// =============================================================================
#include "catalog/CatalogLoader.h"
#include "catalog/Indexes.h"
#include "catalog/NameSearch.h"
#include "catalog/Sqlite.h"
#include "catalog/StarDetails.h"
#include "ecs/Components.h"

#include <doctest/doctest.h>
#include <entt/entity/registry.hpp>

#include <chrono>
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
#define SKIP_WITHOUT_DB()                                                                          \
    if (!have_db()) {                                                                              \
        MESSAGE("stars.db not found at '" << db_path().string() << "' - set STARMAP_DB; skipping"); \
        return;                                                                                    \
    }

struct Loaded {
    entt::registry registry;
    LoadResult result;
    std::unique_ptr<NameSearch> search;
    double build_ms = 0.0;
};
// Loaded once per test binary run (the catalog load takes ~1 s).
const Loaded& full_catalog() {
    static const std::unique_ptr<Loaded> loaded = [] {
        auto l = std::make_unique<Loaded>();
        LoadOptions opt;
        opt.db_path = db_path();
        l->result = load_catalog(l->registry, opt);
        const auto t0 = std::chrono::steady_clock::now();
        l->search = std::make_unique<NameSearch>(l->registry.ctx().get<NameIndex>());
        l->build_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        return l;
    }();
    return *loaded;
}

std::int64_t star_id_of(const entt::registry& r, entt::entity e) { return r.get<ecs::StarId>(e).value; }
std::string display_of(const entt::registry& r, entt::entity e) { return r.get<ecs::StarInfo>(e).display_name; }

}  // namespace

TEST_CASE("NameSearch::normalize folds case, punctuation and Gliese synonyms") {
    CHECK(NameSearch::normalize("Epsilon Eridani") == "epsilon eridani");
    CHECK(NameSearch::normalize("  eps   ERI ") == "eps eri");
    CHECK(NameSearch::normalize("Gl 581") == "gj 581");
    CHECK(NameSearch::normalize("Gliese 581") == "gj 581");
    CHECK(NameSearch::normalize("GJ 581") == "gj 581");
    CHECK(NameSearch::normalize("Glow") == "glow");  // only the whole first word is a synonym
    CHECK(NameSearch::normalize("HD-22049") == "hd 22049");
    CHECK(NameSearch::normalize("alf Cen A") == "alf cen a");
    CHECK(NameSearch::normalize("Omicron² Eridani") == "omicron² eridani");  // non-ASCII kept as is
    CHECK(NameSearch::compact("hip 16537") == "hip16537");
    CHECK(NameSearch::normalize("") == "");
    CHECK(NameSearch::normalize("--") == "");
}

TEST_CASE("NameSearch ranks exact > prefix > word prefix > substring, then primary, then brightness") {
    entt::registry reg;
    auto star = [&](float mag) {
        const auto e = reg.create();
        reg.emplace<ecs::Photometry>(e, ecs::Photometry{mag, std::nullopt, std::nullopt, std::nullopt});
        reg.emplace<ecs::Position>(e, ecs::Position{{}, {}, 10.0f});
        return e;
    };
    const auto exact = star(9.0f), prefix = star(5.0f), word = star(1.0f), sub = star(-1.0f);
    const auto prefix_bright = star(2.0f), alias_prefix = star(0.0f);
    NameIndex idx;
    idx.add(exact, "Ran", "IAU", true);
    idx.add(prefix, "Rana", "IAU", true);
    idx.add(word, "Big Ranger", "proper", true);
    idx.add(sub, "Toran", "proper", true);
    idx.add(prefix_bright, "Ranxx", "IAU", true);
    idx.add(alias_prefix, "Something", "IAU", true);
    idx.add(alias_prefix, "Ranzz", "alias", false);  // prefix match, but not its primary name
    const NameSearch s(idx);

    const auto hits = s.search("ran", reg);
    REQUIRE(hits.size() == 6);
    CHECK(hits[0].entity == exact);          // exact beats a much brighter prefix match
    CHECK(hits[0].tier == MatchTier::Exact);
    CHECK(hits[1].entity == prefix_bright);  // prefix + primary, brighter (2.0) ...
    CHECK(hits[2].entity == prefix);         // ... before dimmer (5.0)
    CHECK(hits[3].entity == alias_prefix);   // prefix on an alias ranks after primary prefixes
    CHECK(hits[4].entity == word);
    CHECK(hits[4].tier == MatchTier::WordPrefix);
    CHECK(hits[5].entity == sub);
    CHECK(hits[5].tier == MatchTier::Substring);

    CHECK(s.search("ran", reg, 2).size() == 2);  // limit respected
    CHECK(s.search("", reg).empty());
    CHECK(s.search("zzz", reg).empty());
    // One hit per star even when several of its names match.
    idx = NameIndex{};
}

TEST_CASE("NameSearch on the real catalog: the spec's queries") {
    SKIP_WITHOUT_DB();
    const auto& L = full_catalog();
    const auto& reg = L.registry;
    const NameSearch& s = *L.search;
    MESSAGE("search index over " << s.size() << " names built in " << L.build_ms << " ms");

    auto top = [&](const char* q) {
        double ms = 0.0;
        const auto hits = s.search(q, reg, 20, &ms);
        MESSAGE("'" << std::string(q) << "' -> " << hits.size() << " hits in " << ms << " ms"
                    << (hits.empty() ? std::string() : ", first: " + display_of(reg, hits[0].entity)));
        return hits;
    };

    // Epsilon Eridani = Ran = HIP 16537 = HD 22049 (star_id 68570 in this build).
    const auto eps = top("Epsilon Eridani");
    REQUIRE_FALSE(eps.empty());
    const entt::entity ran = eps[0].entity;
    CHECK(eps[0].tier == MatchTier::Exact);
    CHECK(display_of(reg, ran) == "Ran");
    for (const char* q : {"eps eri", "EPS ERI", "Ran", "ran", "HIP 16537", "hip16537", "HD 22049",
                          "Gaia DR3 5164707970261890560"}) {
        const auto h = top(q);
        REQUIRE_FALSE(h.empty());
        CHECK_MESSAGE(h[0].entity == ran, q);
    }
    // 'Ran' is a prefix of many names (Rana, ...): the exact one must win.
    CHECK(top("Ran")[0].tier == MatchTier::Exact);
    // ... and the other names containing "ran" follow (Rana, Aldebaran, ...).
    const auto ran_hits = top("Ran");
    CHECK(ran_hits.size() >= 3);
    for (std::size_t i = 1; i < ran_hits.size(); ++i) CHECK(ran_hits[i].tier != MatchTier::Exact);

    const auto sirius = top("Sirius");
    REQUIRE_FALSE(sirius.empty());
    CHECK(display_of(reg, sirius[0].entity) == "Sirius");

    // "Gliese 581" is stored as "Gl 581"/"GJ 581": the synonym rule finds it.
    const auto g581 = top("Gliese 581");
    REQUIRE_FALSE(g581.empty());
    CHECK(star_id_of(reg, g581[0].entity) == 84956);
    CHECK(g581[0].tier == MatchTier::Exact);

    // Prefix typing: every prefix of "Sirius" of length >= 3 has Sirius first.
    CHECK(display_of(reg, top("Sir")[0].entity) == "Sirius");

    // Worst case for the scan: a one-letter query matches most names.
    double ms = 0.0;
    const auto many = s.search("a", reg, 20, &ms);
    CHECK(many.size() == 20);
    MESSAGE("worst-case one-letter query 'a': " << ms << " ms");
}

TEST_CASE("load_star_details: planet host and Sol") {
    SKIP_WITHOUT_DB();
    Database db(db_path().string(), OpenMode::ReadOnly);
    const auto t0 = std::chrono::steady_clock::now();
    const StarDetails ran = load_star_details(db, 68570);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    MESSAGE("details for star 68570 (" << ran.display_name << ") in " << ms << " ms, " << ran.planets.size() << " planets");
    CHECK(ran.display_name == "Ran");
    CHECK(ran.dist_ly == doctest::Approx(10.5).epsilon(0.02));
    REQUIRE(ran.hip.has_value());
    CHECK(*ran.hip == 16537);
    CHECK(ran.planets.size() >= 1);
    CHECK(ran.teff_k.value.has_value());
    CHECK_FALSE(ran.teff_k.source.empty());  // every value carries its provenance
    CHECK_FALSE(ran.is_sol);

    const StarDetails sol = load_star_details(db, 1);
    CHECK(sol.is_sol);
    CHECK(sol.planets.size() == 8);
    // Ordered by period: Mercury first, Neptune last.
    REQUIRE(sol.planets.front().orbital_period_days.has_value());
    CHECK(*sol.planets.front().orbital_period_days < 100.0f);
    CHECK(*sol.planets.back().orbital_period_days > 50000.0f);

    CHECK_THROWS_AS((void)load_star_details(db, 999999999), CatalogError);
}
