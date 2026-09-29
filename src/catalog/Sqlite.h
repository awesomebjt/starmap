#pragma once
// =============================================================================
// Sqlite.h — a tiny RAII layer over the SQLite C API
// =============================================================================
//
// ROLE IN THE ARCHITECTURE
//   Everything that reads stars.db goes through the three classes declared
//   here: Database, Statement and Transaction. CatalogLoader.cpp is the main
//   user. Nothing else in the program includes <sqlite3.h>.
//
// WHY WRAP THE C API AT ALL?
//   SQLite hands out raw handles (sqlite3*, sqlite3_stmt*) that must be closed
//   and finalized by hand, and it reports errors as integer codes. In C++ that
//   leads to leaks whenever an early `return` or an exception skips the
//   cleanup. RAII ("Resource Acquisition Is Initialization") ties each handle's
//   lifetime to a C++ object:
//     * the constructor acquires the handle (open / prepare / BEGIN);
//     * the destructor releases it (close / finalize / ROLLBACK), and it runs
//       automatically on every exit path, including stack unwinding;
//     * error codes become C++ exceptions (CatalogError), so callers cannot
//       silently ignore a failure.
//
// WHY NOT AN ORM OR A THIRD-PARTY WRAPPER (sqlite_orm, SQLiteCpp, ...)?
//   The loader runs about six queries and walks each result set once. Roughly
//   150 lines of wrapper is easier to read, and to learn from, than another
//   dependency. It also leaves the actual SQL visible in CatalogLoader.cpp.
//
// HEADER HYGIENE: FORWARD DECLARATIONS
//   This header does NOT include <sqlite3.h>. It only forward-declares the two
//   opaque handle types below. Any code can therefore include "Sqlite.h"
//   without having SQLite's include path, and SQLite's macros (there are
//   hundreds of SQLITE_* names) don't leak into every file. Only Sqlite.cpp
//   sees the real API. This works because we only ever store POINTERS to
//   these types, and a pointer to an incomplete type is fine in C++.
//
// THREADING
//   One Database per thread. SQLite itself can be built thread-safe, but a
//   Statement belongs to one connection and must not be stepped from two
//   threads at once. The loader is single-threaded, so this never comes up.
// =============================================================================

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

struct sqlite3;       // opaque connection handle (defined inside SQLite)
struct sqlite3_stmt;  // opaque prepared-statement handle

namespace starmap::catalog {

// Every failure in this library throws this: SQLite errors, a missing file,
// a schema mismatch. It derives from std::runtime_error, so a top-level
// `catch (const std::exception& e)` in main() prints e.what() and nothing
// SQLite-specific leaks out. `using runtime_error::runtime_error;` inherits
// the (const std::string&) and (const char*) constructors instead of
// re-declaring them.
class CatalogError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

enum class OpenMode {
    ReadOnly,         // SQLITE_OPEN_READONLY. The loader uses this: the viewer must never modify the catalog,
                      // and a read-only open also works on a file in a read-only directory.
    ReadWriteCreate,  // READWRITE | CREATE, for unit tests and tools that build a small db (often ":memory:").
};

// -----------------------------------------------------------------------------
// Database — owns one sqlite3* connection.
//
// Move-only: copying would give two objects the same handle, and both
// destructors would close it (a double free). Moving transfers ownership and
// leaves the source empty (nullptr), and closing nullptr is a no-op.
// -----------------------------------------------------------------------------
class Database {
public:
    // Opens `path`. The special name ":memory:" gives a private in-RAM
    // database, which is handy for tests. Throws CatalogError on failure.
    // `explicit` stops a std::string from silently converting to a Database.
    explicit Database(const std::string& path, OpenMode mode = OpenMode::ReadOnly);
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    Database(Database&& other) noexcept;             // noexcept matters: std::vector only moves
    Database& operator=(Database&& other) noexcept;  // elements on reallocation if the move can't throw

    // Runs one or more ';'-separated SQL statements whose result rows we
    // don't need (CREATE TABLE, INSERT in tests, BEGIN/COMMIT). This goes
    // through sqlite3_exec, which prepares, steps and finalizes internally.
    // Never build SQL for exec() from untrusted strings; use a Statement with
    // bound parameters for anything that takes a value.
    void exec(std::string_view sql);

    // True if a table (or view) with this name exists. The loader uses it to
    // give a clear "wrong database?" error instead of an obscure prepare failure.
    [[nodiscard]] bool has_table(std::string_view name);

    // The raw handle, for Statement's constructor. [[nodiscard]] makes the
    // compiler warn if someone calls a getter and throws the result away.
    [[nodiscard]] sqlite3* handle() const noexcept { return db_; }
    [[nodiscard]] std::string last_error() const;

private:
    sqlite3* db_ = nullptr;
};

// -----------------------------------------------------------------------------
// Statement — owns one prepared statement (sqlite3_stmt*).
//
// How SQLite runs a query:
//   1. PREPARE: compile the SQL text into bytecode once (the constructor).
//   2. BIND:    fill the '?1' / ':name' placeholders with values. This is the
//               only safe way to put data into SQL: no quoting bugs and no SQL
//               injection, and the compiled plan can be reused.
//   3. STEP:    run until the next result row (true) or the end (false).
//               Read columns while positioned on a row.
//   4. RESET:   rewind to run it again (bindings are kept), or
//      FINALIZE: free it (the destructor).
//
// Index conventions are a classic pitfall, because SQLite mixes them:
//   * bind indices are 1-based (?1 is index 1);
//   * column indices are 0-based.
// -----------------------------------------------------------------------------
class Statement {
public:
    // Prepares `sql` on `db`. Throws with SQLite's message plus the SQL text if
    // the SQL has a syntax error or names a missing table or column.
    // Lifetime rule: the Statement must be destroyed BEFORE its Database.
    // Declaring the Database first in a scope guarantees that, because C++
    // destroys locals in reverse order of declaration.
    Statement(Database& db, std::string_view sql);
    ~Statement();

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    Statement(Statement&& other) noexcept;
    Statement& operator=(Statement&& other) noexcept;

    // --- binding (1-based parameter index, as in SQLite) -----------------
    // Overloads choose the right sqlite3_bind_* function from the C++ type.
    void bind(int index, std::int64_t value);
    // Without this `int` overload, bind(1, 5) would be ambiguous between
    // int64_t and double, since both are standard conversions from int.
    void bind(int index, int value) { bind(index, static_cast<std::int64_t>(value)); }
    void bind(int index, double value);
    void bind(int index, std::string_view value);  // copied by SQLite (SQLITE_TRANSIENT), so the view may die afterwards
    void bind(int index, std::nullopt_t);          // SQL NULL
    // std::optional<T> maps naturally to "value or NULL". This one template
    // forwards to the overloads above.
    template <typename T>
    void bind(int index, const std::optional<T>& value) {
        if (value) bind(index, *value); else bind(index, std::nullopt);
    }
    // Looks up a named parameter such as ":max_ly" and returns its index.
    // Throws if the name isn't in the SQL, which catches typos straight away
    // (SQLite itself would just return 0).
    [[nodiscard]] int parameter_index(const char* name) const;

    // --- execution ---------------------------------------------------------
    // Advances to the next row. Returns true if a row is available and false
    // when the result set is exhausted. Any other outcome (constraint
    // violation, I/O error, SQLITE_BUSY) throws. Typical loop:
    //     while (st.step()) { auto x = st.column_double(0); ... }
    bool step();
    void reset();  // rewind for re-execution (bindings are KEPT; SQLite has sqlite3_clear_bindings to drop them)

    // --- reading columns (0-based). NULL -> std::nullopt ---------------------
    // The catalog has many optional fields (teff_k, radius, age...). Returning
    // std::optional forces the caller to decide what "unknown" means, instead
    // of SQLite's silent default (NULL reads back as 0 or 0.0, which would put
    // a star at 0 K or give it zero radius).
    [[nodiscard]] int column_count() const;
    [[nodiscard]] std::string_view column_name(int col) const;
    [[nodiscard]] bool is_null(int col) const;
    [[nodiscard]] std::optional<std::int64_t> column_int64(int col) const;
    [[nodiscard]] std::optional<double> column_double(int col) const;
    // double narrowed to float. Positions and physical quantities are stored
    // as float in the ECS because that is what the GPU consumes, and 7
    // significant digits is plenty for a 200 ly map.
    [[nodiscard]] std::optional<float> column_float(int col) const;
    [[nodiscard]] std::optional<std::string> column_text(int col) const;
    // Zero-copy view into SQLite's own row buffer. It is valid only until the
    // next step()/reset()/destruction. Keeping it longer is a dangling-view
    // bug, so copy into std::string (column_text) if the value must outlive
    // the row. Returns an empty view for NULL.
    [[nodiscard]] std::string_view column_text_view(int col) const;

private:
    void check(int rc, const char* what) const;  // throws CatalogError unless rc == SQLITE_OK

    sqlite3* db_ = nullptr;  // not owned: used only to fetch error messages
    sqlite3_stmt* stmt_ = nullptr;
};

// -----------------------------------------------------------------------------
// Transaction — BEGIN on construction, COMMIT on commit(), ROLLBACK in the
// destructor otherwise (the "scope guard" idiom).
//
// Why a READ-ONLY loader wants a transaction:
//   * Consistent snapshot: all six loader queries see the same database
//     state, even if another process writes to stars.db meanwhile.
//   * Speed: outside a transaction, every statement implicitly takes and
//     drops SQLite's shared file lock. One BEGIN covers them all.
// For writers (tests building a db) the gain is much larger: without a
// transaction each INSERT is its own fsync'd commit, roughly 1000x slower.
// -----------------------------------------------------------------------------
class Transaction {
public:
    explicit Transaction(Database& db);
    ~Transaction();  // rolls back if commit() was never reached (e.g. an exception)
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    void commit();

private:
    Database& db_;       // a reference, so the Database must outlive the Transaction
    bool done_ = false;  // set by commit(), so the destructor doesn't ROLLBACK a committed txn
};

}  // namespace starmap::catalog
