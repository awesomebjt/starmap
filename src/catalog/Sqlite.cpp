// =============================================================================
// Sqlite.cpp — implementation of the RAII SQLite wrapper (see Sqlite.h)
// =============================================================================
//
// This is the ONLY translation unit that includes <sqlite3.h>. Everything
// else talks to SQLite through Database / Statement / Transaction.
//
// Every SQLite call returns an int result code: SQLITE_OK (0), SQLITE_ROW,
// SQLITE_DONE, or an error. The pattern throughout this file is: call, check
// the code, and on failure throw a CatalogError whose text includes
// sqlite3_errmsg(), which is the human-readable message for the most recent
// failure on that connection.
// =============================================================================
#include "catalog/Sqlite.h"

#include <sqlite3.h>

#include <utility>  // std::exchange

namespace starmap::catalog {

// ============================================================ Database
Database::Database(const std::string& path, OpenMode mode) {
    // sqlite3_open_v2 is the modern open call. Unlike the legacy
    // sqlite3_open, it takes explicit flags. READONLY fails cleanly if the
    // file doesn't exist. Without CREATE, a typo'd path would otherwise
    // silently create an empty database and fail later with "no such table".
    const int flags = (mode == OpenMode::ReadOnly)
                          ? SQLITE_OPEN_READONLY
                          : (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
    const int rc = sqlite3_open_v2(path.c_str(), &db_, flags, nullptr);  // last arg: VFS name (nullptr = default)
    if (rc != SQLITE_OK) {
        // Pitfall: sqlite3_open_v2 usually allocates a handle EVEN ON FAILURE.
        // That handle carries the detailed error message and must still be
        // closed, or it leaks. If allocation itself failed (db_ == nullptr),
        // fall back to the generic text for the code.
        std::string msg = db_ ? sqlite3_errmsg(db_) : sqlite3_errstr(rc);
        sqlite3_close(db_);  // closing nullptr is a harmless no-op
        db_ = nullptr;
        throw CatalogError("cannot open database '" + path + "': " + msg);
    }
    // Extended result codes give more precise errors (e.g. SQLITE_IOERR_READ
    // rather than just SQLITE_IOERR), which makes diagnostics better.
    sqlite3_extended_result_codes(db_, 1);
}

Database::~Database() {
    // sqlite3_close_v2 (not plain close): if a Statement is somehow still
    // alive, v2 turns the connection into a "zombie" that is freed when the
    // last statement is finalized. Plain close would return SQLITE_BUSY and
    // leak. Destructors must not throw, so the result code is ignored.
    if (db_) sqlite3_close_v2(db_);
}

// Move constructor: steal the handle and leave `other` empty. std::exchange
// (C++14) returns the old value and writes the new one in a single expression.
Database::Database(Database&& other) noexcept : db_(std::exchange(other.db_, nullptr)) {}

Database& Database::operator=(Database&& other) noexcept {
    if (this != &other) {                 // self-move guard: `a = std::move(a)` must not close our own handle
        if (db_) sqlite3_close_v2(db_);   // release what we currently own
        db_ = std::exchange(other.db_, nullptr);
    }
    return *this;
}

void Database::exec(std::string_view sql) {
    char* err = nullptr;  // SQLite allocates the message with its own allocator...
    // A string_view isn't guaranteed to be NUL-terminated, and sqlite3_exec
    // reads a C string, so copy it. exec() is only used for small admin SQL,
    // so the copy costs nothing that matters.
    const std::string sql_str(sql);
    // Args 3 and 4 are a per-row callback and its user pointer. We don't read
    // rows here, so both are nullptr.
    if (sqlite3_exec(db_, sql_str.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
        std::string msg = err ? err : "unknown error";
        sqlite3_free(err);  // ...so free it with sqlite3_free, never with free()/delete
        throw CatalogError("SQL error: " + msg + " [in: " + sql_str + "]");
    }
}

bool Database::has_table(std::string_view name) {
    // sqlite_master is SQLite's built-in catalog of schema objects. The name
    // is BOUND (?1), not concatenated into the SQL, so any string is safe.
    Statement st(*this, "SELECT 1 FROM sqlite_master WHERE type IN ('table','view') AND name = ?1");
    st.bind(1, name);
    return st.step();  // a row exists <=> the table exists
}

std::string Database::last_error() const { return db_ ? sqlite3_errmsg(db_) : "no database"; }

// ============================================================ Statement
Statement::Statement(Database& db, std::string_view sql) : db_(db.handle()) {
    // prepare_v2 compiles the SQL. Passing the byte length means the text
    // needn't be NUL-terminated, so a string_view works directly with no copy.
    // The last argument (tail) would receive a pointer to any SQL after the
    // first statement. We prepare exactly one statement, so it is nullptr.
    // (The newer prepare_v3 adds flags such as SQLITE_PREPARE_PERSISTENT;
    // v2 is all we need.)
    const int rc = sqlite3_prepare_v2(db_, sql.data(), static_cast<int>(sql.size()), &stmt_, nullptr);
    if (rc != SQLITE_OK) {
        // Including the SQL in the message makes "no such column: teff" errors
        // quick to trace to the query that caused them.
        throw CatalogError("cannot prepare statement: " + std::string(sqlite3_errmsg(db_)) +
                           " [in: " + std::string(sql) + "]");
    }
}

Statement::~Statement() { sqlite3_finalize(stmt_); }  // finalize(nullptr) is a no-op, so moved-from objects are fine

Statement::Statement(Statement&& other) noexcept
    : db_(std::exchange(other.db_, nullptr)), stmt_(std::exchange(other.stmt_, nullptr)) {}

Statement& Statement::operator=(Statement&& other) noexcept {
    if (this != &other) {
        sqlite3_finalize(stmt_);
        db_ = std::exchange(other.db_, nullptr);
        stmt_ = std::exchange(other.stmt_, nullptr);
    }
    return *this;
}

void Statement::check(int rc, const char* what) const {
    if (rc != SQLITE_OK) {
        throw CatalogError(std::string(what) + " failed: " + sqlite3_errmsg(db_));
    }
}

// bind_* errors are nearly always programmer errors: an index out of range,
// or binding while the statement is mid-step without a reset(). Throwing makes
// them loud.
void Statement::bind(int index, std::int64_t value) {
    check(sqlite3_bind_int64(stmt_, index, value), "sqlite3_bind_int64");
}
void Statement::bind(int index, double value) {
    check(sqlite3_bind_double(stmt_, index, value), "sqlite3_bind_double");
}
void Statement::bind(int index, std::string_view value) {
    // SQLITE_TRANSIENT tells SQLite to COPY the bytes now. The alternative,
    // SQLITE_STATIC, promises the buffer stays alive and unchanged until the
    // statement is reset or finalized. That is a dangerous promise for a
    // string_view whose owner may be a temporary.
    check(sqlite3_bind_text(stmt_, index, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT),
          "sqlite3_bind_text");
}
void Statement::bind(int index, std::nullopt_t) {
    check(sqlite3_bind_null(stmt_, index), "sqlite3_bind_null");
}

int Statement::parameter_index(const char* name) const {
    // The name must include the prefix character exactly as written in the
    // SQL, e.g. ":max_ly". SQLite returns 0 for "no such parameter", and 0 is
    // never a valid index, so we turn it into an exception.
    const int idx = sqlite3_bind_parameter_index(stmt_, name);
    if (idx == 0) throw CatalogError(std::string("no SQL parameter named ") + name);
    return idx;
}

bool Statement::step() {
    // sqlite3_step runs the bytecode until it produces a row (SQLITE_ROW) or
    // finishes (SQLITE_DONE). Anything else is an error: SQLITE_BUSY (another
    // process holds a write lock), SQLITE_CORRUPT, a constraint failure, and
    // so on.
    const int rc = sqlite3_step(stmt_);
    if (rc == SQLITE_ROW) return true;
    if (rc == SQLITE_DONE) return false;
    throw CatalogError(std::string("sqlite3_step failed: ") + sqlite3_errmsg(db_));
}

// sqlite3_reset's return code repeats the error of the PREVIOUS step (if
// any), which step() has already reported, so ignoring it here is deliberate.
void Statement::reset() { sqlite3_reset(stmt_); }

int Statement::column_count() const { return sqlite3_column_count(stmt_); }

std::string_view Statement::column_name(int col) const {
    const char* n = sqlite3_column_name(stmt_, col);  // nullptr only on out-of-memory / bad index
    return n ? std::string_view(n) : std::string_view{};
}

// SQLite is dynamically typed per VALUE, not per column. column_type()
// reports the storage class of this row's value, and SQLITE_NULL is how
// "missing" shows up. Pitfall: call column_type BEFORE any column_* getter.
// The getters may convert the stored value in place (e.g. int -> text), after
// which the reported type changes.
bool Statement::is_null(int col) const { return sqlite3_column_type(stmt_, col) == SQLITE_NULL; }

std::optional<std::int64_t> Statement::column_int64(int col) const {
    if (is_null(col)) return std::nullopt;
    return sqlite3_column_int64(stmt_, col);
}

std::optional<double> Statement::column_double(int col) const {
    if (is_null(col)) return std::nullopt;
    return sqlite3_column_double(stmt_, col);
}

std::optional<float> Statement::column_float(int col) const {
    if (is_null(col)) return std::nullopt;
    return static_cast<float>(sqlite3_column_double(stmt_, col));  // explicit narrowing: -Wconversion stays quiet
}

std::optional<std::string> Statement::column_text(int col) const {
    if (is_null(col)) return std::nullopt;
    return std::string(column_text_view(col));  // owning copy that outlives the row
}

std::string_view Statement::column_text_view(int col) const {
    // Order matters. Call sqlite3_column_text() FIRST, then
    // sqlite3_column_bytes(): text() may convert the value to UTF-8, and
    // bytes() then reports the length of that converted text. The reverse
    // order can return a stale length (documented in "Result Values From A
    // Query"). Using the explicit length also keeps embedded NULs.
    // SQLite returns `const unsigned char*`, hence the reinterpret_cast to char.
    const auto* p = reinterpret_cast<const char*>(sqlite3_column_text(stmt_, col));
    if (!p) return {};
    return {p, static_cast<std::size_t>(sqlite3_column_bytes(stmt_, col))};
}

// ============================================================ Transaction
// Plain BEGIN is "deferred": no lock is taken until the first read. That is
// ideal for a reader. Writers that want to fail fast on contention would use
// BEGIN IMMEDIATE.
Transaction::Transaction(Database& db) : db_(db) { db_.exec("BEGIN"); }

Transaction::~Transaction() {
    if (!done_) {
        try {
            db_.exec("ROLLBACK");
        } catch (...) {  // Never throw from a destructor: during stack unwinding that calls std::terminate.
        }
    }
}

void Transaction::commit() {
    db_.exec("COMMIT");
    done_ = true;  // set only AFTER a successful COMMIT: if COMMIT throws, the destructor still rolls back
}

}  // namespace starmap::catalog
