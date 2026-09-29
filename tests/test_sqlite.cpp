// Unit tests for the RAII sqlite wrapper, using an in-memory database.
#include "catalog/Sqlite.h"

#include <doctest/doctest.h>

#include <filesystem>
#include <string>
#include <utility>

using namespace starmap::catalog;

namespace {
Database make_db() {
    Database db(":memory:", OpenMode::ReadWriteCreate);
    db.exec(R"(
        CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT, val REAL, big INTEGER);
        INSERT INTO t VALUES (1, 'alpha', 1.5, 5164707970261890560);
        INSERT INTO t VALUES (2, NULL, NULL, NULL);
        INSERT INTO t VALUES (3, 'Omicron² Eridani', -0.25, 42);
    )");
    return db;
}
}  // namespace

TEST_CASE("sqlite: open, exec, step and typed columns") {
    Database db = make_db();
    CHECK(db.has_table("t"));
    CHECK_FALSE(db.has_table("nope"));

    Statement st(db, "SELECT id, name, val, big FROM t ORDER BY id");
    CHECK(st.column_count() == 4);
    CHECK(st.column_name(1) == "name");

    REQUIRE(st.step());
    CHECK(st.column_int64(0) == 1);
    CHECK(st.column_text(1) == std::string("alpha"));
    CHECK(st.column_double(2) == doctest::Approx(1.5));
    CHECK(st.column_float(2) == doctest::Approx(1.5f));
    CHECK(st.column_int64(3) == 5164707970261890560LL);  // full 64-bit precision survives

    REQUIRE(st.step());  // row with NULLs -> nullopt
    CHECK(st.is_null(1));
    CHECK_FALSE(st.column_text(1).has_value());
    CHECK_FALSE(st.column_double(2).has_value());
    CHECK_FALSE(st.column_float(2).has_value());
    CHECK_FALSE(st.column_int64(3).has_value());
    CHECK(st.column_text_view(1).empty());

    REQUIRE(st.step());
    CHECK(st.column_text(1) == std::string("Omicron² Eridani"));  // UTF-8 round trip

    CHECK_FALSE(st.step());  // done
}

TEST_CASE("sqlite: bind positional, named and optional parameters; reset") {
    Database db = make_db();
    Statement st(db, "SELECT COUNT(*) FROM t WHERE (:v IS NULL OR val > :v)");
    const int v = st.parameter_index(":v");
    CHECK(v == 1);

    st.bind(v, 0.0);
    REQUIRE(st.step());
    CHECK(st.column_int64(0) == 1);

    st.reset();
    st.bind(v, std::optional<double>{});  // NULL -> no filter
    REQUIRE(st.step());
    CHECK(st.column_int64(0) == 3);

    Statement ins(db, "INSERT INTO t (id, name, val, big) VALUES (?, ?, ?, ?)");
    ins.bind(1, 10);
    ins.bind(2, std::string_view("beta"));
    ins.bind(3, std::optional<double>(2.0));
    ins.bind(4, std::nullopt);
    CHECK_FALSE(ins.step());

    Statement check(db, "SELECT name, big FROM t WHERE id = ?");
    check.bind(1, std::int64_t{10});
    REQUIRE(check.step());
    CHECK(check.column_text(0) == std::string("beta"));
    CHECK_FALSE(check.column_int64(1).has_value());

    CHECK_THROWS_AS((void)st.parameter_index(":missing"), CatalogError);
}

TEST_CASE("sqlite: errors throw CatalogError with the sqlite message") {
    Database db = make_db();
    try {
        Statement bad(db, "SELECT nope FROM t");
        FAIL("expected CatalogError");
    } catch (const CatalogError& e) {
        CHECK(std::string(e.what()).find("no such column") != std::string::npos);
    }
    CHECK_THROWS_AS(db.exec("CREATE TABLE t (x)"), CatalogError);  // already exists

    const auto missing = (std::filesystem::temp_directory_path() / "starmap_definitely_missing.db").string();
    CHECK_THROWS_AS(Database(missing, OpenMode::ReadOnly), CatalogError);
}

TEST_CASE("sqlite: read-only databases reject writes") {
    const auto path = (std::filesystem::temp_directory_path() / "starmap_ro_test.db").string();
    std::filesystem::remove(path);
    {
        Database rw(path, OpenMode::ReadWriteCreate);
        rw.exec("CREATE TABLE a (x INTEGER); INSERT INTO a VALUES (1);");
    }
    {
        Database ro(path, OpenMode::ReadOnly);
        Statement st(ro, "SELECT x FROM a");
        REQUIRE(st.step());
        CHECK(st.column_int64(0) == 1);
        CHECK_THROWS_AS(ro.exec("INSERT INTO a VALUES (2)"), CatalogError);
    }
    std::filesystem::remove(path);
}

TEST_CASE("sqlite: transactions roll back unless committed; handles are movable") {
    Database db = make_db();
    {
        Transaction tx(db);
        db.exec("DELETE FROM t");
    }  // destructor -> ROLLBACK
    {
        Statement st(db, "SELECT COUNT(*) FROM t");
        REQUIRE(st.step());
        CHECK(st.column_int64(0) == 3);
    }
    {
        Transaction tx(db);
        db.exec("DELETE FROM t WHERE id = 2");
        tx.commit();
    }
    Database moved = std::move(db);
    CHECK(moved.handle() != nullptr);
    Statement st(moved, "SELECT COUNT(*) FROM t");
    Statement st2 = std::move(st);
    REQUIRE(st2.step());
    CHECK(st2.column_int64(0) == 2);
}
