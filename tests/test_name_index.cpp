// NameIndex / StarIndex unit tests (no database needed).
#include "catalog/Indexes.h"
#include "catalog/Sqlite.h"

#include <doctest/doctest.h>
#include <entt/entity/registry.hpp>

using namespace starmap::catalog;

TEST_CASE("NameIndex: ASCII case folding, primary preference, per-entity ranges") {
    entt::registry reg;
    const auto a = reg.create();
    const auto b = reg.create();
    NameIndex idx;
    idx.reserve(8, 2);
    CHECK(idx.add(a, "Ran", "IAU", true));
    CHECK(idx.add(a, "Epsilon Eridani", "bayer", false));
    CHECK(idx.add(a, "ε Eri", "bayer_greek", false));
    CHECK_FALSE(idx.add(a, "Ran", "IAU", false));  // exact duplicate dropped
    CHECK(idx.add(b, "Keid", "IAU", true));
    CHECK(idx.add(b, "Omicron² Eridani", "bayer_unicode", false));
    CHECK(idx.add(b, "Shared", "alias", false));

    CHECK(NameIndex::fold("Omicron² ERIDANI") == "omicron² eridani");
    CHECK(idx.find("RAN") == a);
    CHECK(idx.find("epsilon eridani") == a);
    CHECK(idx.find("OMICRON² ERIDANI") == b);
    CHECK(idx.find("ε Eri") == a);
    CHECK_FALSE(idx.find("Ε ERI").has_value());  // Greek capital: no Unicode folding (documented)
    CHECK_FALSE(idx.find("nothing").has_value());

    CHECK(idx.names_of(a).size() == 3);
    CHECK(idx.names_of(a).front().name == "Ran");
    REQUIRE(idx.primary_of(b) != nullptr);
    CHECK(idx.primary_of(b)->name == "Keid");
    CHECK(idx.name_count() == 6);
    CHECK(idx.star_count() == 2);
    CHECK(idx.names_of(reg.create()).empty());

    // Names must arrive grouped by entity.
    CHECK_THROWS_AS(idx.add(a, "late", "x", false), CatalogError);
}

TEST_CASE("NameIndex: a primary name beats the same name used as an alias elsewhere") {
    entt::registry reg;
    const auto comp_a = reg.create();
    const auto comp_b = reg.create();
    NameIndex idx;
    idx.add(comp_a, "Alpha Centauri A", "bayer_component", true);
    idx.add(comp_a, "Alpha Centauri", "bayer_alias", false);
    idx.add(comp_b, "Alpha Centauri", "bayer", true);
    CHECK(idx.find("alpha centauri") == comp_b);
    CHECK(idx.find_all("alpha centauri").size() == 2);
}

TEST_CASE("NameIndex: a primary name added late is moved to the front") {
    entt::registry reg;
    const auto e = reg.create();
    NameIndex idx;
    idx.add(e, "HIP 1", "HIP", false);
    idx.add(e, "Gl 2", "gliese", false);
    idx.add(e, "Star", "proper", true);
    REQUIRE(idx.names_of(e).size() == 3);
    CHECK(idx.names_of(e).front().name == "Star");
    CHECK(idx.names_of(e)[2].name == "HIP 1");
    CHECK(idx.find("hip 1") == e);  // map slots were patched
    CHECK(idx.find("star") == e);
    CHECK(idx.find("gl 2") == e);
}

TEST_CASE("StarIndex") {
    entt::registry reg;
    const auto e = reg.create();
    StarIndex si;
    si.add(42, e);
    CHECK(si.find(42) == e);
    CHECK_FALSE(si.find(7).has_value());
    CHECK(si.size() == 1);
}
